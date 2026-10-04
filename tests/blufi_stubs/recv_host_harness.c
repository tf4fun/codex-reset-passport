/* Host harness for the exact patched ESP-IDF receiver function. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTC_TRACE_ERROR(...) ((void)0)
#define BLUFI_FC_IS_ENC(f) ((f) & 1)
#define BLUFI_FC_IS_CHECK(f) ((f) & 2)
#define BLUFI_FC_IS_REQ_ACK(f) ((f) & 8)
#define BLUFI_FC_IS_FRAG(f) ((f) & 16)
#define ESP_BLUFI_DATA_FORMAT_ERROR 1
#define ESP_BLUFI_SEQUENCE_ERROR 2
#define ESP_BLUFI_DECRYPT_ERROR 3
#define ESP_BLUFI_CHECKSUM_ERROR 4
#define ESP_BLUFI_MSG_STATE_ERROR 5
#define ESP_BLUFI_DH_MALLOC_ERROR 6

struct blufi_hdr {
    uint8_t type, fc, seq, data_len;
    uint8_t data[];
};
struct callbacks {
    int (*decrypt_func)(uint8_t, uint8_t *, int);
    uint16_t (*checksum_func)(uint8_t, uint8_t *, int);
};
static struct {
    uint8_t recv_seq;
    uint16_t offset, total_len;
    uint8_t *aggr_buf;
    struct callbacks *cbs;
} blufi_env;
static unsigned errors, allocations, deliveries;
static int delivered_len;
static uint8_t delivered[1025];
static void btc_blufi_report_error(int error) { (void)error; errors++; }
static void btc_blufi_send_ack(uint8_t seq) { (void)seq; }
static void *osi_malloc(size_t size) {
    assert(size > 0 && size <= 1025);
    allocations++;
    return malloc(size);
}
static void osi_free(void *data) { free(data); }
static void btc_blufi_protocol_handler(uint8_t type, uint8_t *data, int len) {
    (void)type;
    assert(len >= 0 && len <= 1025);
    memcpy(delivered, data, len);
    delivered_len = len;
    deliveries++;
}

/* The test runner inserts the hash-verified real patched receiver here. */
/* INSERT_PINNED_RECEIVER */

static void reset(void) {
    free(blufi_env.aggr_buf);
    memset(&blufi_env, 0, sizeof(blufi_env));
    memset(delivered, 0, sizeof(delivered));
    errors = allocations = deliveries = 0;
    delivered_len = -1;
}
static void frame(uint8_t flags, const uint8_t *payload, uint8_t length) {
    uint8_t bytes[261] = { 1, flags, blufi_env.recv_seq, length };
    if (length) memcpy(bytes + 4, payload, length);
    btc_blufi_recv_handler(bytes, length + 4);
}
static void fragmented(uint16_t remaining, const uint8_t *payload, uint8_t length) {
    uint8_t data[255] = { remaining & 0xff, remaining >> 8 };
    assert(length <= 253);
    if (length) memcpy(data + 2, payload, length);
    frame(16, data, length + 2);
}
int main(void) {
    const uint8_t abc[] = { 'a', 'b', 'c' };
    reset(); frame(16, NULL, 0);
    assert(errors == 1 && allocations == 0 && deliveries == 0);
    reset(); frame(16, abc, 1);
    assert(errors == 1 && allocations == 0 && deliveries == 0);
    reset(); fragmented(0, NULL, 0);
    assert(errors == 1 && allocations == 0);
    reset(); fragmented(1026, abc, 1);
    assert(errors == 1 && allocations == 0);
    reset(); fragmented(65535, abc, 1);
    assert(errors == 1 && allocations == 0);
    reset(); fragmented(1, abc, 2);
    assert(errors == 1 && allocations == 0);
    reset(); fragmented(4, abc, 2); fragmented(2, abc, 3);
    assert(errors == 1 && allocations == 1 && blufi_env.offset == 2 && deliveries == 0);
    reset(); fragmented(4, abc, 2); frame(0, abc, 3);
    assert(errors == 1 && deliveries == 0);
    reset(); fragmented(4, abc, 2); frame(0, abc, 1);
    assert(errors == 1 && deliveries == 0);
    reset(); fragmented(4, abc, 2); blufi_env.offset = 5; fragmented(1, abc, 1);
    assert(errors == 1 && deliveries == 0);
    reset(); fragmented(4, abc, 2); blufi_env.offset = 5; frame(0, abc, 1);
    assert(errors == 1 && deliveries == 0);
    reset(); fragmented(5, abc, 2); fragmented(3, abc + 2, 1); frame(0, abc, 2);
    assert(errors == 0 && allocations == 1 && deliveries == 1 && delivered_len == 5);
    assert(memcmp(delivered, "abcab", 5) == 0 && blufi_env.aggr_buf == NULL);
    reset(); frame(0, abc, 3);
    assert(errors == 0 && deliveries == 1 && delivered_len == 3);
    reset();
    uint8_t large[1025];
    for (unsigned i = 0; i < sizeof(large); i++) large[i] = i % 251;
    unsigned offset = 0;
    while (sizeof(large) - offset > 255) {
        fragmented(sizeof(large) - offset, large + offset, 253);
        offset += 253;
    }
    frame(0, large + offset, sizeof(large) - offset);
    assert(errors == 0 && deliveries == 1 && delivered_len == 1025);
    assert(memcmp(delivered, large, sizeof(large)) == 0);
    reset();
    puts("Pinned BLUFI receiver fragment bounds: PASS (14 cases)");
    return 0;
}
