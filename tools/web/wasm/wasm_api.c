/* wasm_api.c — JS/ctypes ↔ 数据层的契约。第 1 步先建最小入口;
 * state_json / dash_feed_line / drain_signals 在后续任务补全。 */
#include "agent_state.h"
#include "agent_commands.h"

void dash_init(void) {
    agent_state_init();
    agent_commands_register();     /* 经 shim 捕获命令表 */
    agent_commands_load_config();  /* 设默认 device_name="DASHBOARD" 等 */
}
