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
static unsigned inits, deinits, netif_creates, netif_destroys, flash_writes;
static unsigned conn_reports, nvs_opens, nvs_commits, legacy_reads;
static unsigned nvs_initializations;
static int last_conn_report;
static unsigned stop_wait_ms, reported_errors;
static int last_report_code;
static bool wifi_running, security_ready, force_ble_stop_timeout;
static esp_err_t next_wifi_stop_error, next_wifi_start_error;
static esp_err_t next_nvs_init_error, next_wifi_init_error, next_wifi_deinit_error;
static esp_err_t next_wifi_mode_error, next_unregister_error;
static esp_err_t next_nvs_open_error, next_nvs_set_error, next_nvs_commit_error;
static esp_err_t next_nvs_read_error;
static bool cancel_on_nvs_open, cancel_on_nvs_write, suspend_on_wifi_init, write_before_set_error;
static bool fail_readback, corrupt_readback, nvs_opened, blob_present;
static uint8_t durable_blob[CREDENTIAL_RECORD_SIZE + 1];
static size_t durable_size;
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
static esp_err_t take_error(esp_err_t *next)
{ esp_err_t result = *next; *next = ESP_OK; return result; }
esp_err_t nvs_flash_init(void) { ++nvs_initializations; return take_error(&next_nvs_init_error); }
OK0(esp_netif_init)
OK0(esp_event_loop_create_default)
esp_err_t esp_wifi_deinit(void) { ++deinits; return take_error(&next_wifi_deinit_error); }
OK0(esp_wifi_clear_ap_list)
OK0(esp_wifi_scan_stop)
esp_netif_t *esp_netif_create_default_wifi_sta(void) { ++netif_creates; return &fake_netif; }
void esp_netif_destroy_default_wifi(void *netif) { assert(netif == &fake_netif); ++netif_destroys; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config; ++inits; ram_config = flash_config;
    if (suspend_on_wifi_init) { suspend_on_wifi_init = false; reset_wifi_suspend(); }
    return take_error(&next_wifi_init_error);
}
esp_err_t esp_wifi_set_storage(int value) { storage = value; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int value) { assert(value == WIFI_MODE_STA); return take_error(&next_wifi_mode_error); }
esp_err_t esp_wifi_set_ps(int value) { (void)value; return ESP_OK; }
esp_err_t esp_wifi_get_config(int iface, wifi_config_t *config)
{ (void)iface; ++legacy_reads; *config = flash_config; return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface, wifi_config_t *config)
{
    (void)iface;
    /* Real C3 SDK trap: identical RAM config is accepted without a flash write. */
    if (memcmp(&ram_config, config, sizeof(ram_config)) == 0) return ESP_OK;
    ram_config = *config;
    if (storage == WIFI_STORAGE_FLASH) { flash_config = *config; flash_writes++; }
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
{ (void)base; (void)id; (void)instance; return take_error(&next_unregister_error); }
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
void esp_blufi_send_error_info(int code) { ++reported_errors; last_report_code = code; }
void esp_blufi_send_wifi_conn_report(int mode, int state, int count, esp_blufi_extra_info_t *info)
{ (void)mode; (void)count; (void)info; ++conn_reports; last_conn_report = state; }
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

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, CREDENTIAL_NAMESPACE) == 0); assert(!nvs_opened); ++nvs_opens;
    if (next_nvs_open_error) return take_error(&next_nvs_open_error);
    if (mode == NVS_READONLY && !blob_present) return ESP_ERR_NVS_NOT_FOUND;
    nvs_opened = true; *handle = 1;
    if (mode == NVS_READWRITE && cancel_on_nvs_open) {
        cancel_on_nvs_open = false; assert(reset_wifi_set_enabled(false) == ESP_OK);
    }
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 1 && nvs_opened && strcmp(key, CREDENTIAL_KEY) == 0);
    if (next_nvs_read_error) return take_error(&next_nvs_read_error);
    if (!blob_present) return ESP_ERR_NVS_NOT_FOUND;
    if (!value) { *size = durable_size; return ESP_OK; }
    if (*size < durable_size) { *size = durable_size; return ESP_ERR_INVALID_SIZE; }
    memcpy(value, durable_blob, durable_size); *size = durable_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 1 && nvs_opened && strcmp(key, CREDENTIAL_KEY) == 0);
    assert(size == CREDENTIAL_RECORD_SIZE); ++persisted;
    esp_err_t result = take_error(&next_nvs_set_error);
    if (result == ESP_OK || write_before_set_error) {
        memcpy(durable_blob, value, size); durable_size = size; blob_present = true;
    }
    write_before_set_error = false;
    if (cancel_on_nvs_write) {
        cancel_on_nvs_write = false; assert(reset_wifi_set_enabled(false) == ESP_OK);
    }
    return result;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && nvs_opened); ++nvs_commits;
    if (fail_readback) { next_nvs_read_error = ESP_FAIL; fail_readback = false; }
    if (corrupt_readback) { durable_blob[44] ^= 1; corrupt_readback = false; }
    /* IDF 5.5.3 writes in set_blob; commit is not a transaction/rollback point. */
    return take_error(&next_nvs_commit_error);
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 && nvs_opened); nvs_opened = false; }

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

/* A cold boot discards application/driver RAM, queues and BLE ownership while
 * retaining both independent durable stores. No hidden rollback on reset. */
static void boot(bool enabled)
{
    assert(!nvs_opened);
    s_commands = NULL;
    s_host_stopped = s_profile_stopped = s_nimble_stop_done = NULL;
    memset(&queue, 0, sizeof(queue)); memset(sems, 0, sizeof(sems)); sem_count = 0;
    s_status = (reset_wifi_status_t) { .state = RESET_WIFI_STARTING };
    s_public_status = s_status;
    atomic_store(&s_control, 0); atomic_store(&s_enable_requests, 0);
    s_applied_control = s_public_control = s_init_retry_seen = 0;
    s_init_retry_safe = true; s_netif_ready = s_wifi_driver_initialized = false;
    s_netif = NULL; s_wifi_handler = s_ip_handler = NULL;
    memset(&s_saved, 0, sizeof(s_saved)); memset(&s_active, 0, sizeof(s_active));
    s_want_connect = s_in_flight = s_candidate = s_candidate_save_attempted = false;
    s_wifi_started = wifi_running = false;
    s_connect_due = s_reconnect_cooldown = s_attempt_deadline = 0;
    s_setup_deadline = s_success_stop_due = s_expected_disconnect_until = 0;
    s_host_initialized = s_host_running = s_gatt_initialized = s_btc_initialized = false;
    s_stop_pending = false; s_stop_result = ESP_OK;
    atomic_store(&s_setup_open, false); atomic_store(&s_ble_connected, false);
    atomic_store(&s_profile_initialized, false); atomic_store(&s_queue_overflow, false);
    clear_staging(); security_ready = false;
    assert(reset_wifi_init(enabled) == ESP_OK);
    s_init_retry_seen = atomic_load(&s_enable_requests);
    esp_err_t err = wifi_initialize();
    if (err != ESP_OK) fail(err, s_init_retry_safe ? "Wi-Fi initialization failed" :
                            "Wi-Fi init cleanup failed; restart");
    control_step();
}

static void seed_application_credentials(void)
{
    saved_fixture();
    encode_credentials(&flash_config, durable_blob);
    durable_size = CREDENTIAL_RECORD_SIZE; blob_present = true;
}

static void submit_candidate(void)
{
    assert(reset_wifi_start_provisioning() == ESP_OK); drain();
    blufi_sync();
    esp_blufi_cb_param_t phone = { 0 };
    blufi_event(ESP_BLUFI_EVENT_BLE_CONNECT, &phone); drain();
    assert(accepting_credentials());
    uint8_t ssid[] = "test-candidate", password[] = "test-password-2";
    phone.sta_ssid.ssid = ssid; phone.sta_ssid.ssid_len = 14;
    blufi_event(ESP_BLUFI_EVENT_RECV_STA_SSID, &phone);
    phone.sta_passwd.passwd = password; phone.sta_passwd.passwd_len = 15;
    blufi_event(ESP_BLUFI_EVENT_RECV_STA_PASSWD, &phone);
    blufi_event(ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP, &phone); drain();
    advance(1001); assert(s_candidate && s_in_flight);
}

static void assert_private_diagnostic(void)
{
    assert(strstr(status().message, "test-saved") == NULL);
    assert(strstr(status().message, "test-candidate") == NULL);
    assert(strstr(status().message, "test-password") == NULL);
}

static void test_initialization_recovery(void)
{
    seed_application_credentials();
    next_nvs_init_error = ESP_FAIL; boot(true);
    unsigned init_before = nvs_initializations, starts_before = starts;
    assert(!status().initialized && status().last_error == ESP_FAIL && status().radio_stopped);
    advance(RECONNECT_COOLDOWN_MS);
    assert(nvs_initializations == init_before && starts == starts_before);
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(nvs_initializations == init_before); /* Resume is not a retry request. */
    assert(reset_wifi_set_enabled(false) == ESP_OK); control_step();
    assert(nvs_initializations == init_before && !status().control_pending);
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(status().initialized && nvs_initializations == init_before + 1);
    advance(1001); event(EVT_GOT_IP); assert(status().connected);

    /* Fully unwind a later initialization fault before admitting another ON. */
    unsigned destroyed_before = netif_destroys, deinit_before = deinits;
    next_wifi_mode_error = ESP_FAIL; boot(true);
    assert(!status().initialized && s_init_retry_safe && !s_netif);
    assert(!s_wifi_handler && !s_ip_handler && !s_wifi_driver_initialized);
    assert(netif_destroys == destroyed_before + 1 && deinits == deinit_before + 1);
    init_before = nvs_initializations;
    assert(reset_wifi_set_enabled(false) == ESP_OK);
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(status().initialized && nvs_initializations == init_before + 1);

    /* A failed retry consumes its request; later ticks cannot spin initialization. */
    next_nvs_init_error = ESP_FAIL; boot(true); init_before = nvs_initializations;
    assert(reset_wifi_set_enabled(false) == ESP_OK);
    next_nvs_init_error = ESP_FAIL;
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    for (unsigned i = 0; i < 10; ++i) advance(RECONNECT_COOLDOWN_MS);
    assert(nvs_initializations == init_before + 1 && !status().initialized);
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(nvs_initializations == init_before + 1); /* Idempotent ON. */

    /* ON during sleep is deferred. OFF during that hold cancels the retry. */
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    assert(reset_wifi_set_enabled(false) == ESP_OK);
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(nvs_initializations == init_before + 1);
    assert(reset_wifi_set_enabled(false) == ESP_OK); control_step();
    assert(reset_wifi_resume() == ESP_OK); control_step();
    assert(nvs_initializations == init_before + 1 && !status().enabled);
    starts_before = starts;
    suspend_on_wifi_init = true;
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step(); control_step();
    assert(status().initialized && status().suspended && status().radio_stopped);
    assert(starts == starts_before); /* Newer suspend wins during re-init. */

    /* An incomplete unwind is never declared safe or retried by toggles. */
    next_wifi_mode_error = ESP_FAIL; next_unregister_error = ESP_FAIL; boot(true);
    assert(!status().initialized && !s_init_retry_safe && !status().radio_stopped);
    init_before = nvs_initializations;
    assert(reset_wifi_set_enabled(false) == ESP_OK); control_step();
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step();
    assert(nvs_initializations == init_before);
    next_wifi_init_error = ESP_FAIL; next_wifi_deinit_error = ESP_ERR_WIFI_NOT_INIT;
    boot(true);
    assert(!s_init_retry_safe && !status().radio_stopped); /* Partial driver ownership unknown. */
}

static void test_persistence_boot_paths(void)
{
    saved_fixture(); blob_present = false;
    unsigned reads_before = legacy_reads, writes_before = persisted, starts_before = starts;
    boot(false);
    assert(status().has_credentials && !status().enabled && status().radio_stopped);
    assert(legacy_reads == reads_before + 1 && persisted == writes_before && starts == starts_before);
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step(); advance(1001);
    event(EVT_GOT_IP); assert(status().connected && !status().persistence_error);
    submit_candidate();
    /* Same RAM config with FLASH selected is the old SDK persistence trap. */
    wifi_config_t legacy = flash_config;
    assert(esp_wifi_set_storage(WIFI_STORAGE_FLASH) == ESP_OK);
    assert(esp_wifi_set_config(WIFI_IF_STA, &s_active) == ESP_OK);
    assert(memcmp(&flash_config, &legacy, sizeof(legacy)) == 0);
    assert(esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK);
    event(EVT_GOT_IP);
    assert(persisted == writes_before + 1 && !status().persistence_error);
    assert(storage == WIFI_STORAGE_RAM && !flash_writes);
    assert(memcmp(s_saved.sta.ssid, "test-candidate", 14) == 0);
    reads_before = legacy_reads; starts_before = starts;
    boot(false);
    assert(status().has_credentials && !status().connected && starts == starts_before);
    assert(legacy_reads == reads_before && persisted == writes_before + 1);
    assert(reset_wifi_set_enabled(true) == ESP_OK); control_step(); advance(1001); event(EVT_GOT_IP);
    assert(status().connected && memcmp(s_active.sta.ssid, "test-candidate", 14) == 0);
    assert(reset_wifi_suspend() == ESP_OK); control_step();
    assert(reset_wifi_resume() == ESP_OK); control_step(); advance(1001); event(EVT_GOT_IP);
    assert(status().connected && persisted == writes_before + 1);
    boot(true); advance(1001); event(EVT_GOT_IP);
    assert(status().connected && legacy_reads == reads_before && persisted == writes_before + 1);

    /* Explicit clear writes a verified tombstone, leaving legacy NVS untouched. */
    assert(reset_wifi_set_enabled(false) == ESP_OK); control_step(); starts_before = starts;
    assert(reset_wifi_clear_credentials() == ESP_OK); drain();
    assert(!status().has_credentials && !status().persistence_error && starts == starts_before);
    assert(memcmp(&flash_config, &legacy, sizeof(legacy)) == 0);
    boot(false); assert(!status().has_credentials && !status().persistence_error);
    boot(true); advance(RECONNECT_COOLDOWN_MS);
    assert(!status().has_credentials && !s_want_connect && !status().connected);
    assert(legacy_reads == reads_before);

    /* Malformed app records and I/O errors never resurrect legacy credentials. */
    for (unsigned fault = 0; fault < 6; ++fault) {
        seed_application_credentials();
        if (fault == 0) durable_size--;
        if (fault == 1) { record_put_u32(durable_blob, 2); record_put_u32(durable_blob + 104, record_crc(durable_blob, 104)); }
        if (fault == 2) durable_blob[44] ^= 1;
        if (fault == 3) next_nvs_open_error = ESP_FAIL;
        if (fault == 4) next_nvs_read_error = ESP_FAIL;
        if (fault == 5) { durable_blob[8] = 0; record_put_u32(durable_blob + 104, record_crc(durable_blob, 104)); }
        reads_before = legacy_reads; writes_before = persisted;
        boot(true); advance(1001);
        assert(status().initialized && !status().has_credentials && status().persistence_error);
        assert(legacy_reads == reads_before && persisted == writes_before && !s_want_connect);
        assert_private_diagnostic();
    }
    /* A corrupt record remains recoverable through confirmed local clear. */
    assert(reset_wifi_clear_credentials() == ESP_OK); drain();
    assert(!status().persistence_error); boot(true);
    assert(!status().has_credentials && !status().persistence_error);
}

static void test_failed_candidate_saves(void)
{
    for (unsigned fault = 0; fault < 5; ++fault) {
        seed_application_credentials(); boot(true);
        wifi_config_t old = s_saved;
        submit_candidate();
        unsigned writes_before = persisted, reports_before = conn_reports;
        if (fault == 0) next_nvs_set_error = ESP_FAIL;
        if (fault == 1) next_nvs_commit_error = ESP_FAIL;
        if (fault == 2) fail_readback = true;
        if (fault == 3) { next_nvs_set_error = ESP_FAIL; write_before_set_error = true; }
        if (fault == 4) corrupt_readback = true;
        event(EVT_GOT_IP);
        assert(status().connected && status().has_credentials && status().persistence_error);
        assert(memcmp(&s_saved, &old, sizeof(old)) == 0 && persisted == writes_before + 1);
        assert(atomic_load(&s_setup_open) && !s_success_stop_due && s_candidate);
        assert(conn_reports == reports_before); /* No automatic provisioning success. */
        assert_private_diagnostic();
        event(EVT_GOT_IP); advance(SUCCESS_REPORT_GRACE_MS + 1);
        assert(status().persistence_error && atomic_load(&s_setup_open));
        assert(persisted == writes_before + 1 && conn_reports == reports_before);
        event(EVT_REPORT);
        assert(conn_reports == reports_before + 1 && last_conn_report == ESP_BLUFI_STA_CONN_SUCCESS);
        wifi_config_t observed = { 0 };
        esp_err_t read_result = load_credentials(&observed);
        if (fault == 0) assert(read_result == ESP_OK && memcmp(&observed.sta, &old.sta, sizeof(old.sta)) == 0);
        else if (fault < 4) assert(read_result == ESP_OK && memcmp(observed.sta.ssid, "test-candidate", 14) == 0);
        else assert(read_result != ESP_OK);
        assert(reset_wifi_stop_provisioning() == ESP_OK); drain();
        assert(!atomic_load(&s_setup_open) && !s_candidate && status().persistence_error);
        assert(memcmp(s_active.sta.ssid, old.sta.ssid, 32) == 0);
        advance(1001); event(EVT_GOT_IP);
        assert(status().connected && status().persistence_error && persisted == writes_before + 1);
        assert(reset_wifi_suspend() == ESP_OK); control_step();
        assert(reset_wifi_resume() == ESP_OK); control_step();
        assert(status().persistence_error && memcmp(s_active.sta.ssid, old.sta.ssid, 32) == 0);
        /* Boot sees actual durable bytes; do not assert a fictitious rollback. */
        boot(true);
        if (fault == 0) assert(memcmp(s_saved.sta.ssid, old.sta.ssid, 32) == 0);
        else if (fault < 4) assert(memcmp(s_saved.sta.ssid, "test-candidate", 14) == 0);
        else assert(!status().has_credentials && status().persistence_error);
    }

    /* Failed save with no old credentials must never set has_credentials. */
    memset(&flash_config, 0, sizeof(flash_config)); blob_present = false; boot(true);
    submit_candidate(); next_nvs_set_error = ESP_FAIL; event(EVT_GOT_IP);
    assert(status().connected && !status().has_credentials && status().persistence_error);
    advance(RESET_WIFI_PROVISIONING_SECONDS * 1000);
    assert(!atomic_load(&s_setup_open) && !status().connected && status().persistence_error);
    submit_candidate(); event(EVT_GOT_IP);
    assert(status().connected && status().has_credentials && !status().persistence_error);
    advance(SUCCESS_REPORT_GRACE_MS + 1); assert(!atomic_load(&s_setup_open));
}

static void test_save_controls_and_clear_faults(void)
{
    seed_application_credentials(); boot(true); submit_candidate();
    unsigned writes_before = persisted;
    cancel_on_nvs_open = true; event(EVT_GOT_IP);
    assert(!status().enabled && status().radio_stopped && !status().persistence_error);
    assert(persisted == writes_before && memcmp(s_saved.sta.ssid, "test-saved", 10) == 0);

    /* Synchronous set_blob may finish after OFF intent. Preserve its true
     * uncertainty/confirmed result without reopening or reconnecting radios. */
    seed_application_credentials(); boot(true); submit_candidate();
    cancel_on_nvs_write = true; next_nvs_commit_error = ESP_FAIL; event(EVT_GOT_IP);
    assert(!status().enabled && status().radio_stopped && status().persistence_error == ESP_FAIL);
    assert(memcmp(s_saved.sta.ssid, "test-saved", 10) == 0);
    boot(true); assert(memcmp(s_saved.sta.ssid, "test-candidate", 14) == 0);
    seed_application_credentials(); boot(true); submit_candidate();
    cancel_on_nvs_write = true; event(EVT_GOT_IP);
    assert(!status().enabled && status().radio_stopped && !status().persistence_error);
    assert(memcmp(s_saved.sta.ssid, "test-candidate", 14) == 0);

    for (unsigned fault = 0; fault < 3; ++fault) {
        seed_application_credentials(); boot(false);
        wifi_config_t old = s_saved; unsigned starts_before = starts;
        if (fault == 0) next_nvs_set_error = ESP_FAIL;
        if (fault == 1) next_nvs_commit_error = ESP_FAIL;
        if (fault == 2) fail_readback = true;
        assert(reset_wifi_clear_credentials() == ESP_OK); drain();
        assert(status().has_credentials && status().persistence_error && starts == starts_before);
        assert(memcmp(&s_saved, &old, sizeof(old)) == 0);
        assert(strstr(status().message, "Could not confirm") != NULL);
        boot(false);
        if (fault == 0) assert(status().has_credentials);
        else assert(!status().has_credentials); /* Tombstone already durable. */
    }
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
    assert(!starts && !status().has_credentials && flash_config.sta.ssid[0]);
    wifi_config_t verified = { 0 };
    assert(load_credentials(&verified) == ESP_OK && !verified.sta.ssid[0]);
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
    unsigned reports_before = reported_errors;
    phone.report_error.state = ESP_BLUFI_DATA_FORMAT_ERROR;
    blufi_event(ESP_BLUFI_EVENT_REPORT_ERROR, &phone);
    assert(reported_errors == reports_before + 1 && last_report_code == ESP_BLUFI_DATA_FORMAT_ERROR);
    blufi_event(ESP_BLUFI_EVENT_REPORT_ERROR, NULL);
    assert(reported_errors == reports_before + 1);

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
    reports_before = reported_errors;
    blufi_event(ESP_BLUFI_EVENT_REPORT_ERROR, &phone);
    assert(reported_errors == reports_before); /* No sends after OFF/suspend intent. */

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
    assert(load_credentials(&verified) == ESP_OK);
    assert(memcmp(verified.sta.ssid, candidate_ssid, sizeof(candidate_ssid)) == 0);
    assert(memcmp(flash_config.sta.ssid, "test-saved", 10) == 0 && !flash_writes);
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
    test_initialization_recovery();
    test_persistence_boot_paths();
    test_failed_candidate_saves();
    test_save_controls_and_clear_faults();
    printf("reset_wifi recovery/persistence: PASS (%u init, %u blob writes, %u legacy reads)\n",
           nvs_initializations, persisted, legacy_reads);
    return 0;
}
