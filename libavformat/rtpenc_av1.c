/*
 * RTP packetization for AV1 (https://aomediacodec.github.io/av1-rtp-spec/)
 * Copyright (c) 2023 clarkh <hungkuishing@outlook.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * @brief AV1 packetization
 * @author clarkh <hungkuishing@outlook.com>
 */

#include "libavutil/intreadwrite.h"
#include "libavcodec/av1.h"
#include "libavcodec/av1_parse.h"

#include "avformat.h"
#include "rtpenc.h"
#include "av1.h"

#define AV1_PAYLOAD_HEADER_SIZE 1

static int get_leb128(uint64_t in, uint64_t *pout)
{
    uint8_t byte;
    int bytes_num = (av_log2(in) + 7) / 7;
    if (NULL == pout) return bytes_num;

    *pout = 0;
    for (int i = 0; i < bytes_num; i++) {
        byte = (in >> (7 * i)) & 0x7f;
        if (i < bytes_num - 1)
            byte |= 0x80;
        *pout |= ((uint64_t)byte << (8 * i));
    }
    return bytes_num;
}

/*
* -------------------- 
* | leb128 | payload |
* --------------------
*/
static int get_proper_bytes_num(int size)
{
    int bytes_num = 0;
    for (; bytes_num < 8; bytes_num++) {
        if (bytes_num == (get_leb128(size - bytes_num, NULL)))
            return bytes_num;
    }
    return -1;
}

/**
 * @brief Set the payload header
 * 
 * @param p  pointer to buffer
 * @param Z  first OBU element is continuation of previous packet
 * @param Y  last OBU element will continue in the next packet
 * @param W  OBU element count
 * @param N  the packet is the first packet of a coded video sequence
 */
static inline void set_payload_header(uint8_t *p, uint8_t Z, uint8_t Y, uint8_t W, uint8_t N)
{
    /*
    0 1 2 3 4 5 6 7
    +-+-+-+-+-+-+-+-+
    |Z|Y| W |N|-|-|-|
    +-+-+-+-+-+-+-+-+
    */
    *(p) = Z << 7 | Y << 6 | W << 4 | N << 3;
}

void ff_rtp_send_av1(AVFormatContext *s1, const uint8_t *buf1, int size)
{
    RTPMuxContext *s = s1->priv_data;

    uint8_t Z = 0;
    uint8_t N = 0;

    int obu_index = 0;
    int has_seq_header = 0;
    int is_first_packet = 1;
    int is_first_fragment = 1;
    int aggr_header_size;

    int bytes_num = 0;
    int buffered_size = 0;
    int size_with_leb128 = 0;
    int frag_payload_size = 0;
    int obu_num = 0, obu_raw_size = 0;

    uint64_t ele_size = 0;
    const uint8_t *buf = buf1, *end = buf1 + size;

    AV1OBU *obus = NULL;

    s->timestamp = s->cur_timestamp;
    s->buf_ptr   = s->buf;

    while (buf < end) {
        obus = av_realloc_array(obus, obu_num + 1, sizeof(AV1OBU));
        if (!obus) {
            av_log(s1, AV_LOG_ERROR, "av_realloc_array failed.");
            goto end;
        }

        obu_raw_size = ff_av1_extract_obu(&obus[obu_num], buf, end - buf, NULL);
        if (obu_raw_size < 0) {
            av_log(s1, AV_LOG_ERROR, "ff_av1_extract_obu error ret: %d\n", obu_raw_size);
            goto end;
        }

        if (AV1_OBU_SEQUENCE_HEADER == obus[obu_num].type)
            has_seq_header = 1;

        obu_num += 1;
        buf += obu_raw_size;
    }

    while (obu_index < obu_num) {
        if (AV1_OBU_TEMPORAL_DELIMITER == obus[obu_index].type) {
            obu_index++;
            continue;
        }

        buffered_size = s->buf_ptr - s->buf;
        size_with_leb128 = get_leb128(obus[obu_index].raw_size, NULL) + obus[obu_index].raw_size;
        aggr_header_size = buffered_size ? 0 : AV1_PAYLOAD_HEADER_SIZE;
        if ((buffered_size + size_with_leb128 + aggr_header_size) > s->max_payload_size) {
            // buffer has data
            if (s->buf_ptr != s->buf) {
                N = 0;
                if (has_seq_header && is_first_packet) {
                    N = 1;
                    is_first_packet = 0;
                }
                set_payload_header(s->buf, 0, 0, 0, N);

                ff_rtp_send_data(s1, s->buf, buffered_size, 0);
                s->buf_ptr = s->buf;

                obu_index--;
            } else {
                // occurs a big obu, split it to send
                //obu_raw_size = obus[obu_index].raw_size;
                size_with_leb128 = get_leb128(obus[obu_index].raw_size, NULL) + obus[obu_index].raw_size;
                while (size_with_leb128 + AV1_PAYLOAD_HEADER_SIZE > s->max_payload_size) {
                    s->buf_ptr = s->buf;

                    Z = is_first_fragment == 1 ? 0 : 1; // first OBU element is continuation of previous packet?
                    if (is_first_fragment) {
                        is_first_fragment = 0;
                    } 

                    if (has_seq_header && is_first_packet) {
                        N = 1;
                        is_first_packet = 0;
                    }
                    set_payload_header(s->buf_ptr, Z, 1, 0, N);
                    s->buf_ptr += AV1_PAYLOAD_HEADER_SIZE;

                    bytes_num = get_proper_bytes_num(s->max_payload_size - AV1_PAYLOAD_HEADER_SIZE);
                    frag_payload_size = s->max_payload_size - AV1_PAYLOAD_HEADER_SIZE - bytes_num;
                    get_leb128(frag_payload_size, &ele_size);
                    memcpy(s->buf_ptr, &ele_size, bytes_num);
                    s->buf_ptr += bytes_num;

                    memcpy(s->buf_ptr, obus[obu_index].raw_data, frag_payload_size);
                    obus[obu_index].raw_data += frag_payload_size;
                    obus[obu_index].raw_size -= frag_payload_size;
                    ff_rtp_send_data(s1, s->buf, s->max_payload_size, 0);
                    size_with_leb128 = get_leb128(obus[obu_index].raw_size, NULL) + obus[obu_index].raw_size;
                }
                s->buf_ptr = s->buf;
                set_payload_header(s->buf_ptr, 1, 0, 0, 0);
                s->buf_ptr += AV1_PAYLOAD_HEADER_SIZE;

                bytes_num = get_leb128(obus[obu_index].raw_size, &ele_size);
                memcpy(s->buf_ptr, &ele_size, bytes_num);
                s->buf_ptr += bytes_num;

                memcpy(s->buf_ptr, obus[obu_index].raw_data, obus[obu_index].raw_size);
                ff_rtp_send_data(s1, s->buf, obus[obu_index].raw_size + AV1_PAYLOAD_HEADER_SIZE + bytes_num, 1);
                s->buf_ptr = s->buf;
            }
        } else {
            if (s->buf_ptr == s->buf)
                s->buf_ptr += AV1_PAYLOAD_HEADER_SIZE;
            bytes_num = get_leb128(obus[obu_index].raw_size, &ele_size);
            memcpy(s->buf_ptr, &ele_size, bytes_num);
            s->buf_ptr += bytes_num;

            memcpy(s->buf_ptr, obus[obu_index].raw_data, obus[obu_index].raw_size);
            s->buf_ptr += obus[obu_index].raw_size;
        }
        obu_index++;
    }
    if (s->buf_ptr != s->buf) {
        N = 0;
        if (has_seq_header && is_first_packet) {
            N = 1;
            is_first_packet = 0;
        }
        set_payload_header(s->buf, 0, 0, 0, N);

        buffered_size = s->buf_ptr - s->buf;
        ff_rtp_send_data(s1, s->buf, buffered_size, 1);
    }
end:
    av_freep(&obus);
}