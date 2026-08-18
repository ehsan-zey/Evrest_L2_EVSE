/** Minimal FreeRTOS stand-in for host compile checks. */
#ifndef FREERTOS_STUB_H
#define FREERTOS_STUB_H
#include <stdint.h>
#include <stddef.h>

typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void *   TaskHandle_t;
typedef struct { uint32_t dummy[16]; } StaticTask_t;
typedef struct { uint32_t dummy[8];  } StaticSemaphore_t;
typedef void *   SemaphoreHandle_t;

#define configMAX_PRIORITIES   7
#define portMAX_DELAY          0xFFFFFFFFU
#define portTICK_PERIOD_MS     1U
#define pdMS_TO_TICKS(ms)      ((TickType_t)(ms))
#define pdTRUE                 1
#define pdFALSE                0
#endif
