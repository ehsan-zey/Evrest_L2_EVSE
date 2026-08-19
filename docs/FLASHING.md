# Building and flashing with STM32CubeIDE + ST-Link/V2

This is the path from the repository to a running board. It assumes you already
have the CubeMX project that produced `docs/Pinout.csv`.

> The `CMakeLists.txt` in `firmware/` is an **alternative** build for CI and
> command-line use. If you are working in STM32CubeIDE, ignore it — CubeIDE uses
> its own managed build and the two do not need to agree.

---

## 1. Wire the ST-Link/V2

The STM32H573 is programmed over **SWD**. ST-Link/V2 has a 20-pin header; you
need five of them.

| ST-Link/V2 pin | Signal | Board |
|---|---|---|
| 1 or 2 | **VAPP / VTREF** | 3V3 — *reference only, does not power the board* |
| 7 | **SWDIO** | PA13 |
| 9 | **SWCLK** | PA14 |
| 15 | **NRST** | NRST |
| 4, 6, 8, 12, 14, 16, 18, 20 | **GND** | GND (one is enough) |

Three things that cause most first-time failures:

- **VAPP is not a supply.** It tells the ST-Link what logic level to use. The
  board must be powered from its own 12 V → 3V3 rail. If you power the board
  only from the ST-Link, the relay drive and the CP front end will not work even
  if the MCU enumerates.
- **Connect NRST.** It is optional for plain SWD, but "connect under reset" is
  the only reliable way in once the firmware starts driving pins, and you will
  want it the first time something hangs early in boot.
- **Keep SWD leads short.** Under about 15 cm. Long flying leads on SWCLK are a
  classic source of intermittent "cannot connect to target".

If the board is already powered, `PA13`/`PA14` are configured as
`DEBUG_JTMS-SWDIO` / `DEBUG_JTCK-SWCLK` in your pinout, so nothing in the
firmware takes them away.

---

## 2. Update the ST-Link firmware first

The STM32H5 family is newer than a lot of ST-Link/V2 firmware in circulation.
An out-of-date probe fails with "Unknown device" or connects and then cannot
erase.

In CubeIDE: **Help → ST-Link Upgrade**, or run `STM32CubeProgrammer` →
**Firmware upgrade**. Take the latest `V2J4x` build.

**On clones:** many inexpensive "ST-Link V2" dongles are clones with less flash
than the genuine part, and the upgrade either refuses or bricks them. If yours
will not take a current firmware, it will most likely not talk to an H5. A
genuine ST-Link/V2, an ST-Link/V3MINI, or the ST-Link built into any Nucleo
board (use the CN4 SWD header) all work.

---

## 3. Get the application into your CubeIDE project

The application lives in `firmware/App/` and deliberately never includes
anything from `Core/` except the HAL headers, so CubeMX can regenerate freely.

**Recommended — link the folder** so a `git pull` updates the project with no
copying:

1. Right-click the project → **New → Folder → Advanced → Link to alternate
   location**, point at `firmware/App`. Name it `App`.
2. **Project → Properties → C/C++ General → Paths and Symbols → Includes → GNU C**
   → **Add… → Workspace…** → select `App/Inc`. Tick *Add to all configurations*.
3. **Source Location** tab → confirm `App/Src` is listed. If not, **Add Folder…**.

**Or — copy it**, if you would rather the project be self-contained: drag
`App/` into the project root in the Project Explorer and choose *Copy files and
folders*, then do step 2.

Then in `Core/Src/main.c`:

```c
/* USER CODE BEGIN Includes */
#include "evse_app.h"
/* USER CODE END Includes */
```

```c
  /* USER CODE BEGIN 2 */
  evse_app_start();          /* creates the tasks and starts the scheduler */
  /* USER CODE END 2 */

  while (1) { }              /* never reached */
```

and delete `EVSE_CP_Start()`, `EVSE_Set_Charge_Current()`,
`EVSE_Read_Vehicle_State()` and the `EV_State_t` enum — `cp_pilot.c` and
`evse_sm.c` replace all of them. (`docs/PINOUT_REVIEW.md` §2 explains why the
originals could not work.)

---

## 4. CubeMX settings that must change

Open the `.ioc` and set these, then **Project → Generate Code**.

### FreeRTOS — required

**Middleware → FREERTOS → Interface: CMSIS_V2**, then on the **Config parameters**
tab:

| Setting | Value | Why |
|---|---|---|
| `USE_PREEMPTION` | Enabled | |
| `MAX_PRIORITIES` | ≥ 7 | the app uses `configMAX_PRIORITIES - 1 … - 6` |
| `MINIMAL_STACK_SIZE` | ≥ 128 | |
| `Memory Management scheme` | `heap_4` | only the idle/timer tasks use it |
| `USE_MUTEXES` | Enabled | |
| `USE_TIMERS` | Enabled | |
| `SUPPORT_STATIC_ALLOCATION` | **Enabled** | every task and mutex here is static |
| `CHECK_FOR_STACK_OVERFLOW` | Option 2 | `evse_app.c` implements the hook |
| `USE_MALLOC_FAILED_HOOK` | Enabled | ditto |

If CubeMX generates its own `vApplicationGetIdleTaskMemory`, add
`EVSE_CUBEMX_PROVIDES_STATIC_MEMORY` to
**Properties → C/C++ Build → Settings → MCU GCC Compiler → Preprocessor** so
`freertos_static.c` compiles out. Otherwise leave it — the file supplies them.

### Timebase source — the classic FreeRTOS trap

**System Core → SYS → Timebase Source: TIM6** (anything except SysTick).

FreeRTOS takes SysTick for its own tick. Leaving the HAL timebase on SysTick
means `HAL_Delay()` and the HAL timeouts stop working the moment the scheduler
starts, which shows up as the metering driver timing out on every transfer.

### Clock — select the PLL

Your current `SystemClock_Config()` builds PLL1 and then selects HSI, so the
part runs at **32 MHz**. Set:

- **Voltage scaling**: `PWR_REGULATOR_VOLTAGE_SCALE0`
- **System Clock Mux**: `PLLCLK`
- **AHB Prescaler**: `/1`
- **Flash latency**: `5 WS`

The pilot frequency survives this because `evse_hw.c` computes the TIM1
prescaler from `HAL_RCC_GetPCLK2Freq()` at runtime. The hard-coded
`PSC=31 / ARR=999` in `MX_TIM1_Init()` would have become 7.8 kHz — see
`docs/PINOUT_REVIEW.md` §2.7.

### Peripherals

| Peripheral | Setting |
|---|---|
| `UART4` | 9600 8N1 — the ATM90E26 |
| `USART1` | 115200 8N1 — the ESP32 |
| `USART3` | 115200 8N1 — the console `printf` goes to |
| `SPI3` | change NSS from `Hard Input` to **`Software`** (master mode with hard-input NSS raises a mode fault if PA4 is pulled low) |
| `ADC1` | leave as-is; `evse_hw_init()` reconfigures it completely |
| NVIC | enable EXTI lines for **PB10, PC13, PC2, PE8** — `evse_hw_init()` sets the priorities |

### Linker — heap and stack

**Project Manager → Project → Linker Settings**: minimum heap `0x400`, minimum
stack `0x1000`. The application allocates nothing at runtime, but newlib and
the HAL want a little.

---

## 5. Build and flash

**Project → Build All** (Ctrl-B). Then:

1. **Run → Debug Configurations… → STM32 C/C++ Application** → double-click to
   create one for the project.
2. **Debugger** tab:
   - Debug probe: **ST-LINK (ST-LINK GDB server)**
   - Interface: **SWD**
   - Frequency: start at **1000 kHz**; drop to 480 kHz if the leads are long
   - Reset behaviour: **Connect under reset**
   - Leave *Shared ST-LINK* off unless CubeProgrammer is also open
3. **Debug** (F11). CubeIDE erases, programs and halts at `main()`.

For a plain flash with no debugger, **Run → Run** (Ctrl-F11) uses the same
configuration without halting.

### The `.elf` is what gets flashed

CubeIDE programs `Debug/<project>.elf` directly. You do not need a `.bin` or
`.hex` unless you are flashing with STM32CubeProgrammer standalone, in which
case use `Debug/<project>.elf` there too — it carries the addresses.

---

## 6. What you should see on a first successful flash

**Before connecting a vehicle**, with the board powered and the contactor's load
side isolated:

1. **Power LED (PC7) blinks fast**, then goes solid — boot, then self-test done.
2. **The console prints** on USART3 (PD8, 115200 8N1) via any USB-serial adapter:

```
EVREST EVREST-L2 fw 1.0.0  boot #1  installed limit 16 A (DIP 0)
selftest: pe=1 pen=1 relay=1 rcd=1 meter=1 pilot=1 -> PASS
```

3. **The fault LED (PC8) stays off.** Solid red means the self-test failed and
   the contactor is latched out — the console line tells you which check.
4. **A scope on PA8 (CP_PWM)** shows a 1 kHz square wave. With nothing plugged
   in it sits at a static high (state A), not modulating — that is correct.

If the self-test fails, the field that reads `0` names the cause:

| Field | Means |
|---|---|
| `pe=0` | PE_DET (PB8) not asserted — protective earth not detected |
| `pen=0` | PEN_DET (PC2) asserted — only checked on a TN-C-S installation |
| `relay=0` | WELD_DET (PC3) reads live with the contactor open — welded contact, or the sense circuit is inverted |
| `rcd=0` | RCD did not trip when RCD_TEST (PB9) was pulsed |
| `meter=0` | ATM90E26 did not answer on UART4 — check baud, TX/RX swap |
| `pilot=0` | TIM1 or ADC1 rejected its configuration |

**A self-test failure is latching.** That is deliberate — a power cycle must not
be a way to clear a missing protective element. It will simply fail again until
the underlying condition is fixed.

---

## 7. Troubleshooting

| Symptom | Cause |
|---|---|
| "No ST-LINK detected" | USB cable is charge-only; or driver not installed (Windows: ST-Link USB driver ships with CubeIDE) |
| "Unknown device" / "Cannot connect" | ST-Link firmware too old for H5 — upgrade it (§2). If it will not upgrade, it is a clone; use a Nucleo's on-board probe |
| Connects, then "Error in initializing ST-LINK device" | No target power. VAPP is a reference, not a supply — power the board separately |
| Works once, then fails | Reset behaviour not set to **Connect under reset** |
| "Target is not responding, retrying..." | SWCLK too fast or leads too long — drop to 480 kHz |
| `undefined reference to vApplicationGetIdleTaskMemory` | `SUPPORT_STATIC_ALLOCATION` enabled but `freertos_static.c` excluded from the build |
| Duplicate definition of the same symbol | CubeMX also generated it — set `EVSE_CUBEMX_PROVIDES_STATIC_MEMORY` (§4) |
| Builds, runs, no console output | `USART3` not enabled in CubeMX, or the adapter is on the wrong pin — TX is **PD8** |
| Console prints, then everything stops | Timebase source still SysTick (§4) |
| Pilot measures 7.8 kHz | PLL selected but `MX_TIM1_Init()` regenerated over `evse_hw_init()` — confirm `evse_app_start()` runs *after* all `MX_*_Init()` calls |
| Debugger cannot halt, "secure" errors | TrustZone enabled — see below |

### TrustZone and product state

The STM32H573 is a TrustZone part. This firmware is a **single non-secure
image** and assumes `TZEN=0`, which is the factory default.

If CubeMX asked "Do you want to enable TrustZone?" and you said yes, it sets the
`TZEN` option byte and generates a two-project secure/non-secure structure that
this code does not fit. To go back: STM32CubeProgrammer → **OB** tab → set
`TZEN` to `0xC3` (disabled) → apply, then power-cycle.

Similarly, if the part's **product state** is anything other than `OPEN`, debug
access is restricted. Fresh parts ship `OPEN`. Check under CubeProgrammer's
**OB → Product state**; going back from `CLOSED` requires a regression that
mass-erases the flash.

---

## 8. Bench-testing without a vehicle

You can exercise the whole charging sequence with three resistors, which is how
the state decode was designed to be verified:

| Between CP and PE | Emulates | Expected |
|---|---|---|
| 2740 Ω | State B — plugged in, not ready | pilot starts modulating, contactor stays open |
| 2740 Ω ∥ 1300 Ω (≈ 882 Ω) | State C — ready to charge | contactor closes |
| 2740 Ω ∥ 270 Ω (≈ 246 Ω) | State D — ventilation required | contactor stays open unless commissioned as ventilated |

Put a **1N4148 in series** with the resistor, cathode toward PE. Without the
diode the firmware raises `PilotDiodeFault` and refuses — which is the check
that distinguishes a real vehicle from a resistor, and worth confirming works by
shorting the diode out deliberately.

To test the CSMS link at the same time, point `csms_url` at a machine running
the Central System (`csms/README.md`), or run the simulator the other way round
and drive the dashboard from `csms/test/fake-charger.ts`.
