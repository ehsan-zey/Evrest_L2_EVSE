# EVREST Level-2 EVSE — System Architecture

## 1. Scope

EVREST is an AC Level-2 (Mode 3) EV supply equipment built around an
**STM32H573VIT6** (Cortex-M33 @ 250 MHz, 2 MB flash, 640 KB SRAM, TrustZone +
HW crypto). The charger implements IEC 61851-1 / SAE J1772 control pilot
signalling and talks **OCPP 1.6J** over secure WebSocket to a Central System
(CSMS).

This repository contains four deliverables:

| Component   | Path         | Stack                                   |
|-------------|--------------|-----------------------------------------|
| Charger firmware | `firmware/` | C11, FreeRTOS, LwIP, mbedTLS, CMake |
| Central System (CSMS) | `csms/` | Node 20, TypeScript, ws, SQLite    |
| Operator dashboard    | `dashboard/` | React 18, Vite, TypeScript    |
| Driver mobile app     | `mobile/` | React Native (Expo), TypeScript  |

```
   ┌──────────────┐   OCPP 1.6J / WSS   ┌──────────────┐   REST + WS   ┌───────────┐
   │  EVSE (H573) │◄───────────────────►│     CSMS     │◄─────────────►│ Dashboard │
   └──────┬───────┘                     └──────┬───────┘               └───────────┘
          │ CP / relays / CT                   │  REST + WS
   ┌──────▼───────┐                     ┌──────▼───────┐
   │   Vehicle    │                     │  Mobile app  │
   └──────────────┘                     └──────────────┘
```

## 2. Hardware mapping (from `EVREST Block Diagram.pdf`)

The block diagram fixes the following functional blocks. Firmware pin
assignments live in `firmware/App/Inc/evse_board.h` and are the single place to
edit when the schematic settles.

### 2.1 Power path
* **Relay K1** in Neutral, **Relay K2** in Line — a two-pole contactor. Both are
  driven independently so the firmware can verify each pole.
* **Weld Detect** senses mains downstream of the contactor while it is commanded
  open. Any voltage there means a welded contact → permanent lockout.
* **Metering CT** + **Energy Metering** IC on SPI → V, I, P, PF, kWh.
* **AC/DC 12 V** → **3.3 V regulator** (logic) and **−12 V converter** (control
  pilot negative rail). **Metering Isolated DC Power** keeps the metering
  front-end galvanically separate.

### 2.2 Protection
* **RCD CT** + RCD IC with **RCD Int** (trip interrupt, active-low) and
  **RCD Test** (self-test injection). Covers 30 mA AC (IEC 61008) and 6 mA DC
  (IEC 62955 / UL 2231).
* **PEN CT** + **PEN Fault Detection** — open-PEN / lost-neutral detection for
  TN-C-S (PME) supplies, per BS 7671 722.411.4.1.
* **PE Detect** — protective-earth continuity, checked before every energisation.
* **Emergency Off** — hard-wired E-stop input, also cuts the relay drive in
  hardware; firmware observes it and latches a fault.

### 2.3 Control pilot
* **PWM** from a timer → **CP logic** → ±12 V pilot into the vehicle.
* **Pilot Read** → ADC, sampled synchronously with the PWM so both the high and
  the low plateau are captured. See §4.

### 2.4 HMI and connectivity
* Indicators: Power (green), Charge (yellow), WiFi (blue), Fault (red) + Buzzer.
* **LAN** — Ethernet PHY on RMII, LwIP stack on the MCU.
* **ESP32** — Wi-Fi co-processor on UART, used when Ethernet is absent.

## 3. Firmware architecture

FreeRTOS tasks, highest priority first:

| Task        | Period / trigger | Responsibility |
|-------------|------------------|----------------|
| `safety`    | 5 ms             | RCD, PEN, PE, weld, E-stop. Owns the trip path. |
| `pilot`     | 10 ms            | CP sampling, state A–F decode, debounce. |
| `evse`      | 20 ms + events   | Charging state machine, relay sequencing, current allocation. |
| `meter`     | 250 ms           | Energy metering IC readout, kWh accumulation. |
| `ocpp`      | event driven     | WebSocket + OCPP 1.6J client, offline queue. |
| `hmi`       | 50 ms            | LEDs, buzzer patterns. |

### 3.1 Safety is not in the state machine

The safety supervisor is deliberately separate from and higher priority than the
charging state machine. It can drive the contactor open on its own, through
`relay_emergency_open()`, which writes the GPIOs directly rather than requesting
a state transition. The state machine then observes the fault and follows. This
keeps the worst-case trip latency bounded by the safety task period (5 ms) plus
relay drop-out, independent of whatever the OCPP or network code is doing.

Trip latency budget, RCD interrupt to contactor commanded open:
EXTI (immediate GPIO write) → < 50 µs. The 5 ms poll is the backstop for
non-latching conditions.

### 3.2 Current allocation

The pilot duty cycle advertises the *minimum* of:
* hardware rating (`EVSE_MAX_CURRENT_A`, set per SKU),
* the installation limit stored in NVM (electrician-set),
* the active OCPP `TxProfile` / `TxDefaultProfile` / `ChargePointMaxProfile`
  composite schedule,
* any thermal derate.

Recomputed whenever any input changes; the PWM is updated within one pilot
period.

## 4. Control pilot detail (IEC 61851-1 Annex A)

The EVSE drives a 1 kHz ±12 V square wave. The vehicle loads it with a resistor
that pulls the positive plateau down; the plateau voltage encodes the state.

| State | CP high | Meaning                          | Contactor |
|-------|---------|----------------------------------|-----------|
| A     | +12 V   | No vehicle                       | open      |
| B     | +9 V    | Vehicle connected, not ready     | open      |
| C     | +6 V    | Charging, no ventilation needed  | **closed** |
| D     | +3 V    | Charging, ventilation required   | closed (only if the site permits) |
| E     | 0 V     | Error / CP shorted to PE         | open      |
| F     | −12 V   | EVSE unavailable                 | open      |

Detection windows are ±1 V around nominal and are defined in `cp_pilot.h`.

**Duty cycle → available current** (IEC 61851-1 Table A.7):

```
 10 % ≤ D ≤ 85 %  →  I = D × 0.6 A          (6 A … 51 A)
 85 % < D ≤ 96 %  →  I = (D − 64) × 2.5 A   (53 A … 80 A)
 D = 5 %          →  digital communication only (not used by this EVSE)
 D = 100 %        →  no PWM; DC or unlimited — not used
```

`cp_duty_for_current()` and `cp_current_for_duty()` implement both directions
and are unit-tested on the host (`firmware/test/`).

**Sampling.** The ADC is triggered twice per 1 kHz pilot period by two timer
compare channels, positioned at 75 % of the high plateau and 75 % of the low
plateau, so both samples land after the CP network has settled and well before
the next edge. Samples are DMA'd into a ping-pong buffer; the pilot task
median-filters 16 periods before deciding, which rejects the switching noise
that a single sample would pick up.

The CP divider maps ±12 V into the 0–3.3 V ADC window; the affine calibration
(`CP_ADC_SCALE_MV`, `CP_ADC_OFFSET_MV`) lives in `evse_board.h` and is the only
thing that changes if the CP front-end is re-spun.

## 5. Charging state machine

```
        ┌──────────┐  self-test ok   ┌───────────┐
        │  BOOT    │────────────────►│   IDLE    │◄──────────────┐
        └──────────┘                 └─────┬─────┘               │
                                  state B  │                     │
                                           ▼                     │
                                    ┌─────────────┐  timeout /   │
                                    │  CONNECTED  │  unplug      │
                                    └──────┬──────┘──────────────┤
                            authorised     │                     │
                                           ▼                     │
                                    ┌─────────────┐              │
                                    │ PREPARING   │ pilot PWM on │
                                    └──────┬──────┘              │
                              state C/D    │                     │
                                           ▼                     │
                                    ┌─────────────┐              │
                                    │  CHARGING   │◄──┐          │
                                    └──────┬──────┘   │ resume   │
                                 pause /   │          │          │
                                 profile 0 ▼          │          │
                                    ┌─────────────┐───┘          │
                                    │ SUSPENDED   │              │
                                    └──────┬──────┘              │
                                           │ stop / unplug       │
                                           ▼                     │
                                    ┌─────────────┐              │
                                    │  FINISHING  │──────────────┘
                                    └─────────────┘

   any state ──fault──► ┌─────────┐ ──recoverable+cleared──► IDLE
                        │ FAULTED │ ──latching──► LOCKOUT (needs power cycle)
                        └─────────┘
```

`FAULTED` vs `LOCKOUT` matters: a PE loss or an RCD trip during charging is
recoverable once the condition clears and the vehicle is unplugged, but a welded
contactor is not — it must not be silently re-armed, so it latches into
`LOCKOUT` and is written to NVM so it survives a reset.

## 6. OCPP 1.6J

Firmware implements the **Core** profile in full, plus **SmartCharging**,
**LocalAuthListManagement**, **Reservation**, **RemoteTrigger** and
**FirmwareManagement** (`GetDiagnostics`/`UpdateFirmware` hooks).

Offline behaviour: transactions are journalled to a wear-levelled flash ring
(`txn_log.c`). If the CSMS is unreachable the charger keeps charging on cached
authorisation and replays `StartTransaction` / `MeterValues` / `StopTransaction`
in order once the link returns, preserving the original timestamps.

Message routing: `ocpp_client.c` owns a small table of pending CALLs keyed by
`messageId` with a 30 s timeout, and a dispatch table for inbound CALLs. Adding
an action is one table row plus a handler.

## 7. Security

* WSS with server-certificate verification; the CA bundle is in NVM.
* Optional OCPP Security Profile 2 (HTTP Basic over TLS) or 3 (mutual TLS)
  using the H5's on-board crypto accelerator and the device key in TrustZone
  secure storage.
* The CSMS issues short-lived JWTs to dashboard and mobile clients; charger
  credentials are separate from user credentials.
* RFID tag ids are hashed at rest in the CSMS.

## 8. Repository layout

```
firmware/     STM32H573 EVSE firmware (CMake + arm-none-eabi-gcc)
  App/        Application logic — the interesting part
  Core/       MCU init, clock tree, ISRs
  test/       Host-side unit tests for pure logic (no HAL)
csms/         OCPP 1.6J Central System + REST/WS API
dashboard/    Operator web dashboard
mobile/       Driver mobile app
docs/         This document, wiring notes, OCPP conformance table
```

## 9. Compliance notes

This codebase is engineering work, not a certified product. Before deploying on
a real installation the following must be done by a qualified party:
IEC 61851-1 / UL 2594 type testing, IEC 62955 or UL 2231-2 RDC-DD verification,
EMC (IEC 61000-6-x), and functional-safety review of the trip path. The
`safety.c` trip path in particular is written to be reviewable, but a review has
not happened.
