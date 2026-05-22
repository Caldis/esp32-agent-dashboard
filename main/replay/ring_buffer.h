/*
 * replay/ring_buffer — per-agent transcript ring (v1.5.0 scaffold).
 *
 * Fixed-size PSRAM-resident ring of `agent_entry_t` snapshots. The
 * snapshot path appends via ring_buffer_push(); the scrubber walks
 * backwards with ring_buffer_peek_offset().
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "agent_state.h"   /* for agent_entry_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    agent_entry_t *entries;   /* PSRAM-allocated array */
    size_t capacity;          /* number of slots */
    size_t head;              /* next write idx */
    size_t count;             /* number of valid entries (<= capacity) */
    uint32_t generation;      /* bumps each push, lets scenes detect change */
} replay_ring_t;

bool replay_ring_init(replay_ring_t *r, size_t capacity);
void replay_ring_free(replay_ring_t *r);

/* Append (oldest entry dropped when full). */
void replay_ring_push(replay_ring_t *r, const agent_entry_t *e);

/* Read the Nth-from-newest entry into `out`. offset=0 is newest,
 * offset=count-1 is oldest. Returns false when offset >= count. */
bool replay_ring_peek_offset(const replay_ring_t *r, size_t offset,
                              agent_entry_t *out);

/* Serialise the entire ring as JSONL into a caller-provided buffer.
 * Returns bytes written, or -1 on overflow. */
int replay_ring_dump_jsonl(const replay_ring_t *r, char *buf, size_t cap);

/* Wipe the ring without freeing the backing PSRAM. */
void replay_ring_clear(replay_ring_t *r);

#ifdef __cplusplus
}
#endif
