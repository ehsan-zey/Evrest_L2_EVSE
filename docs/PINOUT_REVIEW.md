# Pinout reconciliation and review of the starting `main.c`

Source: `Pinout.csv` (CubeMX export) + the starting `main.c`.
This supersedes the pin assignments guessed from the block diagram.

## 1. Differences from the block diagram

| Block diagram implies | Actual hardware | Consequence |
|---|---|---|
| Relay K1 **and** K2 driven separately | one output, `RELAY_1` = **PD1** | Both poles share a coil driver. The firmware cannot verify poles independently, so weld detection on `WELD_DET` is the *only* contactor integrity check — it carries more weight, not less. |
| Energy metering on SPI | `ENME_TX`/`ENME_RX` = **UART4** (PD12/PD11) + `ENME_IRQ` = **PE8** | Metering driver must be serial, not SPI. SPI3 (PA4/PC10/PC11/PC12) is wired but unlabelled. |
| — | `PP_READ` = **PA0** (ADC1_INP0) | Proximity pilot is present and was not in the block diagram. Cable current rating must be part of the current-limit calculation. |
| — | `DIP_1..3` = **PE2/PE3/PE4** | Installer DIP switches, almost certainly the breaker/current selection. |
| ESP32 on USART2 | Wi-Fi on **USART1** (PB14 TX / PA10 RX), `WIFI_RST` = PA9, `LED_WIFI` = PA3 | |
| Buzzer on TIM3_CH4/PB1 | **TIM3_CH3 / PB0** | |
| E-stop on PB0 | `EMG_OFF` = **PC13** (EXTI13) | |
| Connector/enclosure NTCs | **not assigned** | There is no temperature sensor on the pinout. Thermal derate is compiled out by default; free pins are listed in `evse_board.h` if you add one. |

Unassigned and free for expansion: PA6, PA15, PB1, PB2, PB4–PB7, PB13, PC9,
PD0, PD2, PD4–PD7, PD10, PD13–PD15, PE0, PE5–PE7, PE9–PE15.

## 2. Review of the starting `main.c`

The skeleton is a reasonable bring-up sketch. The items below are the gap
between it and something that can be left connected to a car.

### 2.1 The ADC peak-hold reads two different signals

`MX_ADC1_Init()` configures a **2-conversion scan**: rank 1 = `ADC_CHANNEL_10`
(CP_READ) and rank 2 = `ADC_CHANNEL_0` (PP_READ). `EVSE_Read_Vehicle_State()`
then loops on `HAL_ADC_Start` / `PollForConversion` / `GetValue` and keeps the
running maximum.

With scan mode enabled, successive `GetValue()` calls walk the sequence, so the
peak-hold is taking the maximum **across both channels**. Whichever of CP and PP
happens to be higher wins, and the decoded "vehicle state" is then whatever that
mixed maximum lands in. This is not a marginal effect — the proximity pilot sits
at a static few volts and can easily dominate the CP low plateau.

*Fix:* separate the two. In this firmware CP is sampled by a hardware-triggered
conversion synchronised to the PWM, and PP is read on its own.

### 2.2 Peak-hold cannot see the negative plateau, so the diode is never checked

`HAL_ADC_GetValue()` returns an unsigned code and the code keeps only the
maximum, so the negative half of the pilot is discarded entirely.

IEC 61851-1 requires the EVSE to verify that the vehicle's series **diode** is
present: the negative plateau must stay near −12 V. Without that check, a plain
resistor across CP/PE — a paperclip, a damaged cable, a deliberately faked
"charging" load — decodes as a valid state C and the contactor closes. The diode
check is the thing that distinguishes a real vehicle from a resistor.

*Fix:* sample both plateaus and require `v_low ≤ −10 V` before closing.

### 2.3 Sampling is not synchronised to the PWM

Polling for 3 ms across a 1 kHz waveform catches roughly three periods at
whatever phase the loop happens to hit, and the ADC's 247.5-cycle sampling time
at 32 MHz is long enough to straddle an edge. Readings taken during the slew are
neither plateau.

*Fix:* trigger the conversions from timer compare events placed inside each
plateau (this firmware uses TIM1_CH2 and TIM1_CH4).

### 2.4 No debounce anywhere

A single noisy sample moves the state machine, and the state machine closes a
contactor. One ESD event or one switching transient on a nearby load is enough.

*Fix:* median filter over N pilot periods, then require N consecutive identical
decodes before acting.

### 2.5 Every safety input is configured but never read

This is the largest gap. CubeMX sets these up and nothing consumes them:

| Signal | Pin | Configured as | Read anywhere? |
|---|---|---|---|
| `RCD_INT`  | PB10 | EXTI falling | **no** |
| `EMG_OFF`  | PC13 | EXTI falling | **no** |
| `PEN_DET`  | PC2  | EXTI rising  | **no** |
| `WELD_DET` | PC3  | input        | **no** |
| `PE_DET`   | PB8  | input        | **no** |
| `RCD_TEST` | PB9  | output       | never pulsed |
| `ENME_IRQ` | PE8  | EXTI falling | **no** |

There are no `HAL_GPIO_EXTI_Callback` implementations, so the EXTI lines fire
into the default weak handler and are discarded. Concretely: **the RCD can trip
and the contactor stays closed.** The RCD hardware interrupts the MCU, the MCU
ignores it, and nothing opens the relay. Same for emergency-off and open-PEN.

*Fix:* `safety.c` — EXTI handlers that open the contactor with a direct GPIO
write before any bookkeeping, plus a 5 ms supervisor task for the level inputs
and a `RCD_TEST` self-test at boot.

### 2.6 No weld check before energising

`WELD_DET` senses mains downstream of the contactor. It must be proven clear
*before* the relay is commanded closed and re-checked after it opens. With a
single relay drive line this is the only contactor integrity check available.

### 2.7 The PLL is configured but not selected

`SystemClock_Config()` sets up PLL1 (CSI × … → 250 MHz) and then selects
`RCC_SYSCLKSOURCE_HSI` with `AHBCLKDivider = RCC_SYSCLK_DIV2`. The part runs at
**HCLK = 32 MHz** from HSI, not from the PLL.

The 1 kHz pilot happens to come out correct because TIM1 is fed 32 MHz and
`PSC = 31`, `ARR = 999` gives exactly 1000 Hz — so this is invisible until you
switch to the PLL, at which point the pilot becomes 7.8 kHz and every vehicle
rejects it. Ethernet RMII also wants comfortably more headroom than 32 MHz.

*Fix:* select the PLL (`RCC_SYSCLKSOURCE_PLLCLK`), and derive the timer
prescaler from `HAL_RCC_GetPCLK2Freq()` at runtime instead of hard-coding it, so
the pilot frequency survives a clock change.

### 2.8 Smaller items

* `EVSE_Set_Charge_Current()` implements only the `A = duty × 0.6` branch, so it
  saturates at 51 A and cannot express 51–80 A.
* Duty is not clamped to the 8–96 % legal band.
* `SPI3` is `SPI_NSS_HARD_INPUT` while in master mode — a low on PA4 will raise a
  mode fault. Use software NSS and drive the chip select as a GPIO.
* `MX_TIM1_Init()` never calls `__HAL_TIM_MOE_ENABLE` outside `EVSE_CP_Start()`;
  fine as written, but note that any advanced-timer break event silently kills
  the pilot until MOE is re-enabled.
* The `while(1)` body runs at `HAL_Delay(50)`, so worst-case reaction to a
  vehicle state change is 50 ms plus the 3 ms sample. IEC 61851-1 allows 100 ms
  to open on a state change, so this is inside budget but has no margin for the
  network stack once that is added — hence the separate 5 ms safety task.
