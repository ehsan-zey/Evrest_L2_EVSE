/**
 * @file  hmi.c
 * @brief Indicator and buzzer implementation. See hmi.h.
 */
#include "hmi.h"
#include "evse_board.h"
#include "safety.h"
#include "FreeRTOS.h"
#include "task.h"

extern TIM_HandleTypeDef htim3;

#define LED_OFF_STATE  ((LED_ACTIVE_STATE == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET)

/** One entry of a tone sequence: a frequency held for a duration. */
typedef struct { uint16_t freq_hz; uint16_t ms; } note_t;

static const note_t TONE_PLUG_IN[]      = { {1200, 60}, {1600, 60}, {0, 0} };
static const note_t TONE_AUTHORISED[]   = { {1600, 80}, {2000,120}, {0, 0} };
static const note_t TONE_CHARGE_START[] = { {1000, 80}, {1400, 80}, {1800,120}, {0, 0} };
static const note_t TONE_CHARGE_END[]   = { {1800, 80}, {1400, 80}, {1000,120}, {0, 0} };
static const note_t TONE_REJECTED[]     = { {400, 200}, {0, 80}, {400, 200}, {0, 0} };
static const note_t TONE_FAULT[]        = { {2500, 200}, {0, 200}, {2500, 200}, {0, 600}, {0, 0} };

static const note_t *s_seq;
static uint16_t      s_seq_index;
static uint32_t      s_note_ends_ms;
static bool          s_seq_repeats;

static uint32_t s_phase;   /* increments once per hmi_update() call */

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

static void led_write(GPIO_TypeDef *port, uint16_t pin, bool on)
{
    HAL_GPIO_WritePin(port, pin, on ? LED_ACTIVE_STATE : LED_OFF_STATE);
}

static void buzzer_tone(uint16_t freq_hz)
{
    if (freq_hz == 0u) {
        __HAL_TIM_SET_COMPARE(&htim3, BUZZER_TIM_CHANNEL, 0);
        return;
    }
    /* TIM3 is initialised with a prescaler giving a 1 MHz counter clock. */
    uint32_t arr = (1000000u / freq_hz) - 1u;
    __HAL_TIM_SET_AUTORELOAD(&htim3, arr);
    __HAL_TIM_SET_COMPARE(&htim3, BUZZER_TIM_CHANNEL, arr / 2u);  /* 50 % */
}

void hmi_init(void)
{
    led_write(LED_POWER_GPIO_PORT,  LED_POWER_GPIO_PIN,  false);
    led_write(LED_CHARGE_GPIO_PORT, LED_CHARGE_GPIO_PIN, false);
    led_write(LED_WIFI_GPIO_PORT,   LED_WIFI_GPIO_PIN,   false);
    led_write(LED_FAULT_GPIO_PORT,  LED_FAULT_GPIO_PIN,  false);

    HAL_TIM_PWM_Start(&htim3, BUZZER_TIM_CHANNEL);
    buzzer_tone(0);
    s_seq = NULL;
}

void hmi_set_leds(bool power, bool charge, bool wifi, bool fault)
{
    led_write(LED_POWER_GPIO_PORT,  LED_POWER_GPIO_PIN,  power);
    led_write(LED_CHARGE_GPIO_PORT, LED_CHARGE_GPIO_PIN, charge);
    led_write(LED_WIFI_GPIO_PORT,   LED_WIFI_GPIO_PIN,   wifi);
    led_write(LED_FAULT_GPIO_PORT,  LED_FAULT_GPIO_PIN,  fault);
}

void hmi_play(hmi_tone_t tone)
{
    const note_t *seq = NULL;
    bool repeats = false;

    switch (tone) {
        case HMI_TONE_PLUG_IN:      seq = TONE_PLUG_IN;      break;
        case HMI_TONE_AUTHORISED:   seq = TONE_AUTHORISED;   break;
        case HMI_TONE_CHARGE_START: seq = TONE_CHARGE_START; break;
        case HMI_TONE_CHARGE_END:   seq = TONE_CHARGE_END;   break;
        case HMI_TONE_REJECTED:     seq = TONE_REJECTED;     break;
        case HMI_TONE_FAULT:        seq = TONE_FAULT; repeats = true; break;
        default:                    seq = NULL;              break;
    }

    /* A repeating fault alarm outranks anything else that might be playing. */
    if (s_seq_repeats && !repeats) {
        return;
    }
    s_seq = seq;
    s_seq_index = 0;
    s_seq_repeats = repeats;
    s_note_ends_ms = now_ms();
}

/** Advance the tone sequencer. Called once per hmi_update(). */
static void buzzer_service(void)
{
    if (s_seq == NULL) {
        buzzer_tone(0);
        return;
    }
    uint32_t t = now_ms();
    if ((int32_t)(t - s_note_ends_ms) < 0) {
        return;                                   /* current note still playing */
    }

    const note_t *n = &s_seq[s_seq_index];
    if (n->ms == 0u && n->freq_hz == 0u) {        /* sentinel: end of sequence */
        if (s_seq_repeats) {
            s_seq_index = 0;
            n = &s_seq[0];
        } else {
            s_seq = NULL;
            s_seq_repeats = false;
            buzzer_tone(0);
            return;
        }
    }
    buzzer_tone(n->freq_hz);
    s_note_ends_ms = t + n->ms;
    s_seq_index++;
}

/** @return true when a blink with @p period_ms should currently be lit. */
static bool blink(uint32_t period_ms)
{
    uint32_t ticks_per_period = period_ms / TASK_PERIOD_HMI_MS;
    if (ticks_per_period < 2u) ticks_per_period = 2u;
    return ((s_phase % ticks_per_period) < (ticks_per_period / 2u));
}

void hmi_update(const evse_status_t *st)
{
    s_phase++;

    bool power  = true;                 /* lit whenever the logic is alive */
    bool charge = false;
    bool wifi   = false;
    bool fault  = false;

    switch (st->state) {
        case EVSE_STATE_BOOT:
            power = blink(200);
            break;
        case EVSE_STATE_IDLE:
            break;
        case EVSE_STATE_CONNECTED:
        case EVSE_STATE_PREPARING:
            charge = blink(1000);       /* slow: waiting on authorisation */
            break;
        case EVSE_STATE_CHARGING:
            charge = true;              /* solid: energy flowing */
            break;
        case EVSE_STATE_SUSPENDED_EV:
        case EVSE_STATE_SUSPENDED_EVSE:
            charge = blink(2000);       /* very slow: paused */
            break;
        case EVSE_STATE_FINISHING:
            charge = blink(400);        /* fast: unplug me */
            break;
        case EVSE_STATE_RESERVED:
            charge = blink(3000);
            break;
        case EVSE_STATE_UNAVAILABLE:
            power = blink(2000);
            break;
        case EVSE_STATE_FAULTED:
            fault = blink(500);
            break;
        case EVSE_STATE_LOCKOUT:
            fault = true;               /* solid red: needs service */
            break;
    }

    /*
     * Wi-Fi LED distinguishes three things the installer actually cares about:
     * no link at all, link but no CSMS, and fully online.
     */
    if (!st->network_up)        wifi = false;
    else if (!st->csms_connected) wifi = blink(500);
    else                          wifi = true;

    hmi_set_leds(power, charge, wifi, fault);

    if ((st->faults & EVSE_FAULT_TRIP_MASK) != 0u) {
        hmi_play(HMI_TONE_FAULT);
    } else if (s_seq_repeats) {
        s_seq_repeats = false;          /* let the alarm finish and stop */
        s_seq = NULL;
    }

    buzzer_service();
}
