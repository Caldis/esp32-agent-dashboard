/*
 * tool_icons — see header. Small static table lookup.
 */

#include "tool_icons.h"

#include <string.h>
#include "lvgl.h"

struct entry { const char *name; const char *sym; };

static const struct entry s_table[] = {
    /* Shell */
    { "Bash",         LV_SYMBOL_PLAY  },   /* a play-triangle reads as "exec" */
    /* File ops */
    { "Read",         LV_SYMBOL_FILE  },
    { "Write",        LV_SYMBOL_FILE  },
    { "Edit",         LV_SYMBOL_EDIT  },
    /* Search */
    { "Grep",         LV_SYMBOL_EYE_OPEN },
    { "Glob",         LV_SYMBOL_EYE_OPEN },
    /* Web / fetch */
    { "WebFetch",     LV_SYMBOL_DOWNLOAD },
    { "WebSearch",    LV_SYMBOL_EYE_OPEN },
    /* Misc */
    { "NotebookEdit", LV_SYMBOL_EDIT  },
    { "TodoWrite",    LV_SYMBOL_LIST  },
    { NULL,           NULL            },
};

const char *tool_icon_for(const char *tool)
{
    if (!tool || tool[0] == '\0') return LV_SYMBOL_DUMMY;
    for (const struct entry *e = s_table; e->name; ++e) {
        if (strcmp(tool, e->name) == 0) return e->sym;
    }
    /* Prefix scan — accept "BashOutput", "EditMcp", etc. */
    for (const struct entry *e = s_table; e->name; ++e) {
        size_t n = strlen(e->name);
        if (strncmp(tool, e->name, n) == 0) return e->sym;
    }
    return LV_SYMBOL_DUMMY;
}
