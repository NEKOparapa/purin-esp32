#include "recording_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// 六位编号使目录和文件按名称排序即可还原顺序，且无需实时时钟。
#define MAX_SEQUENCE 999999U
#define SAMPLE_RATE 16000U

static int filesystem_error(void)
{
    return errno ? errno : EIO;
}

static int format_path(char *out, size_t capacity, const char *directory, const char *name)
{
    int length = snprintf(out, capacity, "%s/%s", directory, name);
    return length < 0 || (size_t)length >= capacity ? ENAMETOOLONG : 0;
}

static bool parse_sequence(const char *name, uint32_t *sequence)
{
    if (strlen(name) != 6) {
        return false;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(name[i] - '0');
    }
    *sequence = value;
    return value != 0;
}

static int write_and_sync(int fd, const char *text, size_t size)
{
    while (size) {
        ssize_t count = write(fd, text, size);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return filesystem_error();
        }
        if (!count) {
            return EIO;
        }
        text += count;
        size -= (size_t)count;
    }
    return fsync(fd) == 0 ? 0 : filesystem_error();
}

static int create_session_directory(recording_store_t *store, const char *mount_point)
{
    char root[RECORDING_STORE_PATH_MAX];
    int error = format_path(root, sizeof(root), mount_point, "recordings");
    if (error) {
        return error;
    }
    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        return filesystem_error();
    }
    DIR *dir = opendir(root);
    if (!dir) {
        return filesystem_error();
    }
    uint32_t maximum = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t sequence;
        // 数字同名普通文件也占用编号，避免后来创建目录时与旧内容冲突。
        if (parse_sequence(entry->d_name, &sequence) && sequence > maximum) {
            maximum = sequence;
        }
        errno = 0;
    }
    error = errno;
    if (closedir(dir) != 0 && !error) {
        error = filesystem_error();
    }
    if (error) {
        return error;
    }
    for (uint32_t candidate = maximum + 1; candidate <= MAX_SEQUENCE; ++candidate) {
        char name[7];
        snprintf(name, sizeof(name), "%06" PRIu32, candidate);
        error = format_path(store->directory, sizeof(store->directory), root, name);
        if (error) {
            return error;
        }
        if (mkdir(store->directory, 0700) == 0) {
            store->session_id = candidate;
            return 0;
        }
        if (errno != EEXIST) {
            return filesystem_error();
        }
        // 扫描后新增了同名目录则继续尝试，绝不复用别人的会话。
    }
    return EOVERFLOW;
}

int recording_store_open(recording_store_t *store, const char *mount_point)
{
    if (!store) {
        return EINVAL;
    }
    memset(store, 0, sizeof(*store));
    store->index_fd = -1;
    if (!mount_point || !mount_point[0]) {
        return EINVAL;
    }
    size_t mount_length = strlen(mount_point);
    if (mount_length > 1 && mount_point[mount_length - 1] == '/') {
        return EINVAL;
    }
    int error = create_session_directory(store, mount_point);
    if (error) {
        return error;
    }
    char index_path[RECORDING_STORE_PATH_MAX];
    error = format_path(index_path, sizeof(index_path), store->directory, "index.jsonl");
    if (error) {
        return error;
    }
    store->index_fd = open(index_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (store->index_fd < 0) {
        return filesystem_error();
    }
    char header[192];
    int length = snprintf(header, sizeof(header),
                          "{\"type\":\"session\",\"version\":1,\"session\":%" PRIu32
                          ",\"sample_rate\":16000,\"channels\":1,\"bitrate\":24000}\n",
                          store->session_id);
    if (length < 0 || (size_t)length >= sizeof(header)) {
        error = EOVERFLOW;
    } else {
        error = write_and_sync(store->index_fd, header, (size_t)length);
    }
    if (error) {
        recording_store_close(store);
        return error;
    }
    store->next_sequence = 1;
    return 0;
}

int recording_store_paths(const recording_store_t *store,
                          char *part, size_t part_capacity,
                          char *completed, size_t completed_capacity)
{
    if (!store || store->index_fd < 0 || !part || !completed ||
        !part_capacity || !completed_capacity || !store->next_sequence) {
        return EINVAL;
    }
    if (store->next_sequence > MAX_SEQUENCE) {
        return EOVERFLOW;
    }
    char name[24];
    snprintf(name, sizeof(name), "%06" PRIu32 ".ogg.part", store->next_sequence);
    int error = format_path(part, part_capacity, store->directory, name);
    if (error) {
        return error;
    }
    snprintf(name, sizeof(name), "%06" PRIu32 ".ogg", store->next_sequence);
    return format_path(completed, completed_capacity, store->directory, name);
}

int recording_store_commit(recording_store_t *store, uint32_t sequence,
                           uint64_t samples, uint64_t bytes)
{
    if (!store || store->index_fd < 0 || sequence != store->next_sequence ||
        !samples || samples > UINT64_MAX / 1000 || !bytes) {
        return EINVAL;
    }
    if (UINT64_MAX - store->next_sample < samples) {
        return EOVERFLOW;
    }
    char part[RECORDING_STORE_PATH_MAX];
    char completed[RECORDING_STORE_PATH_MAX];
    int error = recording_store_paths(store, part, sizeof(part), completed, sizeof(completed));
    if (error) {
        return error;
    }
    struct stat info;
    if (stat(completed, &info) == 0) {
        return EEXIST;
    }
    if (errno != ENOENT) {
        return filesystem_error();
    }
    if (stat(part, &info) != 0) {
        return filesystem_error();
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size != bytes) {
        return EINVAL;
    }
    char line[256];
    int length = snprintf(line, sizeof(line),
                          "{\"type\":\"segment\",\"sequence\":%" PRIu32
                          ",\"file\":\"%06" PRIu32 ".ogg\",\"start_sample\":%" PRIu64
                          ",\"samples\":%" PRIu64 ",\"duration_ms\":%" PRIu64
                          ",\"bytes\":%" PRIu64 "}\n",
                          sequence, sequence, store->next_sample, samples,
                          samples * 1000 / SAMPLE_RATE, bytes);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        return EOVERFLOW;
    }
    if (rename(part, completed) != 0) {
        return filesystem_error();
    }
    error = write_and_sync(store->index_fd, line, (size_t)length);
    if (error) {
        // 音频已改名但索引尚未确认；保留现场，调用方应停止本次录音。
        return error;
    }
    ++store->next_sequence;
    store->next_sample += samples;
    return 0;
}

int recording_store_close(recording_store_t *store)
{
    if (!store) {
        return EINVAL;
    }
    int fd = store->index_fd;
    store->index_fd = -1;
    return fd >= 0 && close(fd) != 0 ? filesystem_error() : 0;
}
