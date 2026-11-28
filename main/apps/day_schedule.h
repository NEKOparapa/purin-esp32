#pragma once

#include <stdbool.h>
#include <time.h>

enum { DAY_SCHEDULE_ROWS = 4, DAY_SCHEDULE_CURSOR = 96 };

typedef struct {
    char at[6], title[256 * 4 + 1], preview[321];
    bool notice;
} day_schedule_row_t;

typedef struct {
    time_t start;
    char cursor[DAY_SCHEDULE_CURSOR], next_cursor[DAY_SCHEDULE_CURSOR];
    unsigned index, target; // Zero-based page of the cursor and page requested by the UI.
    const char *error;
    unsigned count;
    day_schedule_row_t rows[DAY_SCHEDULE_ROWS];
} day_schedule_page_t;

// A zero result means the board has not established local civil time yet.
time_t day_schedule_bounds(time_t now, time_t *end);
// Runs on a worker; obtains a network lease and uses the paired device's calendar.
void day_schedule_fetch(day_schedule_page_t *page);
