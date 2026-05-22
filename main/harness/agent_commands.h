/*
 * agent_commands — registers the `dash *` console command family that
 * the host bridge pushes JSON snapshots through. See the project README
 * for the full protocol shape.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register all dash * commands. Call after console_protocol_init()
 * and after agent_state_init(). Safe to call once. */
void agent_commands_register(void);

#ifdef __cplusplus
}
#endif
