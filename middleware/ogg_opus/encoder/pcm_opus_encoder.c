#include "pcm_opus_encoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"

struct pcm_opus_encoder {
    OpusEncoder *codec;
    pcm_opus_packet_fn output;
    void *context;
    pcm_opus_encoder_info_t info;
    size_t pcm_count;
    int16_t pcm[PCM_OPUS_FRAME_SAMPLES];
    uint8_t packet[1500];
    esp_err_t failure;
    bool finished;
};

static esp_err_t fail(pcm_opus_encoder_handle_t encoder, esp_err_t error)
{
    if (encoder->failure == ESP_OK) {
        encoder->failure = error;
    }
    return encoder->failure;
}

static esp_err_t encode_frame(pcm_opus_encoder_handle_t encoder)
{
    int length = opus_encode(encoder->codec, encoder->pcm, PCM_OPUS_FRAME_SAMPLES,
                             encoder->packet, sizeof(encoder->packet));
    if (length <= 0) {
        return fail(encoder, ESP_FAIL);
    }
    encoder->info.encoded_samples += PCM_OPUS_FRAME_SAMPLES;
    encoder->pcm_count = 0;
    return fail(encoder, encoder->output(encoder->context, encoder->packet,
                                        (size_t)length, encoder->info.encoded_samples));
}

esp_err_t pcm_opus_encoder_create(pcm_opus_packet_fn output, void *context,
                                  pcm_opus_encoder_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!output) {
        return ESP_ERR_INVALID_ARG;
    }
    pcm_opus_encoder_handle_t encoder = calloc(1, sizeof(*encoder));
    if (!encoder) {
        return ESP_ERR_NO_MEM;
    }
    int opus_error = OPUS_OK;
    encoder->codec = opus_encoder_create(PCM_OPUS_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    if (!encoder->codec || opus_error != OPUS_OK) {
        pcm_opus_encoder_destroy(encoder);
        return opus_error == OPUS_ALLOC_FAIL ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    opus_int32 delay = 0; // Xtensa 上 opus_int32 可能是 long，不能使用 int* 传入 CTL。
    if (opus_encoder_ctl(encoder->codec, OPUS_SET_BITRATE(PCM_OPUS_BITRATE)) != OPUS_OK ||
        opus_encoder_ctl(encoder->codec, OPUS_SET_VBR(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->codec, OPUS_SET_COMPLEXITY(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->codec, OPUS_SET_DTX(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->codec, OPUS_SET_INBAND_FEC(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->codec, OPUS_GET_LOOKAHEAD(&delay)) != OPUS_OK || delay <= 0) {
        pcm_opus_encoder_destroy(encoder);
        return ESP_FAIL;
    }
    encoder->info.delay_samples = (uint32_t)delay;
    encoder->output = output;
    encoder->context = context;
    *out = encoder;
    return ESP_OK;
}

esp_err_t pcm_opus_encoder_write(pcm_opus_encoder_handle_t encoder, const int16_t *pcm, size_t count)
{
    if (!encoder || (!pcm && count)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (encoder->failure != ESP_OK) {
        return encoder->failure;
    }
    if (encoder->finished) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count > UINT64_MAX - PCM_OPUS_FRAME_SAMPLES - encoder->info.delay_samples - encoder->info.real_samples) {
        return fail(encoder, ESP_ERR_INVALID_SIZE);
    }
    while (count) {
        size_t amount = PCM_OPUS_FRAME_SAMPLES - encoder->pcm_count;
        if (amount > count) {
            amount = count;
        }
        memcpy(encoder->pcm + encoder->pcm_count, pcm, amount * sizeof(*pcm));
        encoder->pcm_count += amount;
        encoder->info.real_samples += amount;
        pcm += amount;
        count -= amount;
        if (encoder->pcm_count == PCM_OPUS_FRAME_SAMPLES) {
            esp_err_t error = encode_frame(encoder);
            if (error != ESP_OK) {
                return error;
            }
        }
    }
    return ESP_OK;
}

esp_err_t pcm_opus_encoder_finish(pcm_opus_encoder_handle_t encoder)
{
    if (!encoder) {
        return ESP_ERR_INVALID_ARG;
    }
    if (encoder->failure != ESP_OK) {
        return encoder->failure;
    }
    if (encoder->finished || !encoder->info.real_samples) {
        return fail(encoder, ESP_ERR_INVALID_STATE);
    }
    const uint64_t final_samples = encoder->info.real_samples + encoder->info.delay_samples;
    while (encoder->info.encoded_samples < final_samples) {
        memset(encoder->pcm + encoder->pcm_count, 0,
               (PCM_OPUS_FRAME_SAMPLES - encoder->pcm_count) * sizeof(encoder->pcm[0]));
        esp_err_t error = encode_frame(encoder);
        if (error != ESP_OK) {
            return error;
        }
    }
    encoder->finished = true;
    return ESP_OK;
}

pcm_opus_encoder_info_t pcm_opus_encoder_info(pcm_opus_encoder_handle_t encoder)
{
    return encoder ? encoder->info : (pcm_opus_encoder_info_t){0};
}

void pcm_opus_encoder_destroy(pcm_opus_encoder_handle_t encoder)
{
    if (encoder) {
        if (encoder->codec) {
            opus_encoder_destroy(encoder->codec);
        }
        free(encoder);
    }
}
