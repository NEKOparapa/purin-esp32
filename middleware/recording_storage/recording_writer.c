#include "recording_writer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include "recording_store.h"
#include "ogg_opus.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "recording_storage";
_Static_assert((int)RECORDING_WRITER_SAMPLE_RATE == (int)OGG_OPUS_SAMPLE_RATE, "sample rate mismatch");

struct recording_writer {
    recording_store_t store;
    ogg_opus_handle_t audio;
    uint64_t segment_limit;
    uint64_t segment_samples;
    recording_write_result_t failure;
    recording_writer_stats_t stats;
};

static recording_write_result_t from_storage(int error)
{
    return error == ENOSPC ? RECORDING_WRITE_CARD_FULL : RECORDING_WRITE_STORAGE_ERROR;
}

static recording_write_result_t from_codec(esp_err_t error, int saved_errno)
{
    if (error == ESP_ERR_NO_MEM) {
        return RECORDING_WRITE_NO_MEMORY;
    }
    return saved_errno ? from_storage(saved_errno) : RECORDING_WRITE_ENCODER_ERROR;
}

static recording_write_result_t remember(recording_writer_handle_t writer, recording_write_result_t error)
{
    if (writer->failure == RECORDING_WRITE_OK) {
        writer->failure = error;
    }
    return writer->failure;
}

static recording_write_result_t open_segment(recording_writer_handle_t writer)
{
    char part[RECORDING_STORE_PATH_MAX];
    char completed[RECORDING_STORE_PATH_MAX];
    int error = recording_store_paths(&writer->store, part, sizeof(part), completed, sizeof(completed));
    if (error) {
        return remember(writer, from_storage(error));
    }
    errno = 0;
    esp_err_t result = ogg_opus_open(part, esp_random(), &writer->audio);
    return result == ESP_OK ? RECORDING_WRITE_OK : remember(writer, from_codec(result, errno));
}

static recording_write_result_t commit_segment(recording_writer_handle_t writer)
{
    ogg_opus_result_t result = {0};
    errno = 0;
    esp_err_t error = ogg_opus_finish(&writer->audio, &result);
    const int saved_errno = errno;
    if (error != ESP_OK) {
        return remember(writer, from_codec(error, saved_errno));
    }
    if (result.samples != writer->segment_samples) {
        return remember(writer, RECORDING_WRITE_ENCODER_ERROR);
    }
    const uint32_t sequence = writer->store.next_sequence;
    int storage_error = recording_store_commit(&writer->store, sequence, result.samples, result.bytes);
    if (storage_error) {
        return remember(writer, from_storage(storage_error));
    }
    ++writer->stats.saved_segments;
    writer->segment_samples = 0;
    ESP_LOGI(TAG, "Saved session %06" PRIu32 " segment %06" PRIu32 ": %" PRIu64
                 " samples, %" PRIu64 " bytes", writer->stats.session_id, sequence, result.samples, result.bytes);
    return RECORDING_WRITE_OK;
}

recording_write_result_t recording_writer_open(const char *root, uint64_t segment_samples,
                                               recording_writer_handle_t *out)
{
    if (!out) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (!root || !root[0] || !segment_samples || segment_samples > UINT64_MAX / 1000) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    recording_writer_handle_t writer = calloc(1, sizeof(*writer));
    if (!writer) {
        return RECORDING_WRITE_NO_MEMORY;
    }
    int error = recording_store_open(&writer->store, root);
    if (error) {
        free(writer);
        return from_storage(error);
    }
    writer->segment_limit = segment_samples;
    writer->stats.session_id = writer->store.session_id;
    *out = writer;
    return RECORDING_WRITE_OK;
}

recording_write_result_t recording_writer_write(recording_writer_handle_t writer,
                                                const int16_t *pcm, size_t count)
{
    if (!writer || (!pcm && count)) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    if (writer->failure != RECORDING_WRITE_OK) {
        return writer->failure;
    }
    if ((uint64_t)count > UINT64_MAX - writer->stats.samples) {
        return remember(writer, RECORDING_WRITE_INVALID_ARGUMENT);
    }
    while (count) {
        if (!writer->audio && open_segment(writer) != RECORDING_WRITE_OK) {
            return writer->failure;
        }
        const uint64_t remaining = writer->segment_limit - writer->stats.samples % writer->segment_limit;
        const size_t amount = remaining < count ? (size_t)remaining : count;
        errno = 0;
        esp_err_t error = ogg_opus_write(writer->audio, pcm, amount);
        if (error != ESP_OK) {
            return remember(writer, from_codec(error, errno));
        }
        writer->segment_samples += amount;
        writer->stats.samples += amount;
        pcm += amount;
        count -= amount;
        if (writer->stats.samples % writer->segment_limit == 0 && commit_segment(writer) != RECORDING_WRITE_OK) {
            return writer->failure;
        }
    }
    return RECORDING_WRITE_OK;
}

recording_write_result_t recording_writer_checkpoint(recording_writer_handle_t writer)
{
    if (!writer) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    if (writer->failure != RECORDING_WRITE_OK) {
        return writer->failure;
    }
    return writer->audio && writer->segment_samples ? commit_segment(writer) : RECORDING_WRITE_OK;
}

recording_write_result_t recording_writer_sync(recording_writer_handle_t writer)
{
    if (!writer) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    if (writer->failure != RECORDING_WRITE_OK) {
        return writer->failure;
    }
    if (writer->audio) {
        errno = 0;
        esp_err_t error = ogg_opus_sync(writer->audio);
        if (error != ESP_OK) {
            return remember(writer, from_codec(error, errno));
        }
    }
    return RECORDING_WRITE_OK;
}

recording_writer_stats_t recording_writer_get_stats(recording_writer_handle_t writer)
{
    return writer ? writer->stats : (recording_writer_stats_t) {0};
}

static recording_write_result_t release(recording_writer_handle_t *handle, recording_writer_stats_t *stats)
{
    recording_writer_handle_t writer = *handle;
    if (writer->audio) {
        errno = 0;
        esp_err_t error = ogg_opus_abort(&writer->audio);
        if (error != ESP_OK) {
            remember(writer, from_codec(error, errno));
        }
    }
    int error = recording_store_close(&writer->store);
    if (error) {
        remember(writer, from_storage(error));
    }
    if (stats) {
        *stats = writer->stats;
    }
    recording_write_result_t result = writer->failure;
    free(writer);
    *handle = NULL;
    return result;
}

recording_write_result_t recording_writer_finish(recording_writer_handle_t *handle,
                                                 recording_writer_stats_t *stats)
{
    if (!handle || !*handle) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    recording_writer_handle_t writer = *handle;
    if (writer->failure == RECORDING_WRITE_OK && writer->audio && writer->segment_samples) {
        commit_segment(writer);
    }
    return release(handle, stats);
}

recording_write_result_t recording_writer_abort(recording_writer_handle_t *handle,
                                                recording_writer_stats_t *stats)
{
    if (!handle || !*handle) {
        return RECORDING_WRITE_INVALID_ARGUMENT;
    }
    return release(handle, stats);
}
