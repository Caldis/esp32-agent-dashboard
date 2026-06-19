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
                if (line[j] == '\\') { j++; continue; }   /* skip escaped byte: \" is not a close */
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

/* g7_tokenise_join: 仅供测试使用,调用方须传入足够大的 buffer(≥ G7_MAX_LINE)。
 * 截断语义:当 outcap 极小时,分隔符 '\x1f' 或 token 字符可能被静默跳过
 * (w+1<outcap 为假时跳过写入);out 始终保证 NUL 终止。 */
int g7_tokenise_join(const char *line, char *out, size_t outcap) {
    char buf[G7_MAX_LINE];
    const char *argv[G7_MAX_ARGS];
    int argc = g7_tokenise(line, buf, sizeof(buf), argv, G7_MAX_ARGS);
    size_t w = 0;
    for (int k = 0; k < argc; ++k) {
        if (k > 0 && w + 1 < outcap) out[w++] = '\x1f';
        for (const char *p = argv[k]; *p && w + 1 < outcap; ++p) out[w++] = *p;
    }
    if (outcap) out[(w < outcap) ? w : (outcap - 1)] = '\0';
    return argc;
}
