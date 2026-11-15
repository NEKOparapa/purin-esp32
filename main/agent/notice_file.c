#include "notice_file.h"
#include "board.h"
#include "terminal_http.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_random.h"

static bool safe_id(const char *id)
{
    return id && *id && strlen(id) < 80 &&
           strspn(id, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") ==
               strlen(id);
}

static void file_name(char out[128], const char *input)
{
    size_t n = 0;
    while (*input) {
        unsigned char c = (unsigned char)*input;
        size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (n + bytes >= 128 || strnlen(input, bytes) < bytes)
            break;
        if (bytes == 1 && (c < 32 || strchr("/\\:*?\"<>|", c)))
            out[n] = '_';
        else
            memcpy(out + n, input, bytes);
        n += bytes;
        input += bytes;
    }
    while (n && (out[n - 1] == '.' || out[n - 1] == ' '))
        --n;
    out[n] = 0;
    if (!n)
        strlcpy(out, "attachment", 128);
}

void notice_save_file(const char *notice_id, const char *file_id, const char *name, int64_t size,
                      char *message, size_t capacity)
{
    if (!safe_id(notice_id) || !safe_id(file_id) || !name || size < 0 || size > 8 * 1024 * 1024) {
        strlcpy(message, "Attachment metadata is invalid", capacity);
        return;
    }
    board_sdcard_info_t info;
    esp_err_t mounted = board_sdcard_mount(&info);
    if (mounted != ESP_OK) {
        strlcpy(message,
                mounted == ESP_ERR_INVALID_STATE ? "SD card busy. Retry after recording."
                                                 : "Insert an SD card and try again.",
                capacity);
        return;
    }

    char dir[48], output[280], temp[96] = "", base[128], route[230];
    snprintf(dir, sizeof(dir), "%s/Wintermute", board_sdcard_path());
    file_name(base, name);
    snprintf(output, sizeof(output), "%s/%s-%s", dir, file_id, base);
    snprintf(route, sizeof(route), "/v1/notifications/%s/files/%s", notice_id, file_id);
    esp_http_client_handle_t client = NULL;
    FILE *file = NULL;
    bool unsaved = false;
    struct stat existing;
    const char *status = "Could not write to SD card";
    if (mkdir(dir, 0777) != 0 && errno != EEXIST)
        goto done;
    if (stat(output, &existing) == 0) {
        if (S_ISREG(existing.st_mode) && existing.st_size == size) {
            status = "File already exists in SD /Wintermute";
            goto done;
        }
        snprintf(output, sizeof(output), "%s/%s-%08" PRIx32 "-%s", dir, file_id, esp_random(),
                 base);
    }
    client = terminal_http_client(route, HTTP_METHOD_GET, NULL, NULL);
    status = "Download failed. Tap the file to retry.";
    if (!client || esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) != size ||
        esp_http_client_get_status_code(client) != 200)
        goto done;
    snprintf(temp, sizeof(temp), "%s/wm-%08" PRIx32 ".tmp", dir, esp_random());
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        goto done;
    unsaved = true;
    file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        goto done;
    }
    char block[2048];
    int64_t copied = 0;
    while (copied < size) {
        int count = esp_http_client_read(
            client, block,
            (int)((size - copied) < sizeof(block) ? (size - copied) : sizeof(block)));
        if (count <= 0 || fwrite(block, 1, count, file) != (size_t)count)
            goto done;
        copied += count;
    }
    if (size && !esp_http_client_is_complete_data_received(client))
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    if (rename(temp, output) != 0)
        goto done;
    unsaved = false;
    status = "Saved to SD /Wintermute";
done:
    if (file)
        fclose(file);
    if (unsaved)
        unlink(temp);
    if (client)
        esp_http_client_cleanup(client);
    if (board_sdcard_unmount() != ESP_OK)
        status = "SD card did not unmount cleanly";
    strlcpy(message, status, capacity);
}
