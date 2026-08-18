/**
 * Minimal stand-in for the ST HAL so the pure application logic can be built
 * and tested on the host. Only the declarations evse_board.h needs are here —
 * anything that actually touches hardware is behind EVSE_HOST_TEST.
 */
#ifndef STM32H5XX_HAL_STUB_H
#define STM32H5XX_HAL_STUB_H

#include <stdint.h>

typedef struct { uint32_t dummy; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

#define GPIO_PIN_0   0x0001U
#define GPIO_PIN_1   0x0002U
#define GPIO_PIN_2   0x0004U
#define GPIO_PIN_3   0x0008U
#define GPIO_PIN_4   0x0010U
#define GPIO_PIN_5   0x0020U
#define GPIO_PIN_6   0x0040U
#define GPIO_PIN_7   0x0080U
#define GPIO_PIN_8   0x0100U
#define GPIO_PIN_9   0x0200U
#define GPIO_PIN_10  0x0400U
#define GPIO_PIN_12  0x1000U
#define GPIO_PIN_13  0x2000U

extern GPIO_TypeDef *GPIOA, *GPIOB, *GPIOC, *GPIOD, *GPIOE;

#define TIM_CHANNEL_1  0x00000000U
#define TIM_CHANNEL_2  0x00000004U
#define TIM_CHANNEL_3  0x00000008U
#define TIM_CHANNEL_4  0x0000000CU

#define ADC_CHANNEL_0   0U
#define ADC_CHANNEL_10  10U

#define FLASH_BASE        0x08000000UL
#define FLASH_BANK_SIZE   0x00100000UL

#endif
