#include "g7_tokenise.h"
#include <string.h>

int g7_tokenise(const char *line, char *buf, size_t bufcap,
                const char *argv[], int max_args) {
    int argc = 0; size_t w = 0; size_t n = strlen(line); size_t i = 0;
    while (i < n && argc < max_args) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= n) break;
        if (w >= bufcap) break;
        argv[argc] = &buf[w];
        if (line[i] == '"') {
            i++;                                  /* drop leading " */
            int close = -1;
            for (size_t j = i; j < n; ++j) {
                if (line[j] == '"' && (j + 1 == n || line[j+1] == ' ' || line[j+1] == '\t')) {
                    close = (int)j; break;
                }
            }
            size_t endp = (close == -1) ? n : (size_t)close;
            for (size_t j = i; j < endp && w + 1 < bufcap; ++j) buf[w++] = line[j];
            i = (close == -1) ? n : (size_t)close + 1;
        } else {
            int in_q = 0;
            while (i < n) {
                char ch = line[i];
                if (!in_q && (ch == ' ' || ch == '\t')) break;
                if (ch == '"') { in_q = !in_q; i++; continue; }
                if (w + 1 < bufcap) buf[w++] = ch;
                i++;
            }
        }
        if (w < bufcap) buf[w++] = 0;             /* NUL-terminate token */
        argc++;
    }
    return argc;
}

int g7_tokenise_join(const char *line, char *out, size_t outcap) {
    char buf[1024];
    const char *argv[8];
    int argc = g7_tokenise(line, buf, sizeof(buf), argv, 8);
    size_t w = 0;
    for (int k = 0; k < argc; ++k) {
        if (k > 0 && w + 1 < outcap) out[w++] = '\x1f';
        for (const char *p = argv[k]; *p && w + 1 < outcap; ++p) out[w++] = *p;
    }
    if (outcap) out[(w < outcap) ? w : (outcap - 1)] = '\0';
    return argc;
}
