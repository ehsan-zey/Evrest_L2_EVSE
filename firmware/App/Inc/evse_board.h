/**
 * @file    evse_board.h
 * @brief   Board definition for the EVREST Level-2 EVSE (STM32H573VIT6, LQFP100).
 *
 * Pin assignments here match the CubeMX export in docs/Pinout.csv exactly.
 * This is the single place that ties firmware to the schematic — a board
 * re-spin should touch this file and nothing else.
 *
 * Labels in comments are the CubeMX user labels, so this file can be diffed
 * against a fresh pinout export.
 */
#ifndef EVSE_BOARD_H
#define EVSE_BOARD_H

#include "stm32h5xx_hal.h"

/* ======================================================================== */
/* Product identity / ratings                                               */
/* ======================================================================== */

#define EVSE_VENDOR_NAME            "EVREST"
#define EVSE_MODEL_NAME             "EVREST-L2"
#define EVSE_FW_VERSION             "1.0.0"

/**
 * Absolute hardware ceiling for this SKU, amps. Never advertise more.
 * The installed limit comes from the DIP switches (see below) and is further
 * capped by this value, so a mis-set DIP can never exceed the hardware.
 */
#define EVSE_MAX_CURRENT_A          48.0f
/** IEC 61851 minimum advertisable current. Below this, no PWM offer is legal. */
#define EVSE_MIN_CURRENT_A          6.0f
#define EVSE_NOMINAL_VOLTAGE_V      240.0f
#define EVSE_PHASE_COUNT            1

/** OCPP connector id. One outlet; id 0 means "the charge point itself". */
#define EVSE_CONNECTOR_ID           1

/* ======================================================================== */
/* Clocks                                                                   */
/* ======================================================================== */

/**
 * Timer clocks are read at runtime with HAL_RCC_GetPCLK1Freq()/GetPCLK2Freq()
 * rather than hard-coded, so the control pilot stays at 1 kHz if the clock
 * tree changes. The starting firmware hard-coded PSC=31/ARR=999, which is only
 * 1 kHz while the part runs from HSI at 32 MHz; selecting the PLL without
 * recomputing would silently move the pilot to 7.8 kHz.
 */

/* ======================================================================== */
/* Control pilot — TIM1_CH1 out (PA8), ADC1_INP10 in (PC0)                  */
/* ======================================================================== */

#define CP_PWM_TIM_HANDLE           htim1
#define CP_PWM_FREQ_HZ              1000U          /* IEC 61851-1: 1 kHz ±0.5 % */
#define CP_PWM_CHANNEL              TIM_CHANNEL_1  /* PA8  "CP_PWM"            */
#define CP_SAMPLE_HI_CHANNEL        TIM_CHANNEL_2  /* compare-only, ADC trigger */
#define CP_SAMPLE_LO_CHANNEL        TIM_CHANNEL_4  /* compare-only, ADC trigger */

/**
 * Timer resolution. ARR+1 counts per 1 kHz period; the prescaler is computed at
 * init from the real APB2 clock. 2000 steps gives 0.05 % duty resolution, far
 * finer than the ±0.5 % the standard allows.
 */
#define CP_PWM_RESOLUTION           2000U

#define CP_PWM_GPIO_PORT            GPIOA
#define CP_PWM_GPIO_PIN             GPIO_PIN_8

/** "CP_READ" — pilot feedback. */
#define CP_ADC_CHANNEL              ADC_CHANNEL_10
#define CP_ADC_GPIO_PORT            GPIOC
#define CP_ADC_GPIO_PIN             GPIO_PIN_0

/**
 * CP front-end calibration.
 *
 * The CP logic block divides and level-shifts the ±12 V pilot into the ADC
 * window with an affine transfer:
 *
 *     v_cp_mv = adc_mv * CP_ADC_SCALE_NUM / CP_ADC_SCALE_DEN - CP_ADC_OFFSET_MV
 *
 * The defaults below reproduce the thresholds the starting firmware was
 * calibrated to on real hardware (state A ≈ 3100 counts, B ≈ 2100–3100,
 * C ≈ 1200–2099, D ≈ 400–1199), i.e. roughly 12 V -> 2.55 V and 0 V -> 0.30 V.
 * Re-measure per board and write the result to NVM; cfg_get()->cp_cal_* wins
 * over these at runtime.
 */
#define CP_ADC_SCALE_NUM            1200
#define CP_ADC_SCALE_DEN            225
#define CP_ADC_OFFSET_MV            1600

/* ======================================================================== */
/* Proximity pilot — ADC1_INP0 (PA0, "PP_READ")                             */
/* ======================================================================== */

/**
 * This is a J1772 (Type 1) unit, where PP is the latch-button network and
 * carries no cable rating — the current limit comes from the SKU and the DIP
 * switches instead. PP_READ is populated on the board but is NOT used to
 * constrain current on this build.
 *
 * It is still sampled and published for diagnostics (and because the latch
 * signal is genuinely useful — see proximity.h), but proximity_current_limit()
 * returns no constraint while the connector type is Type 1.
 *
 * Front end assumed: PP pulled to 3V3 through PP_PULLUP_OHMS, latch resistor
 * to PE as the lower leg.
 */
#define PP_ADC_CHANNEL              ADC_CHANNEL_0
#define PP_ADC_GPIO_PORT            GPIOA
#define PP_ADC_GPIO_PIN             GPIO_PIN_0

#define PP_PULLUP_OHMS              330.0f
#define PP_SUPPLY_MV                3300.0f

/* ======================================================================== */
/* Contactor — single drive line, both poles ("RELAY_1", PD1)               */
/* ======================================================================== */

/**
 * The block diagram shows two poles (K1 in neutral, K2 in line) but the pinout
 * exposes one control line, so both coils are driven together. The firmware
 * therefore cannot verify poles independently — WELD_DET is the only contactor
 * integrity check there is, which makes the pre-close weld test mandatory
 * rather than merely advisable.
 */
#define RELAY_GPIO_PORT             GPIOD
#define RELAY_GPIO_PIN              GPIO_PIN_1
#define RELAY_ACTIVE_STATE          GPIO_PIN_SET

#define RELAY_CLOSE_TIME_MS         25U
#define RELAY_OPEN_TIME_MS          20U

/** "WELD_DET" (PC3): asserted while mains is present downstream of the relay. */
#define WELD_DETECT_GPIO_PORT       GPIOC
#define WELD_DETECT_GPIO_PIN        GPIO_PIN_3
#define WELD_DETECT_ACTIVE_STATE    GPIO_PIN_SET

/* ======================================================================== */
/* Protection inputs                                                        */
/* ======================================================================== */

/** "PE_DET" (PB8): protective-earth continuity present. */
#define PE_DETECT_GPIO_PORT         GPIOB
#define PE_DETECT_GPIO_PIN          GPIO_PIN_8
#define PE_DETECT_OK_STATE          GPIO_PIN_SET

/** "PEN_DET" (PC2, EXTI2 rising): open-PEN / lost-neutral fault. */
#define PEN_FAULT_GPIO_PORT         GPIOC
#define PEN_FAULT_GPIO_PIN          GPIO_PIN_2
#define PEN_FAULT_ACTIVE_STATE      GPIO_PIN_SET

/** "RCD_INT" (PB10, EXTI10 falling): residual-current trip, active low. */
#define RCD_INT_GPIO_PORT           GPIOB
#define RCD_INT_GPIO_PIN            GPIO_PIN_10
#define RCD_INT_ACTIVE_STATE        GPIO_PIN_RESET

/** "RCD_TEST" (PB9): injects a test current into the RCD CT. Pulse only. */
#define RCD_TEST_GPIO_PORT          GPIOB
#define RCD_TEST_GPIO_PIN           GPIO_PIN_9
#define RCD_TEST_ACTIVE_STATE       GPIO_PIN_SET
#define RCD_TEST_TIMEOUT_MS         100U

/** "EMG_OFF" (PC13, EXTI13 falling): emergency-off, NC contact to ground. */
#define ESTOP_GPIO_PORT             GPIOC
#define ESTOP_GPIO_PIN              GPIO_PIN_13
#define ESTOP_ACTIVE_STATE          GPIO_PIN_RESET

/* ======================================================================== */
/* Installer DIP switches — "DIP_1..3" (PE2/PE3/PE4), pulled up             */
/* ======================================================================== */

/**
 * Three switches read as a 3-bit code select the installation current limit —
 * the breaker the unit is wired to. This is the authoritative installed limit;
 * OCPP smart charging can only reduce it, never raise it.
 *
 *   code   current
 *   000     16 A
 *   001     24 A
 *   010     32 A
 *   011     40 A
 *   100     48 A
 *   101 |
 *   110 |-  reserved -> 16 A (fail-safe)
 *   111 |
 *
 * Bit order: DIP_1 is bit 0 (LSB), DIP_3 is bit 2 (MSB).
 *
 * Polarity: the pins are configured with pull-ups, so an OPEN switch reads 1
 * and a CLOSED switch reads 0. The code is taken directly from the pin levels,
 * which means all-closed = 000 = 16 A and all-open = 111 = reserved = 16 A.
 * Both extremes land on the lowest current, so an unset or mis-set switch bank
 * fails safe in either direction. If the silkscreen numbers the other way,
 * invert DIP_CODE_IS_INVERTED rather than rewiring the table.
 */
#define DIP_GPIO_PORT               GPIOE
#define DIP1_GPIO_PIN               GPIO_PIN_2   /* bit 0 */
#define DIP2_GPIO_PIN               GPIO_PIN_3   /* bit 1 */
#define DIP3_GPIO_PIN               GPIO_PIN_4   /* bit 2 */
#define DIP_CODE_IS_INVERTED        0

/** Fail-safe current used for every reserved DIP code. */
#define DIP_RESERVED_CURRENT_A      16.0f

/* ======================================================================== */
/* Temperature sensing — NOT FITTED on this revision                        */
/* ======================================================================== */

/**
 * No NTC is assigned in the pinout, so thermal derating and the over-temperature
 * trip are compiled out. Connector-temperature monitoring is required by
 * UL 2594 / IEC 62196-1 for a Level-2 unit, so this should be populated before
 * the design is certified.
 *
 * Free ADC-capable pins on this package: PA6 (ADC1_INP3), PB1 (ADC1_INP5),
 * PC9 has no ADC. PA6 and PB1 are the natural choices.
 */
#define EVSE_TEMP_SENSORS_FITTED    0
#define TEMP_DERATE_START_C         60
#define TEMP_DERATE_FULL_C          75
#define TEMP_TRIP_C                 85

/* ======================================================================== */
/* Energy metering — ATM90E26 on UART4 ("ENME_TX" PD12 / "ENME_RX" PD11)    */
/* ======================================================================== */

#define METER_UART_HANDLE           huart4
/** ATM90E26 serial mode runs at 9600 8N1 and cannot be changed. */
#define METER_UART_BAUD             9600U
#define METER_RESPONSE_TIMEOUT_MS   50U

/**
 * "ENME_IRQ" (PE8, EXTI8 falling) — ATM90E26 IRQ output.
 *
 * IMPORTANT: the ATM90E26's IRQ has no programmable over-current threshold.
 * Its only interrupt sources are voltage **sag** (SagTh, 0x03) and reverse
 * active/reactive **energy direction** change, enabled through FuncEn (0x02).
 * There is no register that asserts IRQ when Irms exceeds a limit.
 *
 * So the IRQ is wired and used — it gives a genuinely fast voltage-sag trip —
 * but over-current is detected by the MCU comparing Irms against the offered
 * current in the 5 ms safety task. Treating the IRQ alone as over-current
 * protection would leave the unit with none. See docs/METERING.md.
 */
#define METER_IRQ_GPIO_PORT         GPIOE
#define METER_IRQ_GPIO_PIN          GPIO_PIN_8

/**
 * Voltage-sag threshold, volts RMS. Below this the ATM90E26 raises IRQ.
 * Set well under nominal so normal load steps do not trip it.
 */
#define METER_SAG_THRESHOLD_V       180.0f

/* SPI3 (PA4 NSS, PC10 SCK, PC11 MISO, PC12 MOSI) is wired but unlabelled in
 * the pinout. It is left unclaimed here. Note that CubeMX currently sets
 * SPI_NSS_HARD_INPUT while in master mode, which will raise a mode fault if
 * PA4 is pulled low — switch to software NSS before using it. */

/* ======================================================================== */
/* Indicators and buzzer                                                    */
/* ======================================================================== */

#define LED_POWER_GPIO_PORT         GPIOC          /* "LED_PWR"  PC7 */
#define LED_POWER_GPIO_PIN          GPIO_PIN_7
#define LED_CHARGE_GPIO_PORT        GPIOC          /* "LED_CHG"  PC6 */
#define LED_CHARGE_GPIO_PIN         GPIO_PIN_6
#define LED_FAULT_GPIO_PORT         GPIOC          /* "LED_FLT"  PC8 */
#define LED_FAULT_GPIO_PIN          GPIO_PIN_8
#define LED_WIFI_GPIO_PORT          GPIOA          /* "LED_WIFI" PA3 */
#define LED_WIFI_GPIO_PIN           GPIO_PIN_3
#define LED_ACTIVE_STATE            GPIO_PIN_SET

/** Buzzer on TIM3_CH3 / PB0, so the tone frequency is controllable. */
#define BUZZER_TIM_HANDLE           htim3
#define BUZZER_TIM_CHANNEL          TIM_CHANNEL_3
#define BUZZER_TIMER_CLK_HZ         1000000U       /* prescaler set at init */
#define BUZZER_GPIO_PORT            GPIOB
#define BUZZER_GPIO_PIN             GPIO_PIN_0

/* ======================================================================== */
/* Connectivity                                                             */
/* ======================================================================== */

/* Ethernet PHY on RMII.
 * PA1 REF_CLK, PA2 MDIO, PC1 MDC, PA7 CRS_DV, PC4 RXD0, PC5 RXD1,
 * PA5 TX_EN, PB12 TXD0, PB15 TXD1. Reset "ETH_PHY_RST" on PD3. */
#define ETH_PHY_ADDRESS             0x00
#define ETH_PHY_RESET_GPIO_PORT     GPIOD
#define ETH_PHY_RESET_GPIO_PIN      GPIO_PIN_3

/* ESP32 Wi-Fi co-processor on USART1: "WIFI_TX" PB14, "WIFI_RX" PA10. */
#define ESP32_UART_HANDLE           huart1
#define ESP32_UART_BAUD             115200U
#define ESP32_RESET_GPIO_PORT       GPIOA          /* "WIFI_RST" PA9 */
#define ESP32_RESET_GPIO_PIN        GPIO_PIN_9

/* Console / service UART on USART3 (PD8 TX, PD9 RX). */
#define DEBUG_UART_HANDLE           huart3
#define DEBUG_UART_BAUD             115200U

/* ======================================================================== */
/* NVM — configuration and transaction journal in on-chip flash             */
/* ======================================================================== */

/* STM32H573VI: 2 MB flash in two 1 MB banks, 8 KB sectors.
 * The last sectors of bank 2 are reserved for application data. */
#define NVM_CONFIG_ADDR             0x081FC000UL   /* 8 KB, copy A */
#define NVM_CONFIG_ADDR_B           0x081FE000UL   /* 8 KB, copy B */
#define NVM_TXNLOG_ADDR             0x081F8000UL   /* 16 KB ring    */
#define NVM_TXNLOG_SIZE             0x00004000UL
#define NVM_SECTOR_SIZE             0x00002000UL

/* ======================================================================== */
/* Task priorities and periods                                              */
/* ======================================================================== */

#define TASK_PRIO_SAFETY            (configMAX_PRIORITIES - 1)
#define TASK_PRIO_PILOT             (configMAX_PRIORITIES - 2)
#define TASK_PRIO_EVSE              (configMAX_PRIORITIES - 3)
#define TASK_PRIO_METER             (configMAX_PRIORITIES - 4)
#define TASK_PRIO_HMI               (configMAX_PRIORITIES - 5)
#define TASK_PRIO_OCPP              (configMAX_PRIORITIES - 6)

#define TASK_PERIOD_SAFETY_MS       5U
#define TASK_PERIOD_PILOT_MS        10U
#define TASK_PERIOD_EVSE_MS         20U
#define TASK_PERIOD_METER_MS        250U
#define TASK_PERIOD_HMI_MS          50U

#endif /* EVSE_BOARD_H */
