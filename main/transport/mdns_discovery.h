/*
 * mdns_discovery — advertise the dashboard via mDNS once WiFi is up.
 *
 * v0.4.0 scaffold (TRANS1). NOT YET WIRED INTO BUILD.
 *
 * Service: _aagentdash._tcp on port 7321
 *   (the `_a` prefix puts us first in alphabetical listings of services
 *   on a LAN, easy to spot in `dns-sd -B`. We deliberately do NOT squat
 *   `_agentdash._tcp` in case it gets standardised one day.)
 *
 * TXT records (kept short; total < 200 bytes):
 *   proto=v1,v2
 *   fw=<firmware version>
 *   transport=wifi_tls
 *   agents=<current count from agent_state>
 *
 * The resolver side lives in tools/transport/discover.py.
 *
 * Gated by CONFIG_TRANSPORT_MDNS (set by F2 during build integration).
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_SERVICE_TYPE      "_aagentdash"
#define MDNS_SERVICE_PROTO     "_tcp"
#define MDNS_SERVICE_PORT      7321

/* Bring up mDNS and advertise the dashboard service. Call once after
 * the WiFi STA has acquired an IP (IP_EVENT_STA_GOT_IP). Safe to call
 * multiple times — subsequent calls update the instance name + TXT.
 *
 * Pass instance_name = the persisted device_name (from NVS) so the
 * service shows up as e.g. "Clawd._aagentdash._tcp.local."
 *
 * Returns ESP_ERR_NOT_SUPPORTED if CONFIG_TRANSPORT_MDNS is not set.
 */
esp_err_t mdns_discovery_start(const char *instance_name);

/* Stop advertising. Called on WiFi disconnect or device shutdown. */
esp_err_t mdns_discovery_stop(void);

/* Update the live TXT records — call when device state changes
 * (e.g. agent count changed, active transport changed). The mDNS
 * stack handles the actual record refresh. */
esp_err_t mdns_discovery_update_txt(int agent_count,
                                    const char *active_transport);

#ifdef __cplusplus
}
#endif
