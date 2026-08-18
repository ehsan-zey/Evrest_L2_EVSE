# EVREST L2 EVSE firmware

STM32H573VIT6. C11, FreeRTOS, CMake + `arm-none-eabi-gcc`.

## Layout

```
App/Inc, App/Src   application layer -- the part that matters
Core/              CubeMX-generated init (clock tree, peripherals, ISRs)
test/              host-side unit tests and a compile check; no hardware needed
```

`App/` never includes anything from `Core/` except the HAL headers, and CubeMX
never touches `App/`, so the `.ioc` can be regenerated at any time.

## Module map

| File | Responsibility |
|---|---|
| `evse_board.h`   | **Every pin assignment.** Matches `docs/Pinout.csv`. |
| `cp_pilot.c`     | IEC 61851-1 control pilot: PWM, synchronised sampling, state decode |
| `proximity.c`    | J1772 latch / Type 2 cable coding |
| `safety.c`       | RCD, PEN, PE, weld, E-stop, over-current, mains excursions |
| `relay.c`        | Contactor with mandatory pre-close weld check |
| `meter.c`        | ATM90E26 over UART4 |
| `evse_sm.c`      | Charging state machine |
| `evse_config.c`  | A/B redundant flash config, DIP current selection, OCPP keys |
| `hmi.c`          | LEDs and buzzer |
| `evse_hw.c`      | Peripheral setup CubeMX cannot express; EXTI dispatch |
| `evse_app.c`     | Tasks, power-on self-test, entry point |
| `json.c`         | Non-allocating JSON parser and builder |
| `ws_client.c`    | RFC 6455 WebSocket client |
| `net_link.c`     | Transport over Ethernet (LwIP+mbedTLS) or Wi-Fi (ESP32) |
| `esp_at.c`       | ESP-AT driver |
| `ocpp_client.c`  | OCPP 1.6J |
| `ocpp_profile.c` | SmartCharging composite schedule |
| `ocpp_auth.c`    | Local auth list and cache |
| `txn_log.c`      | Offline transaction journal |

## Integrating with the existing CubeMX project

The application is designed to drop into the project you already have. Three
edits to `main.c`, and one CubeMX change.

### 1. Select the PLL

`SystemClock_Config()` currently configures PLL1 and then selects HSI:

```c
RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV2;
```

That runs the part at 32 MHz. Change to:

```c
RCC_ClkInitStruct.SYSCLKSource  = RCC_SYSCLKSOURCE_PLLCLK;
RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
```

and raise the voltage scale to `PWR_REGULATOR_VOLTAGE_SCALE0` with
`FLASH_LATENCY_5`. The pilot frequency is computed from the live clock at
runtime (`evse_hw.c`), so it stays at 1 kHz across this change — but only
because it is computed. The hard-coded `PSC = 31 / ARR = 999` in
`MX_TIM1_Init()` is 1 kHz **only** at 32 MHz; `evse_hw_init()` overwrites both.

### 2. Replace the `main()` body

Everything between `MX_ICACHE_Init()` and the end of `main()` becomes:

```c
  /* USER CODE BEGIN 2 */
  evse_app_start();          /* creates the tasks and starts the scheduler */
  /* USER CODE END 2 */

  while (1) { }              /* never reached */
```

Delete `EVSE_CP_Start()`, `EVSE_Set_Charge_Current()`, `EVSE_Read_Vehicle_State()`
and the `EV_State_t` enum — `cp_pilot.c` and `evse_sm.c` replace all of them.
See `docs/PINOUT_REVIEW.md` for why the original ADC peak-hold could not work.

Add to the includes:

```c
#include "evse_app.h"
```

### 3. Fix the SPI3 NSS setting

`MX_SPI3_Init()` sets `SPI_NSS_HARD_INPUT` while in master mode, which raises a
mode fault if PA4 is ever pulled low. Change to `SPI_NSS_SOFT` in CubeMX. (SPI3
is unclaimed by this firmware, but leave it correct.)

### 4. CubeMX changes

* **FreeRTOS**: enable CMSIS-RTOS v2 or bare FreeRTOS, with
  `configSUPPORT_STATIC_ALLOCATION = 1` — every task and mutex here is
  statically allocated.
* **ADC1**: the application reconfigures it completely in `evse_hw_init()`, so
  the CubeMX settings only need to be valid, not correct. Leave the 2-rank scan
  if you like; it is overwritten.
* **UART4**: 9600 8N1 for the ATM90E26.
* **USART1**: 115200 8N1 for the ESP32.
* **EXTI**: PB10, PC13, PC2 and PE8 must have their NVIC lines enabled.
  `evse_hw_init()` sets the priorities.

## Building

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j
```

Produces `build/evrest-evse.elf` plus `.bin` and `.hex`.

## Tests

No hardware required.

```sh
cd test
make run      # unit tests: pilot, JSON, time, WebSocket, smart charging
make check    # compile-check every App/ source against HAL/FreeRTOS stubs
```

`make check` builds each application source against the stubs in `test/stubs/`.
It does not produce a firmware image; it catches type errors, missing
declarations and HAL misuse without needing the CubeMX tree, which makes it
usable in CI.

## Things this firmware deliberately does not do

* **No connector temperature monitoring.** No NTC is assigned in the pinout, so
  the thermal path is compiled out (`EVSE_TEMP_SENSORS_FITTED 0`) rather than
  faked. UL 2594 and IEC 62196-1 require it for a Level-2 unit. Free ADC pins
  are listed in `evse_board.h`.
* **No hardware over-current trip.** The ATM90E26 has no over-current interrupt
  source, so over-current is caught by the 5 ms safety poll. See
  `docs/METERING.md` for two hardware options if you want one.
* **No connector lock.** `UnlockConnector` answers `NotSupported`, because this
  is a J1772 unit whose release is the button on the plug handle.
* **Billing-grade metering calibration.** The `ATM_CAL_*` values are datasheet
  reference-design defaults. A production calibration against a reference source
  is required before the meter readings mean anything commercially.
