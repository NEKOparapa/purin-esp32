#pragma once

#include "device_apps.h"
#include "book_cover.h"

enum { LIBRARY_SHELF_PAGE_SIZE = 4, LIBRARY_FILENAME_SIZE = 256 };

typedef struct {
    bool local;
    char name[LIBRARY_FILENAME_SIZE];
} library_book_t;

typedef struct {
    char title[LIBRARY_FILENAME_SIZE];
    char *text;
    char bookmark_key[16];
    uint32_t number, start, length, total, resume;
    unsigned version;
    bool previous, next;
} library_section_t;

cJSON *library_shelf(int offset, book_cover_t *covers[LIBRARY_SHELF_PAGE_SIZE], char *error, size_t size);
unsigned library_shelf_version(void);
bool library_section_current(const library_section_t *section);
// Section zero restores a saved position. Other section numbers start at one.
bool library_section_load(const library_book_t *book, uint32_t number,
                          library_section_t *section, char *error, size_t size);
bool library_save_position(const library_book_t *book, const library_section_t *section,
                           uint32_t offset);
