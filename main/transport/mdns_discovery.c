/*
 * mdns_discovery.c — _aagentdash._tcp advertiser (STUB).
 *
 * v0.4.0 scaffold (TRANS1). NOT YET WIRED INTO BUILD.
 *
 * Gated by CONFIG_TRANSPORT_MDNS. F2 wires the `mdns` managed
 * component into REQUIRES during build integration and calls
 * mdns_discovery_start() from the IP_EVENT_STA_GOT_IP handler.
 */

#include "mdns_discovery.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "mdns_discovery";

static bool s_advertised = false;
static char s_instance_name[64] = {0};

esp_err_t mdns_discovery_start(const char *instance_name)
{
#ifdef CONFIG_TRANSPORT_MDNS
    if (!instance_name) instance_name = "agentdash";
    strncpy(s_instance_name, instance_name, sizeof(s_instance_name) - 1);
    s_instance_name[sizeof(s_instance_name) - 1] = '\0';

    /* TODO(v0.4.0 F2):
     *   mdns_init();
     *   mdns_hostname_set(instance_name);   // device.local will resolve
     *   mdns_instance_name_set(instance_name);
     *   mdns_txt_item_t txt[] = {
     *       { "proto",     "v1,v2" },
     *       { "fw",        FW_VERSION },
     *       { "transport", "wifi_tls" },
     *       { "agents",    "0" },           // updated via _update_txt
     *   };
     *   mdns_service_add(instance_name,
     *                    MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO,
     *                    MDNS_SERVICE_PORT,
     *                    txt, sizeof(txt) / sizeof(txt[0]));
     */
    ESP_LOGW(TAG, "mdns_discovery_start(%s): TODO", instance_name);
    s_advertised = true;
    return ESP_OK;
#else
    (void)instance_name;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t mdns_discovery_stop(void)
{
#ifdef CONFIG_TRANSPORT_MDNS
    if (!s_advertised) return ESP_OK;
    /* TODO(v0.4.0 F2):
     *   mdns_service_remove(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO);
     *   mdns_free();
     */
    ESP_LOGW(TAG, "mdns_discovery_stop: TODO");
    s_advertised = false;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t mdns_discovery_update_txt(int agent_count,
                                    const char *active_transport)
{
#ifdef CONFIG_TRANSPORT_MDNS
    if (!s_advertised) return ESP_ERR_INVALID_STATE;
    /* TODO(v0.4.0 F2):
     *   char buf[8];
     *   snprintf(buf, sizeof(buf), "%d", agent_count);
     *   mdns_service_txt_item_set(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO,
     *                             "agents", buf);
     *   if (active_transport)
     *       mdns_service_txt_item_set(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO,
     *                                 "transport", active_transport);
     */
    ESP_LOGW(TAG, "mdns_discovery_update_txt(agents=%d, transport=%s): TODO",
             agent_count, active_transport ? active_transport : "(null)");
    return ESP_OK;
#else
    (void)agent_count; (void)active_transport;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
