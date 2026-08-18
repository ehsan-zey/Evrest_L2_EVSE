/**
 * @file  relay.c
 * @brief Contactor control. See relay.h.
 */
#include "relay.h"
#include "evse_board.h"
#include "FreeRTOS.h"
#include "task.h"

#define RELAY_INACTIVE_STATE \
    ((RELAY_ACTIVE_STATE == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET)

/** Mains half-cycles to observe before believing the weld sense. */
#define WELD_SAMPLE_COUNT   4u
#define WELD_SAMPLE_GAP_MS  6u

static volatile bool s_closed;
static volatile bool s_inhibited;

static inline void relay_write(GPIO_PinState st)
{
    HAL_GPIO_WritePin(RELAY_GPIO_PORT, RELAY_GPIO_PIN, st);
}

static inline bool downstream_live(void)
{
    return HAL_GPIO_ReadPin(WELD_DETECT_GPIO_PORT, WELD_DETECT_GPIO_PIN)
           == WELD_DETECT_ACTIVE_STATE;
}

void relay_init(void)
{
    /* GPIO mode is set by MX_GPIO_Init(); make certain we start de-energised
     * before any other module runs. */
    relay_write(RELAY_INACTIVE_STATE);
    s_closed = false;
    s_inhibited = false;
}

void relay_emergency_open(void)
{
    /*
     * Deliberately the dumbest possible implementation: one register write and
     * one store. Callable from any context, including the RCD interrupt where
     * the trip budget is tens of microseconds.
     */
    relay_write(RELAY_INACTIVE_STATE);
    s_closed = false;
}

void relay_inhibit(void)
{
    s_inhibited = true;
    relay_emergency_open();
}

bool relay_is_inhibited(void) { return s_inhibited; }
bool relay_is_closed(void)    { return s_closed; }

relay_result_t relay_check_weld(void)
{
    if (s_closed) {
        return RELAY_OK;          /* meaningless while energised */
    }
    for (uint32_t i = 0; i < WELD_SAMPLE_COUNT; i++) {
        if (downstream_live()) {
            return RELAY_ERR_WELDED;
        }
        vTaskDelay(pdMS_TO_TICKS(WELD_SAMPLE_GAP_MS));
    }
    return RELAY_OK;
}

relay_result_t relay_close(void)
{
    if (s_inhibited) {
        return RELAY_ERR_INHIBITED;
    }

    /* Never energise without first proving the contactor was genuinely open. */
    if (relay_check_weld() != RELAY_OK) {
        return RELAY_ERR_WELDED;
    }

    relay_write(RELAY_ACTIVE_STATE);
    s_closed = true;

    /* Pull-in time plus one mains cycle before the sense line means anything. */
    vTaskDelay(pdMS_TO_TICKS(RELAY_CLOSE_TIME_MS + 20U));

    if (!downstream_live()) {
        relay_emergency_open();
        return RELAY_ERR_NO_CLOSE;
    }
    return RELAY_OK;
}

relay_result_t relay_open(void)
{
    relay_write(RELAY_INACTIVE_STATE);
    s_closed = false;

    vTaskDelay(pdMS_TO_TICKS(RELAY_OPEN_TIME_MS + 20U));

    if (downstream_live()) {
        return RELAY_ERR_WELDED;
    }
    return RELAY_OK;
}
