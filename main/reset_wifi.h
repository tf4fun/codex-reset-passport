#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RESET_WIFI_PROVISIONING_SECONDS 180U
#define RESET_WIFI_MAX_ATTEMPTS 5U
#define RESET_WIFI_BLE_NAME "BLUFI_FoloPassport"

typedef enum {
    RESET_WIFI_STARTING = 0,
    RESET_WIFI_IDLE,
    RESET_WIFI_CONNECTING,
    RESET_WIFI_CONNECTED,
    RESET_WIFI_ERROR,
} reset_wifi_state_t;

typedef struct {
    reset_wifi_state_t state;
    bool initialized;
    bool enabled;               /* User preference; owned/persisted by application. */
    bool suspended;             /* Temporary sleep hold, independent of enabled. */
    bool control_pending;       /* Worker has not acknowledged current controls. */
    bool radio_stopped;         /* ACK: Wi-Fi stopped and BLE fully deinitialized. */
    bool connected;             /* True only after a station IP has been obtained. */
    bool has_credentials;       /* Successfully persisted station configuration. */
    bool provisioning;          /* Explicit, time-limited local setup window. */
    bool phone_connected;
    uint8_t attempts;           /* At most 5 per cycle; saved network retries again after 5 min. */
    uint16_t provisioning_seconds_left;
    uint16_t disconnect_reason;
    esp_err_t last_error;
    char ip[16];
    char message[64];           /* Nonsecret UI hint; never contains SSID/password. */
} reset_wifi_status_t;

/* Application-lifetime service, sole owner of Wi-Fi/NimBLE. Call once from app
 * startup, before any other radio service. Initialization runs in its own task.
 * It preserves NVS on errors. Pass the saved user preference before initialization
 * so an OFF boot never starts Wi-Fi. An enabled boot connects the saved network.
 * No method accesses LVGL. Commands are nonblocking and safe in ordinary task/
 * button callback context (not in an ISR). ESP_OK means accepted; poll get_status
 * for completion or an asynchronous error. This module does not persist enabled. */
esp_err_t reset_wifi_init(bool enabled);

/* OFF preserves credentials and stops BLE, scans, retries, and the Wi-Fi driver.
 * ON starts/reconnects only when not suspended. Requests are idempotent. */
esp_err_t reset_wifi_set_enabled(bool enabled);

/* Application must pause/drain its network workers before suspending. Await
 * suspended && radio_stopped before deep sleep; teardown is bounded and errors
 * appear in last_error once control_pending is false. A failed stop is never
 * reported as radio_stopped; do not treat older pending errors as stop results.
 * Resume leaves the remembered enabled preference unchanged, and reconnects
 * only when enabled. BLE setup is never reopened automatically. */
esp_err_t reset_wifi_suspend(void);
esp_err_t reset_wifi_resume(void);
esp_err_t reset_wifi_connect_saved(void);

/* Requires a deliberate local user action. BLE is stopped on cancellation,
 * successful provisioning, or after 180 seconds, including with a phone linked.
 * BLUFI's legacy DH/AES/CRC exchange is preserved for the companion mini program.
 * IDF BLUFI does not require incoming ENC/CHECK flags after negotiation, so
 * this is not mandatory transport encryption, authenticated enrollment, or
 * protection against an active nearby attacker. Perform setup only in a trusted
 * physical setting. No BLE at boot. */
esp_err_t reset_wifi_start_provisioning(void);
esp_err_t reset_wifi_stop_provisioning(void);

/* The caller MUST obtain explicit on-device UI confirmation first. Clears only
 * saved station credentials, stops provisioning and disconnects Wi-Fi; never
 * erases the whole NVS partition. There is deliberately no remote erase command. */
esp_err_t reset_wifi_clear_credentials(void);

/* Thread-safe snapshot, safe before init. Does not return credentials or SSID. */
void reset_wifi_get_status(reset_wifi_status_t *out);
