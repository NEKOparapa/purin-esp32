#include "ogg_opus_muxer.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE_BODY_CAPACITY = 8192, PAGE_PACKETS = 50 };

struct ogg_opus_muxer {
    ogg_bytes_write_fn output;
    void *context;
    uint32_t serial;
    uint32_t page_number;
    uint64_t bytes;
    uint64_t page_granule;
    uint64_t emitted_granule;
    esp_err_t failure;
    bool finished;
    uint8_t lacing[255];
    uint8_t body[PAGE_BODY_CAPACITY];
    size_t lacing_count;
    size_t body_count;
    unsigned packets;
};

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

static void put_u64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

// Ogg 使用非反射 CRC-32，初值和最终异或均为零。
static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    return crc;
}

static esp_err_t fail(ogg_opus_muxer_handle_t muxer, esp_err_t error)
{
    if (muxer->failure == ESP_OK) {
        muxer->failure = error;
    }
    return muxer->failure;
}

static esp_err_t emit_page(ogg_opus_muxer_handle_t muxer, uint8_t flags, uint64_t granule)
{
    if (muxer->failure != ESP_OK) {
        return muxer->failure;
    }
    if (!muxer->lacing_count) {
        return ESP_OK;
    }
    uint8_t header[27 + 255] = {0};
    memcpy(header, "OggS", 4);
    header[5] = flags;
    put_u64(header + 6, granule);
    put_u32(header + 14, muxer->serial);
    put_u32(header + 18, muxer->page_number);
    header[26] = (uint8_t)muxer->lacing_count;
    memcpy(header + 27, muxer->lacing, muxer->lacing_count);
    const size_t header_size = 27 + muxer->lacing_count;
    uint32_t crc = crc_update(0, header, header_size);
    crc = crc_update(crc, muxer->body, muxer->body_count);
    put_u32(header + 22, crc);
    esp_err_t error = muxer->output(muxer->context, header, header_size);
    if (error == ESP_OK) {
        error = muxer->output(muxer->context, muxer->body, muxer->body_count);
    }
    if (error != ESP_OK) {
        return fail(muxer, error);
    }
    muxer->bytes += header_size + muxer->body_count;
    muxer->emitted_granule = granule;
    ++muxer->page_number;
    muxer->lacing_count = muxer->body_count = 0;
    muxer->packets = 0;
    return ESP_OK;
}

esp_err_t ogg_opus_muxer_packet(ogg_opus_muxer_handle_t muxer, const uint8_t *packet,
                               size_t length, uint64_t granule)
{
    if (!muxer || !packet || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    if (muxer->failure != ESP_OK) {
        return muxer->failure;
    }
    if (muxer->finished) {
        return ESP_ERR_INVALID_STATE;
    }
    if (length > PAGE_BODY_CAPACITY || granule < muxer->page_granule) {
        return fail(muxer, ESP_ERR_INVALID_SIZE);
    }
    const size_t laces = length / 255 + 1; // 整 255 字节包也需要零长度终止段。
    // 在加入下一包之前冲页，保证最后一包可带 EOS 和裁剪后的 granule。
    if (muxer->packets >= PAGE_PACKETS || muxer->body_count + length > PAGE_BODY_CAPACITY ||
        muxer->lacing_count + laces > 255) {
        esp_err_t error = emit_page(muxer, 0, muxer->page_granule);
        if (error != ESP_OK) {
            return error;
        }
    }
    memcpy(muxer->body + muxer->body_count, packet, length);
    muxer->body_count += length;
    size_t remaining = length;
    while (remaining >= 255) {
        muxer->lacing[muxer->lacing_count++] = 255;
        remaining -= 255;
    }
    muxer->lacing[muxer->lacing_count++] = (uint8_t)remaining;
    muxer->page_granule = granule;
    ++muxer->packets;
    return ESP_OK;
}

esp_err_t ogg_opus_muxer_create(const ogg_opus_muxer_config_t *config,
                               ogg_bytes_write_fn output, void *context,
                               ogg_opus_muxer_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!config || !output || (config->channels != 1 && config->channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    ogg_opus_muxer_handle_t muxer = calloc(1, sizeof(*muxer));
    if (!muxer) {
        return ESP_ERR_NO_MEM;
    }
    muxer->output = output;
    muxer->context = context;
    muxer->serial = config->serial;
    uint8_t head[19] = {0};
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = config->channels;
    put_u16(head + 10, config->pre_skip);
    put_u32(head + 12, config->input_sample_rate);
    esp_err_t error = ogg_opus_muxer_packet(muxer, head, sizeof(head), 0);
    if (error == ESP_OK) {
        error = emit_page(muxer, 2, 0); // BOS；OpusHead 必须单独占页。
    }
    const char vendor[] = "wintermute-esp32";
    uint8_t tags[16 + sizeof(vendor) - 1] = {0};
    memcpy(tags, "OpusTags", 8);
    put_u32(tags + 8, sizeof(vendor) - 1);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    if (error == ESP_OK) {
        error = ogg_opus_muxer_packet(muxer, tags, sizeof(tags), 0);
    }
    if (error == ESP_OK) {
        error = emit_page(muxer, 0, 0); // Tags 最后一页不混入音频包。
    }
    if (error != ESP_OK) {
        ogg_opus_muxer_destroy(muxer);
        return error;
    }
    *out = muxer;
    return ESP_OK;
}

esp_err_t ogg_opus_muxer_flush(ogg_opus_muxer_handle_t muxer)
{
    if (!muxer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (muxer->finished) {
        return ESP_ERR_INVALID_STATE;
    }
    return emit_page(muxer, 0, muxer->page_granule);
}

esp_err_t ogg_opus_muxer_finish(ogg_opus_muxer_handle_t muxer, uint64_t final_granule)
{
    if (!muxer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (muxer->failure != ESP_OK) {
        return muxer->failure;
    }
    if (muxer->finished || !muxer->packets || final_granule < muxer->emitted_granule ||
        final_granule > muxer->page_granule) {
        return fail(muxer, ESP_ERR_INVALID_STATE);
    }
    esp_err_t error = emit_page(muxer, 4, final_granule);
    muxer->finished = error == ESP_OK;
    return error;
}

uint64_t ogg_opus_muxer_bytes(ogg_opus_muxer_handle_t muxer)
{
    return muxer ? muxer->bytes : 0;
}

void ogg_opus_muxer_destroy(ogg_opus_muxer_handle_t muxer)
{
    free(muxer);
}
