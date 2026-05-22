/*
 * theme — palette registry. See theme.h.
 */

#include "theme.h"

#include <string.h>

/* Palette hex codes mirror docs/brand/palette.md exactly. Token names
 * in the right-comment column point at the palette.md entry. When the
 * brand pack changes, this table is the *only* place to update on the
 * firmware side. U1's UX review surfaced 8-token drift between the
 * previous fudged values and the brand source — the brand's
 * "teal is the only place teal appears at full saturation" rule was
 * silently broken because firmware used `#5CD0D9` cyan in place of
 * `#0E7C7B` teal. Honour the brand or own it; don't drift. */
static const theme_palette_t s_palettes[THEME_COUNT] = {
    {
        .id            = THEME_NOIR,
        .name          = "noir",
        .bg            = 0x0B0A09,   /* palette.md: noir bg near-black */
        .surface       = 0x1C1814,   /* ink — for cards / panes */
        .text          = 0xF3EEE2,   /* paper */
        .text_dim      = 0x8A807A,   /* ink-fade */
        .accent_claude = 0xB8431A,   /* rust — warm, Anthropic/harness side */
        .accent_codex  = 0x2BB3B1,   /* teal-bright — on dark, the noir accent */
        .accent_other  = 0x5A514A,   /* ink-mute */
        .warning       = 0xB89020,   /* gold */
        .danger        = 0xB8431A,   /* rust at danger intensity reuses warm */
        .success       = 0x344A36,   /* moss */
    },
    {
        .id            = THEME_LAB,
        .name          = "lab",
        .bg            = 0xF3EEE2,   /* paper */
        .surface       = 0xE3DDC9,   /* derived — paper tinted slightly darker */
        .text          = 0x1C1814,   /* ink */
        .text_dim      = 0x5A514A,   /* ink-mute */
        .accent_claude = 0xB8431A,   /* rust — same warm hue, brand consistency */
        .accent_codex  = 0x0E7C7B,   /* teal — the lab/light accent per palette.md */
        .accent_other  = 0x5A514A,   /* ink-mute */
        .warning       = 0xB89020,   /* gold */
        .danger        = 0xB8431A,   /* rust */
        .success       = 0x344A36,   /* moss */
    },
    {
        .id            = THEME_MONO,
        .name          = "mono",
        .bg            = 0x080808,
        .surface       = 0x141414,
        .text          = 0xEDEDED,
        .text_dim      = 0x6A6A6A,
        .accent_claude = 0xEDEDED,   /* the only hue */
        .accent_codex  = 0xC8C8C8,   /* same hue, dimmer */
        .accent_other  = 0xA0A0A0,
        .warning       = 0xEDEDED,
        .danger        = 0xEDEDED,
        .success       = 0xEDEDED,
    },
};

static const theme_palette_t *s_current = &s_palettes[THEME_NOIR];

void theme_init(void)
{
    s_current = &s_palettes[THEME_NOIR];
}

bool theme_set_by_name(const char *name)
{
    if (!name) return false;
    for (int i = 0; i < THEME_COUNT; ++i) {
        if (strcmp(s_palettes[i].name, name) == 0) {
            s_current = &s_palettes[i];
            return true;
        }
    }
    return false;
}

const theme_palette_t *theme_current(void)
{
    return s_current;
}

const char *theme_current_name(void)
{
    return s_current->name;
}

uint32_t theme_accent_for_kind(const char *kind)
{
    if (!kind || kind[0] == '\0') return s_current->accent_other;
    if (strcmp(kind, "claude-code") == 0) return s_current->accent_claude;
    if (strcmp(kind, "codex")       == 0) return s_current->accent_codex;
    return s_current->accent_other;
}
