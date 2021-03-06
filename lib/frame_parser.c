/*
 * nghq
 *
 * Copyright (c) 2018 British Broadcasting Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "frame_parser.h"
#include "header_compression.h"
#include "util.h"



/*
 * Parse the frame header - returns the length of the frame. @p buf *must* be
 * at least 10 bytes long in order to contain a complete HTTP/QUIC frame header
 */
uint64_t _parse_frame_header (uint8_t* buf, nghq_frame_type *type,
                              uint8_t *flags, size_t *length_size) {
  uint64_t len = _get_varlen_int(buf, length_size);
  *type = buf[*length_size];
  if (flags != NULL) {
    *flags = buf[++(*length_size)];
  }
  *length_size += 2; /* Type + flags */
  return len;
}

ssize_t parse_frames (uint8_t* buf, size_t buf_len, nghq_frame_type *type) {
  uint64_t frame_length = 0;
  size_t header_length = 0;

  if (buf_len < 10) {
    /*
     * Check short buffers are long enough for a full HTTP/QUIC header, so we
     * don't have any risk of overflowing when parsing
     */
    if ((buf[0] & 0xC0) == _VARLEN_INT_62_BIT) {
      return NGHQ_ERROR;
    }
    if (((buf[0] & 0xC0) == _VARLEN_INT_30_BIT) && (buf_len < 6)) {
      return NGHQ_ERROR;
    }
    if (((buf[0] & 0xC0) == _VARLEN_INT_14_BIT) && (buf_len < 4)) {
      return NGHQ_ERROR;
    }
    if (buf_len < 3) {
      return NGHQ_ERROR;
    }
  }

  frame_length = _parse_frame_header (buf, type, NULL, &header_length);

  if (*type == NGHQ_FRAME_TYPE_BAD) {
    return NGHQ_ERROR;
  }

  return frame_length + header_length;
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                         Body Block (*)                        |  DATA
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 */
ssize_t parse_data_frame (uint8_t* buf, size_t buf_len, uint8_t** data,
                          size_t *data_len) {
  size_t varint_len = 0;
  *data_len = _get_varlen_int(buf, &varint_len);

  if (buf[varint_len] != NGHQ_FRAME_TYPE_DATA) {
    return NGHQ_ERROR;
  }

  if (*data_len > buf_len - varint_len - 2) {
    return *data_len + varint_len + 2;
  }

  *data = buf + varint_len + 2;
  return *data_len - buf_len + varint_len + 2;
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                        Header Block (*)                       |  HEADERS
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 */
ssize_t parse_headers_frame (nghq_hdr_compression_ctx* ctx, uint8_t* buf,
                             size_t buf_len, nghq_header*** hdrs,
                             size_t* num_hdrs) {
  size_t header_len = 0, expected_header_block_len;
  nghq_frame_type type;
  expected_header_block_len = _parse_frame_header(buf, &type, NULL, &header_len);

  if (type != NGHQ_FRAME_TYPE_HEADERS) {
    return NGHQ_ERROR;
  }

  if (expected_header_block_len > buf_len - header_len) {
    return buf_len;
  }

  return nghq_inflate_hdr(ctx, buf + header_len, expected_header_block_len,
                          1, hdrs, num_hdrs);
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                 Prioritised Request ID (i)                  ...  PRIORITY
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 * |                  Stream Dependency ID (i)                   ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Weight (8)  |
 * +-+-+-+-+-+-+-+-+
 */
int parse_priority_frame (uint8_t* buf, size_t buf_len, uint8_t* flags,
                          uint64_t* request_id, uint64_t* dependency_id,
                          uint8_t* weight) {
  size_t off = 0;
  nghq_frame_type type;
  uint64_t frame_length = _parse_frame_header(buf, &type, flags, &off);

  if (type != NGHQ_FRAME_TYPE_PRIORITY) {
    return NGHQ_ERROR;
  }

  *request_id = _get_varlen_int(buf+off, &off);
  *dependency_id = _get_varlen_int(buf+off, &off);
  *weight = buf[off];
  return NGHQ_OK;
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                          Push ID (i)                        ... CANCEL_PUSH
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 */
int parse_cancel_push_frame (uint8_t* buf, size_t buf_len, uint64_t* push_id) {
  size_t off = 0;
  nghq_frame_type type;
  uint64_t frame_length = _parse_frame_header(buf, &type, NULL, &off);

  if (type != NGHQ_FRAME_TYPE_CANCEL_PUSH) {
    return NGHQ_ERROR;
  }

  *push_id = _get_varlen_int(buf+off, &off);
  return NGHQ_OK;
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)      ... |    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |         Identifier (16)       |            Length (i)       ...  SETTINGS
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 * |                          Contents (?)                       ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
int parse_settings_frame (uint8_t* buf, size_t buf_len,
                          nghq_settings** settings) {
  size_t off = 0;
  nghq_frame_type type;
  uint64_t frame_length;
  int rv = NGHQ_OK;
  int seen_header_table_size = 0;
  int seen_max_header_list_size = 0;

  frame_length = _parse_frame_header(buf, &type, NULL, &off);

  if (type != NGHQ_FRAME_TYPE_SETTINGS) {
    return NGHQ_ERROR;
  }

  if (settings == NULL) {
    *settings = malloc(sizeof(nghq_settings));
    if (*settings == NULL) {
      return NGHQ_OUT_OF_MEMORY;
    }
    (*settings)->header_table_size = NGHQ_SETTINGS_DEFAULT_HEADER_TABLE_SIZE;
    (*settings)->max_header_list_size =
        NGHQ_SETTINGS_DEFAULT_MAX_HEADER_LIST_SIZE;
  }

  while (off < buf_len) {
    int16_t id = get_int16_from_buf(buf + off);
    off += 2;
    uint64_t len = _get_varlen_int(buf + off, &off);

    switch (id) {
      case NGHQ_SETTINGS_HEADER_TABLE_SIZE:
        if ((len != 4) || (seen_header_table_size)) {
          rv = NGHQ_HTTP_MALFORMED_FRAME;
        } else {
          (*settings)->header_table_size = get_int32_from_buf(buf);
          seen_header_table_size = 1;
        }
        break;
      case NGHQ_SETTINGS_MAX_HEADER_LIST_SIZE:
        if ((len != 4) || (seen_max_header_list_size)) {
          rv = NGHQ_HTTP_MALFORMED_FRAME;
        } else {
          (*settings)->max_header_list_size = get_int32_from_buf(buf);
          seen_max_header_list_size = 1;
        }
        break;
      default:
        rv = NGHQ_HTTP_MALFORMED_FRAME;
    }

    off += len;
  }

  return rv;
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                          Push ID (i)                        ...  PUSH_
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  PROMISE
 * |                       Header Block (*)                      ...  Payload
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t parse_push_promise_frame (nghq_hdr_compression_ctx *ctx, uint8_t* buf,
                              size_t buf_len, uint64_t* push_id,
                              nghq_header ***hdrs, size_t* num_hdrs) {
  size_t push_header_len = 0, push_id_len = 0, frame_payload_len;
  nghq_frame_type type;
  frame_payload_len = _parse_frame_header(buf, &type, NULL, &push_header_len);

  if (type != NGHQ_FRAME_TYPE_PUSH_PROMISE) {
    return NGHQ_ERROR;
  }

  *push_id = _get_varlen_int(buf+push_header_len, &push_id_len);

  if (frame_payload_len > buf_len - push_header_len) {
    return buf_len;
  }

  return nghq_inflate_hdr(ctx, buf + push_header_len + push_id_len,
                          frame_payload_len - push_id_len, 1, hdrs,
                          num_hdrs);
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                         Stream ID (i)                       ...  GOAWAY
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 */
int parse_goaway_frame (uint8_t* buf, size_t buf_len, uint64_t* last_stream_id){
  size_t off = 0;
  nghq_frame_type type;
  uint64_t frame_length = _parse_frame_header(buf, &type, NULL, &off);

  if (type != NGHQ_FRAME_TYPE_GOAWAY) {
    return NGHQ_ERROR;
  }

  *last_stream_id = _get_varlen_int(buf+off, &off);
  return NGHQ_OK;
}

/*
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Length (i)       ...|    Type (8)   |   Flags (8)   |  Frame HDR
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ===========
 * |                      Maximum Push ID (i)                    ... MAX_PUSH_ID
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  Payload
 */
int parse_max_push_id_frame (uint8_t* buf, size_t buf_len,
                             uint64_t* max_push_id) {
  size_t off = 0;
  nghq_frame_type type;
  uint64_t frame_length = _parse_frame_header(buf, &type, NULL, &off);

  if (type != NGHQ_FRAME_TYPE_MAX_PUSH_ID) {
    return NGHQ_ERROR;
  }

  *max_push_id = _get_varlen_int(buf+off, &off);
  return NGHQ_OK;
}
