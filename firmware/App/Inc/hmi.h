/**
 * @file  hmi.h
 * @brief Indicators and buzzer.
 *
 * Four LEDs (Power/green, Charge/yellow, WiFi/blue, Fault/red) plus a buzzer.
 * The HMI derives everything from the aggregate status — it holds no state of
 * its own beyond blink phase, so it can never disagree with the state machine.
 */
#ifndef HMI_H
#define HMI_H

#include "evse_types.h"

typedef enum {
    HMI_TONE_NONE = 0,
    HMI_TONE_PLUG_IN,       /**< short rising chirp when a cable is inserted  */
    HMI_TONE_AUTHORISED,    /**< two-note confirmation                        */
    HMI_TONE_CHARGE_START,
    HMI_TONE_CHARGE_END,
    HMI_TONE_REJECTED,      /**< low buzz: authorisation refused              */
    HMI_TONE_FAULT          /**< repeating alarm while a trip fault is active */
} hmi_tone_t;

void hmi_init(void);

/** Refresh LEDs from @p st. Call at TASK_PERIOD_HMI_MS. */
void hmi_update(const evse_status_t *st);

/** Queue a tone. A fault tone pre-empts anything already playing. */
void hmi_play(hmi_tone_t tone);

/** Direct override, used by the bootloader/service mode. */
void hmi_set_leds(bool power, bool charge, bool wifi, bool fault);

#endif /* HMI_H */
