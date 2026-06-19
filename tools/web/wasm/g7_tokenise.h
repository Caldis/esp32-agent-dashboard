#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* G-7 tokeniser:把一行切成 argv(各 token 以 NUL 分隔写入 buf)。
 * 返回 argc(≤ max_args)。与固件 console_protocol.c /
 * mock_device_v1.py._tokenise 语义等价:
 *  - '"' 起始 token:去前导 '"',累积到「后跟空白或行尾」的 '"' 收尾;
 *  - 非 '"' 起始 token:遇任意 '"' 切换 in_quote,所有 '"' 被剥除。 */
int g7_tokenise(const char *line, char *buf, size_t bufcap,
                const char *argv[], int max_args);

/* 测试友好封装:tokenise 后把各 token 用 '\x1f'(US)连接写入 out,
 * 返回 argc。便于 ctypes 直接断言切分结果。out 始终 NUL 终止。 */
int g7_tokenise_join(const char *line, char *out, size_t outcap);
#ifdef __cplusplus
}
#endif
