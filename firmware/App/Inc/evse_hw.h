/**
 * @file  evse_hw.h
 * @brief Application-specific peripheral setup, layered on top of CubeMX.
 *
 * CubeMX owns MX_*_Init(); this file owns everything CubeMX cannot express.
 * Calling evse_hw_init() after the MX_ inits re-configures TIM1, ADC1 and TIM3
 * into the arrangement the pilot needs, so the project can be regenerated from
 * the .ioc at any time without losing that setup.
 *
 * Specifically it:
 *   - recomputes the TIM1 prescaler from the live APB2 clock so the pilot stays
 *     at exactly 1 kHz whatever the clock tree does,
 *   - adds TIM1_CH2 and TIM1_CH4 as compare-only ADC triggers,
 *   - rebuilds ADC1 as a hardware-triggered regular group (CP high plateau) plus
 *     a two-rank injected group (CP low plateau, then PP),
 *   - sets the TIM3 prescaler for a 1 MHz buzzer time base,
 *   - enables the EXTI lines the safety path depends on.
 */
#ifndef EVSE_HW_H
#define EVSE_HW_H

#include <stdbool.h>

/**
 * @return false if any peripheral rejected its configuration, in which case the
 *         caller must not energise anything.
 */
bool evse_hw_init(void);

/** Assert then release the Ethernet PHY and the ESP32 reset lines. */
void evse_hw_reset_peripherals(void);

#endif /* EVSE_HW_H */
