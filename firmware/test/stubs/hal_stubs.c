/** Definitions for the HAL and FreeRTOS stand-ins. Never executed in anger. */
#include "stm32h5xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static GPIO_TypeDef s_ports[5];
GPIO_TypeDef *GPIOA = &s_ports[0];
GPIO_TypeDef *GPIOB = &s_ports[1];
GPIO_TypeDef *GPIOC = &s_ports[2];
GPIO_TypeDef *GPIOD = &s_ports[3];
GPIO_TypeDef *GPIOE = &s_ports[4];

static uint32_t s_adc1, s_adc2, s_spi3;
void *ADC1 = &s_adc1;
void *ADC2 = &s_adc2;
void *SPI3 = &s_spi3;

static RCC_TypeDef s_rcc;
RCC_TypeDef *RCC = &s_rcc;

void          HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t n, GPIO_PinState s) { (void)p;(void)n;(void)s; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t n) { (void)p;(void)n; return GPIO_PIN_RESET; }
void          HAL_Delay(uint32_t ms) { (void)ms; }
uint32_t      HAL_GetTick(void) { return 0; }

void HAL_NVIC_SetPriority(IRQn_Type i, uint32_t a, uint32_t b) { (void)i;(void)a;(void)b; }
void HAL_NVIC_EnableIRQ(IRQn_Type i) { (void)i; }
void NVIC_SystemReset(void) { }

uint32_t HAL_RCC_GetPCLK1Freq(void) { return 250000000U; }
uint32_t HAL_RCC_GetPCLK2Freq(void) { return 250000000U; }

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *h, uint32_t c) { (void)h;(void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_OC_Start(TIM_HandleTypeDef *h, uint32_t c) { (void)h;(void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_OC_ConfigChannel(TIM_HandleTypeDef *h, TIM_OC_InitTypeDef *o, uint32_t c)
{ (void)h;(void)o;(void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_GenerateEvent(TIM_HandleTypeDef *h, uint32_t s) { (void)h;(void)s; return HAL_OK; }
void     tim_set_compare(TIM_HandleTypeDef *h, uint32_t c, uint32_t v) { (void)h;(void)c;(void)v; }
uint32_t tim_get_autoreload(TIM_HandleTypeDef *h) { return h ? h->Init.Period : 0U; }

HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Start_IT(ADC_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *h, uint32_t t) { (void)h;(void)t; return HAL_OK; }
uint32_t          HAL_ADC_GetValue(ADC_HandleTypeDef *h) { (void)h; return 0; }
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *h, ADC_ChannelConfTypeDef *c) { (void)h;(void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *h, uint32_t a, uint32_t b) { (void)h;(void)a;(void)b; return HAL_OK; }
HAL_StatusTypeDef HAL_ADCEx_InjectedStart_IT(ADC_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_ADCEx_InjectedConfigChannel(ADC_HandleTypeDef *h, ADC_InjectionConfTypeDef *c) { (void)h;(void)c; return HAL_OK; }
uint32_t          HAL_ADCEx_InjectedGetValue(ADC_HandleTypeDef *h, uint32_t r) { (void)h;(void)r; return 0; }

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t t) { (void)h;(void)d;(void)n;(void)t; return HAL_OK; }
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t t) { (void)h;(void)d;(void)n;(void)t; return HAL_TIMEOUT; }
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *d, uint16_t n) { (void)h;(void)d;(void)n; return HAL_OK; }

HAL_StatusTypeDef HAL_RTC_GetTime(RTC_HandleTypeDef *h, RTC_TimeTypeDef *t, uint32_t f) { (void)h;(void)t;(void)f; return HAL_OK; }
HAL_StatusTypeDef HAL_RTC_GetDate(RTC_HandleTypeDef *h, RTC_DateTypeDef *d, uint32_t f) { (void)h;(void)d;(void)f; return HAL_OK; }
HAL_StatusTypeDef HAL_RTC_SetTime(RTC_HandleTypeDef *h, RTC_TimeTypeDef *t, uint32_t f) { (void)h;(void)t;(void)f; return HAL_OK; }
HAL_StatusTypeDef HAL_RTC_SetDate(RTC_HandleTypeDef *h, RTC_DateTypeDef *d, uint32_t f) { (void)h;(void)d;(void)f; return HAL_OK; }
uint32_t HAL_RTCEx_BKUPRead(RTC_HandleTypeDef *h, uint32_t r) { (void)h;(void)r; return 0; }
void     HAL_RTCEx_BKUPWrite(RTC_HandleTypeDef *h, uint32_t r, uint32_t v) { (void)h;(void)r;(void)v; }

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t t, uint32_t a, uint32_t d) { (void)t;(void)a;(void)d; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e, uint32_t *err) { (void)e; if (err) *err = 0xFFFFFFFFU; return HAL_OK; }

TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelayUntil(TickType_t *p, TickType_t t) { (void)p;(void)t; }
void vTaskStartScheduler(void) { }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *n, uint32_t d, void *p,
                               uint32_t pr, StackType_t *s, StaticTask_t *tcb)
{ (void)fn;(void)n;(void)d;(void)p;(void)pr;(void)s;(void)tcb; return (TaskHandle_t)1; }

static int s_mutex;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m) { (void)m; return &s_mutex; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t w) { (void)s;(void)w; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
