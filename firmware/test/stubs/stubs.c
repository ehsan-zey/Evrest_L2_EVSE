/** Storage for the fake GPIO port pointers the board header refers to. */
#include "stm32h5xx_hal.h"
static GPIO_TypeDef s_ports[5];
GPIO_TypeDef *GPIOA = &s_ports[0];
GPIO_TypeDef *GPIOB = &s_ports[1];
GPIO_TypeDef *GPIOC = &s_ports[2];
GPIO_TypeDef *GPIOD = &s_ports[3];
GPIO_TypeDef *GPIOE = &s_ports[4];
