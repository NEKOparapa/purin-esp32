#include "day_schedule.h"
#include "terminal_http.h"
#include "terminal_network.h"
#include <stdio.h>
#include <string.h>

time_t day_schedule_bounds(time_t now, time_t *end)
{
    struct tm local;
    if (!localtime_r(&now, &local) || local.tm_year < 120 || local.tm_year > 199) return 0;
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    time_t start = mktime(&local);
    local.tm_mday++;
    local.tm_isdst = -1;
    *end = mktime(&local);
    return start > 0 && *end > start ? start : 0;
}

static bool utc_stamp(time_t when, char stamp[21])
{
    struct tm utc;
    return gmtime_r(&when, &utc) && strftime(stamp, 21, "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
}

// The API normalizes trigger_at to UTC. Anchor the wall time to the UTC dates
// covered by this local day, including when midnight falls on the previous UTC day.
static bool local_event_time(const char *stamp, time_t start, time_t end, char at[6])
{
    if (!stamp || strlen(stamp) < 20 || stamp[13] != ':' || stamp[16] != ':') return false;
    const char *zone = stamp + 19;
    if (*zone == '.') {
        size_t n = strspn(++zone, "0123456789");
        if (!n) return false;
        zone += n;
    }
    if (strcmp(zone, "Z") && strcmp(zone, "+00:00")) return false;
    int hour, minute, second;
    if (strspn(stamp + 11, "0123456789") != 2 || strspn(stamp + 14, "0123456789") != 2 ||
        strspn(stamp + 17, "0123456789") != 2 ||
        sscanf(stamp + 11, "%2d:%2d:%2d", &hour, &minute, &second) != 3 ||
        hour > 23 || minute > 59 || second > 59) return false;
    time_t anchors[] = {start, end - 1};
    for (unsigned i = 0; i < 2; ++i) {
        struct tm utc, local;
        char date[12];
        if (!gmtime_r(&anchors[i], &utc) || !strftime(date, sizeof(date), "%Y-%m-%dT", &utc) ||
            strncmp(stamp, date, 11)) continue;
        time_t instant = anchors[i] + (hour - utc.tm_hour) * 3600 +
                         (minute - utc.tm_min) * 60 + second - utc.tm_sec;
        return instant >= start && instant < end && localtime_r(&instant, &local) &&
               strftime(at, 6, "%H:%M", &local) == 5;
    }
    return false;
}

static void copy_preview(char out[321], const char *text)
{
    if (!text) return;
    if (strlen(text) < 321) { strcpy(out, text); return; }
    size_t n = 317;
    while (((unsigned char)text[n] & 0xc0) == 0x80) --n;
    memcpy(out, text, n);
    strcpy(out + n, "…");
}

static void encode_cursor(const char *source, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *source; ++source) {
        unsigned char c = (unsigned char)*source;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c == '.' || c == '~') *out++ = (char)c;
        else { *out++ = '%'; *out++ = hex[c >> 4]; *out++ = hex[c & 15]; }
    }
    *out = 0;
}

static const char *read_page(day_schedule_page_t *page, time_t end)
{
    char start_text[21], end_text[21], cursor[DAY_SCHEDULE_CURSOR * 3], path[448], reason[64];
    if (!utc_stamp(page->start, start_text) || !utc_stamp(end, end_text)) return "设备时间尚未同步";
    encode_cursor(page->cursor, cursor);
    int base = snprintf(path, sizeof(path), "/v1/schedule?start=%s&end=%s&include_notifications=1&limit=%d%s%s",
                        start_text, end_text, DAY_SCHEDULE_ROWS, *cursor ? "&cursor=" : "", cursor);
    if (base < 0 || (size_t)base + sizeof("&view=summary") > sizeof(path)) return "日程分页位置无效";
    strcpy(path + base, "&view=summary");
    int status = 0;
    cJSON *json = terminal_http_json_bounded(reason, sizeof(reason), HTTP_METHOD_GET, path, NULL,
                                             200, 24 * 1024, &status);
    if (!json && status == 400) {
        // Older servers reject "view". Keep their full response bounded.
        path[base] = 0;
        json = terminal_http_json_bounded(reason, sizeof(reason), HTTP_METHOD_GET, path, NULL,
                                          200, 896 * 1024, &status);
    }
    if (!json) return status == 401 || status == 403 ? "设备认证失败，请检查配对" : "日程加载失败，请刷新重试";
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(json, "next_cursor");
    const char *token = cJSON_GetStringValue(next);
    bool ok = cJSON_IsArray(items) && cJSON_GetArraySize(items) <= DAY_SCHEDULE_ROWS &&
              (cJSON_IsNull(next) || (token && *token && strlen(token) < sizeof(page->cursor) &&
                                       strcmp(token, page->cursor) > 0));
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (!ok) break;
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "title"));
        const char *at = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "trigger_at"));
        const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "action"));
        day_schedule_row_t *row = &page->rows[page->count];
        if (!at || !*at) { ok = false; break; }
        if (!local_event_time(at, page->start, end, row->at)) continue;
        if (!title || !*title || strlen(title) >= sizeof(row->title) || !action ||
            (strcmp(action, "notify_device") && strcmp(action, "notify_agent"))) { ok = false; break; }
        row->notice = !strcmp(action, "notify_device");
        strcpy(row->title, title);
        row->preview[0] = 0;
        copy_preview(row->preview, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "content")));
        page->count++;
    }
    if (ok) strlcpy(page->next_cursor, token ? token : "", sizeof(page->next_cursor));
    cJSON_Delete(json);
    return ok ? NULL : "日程返回格式有误，请刷新重试";
}

void day_schedule_fetch(day_schedule_page_t *page)
{
    if (!terminal_configured()) { page->error = "请先在设置中配对设备"; return; }
    if (terminal_network_mode() == TERMINAL_NETWORK_OFF) { page->error = "网络已关闭，请在设置中联网"; return; }
    for (;;) {
        if (terminal_network_acquire() != ESP_OK) { page->error = "未能连接，请检查网络后刷新"; return; }
        time_t end;
        time_t start = day_schedule_bounds(time(NULL), &end);
        if (!start || (page->start && start != page->start)) page->error = "请在设备时间同步后刷新";
        else {
            page->start = start;
            page->count = 0;
            page->error = read_page(page, end);
        }
        terminal_network_release();
        if (page->error || page->index == page->target || !*page->next_cursor) return;
        // When an older page falls outside the small UI cursor cache, follow
        // the server's ordered cursors from the nearest known position.
        strcpy(page->cursor, page->next_cursor);
        ++page->index;
    }
}
