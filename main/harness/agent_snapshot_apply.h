/*
 * agent_snapshot_apply - apply one dash snapshot JSON payload to agent_state.
 *
 * The console command owns transport concerns: argv parsing, replies, EVTs,
 * and scene changes. This Module owns the snapshot semantics: v0/v1 shape
 * handling, per-agent slot merge, prompt state, aggregate totals, and slot
 * overflow accounting.
 */

#pragma once

#include "../agent_state.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  total_now;
    bool prompt_set;
    bool prompt_clear;

    int  removed_count;
    char removed_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char removed_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];

    int  added_count;
    char added_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char added_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];

    int  dropped_count;
} agent_snapshot_apply_result_t;

bool agent_snapshot_apply_json(const char *json,
                               const char *end,
                               agent_snapshot_apply_result_t *out);

#ifdef __cplusplus
}
#endif
