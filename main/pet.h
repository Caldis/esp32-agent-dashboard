/*
 * pet — the per-agent status creature on the single-agent dashboard view.
 *
 * A tiny generative mascot built from plain LVGL objects (no image
 * assets): Claude is Clawd — Claude Code's classic coral pixel crab
 * (side claws, slit eyes, four stubby legs), Codex is a teal terminal
 * face, anything else gets a neutral round buddy. The mood drives the
 * animation set:
 *
 *   PET_MOOD_WORKING  bouncing (Clawd's legs scuttle in alternating
 *                     pairs, Codex's eyes scan like it's reading code)
 *                     + periodic blink
 *   PET_MOOD_WAITING  swaying side to side with big expectant eyes
 *   PET_MOOD_IDLE     slow breathing with eyes closed
 *
 * All calls must run on the LVGL task (the dashboard tick) — pet_set is
 * idempotent and only rebuilds animations when kind/mood change.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PET_MOOD_IDLE = 0,
    PET_MOOD_WORKING,
    PET_MOOD_WAITING,
} pet_mood_t;

typedef struct pet pet_t;

/* Create the (96x96) pet container under parent. Position it with
 * lv_obj_align(pet_obj(p), ...). Starts as a generic idle buddy. */
pet_t *pet_create(lv_obj_t *parent);

lv_obj_t *pet_obj(pet_t *p);

/* Morph to the agent kind ("claude-code" / "codex" / other) + mood.
 * Colors follow theme_accent_for_kind. No-op when nothing changed. */
void pet_set(pet_t *p, const char *kind, pet_mood_t mood);

#ifdef __cplusplus
}
#endif
