#include "ogg_opus.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "pcm_opus_encoder.h"
#include "ogg_opus_muxer.h"

_Static_assert((int)OGG_OPUS_SAMPLE_RATE == (int)PCM_OPUS_SAMPLE_RATE, "sample rate mismatch");
_Static_assert((int)OGG_OPUS_FRAME_SAMPLES == (int)PCM_OPUS_FRAME_SAMPLES, "frame size mismatch");
_Static_assert((int)OGG_OPUS_BITRATE == (int)PCM_OPUS_BITRATE, "bitrate mismatch");

struct ogg_opus_writer {
    FILE *file;
    pcm_opus_encoder_handle_t encoder;
    ogg_opus_muxer_handle_t muxer;
    esp_err_t failure;
    int failure_errno;
};

// 仅文件失败携带 errno；清理或后续调用不能覆盖首次失败的原因。
static esp_err_t fail(ogg_opus_handle_t writer, esp_err_t error, int system_error)
{
    if (writer->failure == ESP_OK && error != ESP_OK) {
        writer->failure = error;
        writer->failure_errno = system_error;
    }
    if (writer->failure != ESP_OK) {
        errno = writer->failure_errno;
    }
    return writer->failure;
}

// 文件输出边界：编码器和容器都不接触 FILE 或文件路径。
static esp_err_t write_bytes(void *context, const uint8_t *data, size_t bytes)
{
    ogg_opus_handle_t writer = context;
    return fwrite(data, 1, bytes, writer->file) == bytes ? ESP_OK :
           fail(writer, ESP_FAIL, errno ? errno : EIO);
}

// 数据流连接点：编码层使用输入采样率，容器层统一使用 48 kHz 时间基。
static esp_err_t write_packet(void *context, const uint8_t *packet, size_t bytes, uint64_t samples)
{
    ogg_opus_handle_t writer = context;
    if (samples > UINT64_MAX / 3) {
        return fail(writer, ESP_ERR_INVALID_SIZE, 0);
    }
    return fail(writer, ogg_opus_muxer_packet(writer->muxer, packet, bytes, samples * 3), 0);
}

static esp_err_t sync_file(ogg_opus_handle_t writer)
{
    if (fflush(writer->file) != 0 || fsync(fileno(writer->file)) != 0) {
        return fail(writer, ESP_FAIL, errno ? errno : EIO);
    }
    return ESP_OK;
}

static void release_writer(ogg_opus_handle_t writer)
{
    pcm_opus_encoder_destroy(writer->encoder);
    ogg_opus_muxer_destroy(writer->muxer);
    free(writer);
}

esp_err_t ogg_opus_open(const char *path, uint32_t serial, ogg_opus_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!path || !*path) {
        return ESP_ERR_INVALID_ARG;
    }
    ogg_opus_handle_t writer = calloc(1, sizeof(*writer));
    if (!writer) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = pcm_opus_encoder_create(write_packet, writer, &writer->encoder);
    if (error != ESP_OK) {
        release_writer(writer);
        errno = 0;
        return error;
    }
    const pcm_opus_encoder_info_t info = pcm_opus_encoder_info(writer->encoder);
    if (info.delay_samples > UINT16_MAX / 3) {
        release_writer(writer);
        errno = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef _WIN32
    flags |= _O_BINARY;
#endif
    int fd = open(path, flags, 0666);
    if (fd < 0) {
        const int saved_errno = errno;
        release_writer(writer);
        errno = saved_errno;
        return ESP_FAIL;
    }
    writer->file = fdopen(fd, "wb");
    if (!writer->file) {
        const int saved_errno = errno;
        close(fd);
        release_writer(writer);
        errno = saved_errno;
        return ESP_ERR_NO_MEM;
    }
    const ogg_opus_muxer_config_t config = {
        .serial = serial,
        .input_sample_rate = OGG_OPUS_SAMPLE_RATE,
        .pre_skip = (uint16_t)(info.delay_samples * 3),
        .channels = 1,
    };
    error = ogg_opus_muxer_create(&config, write_bytes, writer, &writer->muxer);
    if (error != ESP_OK) {
        fail(writer, error, 0);
        ogg_opus_abort(&writer);
        return error;
    }
    *out = writer;
    return ESP_OK;
}

esp_err_t ogg_opus_write(ogg_opus_handle_t writer, const int16_t *pcm, size_t count)
{
    if (!writer || (!pcm && count)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->failure != ESP_OK) {
        return fail(writer, writer->failure, 0);
    }
    const pcm_opus_encoder_info_t info = pcm_opus_encoder_info(writer->encoder);
    if (count > UINT64_MAX / 3 - OGG_OPUS_FRAME_SAMPLES - info.delay_samples - info.real_samples) {
        return fail(writer, ESP_ERR_INVALID_SIZE, 0);
    }
    return fail(writer, pcm_opus_encoder_write(writer->encoder, pcm, count), 0);
}

esp_err_t ogg_opus_sync(ogg_opus_handle_t writer)
{
    if (!writer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->failure != ESP_OK) {
        return fail(writer, writer->failure, 0);
    }
    esp_err_t error = ogg_opus_muxer_flush(writer->muxer);
    return fail(writer, error == ESP_OK ? sync_file(writer) : error, 0);
}

esp_err_t ogg_opus_abort(ogg_opus_handle_t *handle)
{
    if (!handle || !*handle) {
        return ESP_ERR_INVALID_ARG;
    }
    ogg_opus_handle_t writer = *handle;
    *handle = NULL;
    if (fclose(writer->file) != 0) {
        fail(writer, ESP_FAIL, errno ? errno : EIO);
    }
    const esp_err_t error = writer->failure;
    const int saved_errno = writer->failure_errno;
    release_writer(writer);
    if (error != ESP_OK) {
        errno = saved_errno;
    }
    return error;
}

esp_err_t ogg_opus_finish(ogg_opus_handle_t *handle, ogg_opus_result_t *out)
{
    if (!handle || !*handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (ogg_opus_result_t){0};
    ogg_opus_handle_t writer = *handle;
    esp_err_t error = writer->failure;
    if (error == ESP_OK) {
        error = fail(writer, pcm_opus_encoder_finish(writer->encoder), 0);
    }
    const pcm_opus_encoder_info_t info = pcm_opus_encoder_info(writer->encoder);
    if (error == ESP_OK) {
        error = fail(writer, ogg_opus_muxer_finish(writer->muxer,
                                                (info.real_samples + info.delay_samples) * 3), 0);
    }
    if (error == ESP_OK) {
        error = sync_file(writer);
    }
    const ogg_opus_result_t result = {
        .samples = info.real_samples,
        .bytes = ogg_opus_muxer_bytes(writer->muxer),
    };
    esp_err_t close_error = ogg_opus_abort(handle);
    if (error == ESP_OK) {
        error = close_error;
    }
    if (error == ESP_OK) {
        *out = result;
    }
    return error;
}
