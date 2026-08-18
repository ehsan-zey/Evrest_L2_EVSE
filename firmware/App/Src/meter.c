/**
 * @file  meter.c
 * @brief ATM90E26 driver over UART4. See meter.h and docs/METERING.md.
 */
#include "meter.h"
#include "evse_board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

extern UART_HandleTypeDef METER_UART_HANDLE;
#define METER_UART (&METER_UART_HANDLE)

/* ====================================================================== */
/* ATM90E26 register map                                                  */
/* All registers are 16 bit.                                              */
/* ====================================================================== */

#define ATM_SOFT_RESET      0x00u   /* write 0x789A to reset               */
#define ATM_SYS_STATUS      0x01u   /* sag / reverse-energy flags          */
#define ATM_FUNC_EN         0x02u   /* interrupt enables                   */
#define ATM_SAG_TH          0x03u   /* voltage sag threshold               */
#define ATM_SMALL_P_MOD     0x04u
#define ATM_LAST_DATA       0x06u   /* last read/write value, for checksum */

#define ATM_CAL_START       0x20u   /* 0x5678 begins, 0x8765 commits       */
#define ATM_U_GAIN          0x21u
#define ATM_I_GAIN_L        0x22u
#define ATM_I_GAIN_N        0x23u
#define ATM_U_OFFSET        0x24u
#define ATM_I_OFFSET_L      0x25u
#define ATM_CS_TWO          0x2Bu   /* checksum over the 0x21..0x2A block  */

#define ATM_MET_CAL_START   0x08u   /* 0x5678 begins, 0x8765 commits       */
#define ATM_PL_CONST_H      0x09u
#define ATM_PL_CONST_L      0x0Au
#define ATM_L_GAIN          0x0Bu
#define ATM_L_PHI           0x0Cu
#define ATM_P_START_TH      0x0Fu
#define ATM_P_NOL_TH        0x10u
#define ATM_Q_START_TH      0x11u
#define ATM_Q_NOL_TH        0x12u
#define ATM_M_MODE          0x13u   /* metering mode                       */
#define ATM_CS_ONE          0x14u   /* checksum over the 0x09..0x13 block  */

#define ATM_AP_ENERGY       0x40u   /* forward active energy, read-clears  */
#define ATM_AN_ENERGY       0x41u
#define ATM_EN_STATUS       0x46u
#define ATM_I_RMS           0x48u
#define ATM_U_RMS           0x49u
#define ATM_P_MEAN          0x4Au   /* signed mean active power            */
#define ATM_Q_MEAN          0x4Bu
#define ATM_FREQ            0x4Cu
#define ATM_POWER_F         0x4Du   /* signed power factor                 */
#define ATM_S_MEAN          0x4Fu

/** Address byte: bit 7 set = read, clear = write. */
#define ATM_READ_FLAG       0x80u

/* SysStatus bits. */
#define ATM_STATUS_SAG_WARN     (1u << 1)
#define ATM_STATUS_REVP_CHG     (1u << 12)
#define ATM_STATUS_REVQ_CHG     (1u << 11)

/* FuncEn bits: enable the sag and reverse-power interrupts on the IRQ pin. */
#define ATM_FUNC_EN_SAG         (1u << 1)
#define ATM_FUNC_EN_REVP        (1u << 2)
#define ATM_FUNC_EN_REVQ        (1u << 3)

/* ---------------------------------------------------------------------- */
/* Register scaling (ATM90E26 datasheet, metering registers)              */
/* ---------------------------------------------------------------------- */

#define ATM_URMS_LSB_V          0.01f    /* Urms  : 1 LSB = 0.01 V         */
#define ATM_IRMS_LSB_A          0.001f   /* Irms  : 1 LSB = 0.001 A        */
#define ATM_PMEAN_LSB_W         1.0f     /* Pmean : 1 LSB = 1 W            */
#define ATM_FREQ_LSB_HZ         0.01f    /* Freq  : 1 LSB = 0.01 Hz        */
#define ATM_PF_LSB              0.001f   /* PowerF: 1 LSB = 0.001          */

/**
 * Calibration.
 *
 * These are the values that make the IC read correctly for this board's CT
 * burden and voltage divider, and they MUST be replaced with the results of a
 * production calibration against a reference source. The defaults below are
 * the ATM90E26 datasheet reference-design values for a 1 mV/A shunt-equivalent
 * front end at 240 V, which will get you within a few percent but not to
 * billing accuracy.
 *
 * MMode 0x9422: L line only, current channel gain 4x, no anti-tamper.
 * PLconst for 3200 imp/kWh at the reference design's gain.
 */
#define ATM_CAL_MMODE           0x9422u
#define ATM_CAL_PLCONST_H       0x0015u
#define ATM_CAL_PLCONST_L       0xD174u
#define ATM_CAL_LGAIN           0x0000u
#define ATM_CAL_LPHI            0x0000u
#define ATM_CAL_PSTARTTH        0x08BDu
#define ATM_CAL_PNOLTH          0x0000u
#define ATM_CAL_QSTARTTH        0x0AECu
#define ATM_CAL_QNOLTH          0x0000u
#define ATM_CAL_UGAIN           0xD464u
#define ATM_CAL_IGAINL          0x6E49u
#define ATM_CAL_IGAINN          0x7530u
#define ATM_CAL_UOFFSET         0x0000u
#define ATM_CAL_IOFFSETL        0x0000u

/** SagTh scaling: threshold = Vsag_rms * sqrt(2) * 100 * (2/Ugain_ratio). */
#define ATM_SAG_SCALE           0.0518f

/** Below this the CT output is indistinguishable from noise. */
#define METER_CURRENT_DEADBAND_A  0.05f
/** Consecutive failed transfers before we declare the IC missing. */
#define METER_FAIL_THRESHOLD      4u

/* ---------------------------------------------------------------------- */

static meter_reading_t   s_reading;
static uint64_t          s_energy_mwh;
static uint32_t          s_last_update_ms;
static uint8_t           s_fail_count;
static volatile bool     s_irq_pending;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_mem;
/** Plain copy of Irms so the safety task never blocks on the UART mutex. */
static volatile float    s_last_current_a;
/** Latched sag event, consumed by meter_take_sag_event(). */
static volatile bool     s_sag_latched;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* ---------------------------------------------------------------------- */
/* Serial protocol                                                        */
/*                                                                        */
/* Write: host sends [addr][dataH][dataL][chksum]                         */
/* Read : host sends [addr|0x80], device replies [dataH][dataL][chksum]   */
/* Checksum is the 8-bit sum of the preceding bytes of the frame.         */
/* ---------------------------------------------------------------------- */

static uint8_t atm_checksum(const uint8_t *bytes, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum = (uint8_t)(sum + bytes[i]);
    return sum;
}

/** Drop anything stale in the receive path before starting a transaction. */
static void atm_flush_rx(void)
{
    uint8_t junk;
    while (HAL_UART_Receive(METER_UART, &junk, 1, 0) == HAL_OK) { }
    __HAL_UART_CLEAR_OREFLAG(METER_UART);
}

static bool atm_read(uint8_t reg, uint16_t *value)
{
    uint8_t addr = (uint8_t)(reg | ATM_READ_FLAG);
    uint8_t rx[3] = {0};

    atm_flush_rx();
    if (HAL_UART_Transmit(METER_UART, &addr, 1, METER_RESPONSE_TIMEOUT_MS) != HAL_OK) {
        return false;
    }
    if (HAL_UART_Receive(METER_UART, rx, sizeof(rx), METER_RESPONSE_TIMEOUT_MS) != HAL_OK) {
        return false;
    }

    /*
     * The device includes the address byte it received in the checksum, so the
     * sum runs over {addr, dataH, dataL}.
     */
    uint8_t frame[3] = { addr, rx[0], rx[1] };
    if (atm_checksum(frame, 3) != rx[2]) {
        return false;
    }

    *value = (uint16_t)((uint16_t)rx[0] << 8 | rx[1]);
    return true;
}

static bool atm_write(uint8_t reg, uint16_t value)
{
    uint8_t tx[4];
    tx[0] = (uint8_t)(reg & 0x7Fu);
    tx[1] = (uint8_t)(value >> 8);
    tx[2] = (uint8_t)(value & 0xFFu);
    tx[3] = atm_checksum(tx, 3);

    atm_flush_rx();
    return HAL_UART_Transmit(METER_UART, tx, sizeof(tx), METER_RESPONSE_TIMEOUT_MS) == HAL_OK;
}

/**
 * Write a register and verify it took.
 *
 * The ATM90E26 mirrors the last value written into LastData (0x06), which is
 * the only acknowledgement the serial protocol offers — a write on its own is
 * fire-and-forget and a corrupted frame is silently discarded by the checksum.
 * Configuration writes therefore always read back.
 */
static bool atm_write_verify(uint8_t reg, uint16_t value)
{
    if (!atm_write(reg, value)) return false;
    uint16_t echo = 0;
    if (!atm_read(ATM_LAST_DATA, &echo)) return false;
    return echo == value;
}

static int16_t atm_signed(uint16_t raw) { return (int16_t)raw; }

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

void meter_irq_isr(void)
{
    /*
     * Only a flag. The ATM90E26's IRQ sources are voltage sag and reverse
     * energy — neither warrants opening the contactor from interrupt context
     * before SysStatus has been read to find out which one fired. The meter
     * task services this within one period.
     */
    s_irq_pending = true;
}

/** SysStatus read. Caller must already hold s_lock. */
static bool atm_status_locked(bool *sag_out, bool *revp_out)
{
    uint16_t status = 0;
    if (!atm_read(ATM_SYS_STATUS, &status)) return false;
    if (sag_out)  *sag_out  = (status & ATM_STATUS_SAG_WARN) != 0u;
    if (revp_out) *revp_out = (status & ATM_STATUS_REVP_CHG) != 0u;
    return true;
}

bool meter_read_status(bool *sag_out, bool *revp_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = atm_status_locked(sag_out, revp_out);
    xSemaphoreGive(s_lock);
    return ok;
}

bool meter_take_sag_event(void)
{
    bool v = s_sag_latched;
    s_sag_latched = false;
    return v;
}

meter_init_result_t meter_init(float sag_threshold_v)
{
    memset(&s_reading, 0, sizeof(s_reading));
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);

    /* Software reset, then let the part settle. */
    (void)atm_write(ATM_SOFT_RESET, 0x789Au);
    vTaskDelay(pdMS_TO_TICKS(100));

    uint16_t status = 0;
    if (!atm_read(ATM_SYS_STATUS, &status)) {
        /*
         * Distinguish "nothing there" from "there but garbled" — a wrong baud
         * or a swapped TX/RX gives the second, and that is worth knowing at
         * the bench rather than chasing a dead IC.
         */
        uint8_t addr = ATM_SYS_STATUS | ATM_READ_FLAG, rx[3];
        atm_flush_rx();
        HAL_UART_Transmit(METER_UART, &addr, 1, METER_RESPONSE_TIMEOUT_MS);
        if (HAL_UART_Receive(METER_UART, rx, 3, METER_RESPONSE_TIMEOUT_MS) == HAL_OK) {
            return METER_INIT_BAD_CHECKSUM;
        }
        return METER_INIT_NO_RESPONSE;
    }

    /* --- Metering calibration block (0x09..0x13), gated by MeterCalStart -- */
    if (!atm_write_verify(ATM_MET_CAL_START, 0x5678u)) return METER_INIT_CAL_REJECTED;
    atm_write(ATM_PL_CONST_H, ATM_CAL_PLCONST_H);
    atm_write(ATM_PL_CONST_L, ATM_CAL_PLCONST_L);
    atm_write(ATM_L_GAIN,     ATM_CAL_LGAIN);
    atm_write(ATM_L_PHI,      ATM_CAL_LPHI);
    atm_write(ATM_P_START_TH, ATM_CAL_PSTARTTH);
    atm_write(ATM_P_NOL_TH,   ATM_CAL_PNOLTH);
    atm_write(ATM_Q_START_TH, ATM_CAL_QSTARTTH);
    atm_write(ATM_Q_NOL_TH,   ATM_CAL_QNOLTH);
    atm_write(ATM_M_MODE,     ATM_CAL_MMODE);
    if (!atm_write_verify(ATM_MET_CAL_START, 0x8765u)) return METER_INIT_CAL_REJECTED;

    /* --- Measurement calibration block (0x21..0x2A), gated by AdjStart ---- */
    if (!atm_write_verify(ATM_CAL_START, 0x5678u))     return METER_INIT_CAL_REJECTED;
    atm_write(ATM_U_GAIN,    ATM_CAL_UGAIN);
    atm_write(ATM_I_GAIN_L,  ATM_CAL_IGAINL);
    atm_write(ATM_I_GAIN_N,  ATM_CAL_IGAINN);
    atm_write(ATM_U_OFFSET,  ATM_CAL_UOFFSET);
    atm_write(ATM_I_OFFSET_L,ATM_CAL_IOFFSETL);
    if (!atm_write_verify(ATM_CAL_START, 0x8765u))     return METER_INIT_CAL_REJECTED;

    /* --- Sag detection and the IRQ line ---------------------------------- */
    uint16_t sag_code = (uint16_t)(sag_threshold_v / ATM_SAG_SCALE);
    atm_write(ATM_SAG_TH, sag_code);
    atm_write(ATM_FUNC_EN, ATM_FUNC_EN_SAG | ATM_FUNC_EN_REVP);

    /* Reading SysStatus clears any flags latched during configuration. */
    (void)atm_read(ATM_SYS_STATUS, &status);

    s_last_update_ms = now_ms();
    s_fail_count = 0;
    s_irq_pending = false;
    return METER_INIT_OK;
}

void meter_update(void)
{
    uint16_t raw_i = 0, raw_u = 0, raw_p = 0, raw_f = 0, raw_pf = 0;

    /*
     * One critical section for the whole update. atm_* helpers assume the
     * caller holds s_lock; taking it twice around a single logical update
     * would let another task interleave a transaction into the middle of ours.
     */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Service a pending IRQ first — reading SysStatus also clears it. */
    if (s_irq_pending) {
        s_irq_pending = false;
        bool sag = false, revp = false;
        if (atm_status_locked(&sag, &revp) && sag) {
            s_sag_latched = true;
        }
    }

    bool ok = atm_read(ATM_I_RMS,   &raw_i)
           && atm_read(ATM_U_RMS,   &raw_u)
           && atm_read(ATM_P_MEAN,  &raw_p)
           && atm_read(ATM_FREQ,    &raw_f)
           && atm_read(ATM_POWER_F, &raw_pf);

    uint32_t t = now_ms();
    uint32_t dt_ms = t - s_last_update_ms;
    s_last_update_ms = t;

    if (!ok) {
        if (s_fail_count < METER_FAIL_THRESHOLD) s_fail_count++;
        if (s_fail_count >= METER_FAIL_THRESHOLD) s_reading.valid = false;
        xSemaphoreGive(s_lock);
        return;
    }
    s_fail_count = 0;

    float amps  = (float)raw_i * ATM_IRMS_LSB_A;
    float volts = (float)raw_u * ATM_URMS_LSB_V;
    float watts = (float)atm_signed(raw_p) * ATM_PMEAN_LSB_W;

    if (amps < METER_CURRENT_DEADBAND_A) {
        amps  = 0.0f;
        watts = 0.0f;
    }

    s_reading.voltage_v      = volts;
    s_reading.current_a      = amps;
    s_reading.active_power_w = watts;
    s_reading.frequency_hz   = (float)raw_f * ATM_FREQ_LSB_HZ;

    /*
     * PowerF is sign-magnitude, not two's complement: bit 15 is the sign and
     * bits 14..0 are the magnitude in 0.001 steps. Treating it as two's
     * complement would turn a leading power factor into a nonsense value.
     */
    {
        float pf_mag = (float)(raw_pf & 0x7FFFu) * ATM_PF_LSB;
        s_reading.power_factor = (raw_pf & 0x8000u) ? -pf_mag : pf_mag;
    }

    /*
     * Energy is integrated in software from mean active power rather than read
     * from APenergy. The energy register is read-to-clear, so any dropped or
     * retried frame silently loses energy; integrating here costs at most one
     * 250 ms sample on a failed read and that failure is visible in the fault
     * counter. Accumulated in mWh so low-power samples are not truncated away.
     */
    if (dt_ms > 0u && dt_ms < 5000u && watts > 0.0f) {
        s_energy_mwh += (uint64_t)((watts * (float)dt_ms) / 3.6f);
    }
    s_reading.energy_wh  = s_energy_mwh / 1000u;
    s_reading.updated_ms = t;
    s_reading.valid = true;
    s_last_current_a = amps;

    xSemaphoreGive(s_lock);
}

void meter_get(meter_reading_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_reading;
    xSemaphoreGive(s_lock);
}

float    meter_last_current_a(void) { return s_last_current_a; }
uint64_t meter_energy_wh(void)      { return s_energy_mwh / 1000u; }
bool     meter_is_faulted(void)     { return s_fail_count >= METER_FAIL_THRESHOLD; }

void meter_set_energy_wh(uint64_t wh)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_energy_mwh = wh * 1000u;
    s_reading.energy_wh = wh;
    xSemaphoreGive(s_lock);
}
