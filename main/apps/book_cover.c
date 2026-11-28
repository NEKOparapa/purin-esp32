#include "book_cover.h"
#include "app_media.h"
#include "esp32s3/rom/tjpgd.h"
#include "src/libs/lodepng/lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    book_cover_t *cover;
    unsigned width, height, draw_w, draw_h;
} thumbnail_t;

static void fit(thumbnail_t *thumb, unsigned w, unsigned h)
{
    thumb->width = w;
    thumb->height = h;
    thumb->draw_w = COVER_WIDTH;
    thumb->draw_h = COVER_HEIGHT;
    if (w * COVER_HEIGHT > h * COVER_WIDTH) thumb->draw_h = h * COVER_WIDTH / w;
    else thumb->draw_w = w * COVER_HEIGHT / h;
    if (!thumb->draw_h) thumb->draw_h = 1;
    if (!thumb->draw_w) thumb->draw_w = 1;
}

// Map each decoded block into the same aspect-fit rectangle, dithering on white paper.
static void draw_block(thumbnail_t *t, const uint8_t *pixels, unsigned x0, unsigned y0,
                       unsigned w, unsigned h, unsigned channels)
{
    static const uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    unsigned end_x = ((x0 + w) * t->draw_w + t->width - 1) / t->width;
    unsigned end_y = ((y0 + h) * t->draw_h + t->height - 1) / t->height;
    for (unsigned y = (y0 * t->draw_h + t->height - 1) / t->height; y < end_y && y < t->draw_h; ++y) {
        unsigned row = y * t->height / t->draw_h - y0;
        unsigned dy = (COVER_HEIGHT - t->draw_h) / 2 + y;
        for (unsigned x = (x0 * t->draw_w + t->width - 1) / t->width; x < end_x && x < t->draw_w; ++x) {
            const uint8_t *p = pixels + (row * w + x * t->width / t->draw_w - x0) * channels;
            unsigned gray = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
            if (channels == 4) gray = 255 - (255 - gray) * p[3] / 255;
            unsigned dx = (COVER_WIDTH - t->draw_w) / 2 + x;
            if (gray < 8u + 16u * bayer[dy % 4][dx % 4]) t->cover->bits[dy][dx / 8] &= ~(0x80u >> (dx % 8));
        }
    }
}

typedef struct { FILE *file; thumbnail_t *thumb; } jpeg_input_t;

static UINT jpeg_read(JDEC *jd, BYTE *buffer, UINT count)
{
    FILE *file = ((jpeg_input_t *)jd->device)->file;
    if (!buffer) return fseek(file, count, SEEK_CUR) == 0 ? count : 0;
    return fread(buffer, 1, count, file);
}

static UINT jpeg_block(JDEC *jd, void *pixels, JRECT *rect)
{
    draw_block(((jpeg_input_t *)jd->device)->thumb, pixels, rect->left, rect->top,
               rect->right - rect->left + 1, rect->bottom - rect->top + 1, 3);
    return 1;
}

static bool decode_jpeg(FILE *file, thumbnail_t *thumb)
{
    jpeg_input_t input = {file, thumb};
    JDEC decoder;
    const unsigned pool_size = 8192;
    void *pool = malloc(pool_size);
    bool ok = pool && jd_prepare(&decoder, jpeg_read, pool, pool_size, &input) == JDR_OK &&
              decoder.width && decoder.height && decoder.width <= 4096 && decoder.height <= 4096;
    if (ok) {
        fit(thumb, decoder.width, decoder.height);
        ok = jd_decomp(&decoder, jpeg_block, 0) == JDR_OK;
    }
    free(pool);
    return ok;
}

static bool decode_png(FILE *file, size_t length, thumbnail_t *thumb)
{
    uint8_t *encoded = malloc(length);
    if (!encoded) return false;
    LodePNGState state;
    lodepng_state_init(&state);
    state.decoder.read_text_chunks = 0;
    // Limit inflated scanlines as well as the compressed file and decoded dimensions.
    state.decoder.zlibsettings.max_output_size = 4 * 1024 * 1024 + 32 * 1024;
    unsigned w, h;
    lv_draw_buf_t *decoded = NULL;
    bool ok = fread(encoded, 1, length, file) == length &&
              !lodepng_inspect(&w, &h, &state, encoded, length) &&
              w && h && w <= 4096 && h <= 4096 && w * h <= 1024 * 1024 &&
              lodepng_get_bpp(&state.info_png.color) <= 32 &&
              !lodepng_decode((unsigned char **)&decoded, &w, &h, &state, encoded, length) && decoded;
    if (ok) {
        fit(thumb, w, h);
        draw_block(thumb, decoded->data, 0, 0, w, h, 4);
    }
    if (decoded) lv_draw_buf_destroy(decoded);
    lodepng_state_cleanup(&state);
    free(encoded);
    return ok;
}

book_cover_t *book_cover_load(const char *book_filename)
{
    const char *ext = strrchr(book_filename, '.');
    if (!ext) return NULL;
    const char *const formats[] = {"jpg", "jpeg", "png"};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        char path[sizeof(BOOK_DIR) + 260];
        int n = snprintf(path, sizeof(path), "%s/%.*s.%s", BOOK_DIR, (int)(ext - book_filename), book_filename, formats[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        book_cover_t *cover = NULL;
        long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
        if (length > 0 && length <= 1024 * 1024 && !fseek(file, 0, SEEK_SET)) cover = malloc(sizeof(*cover));
        if (cover) {
            memset(cover, 0xff, sizeof(*cover));
            memset(cover->palette, 0, 3);
            thumbnail_t thumb = {.cover = cover};
            bool ok = !strcmp(formats[i], "png") ? decode_png(file, length, &thumb) : decode_jpeg(file, &thumb);
            if (!ok) { free(cover); cover = NULL; }
        }
        fclose(file);
        if (cover) return cover;
    }
    return NULL;
}
