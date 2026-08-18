/**
 * Stand-in for the ST HAL, sufficient to compile-check the application layer
 * on the host. Nothing here executes — the point is to catch type errors,
 * missing declarations and typos in HAL usage without a full CubeMX tree.
 */
#ifndef STM32H5XX_HAL_STUB_H
#define STM32H5XX_HAL_STUB_H

#include <stdint.h>
#include <stddef.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { DISABLE = 0, ENABLE = 1 } FunctionalState;

/* ---- GPIO ---- */
typedef struct { uint32_t dummy; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
extern GPIO_TypeDef *GPIOA, *GPIOB, *GPIOC, *GPIOD, *GPIOE;

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
#define GPIO_PIN_11  0x0800U
#define GPIO_PIN_12  0x1000U
#define GPIO_PIN_13  0x2000U

void          HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void          HAL_Delay(uint32_t ms);
uint32_t      HAL_GetTick(void);

/* ---- NVIC ---- */
typedef enum {
    EXTI0_IRQn = 6, EXTI2_IRQn = 8, EXTI5_IRQn = 11, EXTI8_IRQn = 14,
    EXTI10_IRQn = 16, EXTI13_IRQn = 19, ADC1_IRQn = 37
} IRQn_Type;
void HAL_NVIC_SetPriority(IRQn_Type irq, uint32_t pre, uint32_t sub);
void HAL_NVIC_EnableIRQ(IRQn_Type irq);
void NVIC_SystemReset(void);
#define __NOP()          do { } while (0)
#define __disable_irq()  do { } while (0)

/* ---- RCC ---- */
typedef struct { uint32_t CFGR2; } RCC_TypeDef;
extern RCC_TypeDef *RCC;
#define RCC_CFGR2_PPRE1      0x00000070U
#define RCC_CFGR2_PPRE1_Pos  4U
#define RCC_CFGR2_PPRE2      0x00000700U
#define RCC_CFGR2_PPRE2_Pos  8U
uint32_t HAL_RCC_GetPCLK1Freq(void);
uint32_t HAL_RCC_GetPCLK2Freq(void);

/* ---- TIM ---- */
typedef struct { uint32_t Prescaler, Period; } TIM_Init_t;
typedef struct { void *Instance; TIM_Init_t Init; } TIM_HandleTypeDef;
typedef struct {
    uint32_t OCMode, Pulse, OCPolarity, OCNPolarity, OCFastMode,
             OCIdleState, OCNIdleState;
} TIM_OC_InitTypeDef;

#define TIM_CHANNEL_1  0x00000000U
#define TIM_CHANNEL_2  0x00000004U
#define TIM_CHANNEL_3  0x00000008U
#define TIM_CHANNEL_4  0x0000000CU
#define TIM_OCMODE_TIMING        0x00000000U
#define TIM_OCPOLARITY_HIGH      0x00000000U
#define TIM_OCFAST_DISABLE       0x00000000U
#define TIM_OCIDLESTATE_RESET    0x00000000U
#define TIM_OCNIDLESTATE_RESET   0x00000000U
#define TIM_EVENTSOURCE_UPDATE   0x00000001U

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *h, uint32_t ch);
HAL_StatusTypeDef HAL_TIM_OC_Start(TIM_HandleTypeDef *h, uint32_t ch);
HAL_StatusTypeDef HAL_TIM_OC_ConfigChannel(TIM_HandleTypeDef *h,
                                           TIM_OC_InitTypeDef *cfg, uint32_t ch);
HAL_StatusTypeDef HAL_TIM_GenerateEvent(TIM_HandleTypeDef *h, uint32_t src);
void     tim_set_compare(TIM_HandleTypeDef *h, uint32_t ch, uint32_t v);
uint32_t tim_get_autoreload(TIM_HandleTypeDef *h);
#define __HAL_TIM_SET_COMPARE(h, ch, v)  tim_set_compare((h), (ch), (v))
#define __HAL_TIM_SET_AUTORELOAD(h, v)   ((h)->Init.Period = (v))
#define __HAL_TIM_SET_PRESCALER(h, v)    ((h)->Init.Prescaler = (v))
#define __HAL_TIM_GET_AUTORELOAD(h)      tim_get_autoreload(h)
#define __HAL_TIM_MOE_ENABLE(h)          do { (void)(h); } while (0)

/* ---- ADC ---- */
typedef struct {
    uint32_t ScanConvMode, NbrOfConversion, ContinuousConvMode,
             DiscontinuousConvMode, ExternalTrigConv, ExternalTrigConvEdge,
             DMAContinuousRequests, EOCSelection, Overrun;
} ADC_Init_t;
typedef struct { void *Instance; ADC_Init_t Init; } ADC_HandleTypeDef;
extern void *ADC1, *ADC2, *SPI3;
typedef struct {
    uint32_t Channel, Rank, SamplingTime, SingleDiff, OffsetNumber, Offset;
} ADC_ChannelConfTypeDef;
typedef struct {
    uint32_t InjectedChannel, InjectedRank, InjectedSamplingTime,
             InjectedSingleDiff, InjectedOffsetNumber, InjectedNbrOfConversion,
             InjectedDiscontinuousConvMode, AutoInjectedConv,
             QueueInjectedContext, ExternalTrigInjecConv,
             ExternalTrigInjecConvEdge;
} ADC_InjectionConfTypeDef;

#define ADC_CHANNEL_0   0U
#define ADC_CHANNEL_10  10U
#define ADC_CHANNEL_12  12U
#define ADC_CHANNEL_13  13U
#define ADC_SCAN_DISABLE            0U
#define ADC_REGULAR_RANK_1          1U
#define ADC_INJECTED_RANK_1         1U
#define ADC_INJECTED_RANK_2         2U
#define ADC_SAMPLETIME_24CYCLES_5   3U
#define ADC_SAMPLETIME_247CYCLES_5  6U
#define ADC_SINGLE_ENDED            0U
#define ADC_OFFSET_NONE             0U
#define ADC_CALIB_OFFSET            0U
#define ADC_EOC_SINGLE_CONV         0U
#define ADC_OVR_DATA_OVERWRITTEN    0U
#define ADC_EXTERNALTRIG_T1_CC2         2U
#define ADC_EXTERNALTRIGINJEC_T1_CC4    4U
#define ADC_EXTERNALTRIGCONVEDGE_RISING 1U
#define ADC_EXTERNALTRIGINJECCONV_EDGE_RISING 1U

HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_ADC_Start_IT(ADC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *h, uint32_t to);
uint32_t          HAL_ADC_GetValue(ADC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *h, ADC_ChannelConfTypeDef *c);
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *h, uint32_t a, uint32_t b);
HAL_StatusTypeDef HAL_ADCEx_InjectedStart_IT(ADC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_ADCEx_InjectedConfigChannel(ADC_HandleTypeDef *h,
                                                  ADC_InjectionConfTypeDef *c);
uint32_t          HAL_ADCEx_InjectedGetValue(ADC_HandleTypeDef *h, uint32_t rank);

/* ---- UART ---- */
typedef struct { void *Instance; } UART_HandleTypeDef;
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t to);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t to);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *d, uint16_t n);
#define __HAL_UART_CLEAR_OREFLAG(h)  do { (void)(h); } while (0)

/* ---- SPI ---- */
typedef struct { void *Instance; } SPI_HandleTypeDef;

/* ---- RTC ---- */
typedef struct { uint32_t dummy; } RTC_Init_t;
typedef struct { void *Instance; RTC_Init_t Init; } RTC_HandleTypeDef;
typedef struct { uint8_t Hours, Minutes, Seconds;
                 uint32_t DayLightSaving, StoreOperation; } RTC_TimeTypeDef;
typedef struct { uint8_t WeekDay, Month, Date, Year; } RTC_DateTypeDef;
#define RTC_FORMAT_BIN            0U
#define RTC_DAYLIGHTSAVING_NONE   0U
#define RTC_STOREOPERATION_RESET  0U
#define RTC_WEEKDAY_MONDAY        1U
#define RTC_BKP_DR0               0U
HAL_StatusTypeDef HAL_RTC_GetTime(RTC_HandleTypeDef *h, RTC_TimeTypeDef *t, uint32_t f);
HAL_StatusTypeDef HAL_RTC_GetDate(RTC_HandleTypeDef *h, RTC_DateTypeDef *d, uint32_t f);
HAL_StatusTypeDef HAL_RTC_SetTime(RTC_HandleTypeDef *h, RTC_TimeTypeDef *t, uint32_t f);
HAL_StatusTypeDef HAL_RTC_SetDate(RTC_HandleTypeDef *h, RTC_DateTypeDef *d, uint32_t f);
uint32_t          HAL_RTCEx_BKUPRead(RTC_HandleTypeDef *h, uint32_t reg);
void              HAL_RTCEx_BKUPWrite(RTC_HandleTypeDef *h, uint32_t reg, uint32_t v);

/* ---- FLASH ---- */
typedef struct {
    uint32_t TypeErase, Banks, Sector, NbSectors;
} FLASH_EraseInitTypeDef;
#define FLASH_TYPEERASE_SECTORS    0U
#define FLASH_TYPEPROGRAM_QUADWORD 0U
#define FLASH_BANK_2               2U
#define FLASH_BASE                 0x08000000UL
#define FLASH_BANK_SIZE            0x00100000UL
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t addr, uint32_t data);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e, uint32_t *err);

#endif
