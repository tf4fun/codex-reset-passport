// Application-owned Wi-Fi and short-lived BLUFI provisioning service.
// BLUFI lifecycle adapted from FoloToy demo/blufi-provisioning commit
// 9c039cc5127f22072afa83bedb7fa3d8efe635ad; UI and state handling are independent.
#include "reset_wifi.h"
#include "reset_blufi_security.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "mbedtls/platform_util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

#define COMMAND_DEPTH 24
#define AP_LIST_COUNT 12
#define ATTEMPT_TIMEOUT_MS 20000
#define SUCCESS_REPORT_GRACE_MS 1500
#define RECONNECT_COOLDOWN_MS 300000
#define CONTROL_ENABLED 1U
#define CONTROL_SUSPENDED 2U
#define CONTROL_FLAGS 3U
#define CONTROL_SEQUENCE 4U

static const char *TAG = "reset_wifi";

typedef enum {
    CMD_CONNECT_SAVED, CMD_START_SETUP, CMD_STOP_SETUP, CMD_CLEAR,
    EVT_DISCONNECTED, EVT_GOT_IP, EVT_LOST_IP, EVT_SCAN_DONE,
    EVT_BLE_READY, EVT_BLE_CONNECTED, EVT_BLE_DISCONNECTED,
    EVT_CANDIDATE, EVT_REPORT, EVT_SCAN, EVT_BLE_FAILED,
} command_type_t;

typedef struct {
    command_type_t type;
    uint32_t value;
    uint32_t control;           /* Discard work accepted before an OFF/sleep change. */
} command_t;

static QueueHandle_t s_commands;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static reset_wifi_status_t s_public_status = { .state = RESET_WIFI_STARTING };
static reset_wifi_status_t s_status = { .state = RESET_WIFI_STARTING };
/* A durable control mailbox cannot be starved by a full callback event queue.
 * Low bits are desired flags; upper bits invalidate work from an older request. */
static atomic_uint s_control;
static uint32_t s_applied_control;
static uint32_t s_public_control;
/* All following connection fields are worker-owned. */
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static wifi_config_t s_saved;
static wifi_config_t s_active;
static bool s_want_connect;
static bool s_in_flight;
static bool s_candidate;
static bool s_wifi_started;
static int64_t s_connect_due;
static int64_t s_reconnect_cooldown;
static int64_t s_attempt_deadline;
static int64_t s_setup_deadline;
static int64_t s_success_stop_due;
static int64_t s_expected_disconnect_until;
static bool s_host_initialized;
static bool s_host_running;
static bool s_gatt_initialized;
static bool s_btc_initialized;
static SemaphoreHandle_t s_host_stopped;
static SemaphoreHandle_t s_profile_stopped;
static SemaphoreHandle_t s_nimble_stop_done;
static bool s_stop_pending;
static esp_err_t s_stop_result;
/* Accessed by callbacks and worker; never put credentials in the event queue. */
static atomic_bool s_setup_open;
static atomic_bool s_ble_connected;
static atomic_bool s_profile_initialized;
static atomic_bool s_queue_overflow;
static wifi_config_t s_staged;
static wifi_config_t s_requested;
static bool s_stage_ssid;
static bool s_stage_password;
static bool s_request_ready;

static bool radio_requested(void)
{
    return (atomic_load(&s_control) & CONTROL_FLAGS) == CONTROL_ENABLED;
}

static bool radio_allowed(void)
{
    return radio_requested() && s_applied_control == atomic_load(&s_control) &&
           s_status.initialized && s_wifi_started;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void hint(const char *message)
{
    snprintf(s_status.message, sizeof(s_status.message), "%s", message);
}

static void fail(esp_err_t err, const char *message)
{
    s_status.last_error = err;
    if (!s_status.connected) s_status.state = RESET_WIFI_ERROR;
    hint(message);
    /* Error codes only. Never log credentials, SSID, keys, or incoming payloads. */
    ESP_LOGW(TAG, "%s (%s)", message, esp_err_to_name(err));
}

static void publish(void)
{
    s_status.enabled = (s_applied_control & CONTROL_ENABLED) != 0;
    s_status.suspended = (s_applied_control & CONTROL_SUSPENDED) != 0;
    s_status.provisioning = atomic_load(&s_setup_open);
    s_status.phone_connected = atomic_load(&s_ble_connected);
    int64_t remaining = s_setup_deadline - now_ms();
    s_status.provisioning_seconds_left = s_status.provisioning && remaining > 0
        ? (uint16_t)((remaining + 999) / 1000) : 0;
    portENTER_CRITICAL(&s_mux);
    s_public_status = s_status;
    s_public_control = s_applied_control;
    portEXIT_CRITICAL(&s_mux);
}

void reset_wifi_get_status(reset_wifi_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_public_status;
    uint32_t requested = atomic_load(&s_control);
    out->enabled = (requested & CONTROL_ENABLED) != 0;
    out->suspended = (requested & CONTROL_SUSPENDED) != 0;
    out->control_pending = requested != s_public_control;
    if (out->control_pending) out->radio_stopped = false;
    if (!out->enabled || out->suspended) out->connected = false;
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t enqueue(command_type_t type, uint32_t value)
{
    if (!s_commands) return ESP_ERR_INVALID_STATE;
    command_t command = { .type = type, .value = value,
                          .control = atomic_load(&s_control) };
    return xQueueSend(s_commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void post_event(command_type_t type, uint32_t value)
{
    if (enqueue(type, value) != ESP_OK) atomic_store(&s_queue_overflow, true);
}

static esp_err_t request_control(uint32_t mask, bool value)
{
    if (!s_commands) return ESP_ERR_INVALID_STATE;
    unsigned old = atomic_load(&s_control);
    unsigned next;
    do {
        next = value ? old | mask : old & ~mask;
        if (next == old) return ESP_OK;
        next += CONTROL_SEQUENCE;
    } while (!atomic_compare_exchange_weak(&s_control, &old, next));
    /* Close the credential ingress synchronously, before worker teardown. */
    if ((next & CONTROL_FLAGS) != CONTROL_ENABLED) atomic_store(&s_setup_open, false);
    return ESP_OK;
}

esp_err_t reset_wifi_set_enabled(bool enabled) { return request_control(CONTROL_ENABLED, enabled); }
esp_err_t reset_wifi_suspend(void) { return request_control(CONTROL_SUSPENDED, true); }
esp_err_t reset_wifi_resume(void) { return request_control(CONTROL_SUSPENDED, false); }
esp_err_t reset_wifi_connect_saved(void)
{
    return radio_requested() ? enqueue(CMD_CONNECT_SAVED, 0) : ESP_ERR_INVALID_STATE;
}
esp_err_t reset_wifi_start_provisioning(void)
{
    return radio_requested() ? enqueue(CMD_START_SETUP, 0) : ESP_ERR_INVALID_STATE;
}
esp_err_t reset_wifi_stop_provisioning(void) { return enqueue(CMD_STOP_SETUP, 0); }
esp_err_t reset_wifi_clear_credentials(void) { return enqueue(CMD_CLEAR, 0); }

static void clear_staging(void)
{
    portENTER_CRITICAL(&s_mux);
    mbedtls_platform_zeroize(&s_staged, sizeof(s_staged));
    mbedtls_platform_zeroize(&s_requested, sizeof(s_requested));
    s_stage_ssid = false;
    s_stage_password = false;
    s_request_ready = false;
    portEXIT_CRITICAL(&s_mux);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        post_event(EVT_DISCONNECTED, event->reason);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        post_event(EVT_SCAN_DONE, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        post_event(EVT_GOT_IP, event->ip_info.ip.addr);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        post_event(EVT_LOST_IP, 0);
    }
}

static void send_report(void)
{
    if (!radio_allowed() || !atomic_load(&s_setup_open) || !atomic_load(&s_ble_connected)) return;
    esp_blufi_extra_info_t info = { 0 };
    info.sta_ssid = s_active.sta.ssid;
    info.sta_ssid_len = strnlen((const char *)s_active.sta.ssid, sizeof(s_active.sta.ssid));
    info.sta_max_conn_retry_set = true;
    info.sta_max_conn_retry = RESET_WIFI_MAX_ATTEMPTS;
    info.sta_conn_end_reason_set = s_status.disconnect_reason != 0;
    info.sta_conn_end_reason = s_status.disconnect_reason;
    esp_blufi_sta_conn_state_t state = s_status.connected ? ESP_BLUFI_STA_CONN_SUCCESS :
        (s_want_connect ? ESP_BLUFI_STA_CONNECTING : ESP_BLUFI_STA_CONN_FAIL);
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, state, 0, &info);
}

static void send_scan_results(void)
{
    if (!radio_allowed() || !atomic_load(&s_setup_open) || !atomic_load(&s_ble_connected)) {
        esp_wifi_clear_ap_list();
        return;
    }
    uint16_t count = AP_LIST_COUNT;
    wifi_ap_record_t records[AP_LIST_COUNT] = { 0 };
    esp_blufi_ap_record_t list[AP_LIST_COUNT] = { 0 };
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    for (uint16_t i = 0; i < count; i++) {
        memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
        list[i].rssi = records[i].rssi;
    }
    esp_blufi_send_wifi_list(count, list);
}

static void blufi_reset(int reason)
{
    (void)reason;
    post_event(EVT_BLE_FAILED, ESP_FAIL);
}

static void blufi_sync(void)
{
    if (!radio_requested() || !atomic_load(&s_setup_open)) return;
    esp_err_t err = esp_blufi_profile_init();
    if (err != ESP_OK) post_event(EVT_BLE_FAILED, err);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    xSemaphoreGive(s_host_stopped);
    vTaskDelete(NULL);
}

static void stop_host_task(void *arg)
{
    (void)arg;
    /* IDF 5.5.3 port_stop internally waits without a timeout. Keep it out of
     * the application worker so a broken host cannot freeze network recovery. */
    s_stop_result = nimble_port_stop();
    xSemaphoreGive(s_nimble_stop_done);
    vTaskDelete(NULL);
}

static bool accepting_credentials(void)
{
    return radio_requested() && atomic_load(&s_setup_open) && atomic_load(&s_ble_connected) &&
           reset_blufi_security_ready();
}

static bool valid_password(const uint8_t *password, int length)
{
    if (!password || length < 8 || length > 64) return false;
    for (int i = 0; i < length; i++) {
        if (length == 64) {
            if (!((password[i] >= '0' && password[i] <= '9') ||
                  (password[i] >= 'a' && password[i] <= 'f') ||
                  (password[i] >= 'A' && password[i] <= 'F'))) return false;
        } else if (password[i] < 32 || password[i] > 126) {
            return false;
        }
    }
    return true;
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        if (param->init_finish.state == ESP_BLUFI_INIT_OK) {
            atomic_store(&s_profile_initialized, true);
            post_event(EVT_BLE_READY, 0);
        } else {
            post_event(EVT_BLE_FAILED, ESP_FAIL);
        }
        return;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        atomic_store(&s_profile_initialized, false);
        xSemaphoreGive(s_profile_stopped);
        return;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        if (!radio_requested() || !atomic_load(&s_setup_open)) {
            esp_blufi_disconnect();
            return;
        }
        esp_blufi_adv_stop();
        clear_staging();
        if (reset_blufi_security_init() != 0) {
            esp_blufi_disconnect();
            post_event(EVT_BLE_FAILED, ESP_ERR_NO_MEM);
            return;
        }
        atomic_store(&s_ble_connected, true);
        post_event(EVT_BLE_CONNECTED, 0);
        return;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        atomic_store(&s_ble_connected, false);
        reset_blufi_security_deinit();
        /* A submitted configuration remains available until the worker reads it. */
        portENTER_CRITICAL(&s_mux);
        mbedtls_platform_zeroize(&s_staged, sizeof(s_staged));
        s_stage_ssid = false;
        s_stage_password = false;
        portEXIT_CRITICAL(&s_mux);
        post_event(EVT_BLE_DISCONNECTED, 0);
        return;
    default:
        break;
    }
    if (!radio_requested() || !atomic_load(&s_setup_open)) return;
    switch (event) {
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (!accepting_credentials() || !param->sta_ssid.ssid ||
            param->sta_ssid.ssid_len < 1 || param->sta_ssid.ssid_len > 32 ||
            memchr(param->sta_ssid.ssid, 0, param->sta_ssid.ssid_len)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        portENTER_CRITICAL(&s_mux);
        mbedtls_platform_zeroize(&s_staged, sizeof(s_staged));
        memcpy(s_staged.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        s_staged.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        s_staged.sta.pmf_cfg.capable = true;
        s_stage_ssid = true;
        s_stage_password = false;
        portEXIT_CRITICAL(&s_mux);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (!accepting_credentials() ||
            !valid_password(param->sta_passwd.passwd, param->sta_passwd.passwd_len)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        portENTER_CRITICAL(&s_mux);
        mbedtls_platform_zeroize(s_staged.sta.password, sizeof(s_staged.sta.password));
        memcpy(s_staged.sta.password, param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        s_stage_password = true;
        portEXIT_CRITICAL(&s_mux);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
        if (!accepting_credentials()) {
            esp_blufi_send_error_info(ESP_BLUFI_INIT_SECURITY_ERROR);
            break;
        }
        portENTER_CRITICAL(&s_mux);
        bool valid = s_stage_ssid && s_stage_password && !s_request_ready;
        if (valid) {
            s_requested = s_staged;
            s_request_ready = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (valid) post_event(EVT_CANDIDATE, 0);
        else esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        break;
    }
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        post_event(EVT_REPORT, 0);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        post_event(EVT_SCAN, 0);
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        if (param->wifi_mode.op_mode != WIFI_MODE_STA)
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        break;
    /* BSSID pinning, soft-AP configuration, remote disconnect/erase, and arbitrary
     * custom commands are deliberately unsupported. A local confirm owns erase. */
    default:
        break;
    }
}

static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = reset_blufi_negotiate,
    .encrypt_func = reset_blufi_encrypt,
    .decrypt_func = reset_blufi_decrypt,
    .checksum_func = reset_blufi_checksum,
};

static esp_err_t host_start(void)
{
    if (!radio_allowed()) return ESP_ERR_INVALID_STATE;
    if (s_host_initialized) return ESP_ERR_INVALID_STATE;
    /* Coarse headroom guard for C3 without PSRAM. The legacy SDK helpers still
     * contain internal allocation assertions; only device stress testing can
     * establish practical concurrent Wi-Fi/LVGL/BLE memory headroom. */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 64 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24 * 1024)
        return ESP_ERR_NO_MEM;
    if (reset_blufi_security_init() != 0) return ESP_ERR_NO_MEM;
    reset_blufi_security_deinit();
    xSemaphoreTake(s_host_stopped, 0);
    xSemaphoreTake(s_profile_stopped, 0);
    esp_err_t err = esp_blufi_register_callbacks(&s_callbacks);
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    s_host_initialized = true;
    ble_hs_cfg.reset_cb = blufi_reset;
    ble_hs_cfg.sync_cb = blufi_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    if (os_msys_num_free() < 4) return ESP_ERR_NO_MEM;
    if (esp_blufi_gatt_svr_init() != 0) return ESP_FAIL;
    s_gatt_initialized = true;
    if (ble_svc_gap_device_name_set(RESET_WIFI_BLE_NAME) != 0) return ESP_FAIL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8 * 1024)
        return ESP_ERR_NO_MEM;
    esp_blufi_btc_init();
    s_btc_initialized = true;
    /* esp_nimble_enable in this IDF version silently ignores task allocation
     * failure. Use the same stack/core/priority with a checked creation result. */
    if (xTaskCreatePinnedToCore(host_task, "reset_ble", NIMBLE_HS_STACK_SIZE,
                               NULL, configMAX_PRIORITIES - 4, NULL,
                               NIMBLE_CORE) != pdPASS) return ESP_ERR_NO_MEM;
    s_host_running = true;
    return ESP_OK;
}

static esp_err_t host_stop(void)
{
    /* Close acceptance first. Late callbacks cannot reopen advertising. */
    atomic_store(&s_setup_open, false);
    s_success_stop_due = 0;
    if (!s_host_initialized) {
        clear_staging();
        reset_blufi_security_deinit();
        return ESP_OK;
    }
    esp_blufi_adv_stop();
    if (atomic_load(&s_ble_connected)) esp_blufi_disconnect();
    if (s_host_running) {
        if (!s_stop_pending) {
            xSemaphoreTake(s_nimble_stop_done, 0);
            if (xTaskCreate(stop_host_task, "reset_ble_stop", 3072, NULL, 4, NULL) != pdPASS)
                return ESP_ERR_NO_MEM;
            s_stop_pending = true;
        }
        if (xSemaphoreTake(s_nimble_stop_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        s_stop_pending = false;
        if (s_stop_result != ESP_OK ||
            xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(500)) != pdTRUE) {
            /* Keep ownership/allocations intact if the host could still access
             * them. Do not pretend restart is safe after a failed shutdown. */
            return ESP_ERR_TIMEOUT;
        }
        s_host_running = false;
    }
    if (atomic_load(&s_profile_initialized)) {
        esp_err_t err = esp_blufi_profile_deinit();
        if (err != ESP_OK || xSemaphoreTake(s_profile_stopped, pdMS_TO_TICKS(2000)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
    }
    if (s_gatt_initialized) {
        esp_blufi_gatt_svr_deinit();
        s_gatt_initialized = false;
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) return err;
    if (s_btc_initialized) {
        esp_blufi_btc_deinit();
        s_btc_initialized = false;
    }
    s_host_initialized = false;
    atomic_store(&s_ble_connected, false);
    clear_staging();
    reset_blufi_security_deinit();
    return ESP_OK;
}

static esp_err_t wifi_initialize(void)
{
    esp_err_t err = nvs_flash_init();
    /* Never erase NVS to "repair" startup. Other application data may be there. */
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;
    /* The vendor driver has diagnostic format strings for network identifiers
     * and passwords. Keep its verbose logs disabled before initialization. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) goto fail_netif;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail_wifi;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail_wifi_handler;
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_get_config(WIFI_IF_STA, &s_saved);
    /* Do not persist partial or untested configurations. */
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        s_status.has_credentials = s_saved.sta.ssid[0] != 0;
        s_status.initialized = true;
        s_status.radio_stopped = true;
        s_status.state = RESET_WIFI_IDLE;
        hint("Wi-Fi ready; radio stopped");
        return ESP_OK;
    }
    esp_wifi_stop();
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler);
fail_wifi_handler:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
fail_wifi:
    esp_wifi_deinit();
fail_netif:
    esp_netif_destroy_default_wifi(s_netif);
    s_netif = NULL;
    return err;
}

static void connection_stop(void)
{
    s_want_connect = false;
    s_in_flight = false;
    s_connect_due = 0;
    s_reconnect_cooldown = 0;
    s_attempt_deadline = 0;
    s_status.connected = false;
    s_status.ip[0] = '\0';
    s_expected_disconnect_until = now_ms() + 1000;
    if (s_wifi_started) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        esp_wifi_disconnect();
    }
}

static void begin_connection(const wifi_config_t *config, bool candidate)
{
    if (!radio_allowed()) return;
    connection_stop();
    s_candidate = candidate;
    s_active = *config;
    s_active.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    s_active.sta.pmf_cfg.capable = true;
    s_status.attempts = 0;
    s_status.disconnect_reason = 0;
    s_status.last_error = ESP_OK;
    if (s_active.sta.ssid[0] == 0) {
        s_status.state = RESET_WIFI_IDLE;
        hint("Hold setup to add Wi-Fi");
        return;
    }
    /* Allow a previous station disconnect to finish before changing config. */
    s_connect_due = now_ms() + 1000;
    s_want_connect = true;
    s_status.state = RESET_WIFI_CONNECTING;
    hint(candidate ? "Checking new Wi-Fi" : "Connecting saved Wi-Fi");
}

static void retry_later(esp_err_t error)
{
    s_in_flight = false;
    s_attempt_deadline = 0;
    s_status.connected = false;
    s_status.ip[0] = '\0';
    if (!radio_allowed() || !s_want_connect) return;
    if (s_status.attempts >= RESET_WIFI_MAX_ATTEMPTS) {
        s_want_connect = false;
        s_connect_due = 0;
        if (!s_candidate && s_status.has_credentials) {
            s_reconnect_cooldown = now_ms() + RECONNECT_COOLDOWN_MS;
            fail(error, "Wi-Fi offline; retry in 5 min");
        } else {
            fail(error, "Wi-Fi unavailable; check setup");
        }
        send_report();
        return;
    }
    uint32_t exponent = s_status.attempts ? s_status.attempts - 1 : 0;
    uint32_t delay = 1000U << exponent;
    if (delay > 16000U) delay = 16000U;
    s_connect_due = now_ms() + delay;
    s_status.state = RESET_WIFI_CONNECTING;
    hint("Wi-Fi retry scheduled");
}

static esp_err_t persist_config(const wifi_config_t *config)
{
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *)config);
    esp_err_t ram_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    return err == ESP_OK ? ram_err : err;
}

static void end_setup(bool restore_saved)
{
    esp_err_t err = host_stop();
    s_setup_deadline = 0;
    if (err != ESP_OK) fail(err, "BLE stop failed; restart device");
    if (restore_saved && s_candidate && radio_allowed()) {
        s_candidate = false;
        begin_connection(&s_saved, false);
    } else if (err == ESP_OK && !s_status.connected && !s_want_connect) {
        hint("Setup closed; hold setup to retry");
    }
}

static esp_err_t radio_stop(void)
{
    /* Cancellation precedes potentially blocking teardown. Never persist a
     * candidate, reconnect, or reopen BLE after the application asks for OFF. */
    atomic_store(&s_setup_open, false);
    connection_stop();
    s_candidate = false;
    s_setup_deadline = 0;
    s_success_stop_due = 0;
    s_status.attempts = 0;
    clear_staging();
    mbedtls_platform_zeroize(&s_active, sizeof(s_active));
    esp_err_t ble_error = host_stop();
    esp_err_t wifi_error = ESP_OK;
    /* Still attempt Wi-Fi shutdown if BLE teardown failed. */
    if (s_wifi_started) {
        wifi_error = esp_wifi_stop();
        if (wifi_error == ESP_OK || wifi_error == ESP_ERR_WIFI_NOT_STARTED) {
            s_wifi_started = false;
            wifi_error = ESP_OK;
        }
    }
    s_status.radio_stopped = !s_wifi_started && !s_host_initialized &&
                             !s_host_running && !atomic_load(&s_profile_initialized);
    return ble_error != ESP_OK ? ble_error : wifi_error;
}

static void apply_controls(void)
{
    uint32_t requested = atomic_load(&s_control);
    if (requested == s_applied_control) return;
    s_applied_control = requested;
    if (!s_status.initialized) {
        /* Preserve the initialization failure for the application. */
        return;
    }
    esp_err_t err = radio_stop();
    if (err != ESP_OK) {
        fail(err, "Radio stop failed; restart device");
        return;
    }
    s_status.last_error = ESP_OK;
    s_status.state = RESET_WIFI_IDLE;
    if ((requested & CONTROL_FLAGS) != CONTROL_ENABLED) {
        hint((requested & CONTROL_SUSPENDED) ? "Radios stopped for sleep" : "Wi-Fi off");
        return;
    }
    /* A newer OFF/suspend request can arrive during bounded BLE teardown. */
    if (requested != atomic_load(&s_control)) return;
    err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started = true;
        s_status.radio_stopped = false;
        err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    if (err != ESP_OK) {
        radio_stop();
        fail(err, "Could not start Wi-Fi");
        return;
    }
    begin_connection(&s_saved, false);
}

static void handle_command(const command_t *command)
{
    /* Clear and local setup cancellation work even with the driver stopped.
     * Everything else requires the current enabled, awake control generation. */
    if (command->type != CMD_CLEAR && command->type != CMD_STOP_SETUP &&
        (!radio_allowed() || command->control != s_applied_control)) return;
    switch (command->type) {
    case CMD_CONNECT_SAVED:
        if (s_status.connected && !s_candidate) break;
        begin_connection(&s_saved, false);
        break;
    case CMD_START_SETUP:
        if (atomic_load(&s_setup_open)) break; /* No accidental indefinite extension. */
        if (s_host_initialized) {
            fail(ESP_ERR_INVALID_STATE, "BLE needs a device restart");
            break;
        }
        clear_staging();
        s_setup_deadline = now_ms() + RESET_WIFI_PROVISIONING_SECONDS * 1000;
        s_success_stop_due = 0;
        atomic_store(&s_setup_open, true);
        hint("Opening local Wi-Fi setup");
        {
            esp_err_t err = host_start();
            if (err != ESP_OK) {
                end_setup(false);
                fail(err, "Cannot start Bluetooth setup");
            }
        }
        break;
    case CMD_STOP_SETUP:
        end_setup(true);
        break;
    case CMD_CLEAR: {
        esp_err_t stop_error = host_stop();
        s_setup_deadline = 0;
        connection_stop();
        s_candidate = false;
        wifi_config_t empty = { 0 };
        esp_err_t err = persist_config(&empty);
        if (err != ESP_OK) {
            fail(err, "Could not clear saved Wi-Fi");
            break;
        }
        mbedtls_platform_zeroize(&s_saved, sizeof(s_saved));
        mbedtls_platform_zeroize(&s_active, sizeof(s_active));
        clear_staging();
        s_status.has_credentials = false;
        s_status.attempts = 0;
        s_status.last_error = ESP_OK;
        s_status.state = RESET_WIFI_IDLE;
        if (stop_error != ESP_OK) fail(stop_error, "Wi-Fi cleared; BLE stop failed");
        else hint("Saved Wi-Fi cleared");
        break;
    }
    case EVT_BLE_READY:
    case EVT_BLE_DISCONNECTED:
        if (atomic_load(&s_setup_open)) {
            esp_blufi_adv_start_with_name(RESET_WIFI_BLE_NAME);
            hint("Setup open; connect your phone");
        }
        break;
    case EVT_BLE_CONNECTED:
        if (atomic_load(&s_setup_open)) hint("Phone connected; send Wi-Fi");
        break;
    case EVT_CANDIDATE: {
        wifi_config_t candidate = { 0 };
        portENTER_CRITICAL(&s_mux);
        bool ready = s_request_ready;
        if (ready) candidate = s_requested;
        mbedtls_platform_zeroize(&s_requested, sizeof(s_requested));
        s_request_ready = false;
        portEXIT_CRITICAL(&s_mux);
        if (ready && atomic_load(&s_setup_open)) begin_connection(&candidate, true);
        mbedtls_platform_zeroize(&candidate, sizeof(candidate));
        break;
    }
    case EVT_GOT_IP:
        if (!s_want_connect || (!s_in_flight && !s_status.connected)) break;
        /* A late IP event from a replaced configuration must never save the new
         * credentials as if they had been tested. Check the actual association. */
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
                memcmp(ap.ssid, s_active.sta.ssid, sizeof(s_active.sta.ssid)) != 0) break;
        }
        if (!radio_allowed()) break;
        s_in_flight = false;
        s_connect_due = 0;
        s_attempt_deadline = 0;
        s_status.connected = true;
        s_status.state = RESET_WIFI_CONNECTED;
        s_status.last_error = ESP_OK;
        s_status.disconnect_reason = 0;
        {
            esp_ip4_addr_t ip = { .addr = command->value };
            snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ip));
        }
        hint("Wi-Fi connected");
        if (s_candidate) {
            /* Recheck immediately before the only candidate persistence path.
             * OFF/suspend closes acceptance even before its worker turn. */
            if (!radio_allowed()) break;
            esp_err_t err = persist_config(&s_active);
            if (err == ESP_OK) {
                s_saved = s_active;
                s_status.has_credentials = true;
            } else {
                fail(err, "Connected, but saving Wi-Fi failed");
            }
            s_candidate = false;
            s_success_stop_due = now_ms() + SUCCESS_REPORT_GRACE_MS;
        }
        s_status.attempts = 0;
        send_report();
        break;
    case EVT_DISCONNECTED:
        s_status.disconnect_reason = command->value;
        if (!s_in_flight && !s_status.connected && now_ms() < s_expected_disconnect_until) break;
        retry_later(ESP_FAIL);
        break;
    case EVT_LOST_IP:
        if (s_status.connected) {
            esp_wifi_disconnect();
            retry_later(ESP_ERR_TIMEOUT);
        }
        break;
    case EVT_SCAN:
        if (atomic_load(&s_setup_open) && atomic_load(&s_ble_connected)) {
            wifi_scan_config_t config = { 0 };
            if (esp_wifi_scan_start(&config, false) != ESP_OK)
                esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    case EVT_SCAN_DONE:
        send_scan_results();
        break;
    case EVT_REPORT:
        send_report();
        break;
    case EVT_BLE_FAILED:
        end_setup(true);
        fail(command->value, "Bluetooth setup failed");
        break;
    }
}

static void tick(void)
{
    int64_t now = now_ms();
    if (atomic_exchange(&s_queue_overflow, false)) {
        if (radio_allowed()) {
            end_setup(true);
            fail(ESP_ERR_TIMEOUT, "Radio busy; retry local setup");
        }
    }
    if (!radio_allowed()) { publish(); return; }
    if (atomic_load(&s_setup_open) &&
        (now >= s_setup_deadline || (s_success_stop_due && now >= s_success_stop_due))) {
        end_setup(true);
    }
    /* Setup shutdown can wait for BLE; controls may have changed meanwhile. */
    if (!radio_allowed()) { publish(); return; }
    if (s_reconnect_cooldown && now >= s_reconnect_cooldown &&
        !atomic_load(&s_setup_open)) {
        begin_connection(&s_saved, false);
    }
    if (s_want_connect && s_in_flight && now >= s_attempt_deadline) {
        s_expected_disconnect_until = now + 1000;
        esp_wifi_disconnect();
        retry_later(ESP_ERR_TIMEOUT);
    }
    if (s_want_connect && !s_status.connected && !s_in_flight &&
        s_connect_due && now >= s_connect_due) {
        s_status.attempts++;
        s_connect_due = 0;
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_active);
        if (err == ESP_OK && radio_allowed()) err = esp_wifi_connect();
        else if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) {
            s_in_flight = true;
            s_attempt_deadline = now + ATTEMPT_TIMEOUT_MS;
            hint("Connecting Wi-Fi");
        } else {
            retry_later(err);
        }
    }
    publish();
}

static void worker(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_initialize();
    if (err != ESP_OK) fail(err, "Wi-Fi initialization failed");
    apply_controls();
    publish();
    for (;;) {
        command_t command;
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(200)) == pdTRUE) {
            apply_controls();
            if (s_status.initialized) handle_command(&command);
        }
        apply_controls();
        tick();
    }
}

esp_err_t reset_wifi_init(bool enabled)
{
    if (s_commands) return ESP_OK;
    s_commands = xQueueCreate(COMMAND_DEPTH, sizeof(command_t));
    s_host_stopped = xSemaphoreCreateBinary();
    s_profile_stopped = xSemaphoreCreateBinary();
    s_nimble_stop_done = xSemaphoreCreateBinary();
    if (!s_commands || !s_host_stopped || !s_profile_stopped || !s_nimble_stop_done) goto failed;
    atomic_store(&s_control, CONTROL_SEQUENCE | (enabled ? CONTROL_ENABLED : 0));
    if (xTaskCreate(worker, "reset_wifi", 6144, NULL, 4, NULL) != pdPASS) goto failed;
    return ESP_OK;
failed:
    if (s_commands) vQueueDelete(s_commands);
    if (s_host_stopped) vSemaphoreDelete(s_host_stopped);
    if (s_profile_stopped) vSemaphoreDelete(s_profile_stopped);
    if (s_nimble_stop_done) vSemaphoreDelete(s_nimble_stop_done);
    s_nimble_stop_done = NULL;
    s_commands = NULL;
    s_host_stopped = NULL;
    s_profile_stopped = NULL;
    return ESP_ERR_NO_MEM;
}
