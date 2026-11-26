#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEDUP_WINDOW_SIZE 64u

typedef struct {
    uint8_t sender_id;
    uint32_t msg_id;
    bool valid;
} dedup_entry_t;

typedef struct {
    dedup_entry_t entries[DEDUP_WINDOW_SIZE];
    size_t write_idx;
} dedup_filter_t;

void dedup_filter_init(dedup_filter_t *filter);
bool dedup_filter_contains(const dedup_filter_t *filter, uint8_t sender_id, uint32_t msg_id);
/**
 * @brief Checks if (sender_id, msg_id) is already in the sliding window; inserts if unseen.
 * @return true if duplicate (already seen), false if new.
 */
bool dedup_filter_check_and_insert(dedup_filter_t *filter, uint8_t sender_id, uint32_t msg_id);

#ifdef __cplusplus
}
#endif
