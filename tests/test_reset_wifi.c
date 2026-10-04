#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "../main/reset_wifi.c"

/* Execute production command/tick/control paths with a deterministic platform.
 * These tests prove worker policy, not real radio timing or phone compatibility. */
struct test_sem { bool ready; };
struct test_queue { unsigned bytes, count, head, tail; command_t items[COMMAND_DEPTH]; };
static struct test_queue queue;
static struct test_sem sems[3];
static unsigned sem_count;
static int64_t clock_ms;
static int storage;
static unsigned starts, stops, connects, scans, persisted, adverts, ble_starts, ble_stops;
static unsigned stop_wait_ms;
static bool wifi_running, security_ready, force_ble_stop_timeout;
static esp_err_t next_wifi_stop_error, next_wifi_start_error;
static wifi_config_t flash_config, ram_config;
static esp_netif_t fake_netif;
const char *WIFI_EVENT = "wifi";
const char *IP_EVENT = "ip";
struct test_ble_hs_cfg ble_hs_cfg;

const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void esp_log_level_set(const char *tag, int level) { (void)tag; (void)level; }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 256 * 1024; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 128 * 1024; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
void mbedtls_platform_zeroize(void *buffer, size_t bytes) { memset(buffer, 0, bytes); }
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)priority; (void)task;
    if (entry == stop_host_task && !force_ble_stop_timeout) entry(arg);
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name,
                                  unsigned stack, void *arg, unsigned priority,
                                  TaskHandle_t *task, int core)
{
    (void)core;
    return xTaskCreate(entry, name, stack, arg, priority, task);
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { assert(sem_count < 3); return &sems[sem_count++]; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) { sem->ready = true; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    stop_wait_ms += ticks;
    if (!sem->ready) return pdFALSE;
    sem->ready = false;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) { (void)sem; }
QueueHandle_t xQueueCreate(unsigned count, unsigned bytes)
{
    assert(count == COMMAND_DEPTH); queue.bytes = bytes; return &queue;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    if (q->count == COMMAND_DEPTH) return pdFALSE;
    memcpy(&q->items[q->tail++ % COMMAND_DEPTH], item, q->bytes); q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    (void)ticks;
    if (!q->count) return pdFALSE;
    memcpy(item, &q->items[q->head++ % COMMAND_DEPTH], q->bytes); q->count--;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { (void)q; }
#define OK0(name) esp_err_t name(void) { return ESP_OK; }
OK0(nvs_flash_init)
OK0(esp_netif_init)
OK0(esp_event_loop_create_default)
OK0(esp_wifi_deinit)
OK0(esp_wifi_clear_ap_list)
OK0(esp_wifi_scan_stop)
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &fake_netif; }
void esp_netif_destroy_default_wifi(void *netif) { (void)netif; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int value) { storage = value; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int value) { assert(value == WIFI_MODE_STA); return ESP_OK; }
esp_err_t esp_wifi_set_ps(int value) { (void)value; return ESP_OK; }
esp_err_t esp_wifi_get_config(int iface, wifi_config_t *config)
{ (void)iface; *config = flash_config; return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface, wifi_config_t *config)
{
    (void)iface; ram_config = *config;
    if (storage == WIFI_STORAGE_FLASH) { flash_config = *config; persisted++; }
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    starts++;
    if (next_wifi_start_error) { esp_err_t result = next_wifi_start_error; next_wifi_start_error = 0; return result; }
    wifi_running = true; return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    stops++;
    if (next_wifi_stop_error) { esp_err_t result = next_wifi_stop_error; next_wifi_stop_error = 0; return result; }
    wifi_running = false; return ESP_OK;
}
esp_err_t esp_wifi_connect(void) { assert(wifi_running); connects++; return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return wifi_running ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    memset(ap, 0, sizeof(*ap)); memcpy(ap->ssid, ram_config.sta.ssid, 32); return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking)
{ (void)config; (void)blocking; assert(wifi_running); scans++; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{ (void)records; *count = 0; return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*callback)(void *, esp_event_base_t, int32_t, void *), void *arg,
    esp_event_handler_instance_t *instance)
{ (void)base; (void)id; (void)callback; (void)arg; *instance = (void *)1; return ESP_OK; }
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance)
{ (void)base; (void)id; (void)instance; return ESP_OK; }
int ble_svc_gap_device_name_set(const char *name) { (void)name; return 0; }
esp_err_t nimble_port_init(void) { ble_starts++; return ESP_OK; }
esp_err_t nimble_port_deinit(void) { ble_stops++; return ESP_OK; }
int nimble_port_stop(void) { xSemaphoreGive(s_host_stopped); return ESP_OK; }
void nimble_port_run(void) { }
int os_msys_num_free(void) { return 8; }
esp_err_t esp_blufi_register_callbacks(esp_blufi_callbacks_t *callbacks)
{ assert(callbacks == &s_callbacks); return ESP_OK; }
esp_err_t esp_blufi_profile_init(void) { atomic_store(&s_profile_initialized, true); return ESP_OK; }
esp_err_t esp_blufi_profile_deinit(void)
{ atomic_store(&s_profile_initialized, false); xSemaphoreGive(s_profile_stopped); return ESP_OK; }
void esp_blufi_gatt_svr_register_cb(void) { }
int esp_blufi_gatt_svr_init(void) { return 0; }
void esp_blufi_gatt_svr_deinit(void) { }
void esp_blufi_btc_init(void) { }
void esp_blufi_btc_deinit(void) { }
void esp_blufi_adv_stop(void) { }
void esp_blufi_adv_start_with_name(const char *name) { (void)name; adverts++; }
void esp_blufi_disconnect(void) { atomic_store(&s_ble_connected, false); }
void esp_blufi_send_error_info(int code) { (void)code; }
void esp_blufi_send_wifi_conn_report(int mode, int state, int count, esp_blufi_extra_info_t *info)
{ (void)mode; (void)state; (void)count; (void)info; }
void esp_blufi_send_wifi_list(unsigned count, esp_blufi_ap_record_t *records)
{ (void)count; (void)records; }
void reset_blufi_negotiate(uint8_t *data, int len, uint8_t **output, int *output_len, bool *need_free)
{ (void)data; (void)len; (void)output; (void)output_len; (void)need_free; }
int reset_blufi_encrypt(uint8_t iv, uint8_t *data, int len) { (void)iv; (void)data; return len; }
int reset_blufi_decrypt(uint8_t iv, uint8_t *data, int len) { (void)iv; (void)data; return len; }
uint16_t reset_blufi_checksum(uint8_t iv, uint8_t *data, int len)
{ (void)iv; (void)data; (void)len; return 0; }
int reset_blufi_security_init(void) { security_ready = true; return 0; }
void reset_blufi_security_deinit(void) { security_ready = false; }
bool reset_blufi_security_ready(void) { return security_ready; }

static reset_wifi_status_t status(void)
{ reset_wifi_status_t result; reset_wifi_get_status(&result); return result; }
static void control_step(void) { apply_controls(); tick(); }
static void drain(void)
{
    command_t command;
    while (xQueueReceive(s_commands, &command, 0)) {
        apply_controls(); handle_command(&command); apply_controls(); tick();
    }
    control_step();
}
static void event(command_type_t type)
{ assert(enqueue(type, 0) == ESP_OK); drain(); }
static void advance(int64_t ms) { clock_ms += ms; control_step(); }
static void saved_fixture(void)
{
    memset(&flash_config, 0, sizeof(flash_config));
    memcpy(flash_config.sta.ssid, "test-saved", 10);
    memcpy(flash_config.sta.password, "test-password", 13);
    s_saved = flash_config; s_status.has_credentials = true;
}

int main(void)
{
    saved_fixture();
    assert(reset_wifi_init(false) == ESP_OK);
    assert(wifi_initialize() == ESP_OK);
    control_step();
    assert(!starts && !ble_starts && !connects);
    assert(!status().enabled && status().radio_stopped && status().has_credentials);
    assert(reset_wifi_connect_saved() == ESP_ERR_INVALID_STATE);
    assert(reset_wifi_start_provisioning() == ESP_ERR_INVALID_STATE);

    /* Offline clear does not temporarily start the radio. */
    assert(reset_wifi_clear_credentials() == ESP_OK); drain();
    assert(!starts && !status().has_credentials && !flash_config.sta.ssid[0]);
    saved_fixture();

    /* Re-enable remains processable after the driver was stopped. */
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(starts == 1 && status().enabled && !status().radio_stopped);
    advance(1001); assert(connects == 1);
    event(EVT_GOT_IP); assert(status().connected);

    /* Queue saturation cannot drop an OFF request; stale IP/candidate/scan and
     * BLE events cannot reconnect or persist before or after its worker ACK. */
    s_candidate = true;
    s_request_ready = true; s_requested = s_saved;
    for (unsigned i = 0; i < COMMAND_DEPTH; i++) assert(enqueue(EVT_GOT_IP, 0) == ESP_OK);
    unsigned persisted_before = persisted;
    assert(reset_wifi_set_enabled(false) == ESP_OK);
    assert(!status().connected && !status().radio_stopped && status().control_pending);
    command_t late = { .type = EVT_GOT_IP, .control = atomic_load(&s_control) };
    handle_command(&late);
    assert(persisted == persisted_before);
    drain();
    assert(status().radio_stopped && !status().enabled && status().has_credentials && !status().control_pending);
    assert(!s_candidate && !s_request_ready && !s_requested.sta.ssid[0]);
    unsigned connect_before = connects, scan_before = scans, advert_before = adverts;
    event(EVT_CANDIDATE); event(EVT_DISCONNECTED); event(EVT_LOST_IP);
    event(EVT_SCAN); event(EVT_SCAN_DONE); event(EVT_BLE_READY); event(EVT_BLE_DISCONNECTED);
    s_reconnect_cooldown = clock_ms + 1;
    advance(RECONNECT_COOLDOWN_MS + ATTEMPT_TIMEOUT_MS);
    assert(connects == connect_before && scans == scan_before && adverts == advert_before);
    assert(persisted == persisted_before && !status().connected);

    /* Temporary sleep never overwrites the user OFF preference. */
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    assert(status().suspended && !status().enabled && status().radio_stopped);
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(!status().suspended && !status().enabled && status().radio_stopped && starts == 1);

    /* A live setup/candidate is discarded on suspension, including late IP. */
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(reset_wifi_start_provisioning() == ESP_OK); drain();
    assert(ble_starts == 1 && atomic_load(&s_setup_open));
    blufi_sync();
    esp_blufi_cb_param_t phone = { 0 }; blufi_event(ESP_BLUFI_EVENT_BLE_CONNECT, &phone); drain();
    assert(accepting_credentials());
    uint8_t candidate_ssid[] = "test-candidate", candidate_password[] = "test-password-2";
    phone.sta_ssid.ssid = candidate_ssid; phone.sta_ssid.ssid_len = 14;
    blufi_event(ESP_BLUFI_EVENT_RECV_STA_SSID, &phone);
    phone.sta_passwd.passwd = candidate_password; phone.sta_passwd.passwd_len = 15;
    blufi_event(ESP_BLUFI_EVENT_RECV_STA_PASSWD, &phone);
    blufi_event(ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP, &phone); drain();
    advance(1001); assert(s_candidate && s_in_flight);
    late.control = atomic_load(&s_control);
    assert(reset_wifi_suspend() == ESP_OK);
    assert(!accepting_credentials());
    handle_command(&late); assert(persisted == persisted_before);
    control_step();
    assert(status().enabled && status().suspended && status().radio_stopped);
    assert(!s_host_initialized && !s_host_running && !s_candidate && ble_stops == 1);
    assert(stop_wait_ms <= 5500);
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(status().enabled && !status().suspended && s_want_connect);
    assert(!atomic_load(&s_setup_open) && ble_starts == 1);
    assert(memcmp(s_active.sta.ssid, s_saved.sta.ssid, 32) == 0);

    /* Rapid OFF/ON still invalidates old work even when the final flags match. */
    command_t obsolete_setup = { .type = CMD_START_SETUP, .control = atomic_load(&s_control) };
    assert(reset_wifi_set_enabled(false) == ESP_OK);
    assert(reset_wifi_set_enabled(true) == ESP_OK);
    control_step(); handle_command(&obsolete_setup);
    assert(!atomic_load(&s_setup_open) && ble_starts == 1);

    /* Current-generation successful provisioning still saves exactly once. */
    assert(reset_wifi_start_provisioning() == ESP_OK); drain();
    blufi_sync(); blufi_event(ESP_BLUFI_EVENT_BLE_CONNECT, &phone); drain();
    phone.sta_ssid.ssid = candidate_ssid; phone.sta_ssid.ssid_len = 14;
    blufi_event(ESP_BLUFI_EVENT_RECV_STA_SSID, &phone);
    phone.sta_passwd.passwd = candidate_password; phone.sta_passwd.passwd_len = 15;
    blufi_event(ESP_BLUFI_EVENT_RECV_STA_PASSWD, &phone);
    blufi_event(ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP, &phone); drain();
    advance(1001); event(EVT_GOT_IP);
    assert(persisted == persisted_before + 1 && !s_candidate && status().connected);
    event(EVT_GOT_IP); assert(persisted == persisted_before + 1);
    assert(memcmp(flash_config.sta.ssid, candidate_ssid, sizeof(candidate_ssid)) == 0);
    advance(SUCCESS_REPORT_GRACE_MS + 1);
    assert(!s_host_initialized && !atomic_load(&s_setup_open));

    /* ON while suspended remains radio-off; resume then reconnects. */
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    unsigned starts_before = starts;
    assert(reset_wifi_set_enabled(false) == ESP_OK); control_step();
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(status().enabled && status().suspended && status().radio_stopped && starts == starts_before);
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(starts == starts_before + 1);

    /* A failed stop is not a sleep ACK; a subsequent control change retries. */
    next_wifi_stop_error = ESP_FAIL;
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    assert(!status().radio_stopped && status().last_error == ESP_FAIL && wifi_running);
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(!status().suspended && status().last_error == ESP_OK);
    assert(reset_wifi_start_provisioning() == ESP_OK); drain();
    force_ble_stop_timeout = true;
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    assert(!status().radio_stopped && status().last_error == ESP_ERR_TIMEOUT);
    assert(!wifi_running); /* Wi-Fi shutdown still attempted after BLE failure. */
    force_ble_stop_timeout = false;
    stop_host_task(NULL); /* Complete the existing, previously timed-out helper. */
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(status().last_error == ESP_OK && wifi_running);

    /* Failed start stays stopped with a useful error and no retry/cooldown. */
    assert(reset_wifi_set_enabled(false) == ESP_OK); control_step();
    next_wifi_start_error = ESP_FAIL;
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(status().radio_stopped && status().last_error == ESP_FAIL && !s_want_connect);
    advance(RECONNECT_COOLDOWN_MS); assert(!wifi_running);
    printf("reset_wifi production worker: PASS (%u starts, %u stops)\n", starts, stops);
    return 0;
}
