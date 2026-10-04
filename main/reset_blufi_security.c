// SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Unlicense OR CC0-1.0
// Adapted from FoloToy demo/blufi-provisioning at
// 9c039cc5127f22072afa83bedb7fa3d8efe635ad (ESP-IDF 5.5 BLUFI example).
// Preserve the companion app's legacy wire protocol, with bounded parsing,
// key readiness checks, serialized access, and non-optimizable key cleanup.
#include "reset_blufi_security.h"

#include "esp_blufi_api.h"
#include "esp_crc.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>
#include <string.h>

#define SEC_TYPE_DH_PARAM_LEN  0x00
#define SEC_TYPE_DH_PARAM_DATA 0x01
#define DH_KEY_LEN              128
#define DH_PARAM_LEN_MAX        1024
#define PSK_LEN                 16

typedef struct {
    uint8_t public_key[DH_KEY_LEN];
    uint8_t shared_key[DH_KEY_LEN];
    size_t shared_len;
    uint8_t psk[PSK_LEN];
    uint8_t *dh_param;
    int dh_param_len;
    mbedtls_dhm_context dhm;
    mbedtls_aes_context aes;
    bool active;
    bool ready;
} blufi_security_t;

/* Lifetime is the application. The mutex survives BLE shutdown; no callback can
 * race a freed lock or a freed public-key output buffer during teardown. */
static blufi_security_t s_security;
static SemaphoreHandle_t s_lock;
extern void btc_blufi_report_error(esp_blufi_error_state_t state);

static int random_bytes(void *state, unsigned char *output, size_t len)
{
    (void)state;
    esp_fill_random(output, len);
    return 0;
}

static void clear_locked(void)
{
    free(s_security.dh_param);
    if (s_security.active) {
        mbedtls_dhm_free(&s_security.dhm);
        mbedtls_aes_free(&s_security.aes);
    }
    mbedtls_platform_zeroize(&s_security, sizeof(s_security));
}

int reset_blufi_security_init(void)
{
    /* The worker creates this mutex before exposing the BLE service. */
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_locked();
    mbedtls_dhm_init(&s_security.dhm);
    mbedtls_aes_init(&s_security.aes);
    s_security.active = true;
    xSemaphoreGive(s_lock);
    return 0;
}

void reset_blufi_security_deinit(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_locked();
    xSemaphoreGive(s_lock);
}

bool reset_blufi_security_ready(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_security.active && s_security.ready;
    xSemaphoreGive(s_lock);
    return ready;
}

static void negotiate_locked(uint8_t *data, int len, uint8_t **output_data,
                             int *output_len, bool *need_free)
{
    s_security.ready = false;
    if (!data || len < 3) {
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }
    if (!s_security.active) {
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }
    if (data[0] == SEC_TYPE_DH_PARAM_LEN) {
        int param_len = (data[1] << 8) | data[2];
        if (len != 3 || param_len <= 0 || param_len > DH_PARAM_LEN_MAX) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        free(s_security.dh_param);
        s_security.dh_param = malloc(param_len);
        s_security.dh_param_len = s_security.dh_param ? param_len : 0;
        if (!s_security.dh_param) btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
        return;
    }
    if (data[0] != SEC_TYPE_DH_PARAM_DATA || !s_security.dh_param ||
        len != s_security.dh_param_len + 1) {
        btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }
    memcpy(s_security.dh_param, data + 1, s_security.dh_param_len);
    uint8_t *param = s_security.dh_param;
    uint8_t *end = param + s_security.dh_param_len;
    int rc = mbedtls_dhm_read_params(&s_security.dhm, &param, end);
    bool exact = param == end;
    free(s_security.dh_param);
    s_security.dh_param = NULL;
    s_security.dh_param_len = 0;
    if (rc != 0 || !exact) {
        btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
        return;
    }
    int dh_len = (int)mbedtls_dhm_get_len(&s_security.dhm);
    /* The supported companion protocol uses 1024-bit DH. Do not silently
     * negotiate a shorter, even weaker group. This is not a modern PAKE. */
    if (dh_len != DH_KEY_LEN) {
        btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }
    rc = mbedtls_dhm_make_public(&s_security.dhm, dh_len,
                                s_security.public_key, DH_KEY_LEN,
                                random_bytes, NULL);
    if (rc == 0) {
        rc = mbedtls_dhm_calc_secret(&s_security.dhm, s_security.shared_key,
                                    DH_KEY_LEN, &s_security.shared_len,
                                    random_bytes, NULL);
    }
    if (rc != 0) {
        btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }
    rc = mbedtls_md5(s_security.shared_key, s_security.shared_len, s_security.psk);
    if (rc == 0) rc = mbedtls_aes_setkey_enc(&s_security.aes, s_security.psk, PSK_LEN * 8);
    mbedtls_platform_zeroize(s_security.shared_key, sizeof(s_security.shared_key));
    mbedtls_platform_zeroize(s_security.psk, sizeof(s_security.psk));
    if (rc != 0) {
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }
    s_security.ready = true;
    *output_data = s_security.public_key;
    *output_len = dh_len;
    *need_free = false;
}

void reset_blufi_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free)
{
    if (!output_data || !output_len || !need_free) return;
    *output_data = NULL;
    *output_len = 0;
    *need_free = false;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    negotiate_locked(data, len, output_data, output_len, need_free);
    xSemaphoreGive(s_lock);
}

static int crypt(bool encrypt, uint8_t iv8, uint8_t *data, int len)
{
    if (!s_lock || !data || len < 0) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int result = -1;
    if (s_security.active && s_security.ready) {
        size_t offset = 0;
        uint8_t iv[16] = { iv8 };
        int mode = encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT;
        if (mbedtls_aes_crypt_cfb128(&s_security.aes, mode, len, &offset,
                                   iv, data, data) == 0) result = len;
    }
    xSemaphoreGive(s_lock);
    return result;
}

int reset_blufi_encrypt(uint8_t iv8, uint8_t *data, int len)
{
    return crypt(true, iv8, data, len);
}

int reset_blufi_decrypt(uint8_t iv8, uint8_t *data, int len)
{
    return crypt(false, iv8, data, len);
}

uint16_t reset_blufi_checksum(uint8_t iv8, uint8_t *data, int len)
{
    (void)iv8;
    return (!data || len < 0) ? 0 : esp_crc16_be(0, data, len);
}
