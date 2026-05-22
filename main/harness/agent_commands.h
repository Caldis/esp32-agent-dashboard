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

/* Load persisted config (theme, device_name, owner, default_scene)
 * from NVS into the global agent_state. Call once at boot, after
 * agent_state_init + theme_init. */
void agent_commands_load_config(void);

#ifdef __cplusplus
}
#endif
