/**
 * @file  evse_hw.c
 * @brief Peripheral setup on top of CubeMX. See evse_hw.h.
 */
#include "evse_hw.h"
#include "evse_board.h"
#include "cp_pilot.h"
#include "safety.h"
#include "meter.h"
#include "relay.h"

extern ADC_HandleTypeDef  hadc1;
extern TIM_HandleTypeDef  htim1;
extern TIM_HandleTypeDef  htim3;

/* ---------------------------------------------------------------------- */
/* TIM1 — control pilot                                                   */
/* ---------------------------------------------------------------------- */

/**
 * Re-derive the pilot timer from the real clock.
 *
 * The generated code hard-codes PSC=31 / ARR=999, which is 1 kHz only while
 * the part runs from HSI at 32 MHz. Selecting the PLL would silently move the
 * pilot to 7.8 kHz and every vehicle would reject it, with nothing in the code
 * indicating why. Computing it here means the pilot frequency is a function of
 * the actual clock, not of an assumption about it.
 */
static bool tim1_configure_pilot(void)
{
    uint32_t timer_clk = HAL_RCC_GetPCLK2Freq();

    /*
     * When the APB2 prescaler is not 1, the timer clock is doubled. HAL exposes
     * the bus clock, not the timer clock, so the doubling has to be applied by
     * hand or the pilot comes out at half frequency.
     */
    uint32_t apb2_div = (RCC->CFGR2 & RCC_CFGR2_PPRE2) >> RCC_CFGR2_PPRE2_Pos;
    if (apb2_div >= 4u) {
        timer_clk *= 2u;
    }

    uint32_t arr = CP_PWM_RESOLUTION - 1u;
    uint32_t psc = (timer_clk / (CP_PWM_FREQ_HZ * CP_PWM_RESOLUTION));
    if (psc == 0u) {
        return false;             /* clock too slow for the chosen resolution */
    }
    psc -= 1u;
    if (psc > 0xFFFFu) {
        return false;
    }

    __HAL_TIM_SET_PRESCALER(&htim1, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);

    /* CH2 and CH4 generate compare events for the ADC but drive no pin. */
    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_TIMING;      /* compare event only, no output */
    oc.Pulse      = arr / 2u;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    if (HAL_TIM_OC_ConfigChannel(&htim1, &oc, CP_SAMPLE_HI_CHANNEL) != HAL_OK) return false;
    oc.Pulse = arr - (arr / 8u);
    if (HAL_TIM_OC_ConfigChannel(&htim1, &oc, CP_SAMPLE_LO_CHANNEL) != HAL_OK) return false;

    /* Force the new PSC/ARR to take effect immediately rather than at the next
     * update event, so the very first pilot period is already correct. */
    HAL_TIM_GenerateEvent(&htim1, TIM_EVENTSOURCE_UPDATE);
    return true;
}

/* ---------------------------------------------------------------------- */
/* ADC1 — pilot and proximity                                             */
/* ---------------------------------------------------------------------- */

/**
 * Rebuild ADC1 as:
 *   regular  : 1 conversion, CP, triggered by TIM1_CC2  (high plateau)
 *   injected : 2 conversions, CP then PP, triggered by TIM1_CC4 (low plateau)
 *
 * The generated code sets up a 2-rank *regular* scan of CP and PP, which is
 * why the original peak-hold read a mixture of the two signals. Splitting them
 * across the two groups is what makes each reading mean one thing.
 */
static bool adc1_configure(void)
{
    (void)HAL_ADC_Stop(&hadc1);   /* harmless if it was never started */

    hadc1.Init.ScanConvMode         = ADC_SCAN_DISABLE;
    hadc1.Init.NbrOfConversion      = 1u;
    hadc1.Init.ContinuousConvMode   = DISABLE;
    hadc1.Init.DiscontinuousConvMode= DISABLE;
    hadc1.Init.ExternalTrigConv     = ADC_EXTERNALTRIG_T1_CC2;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests= DISABLE;
    hadc1.Init.EOCSelection         = ADC_EOC_SINGLE_CONV;
    hadc1.Init.Overrun              = ADC_OVR_DATA_OVERWRITTEN;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) return false;

    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel      = CP_ADC_CHANNEL;
    ch.Rank         = ADC_REGULAR_RANK_1;
    /*
     * The CP divider is a low-impedance resistive network and the sampling
     * window has to fit inside the shortest plateau — 4 % of 1 ms at 96 % duty,
     * i.e. 40 us. 24.5 cycles is a few hundred nanoseconds and leaves ample
     * margin, where the generated 247.5 cycles would straddle the edge.
     */
    ch.SamplingTime = ADC_SAMPLETIME_24CYCLES_5;
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset       = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) return false;

    ADC_InjectionConfTypeDef inj = {0};
    inj.InjectedChannel               = CP_ADC_CHANNEL;
    inj.InjectedRank                  = ADC_INJECTED_RANK_1;
    inj.InjectedSamplingTime          = ADC_SAMPLETIME_24CYCLES_5;
    inj.InjectedSingleDiff            = ADC_SINGLE_ENDED;
    inj.InjectedOffsetNumber          = ADC_OFFSET_NONE;
    inj.InjectedNbrOfConversion       = 2u;
    inj.InjectedDiscontinuousConvMode = DISABLE;
    inj.AutoInjectedConv              = DISABLE;
    inj.QueueInjectedContext          = DISABLE;
    inj.ExternalTrigInjecConv         = ADC_EXTERNALTRIGINJEC_T1_CC4;
    inj.ExternalTrigInjecConvEdge     = ADC_EXTERNALTRIGINJECCONV_EDGE_RISING;
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj) != HAL_OK) return false;

    /* Rank 2 is the proximity pilot. It is a DC level, so it can afford — and
     * with its higher source impedance, wants — a much longer sampling time. */
    inj.InjectedChannel      = PP_ADC_CHANNEL;
    inj.InjectedRank         = ADC_INJECTED_RANK_2;
    inj.InjectedSamplingTime = ADC_SAMPLETIME_247CYCLES_5;
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj) != HAL_OK) return false;

    return true;
}

/* ---------------------------------------------------------------------- */
/* TIM3 — buzzer                                                          */
/* ---------------------------------------------------------------------- */

static bool tim3_configure_buzzer(void)
{
    uint32_t timer_clk = HAL_RCC_GetPCLK1Freq();
    uint32_t apb1_div = (RCC->CFGR2 & RCC_CFGR2_PPRE1) >> RCC_CFGR2_PPRE1_Pos;
    if (apb1_div >= 4u) {
        timer_clk *= 2u;
    }
    /* hmi.c computes ARR assuming a 1 MHz counter clock. */
    uint32_t psc = (timer_clk / BUZZER_TIMER_CLK_HZ);
    if (psc == 0u) return false;
    __HAL_TIM_SET_PRESCALER(&htim3, psc - 1u);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Public                                                                 */
/* ---------------------------------------------------------------------- */

void evse_hw_reset_peripherals(void)
{
    HAL_GPIO_WritePin(ETH_PHY_RESET_GPIO_PORT, ETH_PHY_RESET_GPIO_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ESP32_RESET_GPIO_PORT,   ESP32_RESET_GPIO_PIN,   GPIO_PIN_RESET);
    HAL_Delay(20);
    HAL_GPIO_WritePin(ETH_PHY_RESET_GPIO_PORT, ETH_PHY_RESET_GPIO_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ESP32_RESET_GPIO_PORT,   ESP32_RESET_GPIO_PIN,   GPIO_PIN_SET);
    HAL_Delay(50);
}

bool evse_hw_init(void)
{
    /*
     * The contactor is de-energised before anything else. If a later step
     * fails and we return false, the caller must be able to rely on the power
     * path already being open.
     */
    relay_init();

    if (!tim1_configure_pilot())  return false;
    if (!adc1_configure())        return false;
    if (!tim3_configure_buzzer()) return false;

    /*
     * EXTI priorities. The RCD line must be able to pre-empt everything,
     * including FreeRTOS's own critical sections — but it calls no FreeRTOS
     * API, only two register writes, so it is safe to place it above
     * configMAX_SYSCALL_INTERRUPT_PRIORITY. Emergency-off is the same.
     * Everything that does touch the kernel stays below it.
     */
    HAL_NVIC_SetPriority(EXTI10_IRQn, 0, 0);   /* RCD_INT  PB10 */
    HAL_NVIC_EnableIRQ(EXTI10_IRQn);
    HAL_NVIC_SetPriority(EXTI13_IRQn, 0, 1);   /* EMG_OFF  PC13 */
    HAL_NVIC_EnableIRQ(EXTI13_IRQn);
    HAL_NVIC_SetPriority(EXTI2_IRQn,  5, 0);   /* PEN_DET  PC2  */
    HAL_NVIC_EnableIRQ(EXTI2_IRQn);
    HAL_NVIC_SetPriority(EXTI8_IRQn,  6, 0);   /* ENME_IRQ PE8  */
    HAL_NVIC_EnableIRQ(EXTI8_IRQn);

    HAL_NVIC_SetPriority(ADC1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ADC1_IRQn);

    evse_hw_reset_peripherals();
    return true;
}

/* ---------------------------------------------------------------------- */
/* HAL callbacks                                                          */
/* ---------------------------------------------------------------------- */

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        cp_pilot_regular_isr((uint16_t)HAL_ADC_GetValue(hadc));
    }
}

void HAL_ADCEx_InjectedConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    uint16_t cp_low = (uint16_t)HAL_ADCEx_InjectedGetValue(hadc, ADC_INJECTED_RANK_1);
    uint16_t pp     = (uint16_t)HAL_ADCEx_InjectedGetValue(hadc, ADC_INJECTED_RANK_2);
    cp_pilot_injected_isr(cp_low, pp);
}

/**
 * All EXTI lines land here.
 *
 * The RCD and emergency-off branches open the contactor before doing anything
 * else — no logging, no queueing, no kernel call. That is what bounds the trip
 * latency to a couple of microseconds and makes it independent of whatever the
 * network stack happens to be doing.
 */
void HAL_GPIO_EXTI_Falling_Callback(uint16_t pin)
{
    switch (pin) {
        case RCD_INT_GPIO_PIN:   safety_rcd_isr();   break;   /* PB10 */
        case ESTOP_GPIO_PIN:     safety_estop_isr(); break;   /* PC13 */
        case METER_IRQ_GPIO_PIN: meter_irq_isr();    break;   /* PE8  */
        default: break;
    }
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t pin)
{
    if (pin == PEN_FAULT_GPIO_PIN) {          /* PC2 */
        /*
         * Open PEN means the neutral has been lost and exposed metalwork may be
         * sitting at line potential. Nothing about that is worth deferring.
         */
        relay_emergency_open();
        safety_pen_isr();
    }
}
