/*
 * RTP parser for AV1 payload format (https://aomediacodec.github.io/av1-rtp-spec/)
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

#include "libavutil/attributes.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "libavcodec/av1_parse.h"
#include "avformat.h"

#include "rtpdec.h"
#include "rtpdec_formats.h"

struct PayloadContext {
    // sdp setup parameters
    uint8_t seq_profile;
    uint8_t seq_level_idx;
    uint8_t seq_tier;

    uint32_t obu_total_size;
    uint32_t obu_readed_size;

    uint8_t *obu_buf;
    uint32_t obu_buf_size;
};

static inline void rtp_leb128(uint8_t *p, int64_t *size, int *bytes_num) 
{
    int i;
    *size = 0;

    for (i = 0; i < 8; i++) {
        uint8_t byte = p[i];
        *size |= (int64_t)(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80))
            break;
    }
    *bytes_num = i + 1;
}

static int sdp_parse_fmtp_config_av1(AVFormatContext *s,
                                      AVStream *stream,
                                      PayloadContext *av1_data,
                                      const char *attr, const char *value)
{
    AVCodecParameters *par = stream->codecpar;

    if (!strcmp(attr, "profile")) {
        av1_data->seq_profile = atoi(value);
    } else if (!strcmp(attr, "level-idx")) {
        av1_data->seq_level_idx = atoi(value);
    } else if (!strcmp(attr, "tier")) {
        av1_data->seq_tier = atoi(value);
    }
    return 0;
}

static int parse_av1_sdp_line(AVFormatContext *s, int st_index,
                               PayloadContext *av1_data, const char *line)
{
    AVStream *stream;
    const char *p = line;

    if (st_index < 0)
        return 0;

    stream = s->streams[st_index];

    if (av_strstart(p, "fmtp:", &p)) {
        return ff_parse_fmtp(s, stream, av1_data, p, sdp_parse_fmtp_config_av1);
    }

    return 0;
}

static void av1_close_context(PayloadContext *data)
{
    if (data->obu_buf) {
        av_freep(&data->obu_buf);
    }
}

static int av1_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    uint8_t aggr_hdr;
    uint8_t Z, Y, W, N;
    int64_t elem_size;
    int pos = 0, bytes_num;
    int i = 0, result = 0;

    if (!len) {
        //av_log(ctx, AV_LOG_ERROR, "Empty AV1 RTP packet\n");
        return AVERROR_INVALIDDATA;
    }
    aggr_hdr = buf[0];
    Z = aggr_hdr >> 7;
    Y = (aggr_hdr >> 6) & 0x01;
    W = (aggr_hdr >> 4) & 0x03;
    N = (aggr_hdr >> 3) & 0x01;

    buf += 1;
    len -= 1;

    if (0 == Z && 0 == Y) {
        // aggregation packet
        if ((result = av_new_packet(pkt, 0)) < 0)
            return result;
        if (0 == W) {
            // each OBU element MUST be preceded by a length field
            while (len > 0) {
                rtp_leb128(buf, &elem_size, &bytes_num);
                buf += bytes_num;
                len -= bytes_num;

                av_grow_packet(pkt, elem_size);
                memcpy(pkt->data + pos, buf, elem_size);
                buf += elem_size;
                len -= elem_size;

                pos += elem_size;
            }
        } else {
            // the last OBU element MUST NOT be preceded by a length field
            for (i = 0; i < W; i++) {
                if ((W - 1) == i) { // last OBU element
                    av_grow_packet(pkt, len);
                    memcpy(pkt->data + pos, buf, len);
                } else {
                    rtp_leb128(buf, &elem_size, &bytes_num);
                    buf += bytes_num;
                    len -= bytes_num;

                    av_grow_packet(pkt, elem_size);
                    memcpy(pkt->data + pos, buf, elem_size);
                    buf += elem_size;
                    len -= elem_size;

                    pos += elem_size;
                }
            }
        }
    } else {
        // fragment packet
        assert(W == 0 || W == 1);
        if (0 == W) {
            rtp_leb128(buf, &elem_size, &bytes_num);
            assert(elem_size == (len - bytes_num));
            buf += bytes_num;
            len -= bytes_num;
        }

        if (!data->obu_total_size) {
            uint8_t obu_header    = buf[0];
            uint8_t forbidden_bit = obu_header >> 7;
            uint8_t type          = (obu_header >> 3) & 0x0f;
            uint8_t ext           = (obu_header >> 2) & 0x01;
            uint8_t size_field    = (obu_header >> 1) & 0x01;

            assert(size_field);
            pos += (ext ? 2 : 1);

            rtp_leb128(buf + pos, &elem_size, &bytes_num);
            data->obu_total_size = elem_size + pos + bytes_num;

            if (data->obu_buf_size < data->obu_total_size) {
                data->obu_buf = av_realloc(data->obu_buf, data->obu_total_size);
                data->obu_buf_size = data->obu_total_size;
            }
        }

        memcpy(data->obu_buf + data->obu_readed_size, buf, len);
        data->obu_readed_size += len;

        if (data->obu_readed_size >= data->obu_total_size) {
            if ((result = av_new_packet(pkt, data->obu_total_size)) < 0)
                return result;
            memcpy(pkt->data, data->obu_buf, data->obu_total_size);
            
            data->obu_readed_size = 0;
            data->obu_total_size = 0;
            result = 0;
        } else {
            result = 1;
        }
    }
    pkt->stream_index = st->index;
    return result;
}

const RTPDynamicProtocolHandler ff_av1_dynamic_handler = {
    .enc_name         = "AV1",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_AV1,
    .need_parsing     = AVSTREAM_PARSE_FULL,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = parse_av1_sdp_line,
    .close            = av1_close_context,
    .parse_packet     = av1_handle_packet,
};