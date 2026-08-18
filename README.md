# EVREST — Level-2 EV charger

An AC Level-2 (Mode 3) EV supply equipment built on an **STM32H573VIT6**, with
the OCPP 1.6J Central System, operator dashboard and driver app that go with it.

| Component | Path | Stack |
|---|---|---|
| Charger firmware | [`firmware/`](firmware/) | C11, FreeRTOS, CMake |
| Central System (CSMS) | [`csms/`](csms/) | Node 20, TypeScript, SQLite |
| Operator dashboard | [`dashboard/`](dashboard/) | React 18, Vite |
| Driver app | [`mobile/`](mobile/) | React Native (Expo) |

```
   ┌──────────────┐   OCPP 1.6J / WSS   ┌──────────────┐   REST + WS   ┌───────────┐
   │  EVSE (H573) │◄───────────────────►│     CSMS     │◄─────────────►│ Dashboard │
   └──────┬───────┘                     └──────┬───────┘               └───────────┘
          │ CP / relay / CT                    │  REST
   ┌──────▼───────┐                     ┌──────▼───────┐
   │   Vehicle    │                     │  Driver app  │
   └──────────────┘                     └──────────────┘
```

## Try it in five minutes, no hardware

```sh
cd csms
npm install
npx tsx test/seed-demo.ts ./demo.db                 # 6 chargers, 30 days of sessions
DB_PATH=./demo.db JWT_SECRET=$(openssl rand -hex 32) npm run dev

# in another terminal — a charge point simulator speaking real OCPP 1.6J
npx tsx test/fake-charger.ts EVREST-0001 demo-key

# and another — the dashboard
cd ../dashboard && npm install && npm run dev        # http://localhost:5173
```

Sign in as `admin@evrest.local` / `demo-password-1234`. Remote-start the
simulated charger from the dashboard, drop its current limit to 16 A, and watch
it throttle.

## What is here

**Firmware** implements IEC 61851-1 / SAE J1772 control pilot signalling, a
protection supervisor that owns the trip path, an ATM90E26 metering driver, and
a full OCPP 1.6J client with an offline transaction journal. Five host-side test
suites and a compile check run without hardware.

**CSMS** terminates OCPP over WebSocket, validates every inbound payload, and
exposes a REST + live-WebSocket API. 29 integration tests drive a fake charge
point over a real socket.

**Dashboard** is the operator view: fleet status, live charts, remote control,
and the raw OCPP log.

**Mobile app** is the driver view: find a charger, start and stop, watch the
session, see history.

## Documentation

| Document | What it covers |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | System design, the control pilot in detail, the charging state machine, the safety split |
| [`docs/PINOUT_REVIEW.md`](docs/PINOUT_REVIEW.md) | Pin map reconciled against the CubeMX export, and a review of the starting `main.c` |
| [`docs/METERING.md`](docs/METERING.md) | What the ATM90E26's IRQ can and cannot do, and where over-current protection actually lives |
| [`firmware/README.md`](firmware/README.md) | Integrating with the existing CubeMX project |
| [`csms/README.md`](csms/README.md) | API reference, charger provisioning, design notes |

## Known gaps

These are deliberate and documented rather than overlooked. They matter before
this becomes a product:

- **No connector temperature sensing.** No NTC is assigned in the pinout, so the
  thermal derate and over-temperature trip are compiled out rather than faked.
  UL 2594 and IEC 62196-1 require it for a Level-2 unit. Free ADC pins are
  listed in `firmware/App/Inc/evse_board.h`.
- **No hardware over-current trip.** The ATM90E26 has no over-current interrupt
  source — its IRQ fires on voltage sag and reverse energy only — so
  over-current is caught by the 5 ms safety poll. Two hardware routes are
  described in `docs/METERING.md`.
- **Metering is not calibrated.** The `ATM_CAL_*` values are datasheet
  reference-design defaults. A production calibration against a reference source
  is needed before the readings mean anything commercially.
- **The single relay drive line** means the two contactor poles cannot be
  verified independently, which makes the weld-detect check the only contactor
  integrity test there is.
- **Nothing here is certified.** IEC 61851-1 / UL 2594 type testing, RDC-DD
  verification to IEC 62955 or UL 2231-2, EMC, and a functional-safety review of
  the trip path all remain to be done by a qualified party.

## Tests

```sh
cd firmware/test && make run && make check   # 5 unit suites + compile check
cd csms && npm test && npm run typecheck     # 29 integration tests
cd dashboard && npm run build                # typecheck + production build
cd mobile && npm run typecheck
```
