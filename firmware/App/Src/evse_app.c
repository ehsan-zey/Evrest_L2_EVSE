/**
 * @file  evse_app.c
 * @brief Task creation, power-on self-test and the main loops.
 */
#include "evse_app.h"
#include "evse_board.h"
#include "evse_config.h"
#include "evse_hw.h"
#include "evse_sm.h"
#include "cp_pilot.h"
#include "proximity.h"
#include "relay.h"
#include "safety.h"
#include "meter.h"
#include "hmi.h"
#include "rtc_time.h"
#include "ocpp_client.h"
#include "net_link.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* Static allocation throughout: no heap means no fragmentation and no
 * allocation failure to handle at 3am on a live site. */
#define STACK_SAFETY   512
#define STACK_PILOT    512
#define STACK_EVSE     1024
#define STACK_METER    512
#define STACK_HMI      384
#define STACK_OCPP     4096   /* TLS and JSON both want room */

static StackType_t  s_stack_safety[STACK_SAFETY];
static StackType_t  s_stack_pilot[STACK_PILOT];
static StackType_t  s_stack_evse[STACK_EVSE];
static StackType_t  s_stack_meter[STACK_METER];
static StackType_t  s_stack_hmi[STACK_HMI];
static StackType_t  s_stack_ocpp[STACK_OCPP];
static StaticTask_t s_tcb_safety, s_tcb_pilot, s_tcb_evse,
                    s_tcb_meter, s_tcb_hmi, s_tcb_ocpp;

static volatile bool s_selftest_done;

/* ---------------------------------------------------------------------- */
/* Tasks                                                                  */
/* ---------------------------------------------------------------------- */

static void task_safety(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        meter_reading_t m;
        meter_get(&m);

        evse_status_t st;
        evse_sm_get_status(&st);

        safety_update(&m, st.offered_a, relay_is_closed());

        vTaskDelayUntil(&next, pdMS_TO_TICKS(TASK_PERIOD_SAFETY_MS));
    }
}

static void task_pilot(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        cp_pilot_update();
        vTaskDelayUntil(&next, pdMS_TO_TICKS(TASK_PERIOD_PILOT_MS));
    }
}

static void task_meter(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    uint32_t checkpoint_counter = 0;

    for (;;) {
        meter_update();

        /*
         * Checkpoint the lifetime energy register to flash every few minutes.
         * Often enough that a power cut loses only a trivial amount, rarely
         * enough not to wear the sector out — at one write per 5 minutes a
         * 10 000-cycle sector lasts about 95 years.
         */
        if (++checkpoint_counter >= (300000u / TASK_PERIOD_METER_MS)) {
            checkpoint_counter = 0;
            uint64_t wh = meter_energy_wh();
            if (cfg_get()->energy_total_wh != wh) {
                cfg_mutable()->energy_total_wh = wh;
                (void)cfg_save();
            }
        }

        vTaskDelayUntil(&next, pdMS_TO_TICKS(TASK_PERIOD_METER_MS));
    }
}

static void task_hmi(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        evse_status_t st;
        evse_sm_get_status(&st);
        st.network_up     = net_link_is_up();
        st.csms_connected = ocpp_is_connected();
        hmi_update(&st);
        vTaskDelayUntil(&next, pdMS_TO_TICKS(TASK_PERIOD_HMI_MS));
    }
}

/* ---------------------------------------------------------------------- */
/* Power-on self-test                                                     */
/* ---------------------------------------------------------------------- */

/**
 * Run before the pilot is ever offered to a vehicle.
 *
 * The order matters: prove the contactor is open before doing anything that
 * could energise it, and run the RCD test while it is still open, because the
 * test deliberately trips the device.
 */
static void run_selftest(void)
{
    safety_selftest_result_t r;

    hmi_set_leds(true, false, false, false);

    safety_selftest(&r);

    meter_init_result_t mr = meter_init(METER_SAG_THRESHOLD_V);
    r.meter_ok = (mr == METER_INIT_OK);
    if (!r.meter_ok) {
        /*
         * A dead meter is not a reason to refuse to charge — but it is a reason
         * to refuse to *bill*, and without current measurement there is no
         * over-current protection at all (see docs/METERING.md), so it is
         * treated as a hard failure rather than a degraded mode.
         */
        printf("selftest: meter init failed (%d)\r\n", (int)mr);
    }

    r.pilot_ok = cp_pilot_init();
    if (!r.pilot_ok) {
        printf("selftest: pilot init failed\r\n");
    }

    bool passed = r.pe_ok && r.pen_ok && r.relay_open_ok
               && r.rcd_selftest_ok && r.meter_ok && r.pilot_ok;

    printf("selftest: pe=%d pen=%d relay=%d rcd=%d meter=%d pilot=%d -> %s\r\n",
           r.pe_ok, r.pen_ok, r.relay_open_ok, r.rcd_selftest_ok,
           r.meter_ok, r.pilot_ok, passed ? "PASS" : "FAIL");

    if (!passed) {
        /*
         * Self-test failure is latching. Something that protects the user is
         * missing or unverified, and a power cycle must not be a way to make
         * that go away — it will simply fail the test again.
         */
        relay_inhibit();
    }
    s_selftest_done = true;
}

/* ---------------------------------------------------------------------- */
/* EVSE task — owns boot sequencing and then the state machine             */
/* ---------------------------------------------------------------------- */

static void task_evse(void *arg)
{
    (void)arg;

    cfg_init();
    rtc_time_init();

    const evse_config_t *cfg = cfg_get();
    safety_init(cfg->earthing);
    proximity_init(cfg->connector_type);
    cp_pilot_set_calibration(cfg->cp_cal_scale_num,
                             cfg->cp_cal_scale_den,
                             cfg->cp_cal_offset_mv);
    meter_set_energy_wh(cfg->energy_total_wh);

    printf("\r\n%s %s fw %s  boot #%lu  installed limit %.0f A (DIP %u)\r\n",
           EVSE_VENDOR_NAME, EVSE_MODEL_NAME, EVSE_FW_VERSION,
           (unsigned long)cfg->boot_count,
           (double)cfg_dip_current_a(), cfg_dip_code());

    run_selftest();

    evse_sm_init(ocpp_get_sm_callbacks());

    /* Only now is it safe to let the network stack start talking. */
    ocpp_client_start();

    TickType_t next = xTaskGetTickCount();
    for (;;) {
        evse_sm_update();
        vTaskDelayUntil(&next, pdMS_TO_TICKS(TASK_PERIOD_EVSE_MS));
    }
}

/* ---------------------------------------------------------------------- */

void evse_app_start(void)
{
    if (!evse_hw_init()) {
        /*
         * A peripheral refused its configuration. The contactor is already open
         * (evse_hw_init opens it first), so sit here with the fault light on
         * rather than starting a scheduler that would drive hardware we could
         * not configure.
         */
        hmi_set_leds(true, false, false, true);
        for (;;) { __NOP(); }
    }
    hmi_init();

    xTaskCreateStatic(task_safety, "safety", STACK_SAFETY, NULL,
                      TASK_PRIO_SAFETY, s_stack_safety, &s_tcb_safety);
    xTaskCreateStatic(task_pilot,  "pilot",  STACK_PILOT,  NULL,
                      TASK_PRIO_PILOT,  s_stack_pilot,  &s_tcb_pilot);
    xTaskCreateStatic(task_evse,   "evse",   STACK_EVSE,   NULL,
                      TASK_PRIO_EVSE,   s_stack_evse,   &s_tcb_evse);
    xTaskCreateStatic(task_meter,  "meter",  STACK_METER,  NULL,
                      TASK_PRIO_METER,  s_stack_meter,  &s_tcb_meter);
    xTaskCreateStatic(task_hmi,    "hmi",    STACK_HMI,    NULL,
                      TASK_PRIO_HMI,    s_stack_hmi,    &s_tcb_hmi);
    xTaskCreateStatic(task_ocpp,   "ocpp",   STACK_OCPP,   NULL,
                      TASK_PRIO_OCPP,   s_stack_ocpp,   &s_tcb_ocpp);

    vTaskStartScheduler();

    /* Only reached if the scheduler could not start. */
    hmi_set_leds(true, false, false, true);
    for (;;) { __NOP(); }
}

/* ---------------------------------------------------------------------- */
/* FreeRTOS hooks                                                         */
/* ---------------------------------------------------------------------- */

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    /*
     * A stack overflow has already corrupted something. Open the contactor
     * before doing anything else, then stop — continuing to run charging logic
     * on a corrupted stack is worse than being visibly dead.
     */
    relay_emergency_open();
    printf("FATAL: stack overflow in %s\r\n", name);
    __disable_irq();
    for (;;) { }
}

void vApplicationMallocFailedHook(void)
{
    relay_emergency_open();
    printf("FATAL: malloc failed\r\n");
    __disable_irq();
    for (;;) { }
}
