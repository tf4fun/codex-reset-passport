#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define RTC_DATA_ATTR

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *task);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

int64_t esp_timer_get_time(void);
esp_err_t esp_crt_bundle_attach(void *config);

typedef void *esp_http_client_handle_t;
typedef struct {
    int event_id;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_METHOD_GET 0
#define HTTP_TRANSPORT_OVER_SSL 2
typedef struct {
    const char *url;
    int method;
    int transport_type;
    int timeout_ms;
    bool disable_auto_redirect;
    int max_authorization_retries;
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*crt_bundle_attach)(void *);
    bool skip_cert_common_name_check;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    const char *user_agent;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
