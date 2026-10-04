#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#define ESP_ERR_NVS_NOT_FOUND 0x1102

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value);
esp_err_t nvs_get_i16(nvs_handle_t handle, const char *key, int16_t *value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_set_i16(nvs_handle_t handle, const char *key, int16_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
