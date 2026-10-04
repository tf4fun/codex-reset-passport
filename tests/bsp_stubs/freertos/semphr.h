#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
