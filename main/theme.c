/*
 * theme — palette registry. See theme.h.
 */

#include "theme.h"

#include <string.h>

static const theme_palette_t s_palettes[THEME_COUNT] = {
    {
        .id            = THEME_NOIR,
        .name          = "noir",
        .bg            = 0x0A0A0E,
        .surface       = 0x14141C,
        .text          = 0xE8E5DE,
        .text_dim      = 0x6B6F7A,
        .accent_claude = 0xFF8B5C,   /* rust-orange */
        .accent_codex  = 0x5CD0D9,   /* teal */
        .accent_other  = 0xB89CFF,   /* lavender */
        .warning       = 0xFFC857,
        .danger        = 0xE04545,
        .success       = 0x9EE493,
    },
    {
        .id            = THEME_LAB,
        .name          = "lab",
        .bg            = 0xF0EEE7,
        .surface       = 0xE3E0D6,
        .text          = 0x1A1814,
        .text_dim      = 0x807A6E,
        .accent_claude = 0xFF8B5C,
        .accent_codex  = 0x2BAFBA,   /* deeper teal — better contrast on light */
        .accent_other  = 0x7A60E0,
        .warning       = 0xC59421,
        .danger        = 0xB52E2E,
        .success       = 0x4E9E55,
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
