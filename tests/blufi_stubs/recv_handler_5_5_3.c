/* SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Unmodified ESP-IDF 5.5.3 receiver, used as a pinned host-test fixture. */
void btc_blufi_recv_handler(uint8_t *data, int len)
{
    if (len < sizeof(struct blufi_hdr)) {
        BTC_TRACE_ERROR("%s invalid data length: %d", __func__, len);
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    struct blufi_hdr *hdr = (struct blufi_hdr *)data;

    // Verify if the received data length matches the expected length based on the BLUFI protocol
    int target_data_len;

    if (BLUFI_FC_IS_CHECK(hdr->fc)) {
        target_data_len = hdr->data_len + 4 + 2; // Data + (Type + Frame Control + Sequence Number + Data Length) + Checksum
    } else {
        target_data_len = hdr->data_len + 4; // Data + (Type + Frame Control + Sequence Number + Data Length)
    }

    if (len != target_data_len) {
        BTC_TRACE_ERROR("%s: Invalid data length: %d, expected: %d", __func__, len, target_data_len);
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint16_t checksum, checksum_pkt;
    int ret;

    if (hdr->seq != blufi_env.recv_seq) {
        BTC_TRACE_ERROR("%s seq %d is not expect %d\n", __func__, hdr->seq, blufi_env.recv_seq + 1);
        btc_blufi_report_error(ESP_BLUFI_SEQUENCE_ERROR);
        return;
    }

    blufi_env.recv_seq++;

    // first step, decrypt
    if (BLUFI_FC_IS_ENC(hdr->fc)
            && (blufi_env.cbs && blufi_env.cbs->decrypt_func)) {
        ret = blufi_env.cbs->decrypt_func(hdr->seq, hdr->data, hdr->data_len);
        if (ret != hdr->data_len) { /* enc must be success and enc len must equal to plain len */
            BTC_TRACE_ERROR("%s decrypt error %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_DECRYPT_ERROR);
            return;
        }
    }

    // second step, check sum
    if (BLUFI_FC_IS_CHECK(hdr->fc)
            && (blufi_env.cbs && blufi_env.cbs->checksum_func)) {
        checksum = blufi_env.cbs->checksum_func(hdr->seq, &hdr->seq, hdr->data_len + 2);
        checksum_pkt = hdr->data[hdr->data_len] | (((uint16_t) hdr->data[hdr->data_len + 1]) << 8);
        if (checksum != checksum_pkt) {
            BTC_TRACE_ERROR("%s checksum error %04x, pkt %04x\n", __func__, checksum, checksum_pkt);
            btc_blufi_report_error(ESP_BLUFI_CHECKSUM_ERROR);
            return;
        }
    }

    if (BLUFI_FC_IS_REQ_ACK(hdr->fc)) {
        btc_blufi_send_ack(hdr->seq);
    }

    if (BLUFI_FC_IS_FRAG(hdr->fc)) {
        if (blufi_env.offset == 0) {
            /*
            blufi_env.aggr_buf should be NULL if blufi_env.offset is 0.
            It is possible that the process of sending fragment packet
            has not been completed
            */
            if (blufi_env.aggr_buf) {
                BTC_TRACE_ERROR("%s msg error, blufi_env.aggr_buf is not freed\n", __func__);
                btc_blufi_report_error(ESP_BLUFI_MSG_STATE_ERROR);
                return;
            }
            blufi_env.total_len = hdr->data[0] | (((uint16_t) hdr->data[1]) << 8);
            blufi_env.aggr_buf = osi_malloc(blufi_env.total_len);
            if (blufi_env.aggr_buf == NULL) {
                BTC_TRACE_ERROR("%s no mem, len %d\n", __func__, blufi_env.total_len);
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
                return;
            }
        }
        if (blufi_env.offset + hdr->data_len  - 2 <= blufi_env.total_len){
            memcpy(blufi_env.aggr_buf + blufi_env.offset, hdr->data + 2, hdr->data_len  - 2);
            blufi_env.offset += (hdr->data_len - 2);
        } else {
            BTC_TRACE_ERROR("%s payload is longer than packet length, len %d \n", __func__, blufi_env.total_len);
            btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
            return;
        }

    } else {
        if (blufi_env.offset > 0) {   /* if previous pkt is frag */
            /* blufi_env.aggr_buf should not be NULL */
            if (blufi_env.aggr_buf == NULL) {
                BTC_TRACE_ERROR("%s buffer is NULL\n", __func__);
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
                return;
            }
            /* payload length should be equal to total_len */
            if ((blufi_env.offset + hdr->data_len) != blufi_env.total_len) {
                BTC_TRACE_ERROR("%s payload is longer than packet length, len %d \n", __func__, blufi_env.total_len);
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }
            memcpy(blufi_env.aggr_buf + blufi_env.offset, hdr->data, hdr->data_len);

            btc_blufi_protocol_handler(hdr->type, blufi_env.aggr_buf, blufi_env.total_len);
            blufi_env.offset = 0;
            osi_free(blufi_env.aggr_buf);
            blufi_env.aggr_buf = NULL;
        } else {
            btc_blufi_protocol_handler(hdr->type, hdr->data, hdr->data_len);
            blufi_env.offset = 0;
        }
    }
}
