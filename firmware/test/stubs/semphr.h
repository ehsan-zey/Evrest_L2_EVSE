#ifndef SEMPHR_STUB_H
#define SEMPHR_STUB_H
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *mem);
int  xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
int  xSemaphoreGive(SemaphoreHandle_t s);
#endif
