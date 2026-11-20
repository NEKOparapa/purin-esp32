#include "app_media.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool cancelled;
static bool busy;
static const char *state = "idle";
static const char *failure = "";
static char track[65];
static int volume;

static bool wav_name(const char *s)
{
    size_t len = strlen(s);
    return len > 4 && len <= 64 && (isalnum((unsigned char)*s) || *s == '_' || *s == '-') &&
        strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_. -") == len &&
        !strcasecmp(s + len - 4, ".wav");
}

static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }

static bool wav_header(FILE *f, unsigned *channels, uint32_t *bytes)
{
    uint8_t h[16];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    uint32_t left = le32(h + 4);
    if (left < 4 || left > INT32_MAX) return false;
    left -= 4;
    while (left >= 8 && fread(h, 1, 8, f) == 8) {
        uint32_t len = le32(h + 4);
        left -= 8;
        if (len > left || len + (len & 1u) > left) return false;
        uint32_t skip = len + (len & 1u);
        left -= skip;
        if (!memcmp(h, "fmt ", 4)) {
            if (len < 16 || fread(h, 1, 16, f) != 16 || le16(h) != 1 ||
                le32(h + 4) != 16000 || le16(h + 14) != 16) return false;
            *channels = le16(h + 2);
            if ((*channels != 1 && *channels != 2) || le16(h + 12) != 2 * *channels) return false;
            skip -= 16;
        } else if (!memcmp(h, "data", 4)) {
            *bytes = len;
            return *channels && len && len % (2 * *channels) == 0;
        }
        if (skip && fseek(f, (long)skip, SEEK_CUR)) return false;
    }
    return false;
}

static const char *stream(FILE *file, unsigned channels, uint32_t frames, int gain)
{
    int16_t pcm[160 * 2];
    int32_t stereo[160 * 2];
    for (uint32_t at = 0; at < frames && !atomic_load(&cancelled);) {
        unsigned count = frames - at < 160 ? frames - at : 160;
        if (file && fread(pcm, sizeof(int16_t) * channels, count, file) != count) return "WAV read failed";
        for (unsigned i = 0; i < count; i++) {
            int left, right;
            if (file) {
                left = pcm[i * channels];
                right = pcm[i * channels + channels - 1];
            } else {
                float t = (at + i) / 16000.0f;
                left = right = (int)(sinf(6.2831853f * (t < .16f ? 660 : 880) * t) *
                                    12000 * (1 - (at + i) / (float)frames));
            }
            stereo[2*i] = (int32_t)((int64_t)left * gain * 65536 / 100);
            stereo[2*i+1] = (int32_t)((int64_t)right * gain * 65536 / 100);
        }
        if (board_speaker_write(stereo, count) != ESP_OK) return "I2S playback failed";
        at += count;
    }
    // Flush the last samples through the six 240-frame DMA buffers before disabling the PA.
    memset(stereo, 0, sizeof(stereo));
    for (int i = 0; i < 10 && !atomic_load(&cancelled); ++i)
        if (board_speaker_write(stereo, 160) != ESP_OK) return "I2S drain failed";
    return "";
}

static void playback_task(void *unused)
{
    (void)unused;
    FILE *file = NULL;
    bool mounted = false, started = false;
    const char *err = "";
    unsigned channels = 1;
    uint32_t frames = 6400;
    if (track[0]) {
        board_sdcard_info_t info;
        mounted = board_sdcard_mount(&info) == ESP_OK;
        if (!mounted) err = "SD card busy or unavailable";
        else {
            char path[120];
            snprintf(path, sizeof(path), "%s/%s", AUDIO_DIR, track);
            file = fopen(path, "rb");
            uint32_t bytes = 0;
            channels = 0;
            if (!file) err = "WAV file not found";
            else if (!wav_header(file, &channels, &bytes)) err = "Expected 16 kHz PCM16 mono/stereo WAV";
            else frames = bytes / (channels * 2);
        }
    }
    if (!*err && !atomic_load(&cancelled)) {
        started = board_speaker_start() == ESP_OK;
        if (!started) err = "Speaker initialization failed";
        else {
            portENTER_CRITICAL(&guard);
            state = "playing";
            portEXIT_CRITICAL(&guard);
            err = stream(file, channels, frames, volume);
        }
    }
    if (started && board_speaker_stop() != ESP_OK) err = "Speaker shutdown failed";
    if (file) fclose(file);
    if (mounted && board_sdcard_unmount() != ESP_OK) err = "SD card unmount failed";
    portENTER_CRITICAL(&guard);
    state = *err ? "failed" : atomic_load(&cancelled) ? "stopped" : "finished";
    failure = err;
    busy = false;
    board_audio_release();
    portEXIT_CRITICAL(&guard);
    vTaskDelete(NULL);
}

void player_stop(void)
{
    portENTER_CRITICAL(&guard);
    if (busy) atomic_store(&cancelled, true);
    portEXIT_CRITICAL(&guard);
}

void player_description(char *value, size_t size)
{
    portENTER_CRITICAL(&guard);
    const char *s = state;
    const char *err = failure;
    bool stopping = busy && atomic_load(&cancelled);
    portEXIT_CRITICAL(&guard);
    const char *text = stopping ? "正在停止" : !strcmp(s, "idle") ? "就绪" :
        !strcmp(s, "starting") ? "正在准备播放" : !strcmp(s, "playing") ? "正在播放" :
        !strcmp(s, "finished") ? "播放完成" : !strcmp(s, "failed") ? err : "已停止";
    strlcpy(value, text, size);
}

static cJSON *player_status(void)
{
    char name[65];
    portENTER_CRITICAL(&guard);
    const char *status = busy && atomic_load(&cancelled) ? "stopping" : state;
    const char *err = failure;
    strlcpy(name, track, sizeof(name));
    portEXIT_CRITICAL(&guard);
    cJSON *out = app_result(status);
    if (out) {
        cJSON_AddStringToObject(out, "file", name);
        if (*err) cJSON_AddStringToObject(out, "error", err);
    }
    return out;
}

cJSON *player_call(const cJSON *args, char *error, size_t size)
{
    const char *action = app_text(args, "action", 16);
    if (!action) return app_error(error, size, "Missing player action");
    if (!strcmp(action, "list")) {
        board_sdcard_info_t info;
        if (board_sdcard_mount(&info) != ESP_OK) return app_error(error, size, "SD card busy or unavailable");
        cJSON *out = media_list(AUDIO_DIR, app_number(args, "offset", 0, 65535, 0), wav_name,
                                cJSON_CreateString, error, size);
        if (board_sdcard_unmount() != ESP_OK) {
            cJSON_Delete(out);
            return app_error(error, size, "SD unmount failed");
        }
        return out;
    }
    if (!strcmp(action, "stop")) player_stop();
    else if (!strcmp(action, "play")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "file");
        const char *name = v ? cJSON_GetStringValue(v) : "";
        int gain = app_number(args, "volume", 1, 100, 45);
        if (!name || (*name && !wav_name(name)) || gain < 1)
            return app_error(error, size, "Invalid WAV file name or volume");
        if (!board_audio_reserve()) return app_error(error, size, "Audio is in use");
        portENTER_CRITICAL(&guard);
        strlcpy(track, name, sizeof(track));
        volume = gain;
        state = "starting"; failure = ""; busy = true;
        atomic_store(&cancelled, false);
        portEXIT_CRITICAL(&guard);
        if (xTaskCreate(playback_task, "speaker", 6144, NULL, 3, NULL) != pdPASS) {
            portENTER_CRITICAL(&guard);
            busy = false; state = "failed"; failure = "Out of memory";
            board_audio_release();
            portEXIT_CRITICAL(&guard);
            return app_error(error, size, "Cannot start player");
        }
    } else if (strcmp(action, "status")) return app_error(error, size, "Unknown player action");
    return player_status();
}
