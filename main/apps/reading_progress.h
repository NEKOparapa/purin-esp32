#pragma once

#include "library.h"

#define COLLECTION_PREFS_NAMESPACE "pr_collection"
#define READING_PROGRESS_PREF "reader_upload"

// Called from the UI task at boot and when the upload preference is enabled.
void reading_progress_wake(void);

// Save a reading update for background upload, using the end of the visible page.
bool reading_progress_submit(const library_book_t *book, const library_section_t *section,
                             uint32_t page_end, uint32_t page, uint32_t pages);
