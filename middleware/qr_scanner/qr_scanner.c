#include "qr_scanner.h"

#include <stdlib.h>
#include <string.h>
#include "esp32s3/rom/tjpgd.h"
#include "esp_heap_caps.h"
#include "quirc.h"

enum { TJPGD_WORK_BYTES = 8192 };

struct qr_scanner {
    struct quirc *quirc;
    struct quirc_code code;
    struct quirc_data data;
    JDEC jdec;
    uint8_t work_pool[TJPGD_WORK_BYTES];
    int cur_w;
    int cur_h;
};

// Standard ITU-T.81 Annex K Huffman tables for UVC MJPEG streams that omit DHT.
static const uint8_t mjpeg_default_dht[420] = {
    0xFF, 0xC4, 0x01, 0xA2,
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7D,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
    0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
    0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04,
    0x04, 0x00, 0x01, 0x02, 0x77,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
    0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
    0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
    0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4,
    0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};

typedef struct {
    const uint8_t *segs[3];
    size_t lens[3];
    size_t seg_idx;
    size_t seg_off;
    uint8_t *gray;
    int out_w;
    int out_h;
} decode_io_t;

static void inspect_mjpeg_markers(const uint8_t *jpeg, size_t len, bool *has_dht, size_t *sos_pos)
{
    *has_dht = false;
    *sos_pos = 0;
    if (len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return;
    size_t i = 2;
    while (i + 1 < len) {
        if (jpeg[i] != 0xFF) {
            ++i;
            continue;
        }
        while (i < len && jpeg[i] == 0xFF) ++i;
        if (i >= len) break;
        uint8_t marker = jpeg[i++];
        if (marker == 0x00 || marker == 0xD9) break;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xC4) {
            *has_dht = true;
            return;
        }
        if (marker == 0xDA) {
            *sos_pos = i - 2;
            return;
        }
        if (i + 2 > len) break;
        size_t seg_len = ((size_t)jpeg[i] << 8) | jpeg[i + 1];
        if (seg_len < 2 || i + seg_len > len) break;
        i += seg_len;
    }
}

static UINT tjpgd_infunc(JDEC *jd, BYTE *buff, UINT nbyte)
{
    decode_io_t *io = jd->device;
    UINT total = 0;
    while (total < nbyte && io->seg_idx < 3) {
        size_t avail = io->lens[io->seg_idx] - io->seg_off;
        if (avail == 0) {
            ++io->seg_idx;
            io->seg_off = 0;
            continue;
        }
        size_t chunk = nbyte - total;
        if (chunk > avail) chunk = avail;
        if (buff) {
            memcpy(buff + total, io->segs[io->seg_idx] + io->seg_off, chunk);
        }
        io->seg_off += chunk;
        total += (UINT)chunk;
    }
    return total;
}

static UINT tjpgd_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_io_t *io = jd->device;
    const uint8_t *src = bitmap;
    const int rect_w = (int)rect->right - (int)rect->left + 1;
    const int max_x = (int)rect->right < io->out_w ? (int)rect->right : io->out_w - 1;
    const int max_y = (int)rect->bottom < io->out_h ? (int)rect->bottom : io->out_h - 1;

    for (int y = (int)rect->top; y <= max_y; ++y) {
        const uint8_t *row_src = src + (size_t)(y - (int)rect->top) * (size_t)rect_w * 3U;
        uint8_t *row_dst = io->gray + (size_t)y * (size_t)io->out_w;
        for (int x = (int)rect->left; x <= max_x; ++x) {
            const uint8_t *p = row_src + (size_t)(x - (int)rect->left) * 3U;
            row_dst[x] = (uint8_t)((p[0] * 77U + p[1] * 150U + p[2] * 29U) >> 8);
        }
    }
    return 1;
}

static void render_preview_i1(const uint8_t *gray, int src_w, int src_h,
                              uint8_t *dst_bits, int dst_w, int dst_h)
{
    static const uint8_t bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    const int stride = (dst_w + 7) / 8;
    memset(dst_bits, 0, (size_t)stride * (size_t)dst_h);

    // Center-crop source to preserve aspect ratio without distorting QR squares.
    int crop_w = src_w;
    int crop_h = src_h;
    if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
        crop_w = (int)((int64_t)src_h * dst_w / dst_h);
    } else {
        crop_h = (int)((int64_t)src_w * dst_h / dst_w);
    }
    if (crop_w < 1) crop_w = 1;
    if (crop_h < 1) crop_h = 1;
    const int x0 = (src_w - crop_w) / 2;
    const int y0 = (src_h - crop_h) / 2;

    uint8_t min_v = 255, max_v = 0;
    for (int y = y0; y < y0 + crop_h; y += 4) {
        const uint8_t *row = gray + (size_t)y * (size_t)src_w;
        for (int x = x0; x < x0 + crop_w; x += 4) {
            uint8_t v = row[x];
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }
    }
    const int span = (int)max_v - (int)min_v;

    for (int y = 0; y < dst_h; ++y) {
        const int sy = y0 + (int)((int64_t)y * crop_h / dst_h);
        const uint8_t *src_row = gray + (size_t)sy * (size_t)src_w;
        uint8_t *dst_row = dst_bits + (size_t)y * (size_t)stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x0 + (int)((int64_t)x * crop_w / dst_w);
            int g = src_row[sx];
            if (span >= 32) {
                g = (g - (int)min_v) * 255 / span;
                if (g < 0) g = 0;
                if (g > 255) g = 255;
            }
            int darkness = (255 - g) * 16 / 256;
            if (darkness <= bayer4[y & 3][x & 3]) {
                dst_row[x >> 3] |= (uint8_t)(0x80U >> (x & 7));
            }
        }
    }
}

qr_scanner_handle_t qr_scanner_create(void)
{
    qr_scanner_handle_t scanner = heap_caps_calloc(1, sizeof(*scanner),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scanner) scanner = calloc(1, sizeof(*scanner));
    if (!scanner) return NULL;
    scanner->quirc = quirc_new();
    if (!scanner->quirc) {
        free(scanner);
        return NULL;
    }
    return scanner;
}

void qr_scanner_destroy(qr_scanner_handle_t scanner)
{
    if (!scanner) return;
    if (scanner->quirc) quirc_destroy(scanner->quirc);
    free(scanner);
}

esp_err_t qr_scanner_decode_jpeg(qr_scanner_handle_t scanner,
                                 const uint8_t *jpeg, size_t jpeg_len,
                                 uint8_t *preview_bits, int preview_w, int preview_h,
                                 char *payload_out, size_t payload_size,
                                 qr_scanner_match_fn match, bool *any_qr_out)
{
    if (!scanner || !jpeg || jpeg_len < 4 || !payload_out || !payload_size) {
        return ESP_ERR_INVALID_ARG;
    }
    payload_out[0] = '\0';
    if (any_qr_out) *any_qr_out = false;

    bool has_dht = false;
    size_t sos_pos = 0;
    inspect_mjpeg_markers(jpeg, jpeg_len, &has_dht, &sos_pos);

    decode_io_t io = {0};
    if (!has_dht && sos_pos > 0) {
        io.segs[0] = jpeg;
        io.lens[0] = sos_pos;
        io.segs[1] = mjpeg_default_dht;
        io.lens[1] = sizeof(mjpeg_default_dht);
        io.segs[2] = jpeg + sos_pos;
        io.lens[2] = jpeg_len - sos_pos;
    } else {
        io.segs[0] = jpeg;
        io.lens[0] = jpeg_len;
    }

    if (jd_prepare(&scanner->jdec, tjpgd_infunc, scanner->work_pool,
                   sizeof(scanner->work_pool), &io) != JDR_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    BYTE scale = (scanner->jdec.width > 800 || scanner->jdec.height > 600) ? 1 : 0;
    int out_w = (int)(scanner->jdec.width >> scale);
    int out_h = (int)(scanner->jdec.height >> scale);
    if (out_w < 32 || out_h < 32) return ESP_ERR_INVALID_SIZE;

    if (scanner->cur_w != out_w || scanner->cur_h != out_h) {
        if (quirc_resize(scanner->quirc, out_w, out_h) < 0) {
            return ESP_ERR_NO_MEM;
        }
        scanner->cur_w = out_w;
        scanner->cur_h = out_h;
    }

    io.gray = quirc_begin(scanner->quirc, NULL, NULL);
    io.out_w = out_w;
    io.out_h = out_h;
    if (!io.gray) return ESP_ERR_NO_MEM;

    if (jd_decomp(&scanner->jdec, tjpgd_outfunc, scale) != JDR_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Generate 1-bit viewfinder preview before quirc_end() thresholds the image in place.
    if (preview_bits && preview_w > 0 && preview_h > 0) {
        render_preview_i1(io.gray, out_w, out_h, preview_bits, preview_w, preview_h);
    }

    quirc_end(scanner->quirc);

    const int count = quirc_count(scanner->quirc);
    for (int i = 0; i < count; ++i) {
        quirc_extract(scanner->quirc, i, &scanner->code);
        quirc_decode_error_t qerr = quirc_decode(&scanner->code, &scanner->data);
        if (qerr == QUIRC_ERROR_DATA_ECC || qerr == QUIRC_ERROR_FORMAT_ECC) {
            quirc_flip(&scanner->code);
            qerr = quirc_decode(&scanner->code, &scanner->data);
        }
        if (qerr != QUIRC_SUCCESS || scanner->data.payload_len <= 0) continue;

        size_t copy_len = (size_t)scanner->data.payload_len;
        if (copy_len >= payload_size) copy_len = payload_size - 1;
        scanner->data.payload[copy_len] = '\0';
        const char *text = (const char *)scanner->data.payload;
        if (any_qr_out) *any_qr_out = true;
        if (!match || match(text)) {
            memcpy(payload_out, text, copy_len + 1);
            break;
        }
    }
    return ESP_OK;
}
