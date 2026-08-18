#ifndef TASK_STUB_H
#define TASK_STUB_H
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void       vTaskDelay(TickType_t ticks);
void       vTaskDelayUntil(TickType_t *prev, TickType_t period);
void       vTaskStartScheduler(void);
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name,
                               uint32_t depth, void *param, uint32_t prio,
                               StackType_t *stack, StaticTask_t *tcb);
#endif
