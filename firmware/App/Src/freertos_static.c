/**
 * @file  freertos_static.c
 * @brief Memory providers required by FreeRTOS static allocation.
 *
 * This firmware creates every task with xTaskCreateStatic() and every mutex
 * with xSemaphoreCreateMutexStatic(), so there is no heap and no allocation
 * failure to handle at three in the morning on a live site.
 *
 * The cost is that FreeRTOS then requires the application to supply the memory
 * for its own two internal tasks. Without these the build fails at link time
 * with an undefined reference to vApplicationGetIdleTaskMemory, which is a
 * confusing error if you have not seen it before — it looks like a FreeRTOS
 * problem rather than a "you have to provide this" problem.
 *
 * STM32CubeMX generates these itself when "Memory Management" is set to a
 * static-capable scheme. If your generated code already defines them, set
 * EVSE_CUBEMX_PROVIDES_STATIC_MEMORY=1 in the project's preprocessor symbols to
 * compile this file out, rather than deleting it — regenerating from the .ioc
 * would otherwise bring the duplicate back.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#if (configSUPPORT_STATIC_ALLOCATION == 1) && !defined(EVSE_CUBEMX_PROVIDES_STATIC_MEMORY)

/* The idle task runs only when nothing else can; its stack needs are minimal,
 * but it also runs any thread-local cleanup, so do not trim this further. */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   uint32_t *stack_size)
{
    *tcb = &s_idle_tcb;
    *stack = s_idle_stack;
    *stack_size = configMINIMAL_STACK_SIZE;
}

#if (configUSE_TIMERS == 1)

/* The timer service task runs software timer callbacks. Nothing in this
 * firmware uses them, but FreeRTOS still creates the task when timers are
 * enabled, so it still needs somewhere to live. */
static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                    uint32_t *stack_size)
{
    *tcb = &s_timer_tcb;
    *stack = s_timer_stack;
    *stack_size = configTIMER_TASK_STACK_DEPTH;
}

#endif /* configUSE_TIMERS */
#endif /* configSUPPORT_STATIC_ALLOCATION && !EVSE_CUBEMX_PROVIDES_STATIC_MEMORY */
