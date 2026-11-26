#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qr_scanner *qr_scanner_handle_t;
typedef bool (*qr_scanner_match_fn)(const char *payload);

qr_scanner_handle_t qr_scanner_create(void);
void qr_scanner_destroy(qr_scanner_handle_t scanner);

// Decodes an MJPEG/JPEG image to grayscale, optionally renders a center-cropped
// 1-bit MSB-first preview bitmap (1 = white, 0 = black; stride = (preview_w + 7) / 8),
// and searches for QR codes. When `match` is non-NULL, `payload_out` is filled only
// with a QR code accepted by `match`, while `*any_qr_out` reports whether any QR
// symbol was decoded in the frame.
esp_err_t qr_scanner_decode_jpeg(qr_scanner_handle_t scanner,
                                 const uint8_t *jpeg, size_t jpeg_len,
                                 uint8_t *preview_bits, int preview_w, int preview_h,
                                 char *payload_out, size_t payload_size,
                                 qr_scanner_match_fn match, bool *any_qr_out);

#ifdef __cplusplus
}
#endif
