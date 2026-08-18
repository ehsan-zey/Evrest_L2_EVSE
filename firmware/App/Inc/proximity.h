/**
 * @file  proximity.h
 * @brief Proximity pilot (PP) decoding — cable current rating and latch state.
 *
 * PP is read on ADC1_INP0 (PA0, "PP_READ"). What it means depends on the
 * connector standard, and this board is built for both:
 *
 * **IEC 62196-2 Type 2** — the cable assembly carries a coding resistor between
 * PP and PE that states what the *cable* is rated for. A 32 A charger with a
 * 20 A cable plugged in must offer 20 A, so this value is a hard input to the
 * current limit, not advisory.
 *
 *     1500 R -> 13 A      680 R -> 20 A      220 R -> 32 A      100 R -> 63 A
 *
 * **SAE J1772 Type 1** — PP is a switch/resistor network in the latch button.
 * It carries no rating, only whether the release button is pressed:
 *
 *     ~2700 R  latch closed, cable secure
 *     ~480 R   latch released — the driver is pulling the plug
 *
 * On Type 1 the release must stop the current *before* the contacts part, which
 * is why the latch is polled by the pilot task rather than the OCPP task.
 *
 * Front end assumed: PP pulled to 3V3 through PP_PULLUP_OHMS with the cable
 * resistor as the lower leg. Adjust PP_PULLUP_OHMS in evse_board.h to match.
 */
#ifndef PROXIMITY_H
#define PROXIMITY_H

#include "evse_types.h"

typedef enum {
    CONNECTOR_TYPE1_J1772 = 0,  /**< PP = latch button                  */
    CONNECTOR_TYPE2_IEC62196,   /**< PP = cable coding resistor         */
    CONNECTOR_TETHERED          /**< captive cable, PP unused           */
} connector_type_t;

typedef struct {
    bool     cable_present;   /**< a cable is plugged into the socket     */
    bool     latch_engaged;   /**< Type 1 only: release button not pressed */
    float    cable_rating_a;  /**< Type 2: cable limit. 0 if unknown.      */
    uint32_t resistance_ohms; /**< measured PP resistance, for diagnostics */
} pp_status_t;

/* ---- Pure logic, unit-tested on the host ---- */

/** Convert a PP ADC code to the coding resistance in ohms. UINT32_MAX if open. */
uint32_t pp_adc_to_ohms(uint16_t adc_raw);

/**
 * Decode a PP resistance for the given connector standard.
 * @param ohms   measured resistance
 * @param type   connector standard in use
 * @param out    decoded status
 */
void pp_decode(uint32_t ohms, connector_type_t type, pp_status_t *out);

/* ---- Hardware-backed ---- */

void proximity_init(connector_type_t type);

/** Re-read PP from the pilot task's latest injected sample. */
void proximity_update(void);

/** Latest decoded proximity status. */
void proximity_get(pp_status_t *out);

/**
 * Cable-imposed current ceiling in amps, for the limit calculation.
 * Returns EVSE_MAX_CURRENT_A when PP carries no rating (Type 1 or tethered), so
 * callers can always fold this into a min() without special-casing.
 */
float proximity_current_limit(void);

#endif /* PROXIMITY_H */
