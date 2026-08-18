/**
 * @file  evse_types.c
 * @brief Name lookups for logging, the console and OCPP diagnostics.
 */
#include "evse_types.h"

const char *evse_state_name(evse_state_t s)
{
    switch (s) {
        case EVSE_STATE_BOOT:           return "Boot";
        case EVSE_STATE_IDLE:           return "Idle";
        case EVSE_STATE_CONNECTED:      return "Connected";
        case EVSE_STATE_PREPARING:      return "Preparing";
        case EVSE_STATE_CHARGING:       return "Charging";
        case EVSE_STATE_SUSPENDED_EV:   return "SuspendedEV";
        case EVSE_STATE_SUSPENDED_EVSE: return "SuspendedEVSE";
        case EVSE_STATE_FINISHING:      return "Finishing";
        case EVSE_STATE_RESERVED:       return "Reserved";
        case EVSE_STATE_UNAVAILABLE:    return "Unavailable";
        case EVSE_STATE_FAULTED:        return "Faulted";
        case EVSE_STATE_LOCKOUT:        return "Lockout";
    }
    return "?";
}

const char *cp_state_name(cp_state_t s)
{
    switch (s) {
        case CP_STATE_A:       return "A";
        case CP_STATE_B:       return "B";
        case CP_STATE_C:       return "C";
        case CP_STATE_D:       return "D";
        case CP_STATE_E:       return "E";
        case CP_STATE_F:       return "F";
        case CP_STATE_INVALID: return "invalid";
    }
    return "?";
}

/**
 * Name of the most significant fault present.
 *
 * Ordered by how much it matters to whoever is standing at the charger: the
 * protective failures first, then the electrical ones, then the pilot. When
 * several are set at once — an RCD trip usually drags others with it — this is
 * the one worth showing.
 */
const char *evse_fault_name(uint32_t faults)
{
    if (faults & EVSE_FAULT_ESTOP)          return "EmergencyStop";
    if (faults & EVSE_FAULT_RELAY_WELD)     return "ContactorWelded";
    if (faults & EVSE_FAULT_RCD_TRIP)       return "ResidualCurrent";
    if (faults & EVSE_FAULT_RCD_SELFTEST)   return "RcdSelfTestFailed";
    if (faults & EVSE_FAULT_PEN)            return "OpenPenFault";
    if (faults & EVSE_FAULT_PE_LOST)        return "EarthLost";
    if (faults & EVSE_FAULT_OVER_TEMP)      return "OverTemperature";
    if (faults & EVSE_FAULT_OVER_CURRENT)   return "OverCurrent";
    if (faults & EVSE_FAULT_OVER_VOLTAGE)   return "OverVoltage";
    if (faults & EVSE_FAULT_UNDER_VOLTAGE)  return "UnderVoltage";
    if (faults & EVSE_FAULT_RELAY_NO_CLOSE) return "ContactorFailedToClose";
    if (faults & EVSE_FAULT_CP_SHORT)       return "PilotShortCircuit";
    if (faults & EVSE_FAULT_CP_DIODE)       return "PilotDiodeFault";
    if (faults & EVSE_FAULT_CP_INVALID)     return "PilotInvalid";
    if (faults & EVSE_FAULT_VENT_REQUIRED)  return "VentilationRequired";
    if (faults & EVSE_FAULT_METER_COMM)     return "MeterCommunicationFault";
    if (faults & EVSE_FAULT_SELFTEST)       return "SelfTestFailed";
    return "None";
}
