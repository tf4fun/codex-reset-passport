#pragma once
/* Minimal typed ESP-IDF boundary for tests of the production Wi-Fi worker. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
const char *esp_err_to_name(esp_err_t error);
void test_log(const char *tag, const char *format, ...);
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOG_WARN 2
void esp_log_level_set(const char *tag, int level);
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
int64_t esp_timer_get_time(void);
void mbedtls_platform_zeroize(void *buffer, size_t size);

typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef struct test_sem *SemaphoreHandle_t;
typedef struct test_queue *QueueHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define configMAX_PRIORITIES 25
#define NIMBLE_HS_STACK_SIZE 4096
#define NIMBLE_CORE 0
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name,
                                 unsigned stack, void *arg, unsigned priority,
                                 TaskHandle_t *task, int core);
void vTaskDelete(TaskHandle_t task);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
void vSemaphoreDelete(SemaphoreHandle_t sem);
QueueHandle_t xQueueCreate(unsigned count, unsigned bytes);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);

typedef struct { int unused; } esp_netif_t;
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
extern const char *WIFI_EVENT;
extern const char *IP_EVENT;
#define WIFI_EVENT_STA_DISCONNECTED 1
#define WIFI_EVENT_SCAN_DONE 2
#define IP_EVENT_STA_GOT_IP 3
#define IP_EVENT_STA_LOST_IP 4
#define ESP_EVENT_ANY_ID -1
#define WIFI_STORAGE_RAM 0
#define WIFI_STORAGE_FLASH 1
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_PS_MIN_MODEM 1
typedef struct { int unused; } wifi_init_config_t;
typedef struct { int unused; } wifi_scan_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { 0 }
typedef struct {
    struct {
        uint8_t ssid[32]; uint8_t password[64];
        struct { int authmode; } threshold;
        struct { bool capable; } pmf_cfg;
    } sta;
} wifi_config_t;
typedef struct { int rssi; uint8_t ssid[33]; } wifi_ap_record_t;
typedef struct { uint16_t reason; } wifi_event_sta_disconnected_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { struct { esp_ip4_addr_t ip; } ip_info; } ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)((ip)->addr & 255), (unsigned)(((ip)->addr >> 8) & 255), (unsigned)(((ip)->addr >> 16) & 255), (unsigned)(((ip)->addr >> 24) & 255)
esp_err_t nvs_flash_init(void);
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(void *netif);
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_get_config(int iface, wifi_config_t *config);
esp_err_t esp_wifi_set_config(int iface, wifi_config_t *config);
esp_err_t esp_wifi_set_ps(int ps);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg, bool blocking);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*callback)(void *, esp_event_base_t, int32_t, void *), void *arg,
    esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance);

struct test_ble_hs_cfg {
    void (*reset_cb)(int); void (*sync_cb)(void); void (*gatts_register_cb)(void);
};
extern struct test_ble_hs_cfg ble_hs_cfg;
int ble_svc_gap_device_name_set(const char *name);
esp_err_t nimble_port_init(void);
esp_err_t nimble_port_deinit(void);
int nimble_port_stop(void);
void nimble_port_run(void);
int os_msys_num_free(void);

typedef enum {
    ESP_BLUFI_EVENT_INIT_FINISH, ESP_BLUFI_EVENT_DEINIT_FINISH,
    ESP_BLUFI_EVENT_BLE_CONNECT, ESP_BLUFI_EVENT_BLE_DISCONNECT,
    ESP_BLUFI_EVENT_RECV_STA_SSID, ESP_BLUFI_EVENT_RECV_STA_PASSWD,
    ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP, ESP_BLUFI_EVENT_GET_WIFI_STATUS,
    ESP_BLUFI_EVENT_GET_WIFI_LIST, ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE,
    ESP_BLUFI_EVENT_SET_WIFI_OPMODE,
} esp_blufi_cb_event_t;
typedef struct {
    struct { int state; } init_finish;
    struct { int op_mode; } wifi_mode;
    struct { uint8_t *ssid; int ssid_len; } sta_ssid;
    struct { uint8_t *passwd; int passwd_len; } sta_passwd;
} esp_blufi_cb_param_t;
typedef struct {
    uint8_t *sta_ssid; int sta_ssid_len; bool sta_max_conn_retry_set;
    uint8_t sta_max_conn_retry; bool sta_conn_end_reason_set; uint8_t sta_conn_end_reason;
} esp_blufi_extra_info_t;
typedef struct { uint8_t ssid[33]; int rssi; } esp_blufi_ap_record_t;
typedef int esp_blufi_sta_conn_state_t;
#define ESP_BLUFI_STA_CONN_SUCCESS 0
#define ESP_BLUFI_STA_CONNECTING 1
#define ESP_BLUFI_STA_CONN_FAIL 2
#define ESP_BLUFI_INIT_OK 0
#define ESP_BLUFI_WIFI_SCAN_FAIL 1
#define ESP_BLUFI_DATA_FORMAT_ERROR 2
#define ESP_BLUFI_INIT_SECURITY_ERROR 3
typedef struct {
    void (*event_cb)(esp_blufi_cb_event_t, esp_blufi_cb_param_t *);
    void (*negotiate_data_handler)(uint8_t *, int, uint8_t **, int *, bool *);
    int (*encrypt_func)(uint8_t, uint8_t *, int);
    int (*decrypt_func)(uint8_t, uint8_t *, int);
    uint16_t (*checksum_func)(uint8_t, uint8_t *, int);
} esp_blufi_callbacks_t;
esp_err_t esp_blufi_register_callbacks(esp_blufi_callbacks_t *callbacks);
esp_err_t esp_blufi_profile_init(void);
esp_err_t esp_blufi_profile_deinit(void);
void esp_blufi_gatt_svr_register_cb(void);
int esp_blufi_gatt_svr_init(void);
void esp_blufi_gatt_svr_deinit(void);
void esp_blufi_btc_init(void);
void esp_blufi_btc_deinit(void);
void esp_blufi_adv_stop(void);
void esp_blufi_adv_start_with_name(const char *name);
void esp_blufi_disconnect(void);
void esp_blufi_send_error_info(int code);
void esp_blufi_send_wifi_conn_report(int mode, int state, int count, esp_blufi_extra_info_t *info);
void esp_blufi_send_wifi_list(unsigned count, esp_blufi_ap_record_t *records);
