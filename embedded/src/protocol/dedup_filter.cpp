#include "dedup_filter.h"
#include <string.h>

void dedup_filter_init(dedup_filter_t *filter)
{
    if (!filter) {
        return;
    }
    memset(filter, 0, sizeof(*filter));
}

bool dedup_filter_contains(const dedup_filter_t *filter, uint8_t sender_id, uint32_t msg_id)
{
    if (!filter) {
        return false;
    }
    for (size_t i = 0; i < DEDUP_WINDOW_SIZE; ++i) {
        if (filter->entries[i].valid &&
            filter->entries[i].sender_id == sender_id &&
            filter->entries[i].msg_id == msg_id) {
            return true;
        }
    }
    return false;
}

bool dedup_filter_check_and_insert(dedup_filter_t *filter, uint8_t sender_id, uint32_t msg_id)
{
    if (!filter) {
        return false;
    }
    if (dedup_filter_contains(filter, sender_id, msg_id)) {
        return true;
    }
    filter->entries[filter->write_idx].sender_id = sender_id;
    filter->entries[filter->write_idx].msg_id = msg_id;
    filter->entries[filter->write_idx].valid = true;
    filter->write_idx = (filter->write_idx + 1u) % DEDUP_WINDOW_SIZE;
    return false;
}
