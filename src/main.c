#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "servo.h"
#include "dc_motor.h"
#include "web_server.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── WiFi SoftAP ────────────────────────────────────────────────────────────── */

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

static struct net_mgmt_event_callback wifi_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);

    switch (event) {
    case NET_EVENT_WIFI_AP_ENABLE_RESULT:
        LOG_INF("SoftAP enabled — SSID \"%s\" on channel %d",
                CONFIG_CYBERBRICK_WIFI_SSID, CONFIG_CYBERBRICK_WIFI_CHANNEL);
        LOG_INF("Open http://%s in your browser", CONFIG_CYBERBRICK_AP_IP);
        break;
    case NET_EVENT_WIFI_AP_DISABLE_RESULT:
        LOG_WRN("SoftAP disabled");
        break;
    case NET_EVENT_WIFI_AP_STA_CONNECTED: {
        const struct wifi_ap_sta_info *sta =
            (const struct wifi_ap_sta_info *)cb->info;
        if (sta) {
            LOG_INF("Client joined: " MACSTR, MAC2STR(sta->mac));
        }
        break;
    }
    case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
        const struct wifi_ap_sta_info *sta =
            (const struct wifi_ap_sta_info *)cb->info;
        if (sta) {
            LOG_INF("Client left:   " MACSTR, MAC2STR(sta->mac));
        }
        break;
    }
    default:
        break;
    }
}

static int configure_ap_ipv4(struct net_if *iface)
{
    struct in_addr addr;
    struct in_addr netmask;

    if (net_addr_pton(AF_INET, CONFIG_CYBERBRICK_AP_IP, &addr) < 0) {
        LOG_ERR("Invalid AP IP: %s", CONFIG_CYBERBRICK_AP_IP);
        return -EINVAL;
    }
    if (net_addr_pton(AF_INET, CONFIG_CYBERBRICK_AP_NETMASK, &netmask) < 0) {
        LOG_ERR("Invalid AP netmask: %s", CONFIG_CYBERBRICK_AP_NETMASK);
        return -EINVAL;
    }

    net_if_ipv4_set_gw(iface, &addr);

    if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
        LOG_WRN("AP IP address may already be set");
    }
    if (!net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask)) {
        LOG_WRN("Failed to set AP netmask");
    }

    /* DHCP pool starts at <gw>+10 and serves NET_DHCPV4_SERVER_ADDR_COUNT IPs. */
    struct in_addr pool_start = addr;
    pool_start.s4_addr[3] += 10;

    int ret = net_dhcpv4_server_start(iface, &pool_start);
    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("DHCPv4 server start failed: %d", ret);
        return ret;
    }
    LOG_INF("DHCPv4 server running, pool starts at %d.%d.%d.%d",
            pool_start.s4_addr[0], pool_start.s4_addr[1],
            pool_start.s4_addr[2], pool_start.s4_addr[3]);
    return 0;
}

static int wifi_start_ap(void)
{
    struct net_if *iface = net_if_get_wifi_sap();
    if (!iface) {
        /* Fall back to default iface when AP/STA coexistence isn't enabled. */
        iface = net_if_get_default();
    }
    if (!iface) {
        LOG_ERR("No WiFi AP interface");
        return -ENODEV;
    }

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
        NET_EVENT_WIFI_AP_ENABLE_RESULT |
        NET_EVENT_WIFI_AP_DISABLE_RESULT |
        NET_EVENT_WIFI_AP_STA_CONNECTED |
        NET_EVENT_WIFI_AP_STA_DISCONNECTED);
    net_mgmt_add_event_callback(&wifi_cb);

    int ret = configure_ap_ipv4(iface);
    if (ret < 0) {
        return ret;
    }

    const size_t psk_len = strlen(CONFIG_CYBERBRICK_WIFI_PASSWORD);
    struct wifi_connect_req_params ap_params = {
        .ssid        = (const uint8_t *)CONFIG_CYBERBRICK_WIFI_SSID,
        .ssid_length = strlen(CONFIG_CYBERBRICK_WIFI_SSID),
        .psk         = (const uint8_t *)CONFIG_CYBERBRICK_WIFI_PASSWORD,
        .psk_length  = psk_len,
        .channel     = CONFIG_CYBERBRICK_WIFI_CHANNEL,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
        .security    = (psk_len == 0) ? WIFI_SECURITY_TYPE_NONE
                                      : WIFI_SECURITY_TYPE_PSK,
        .mfp         = WIFI_MFP_OPTIONAL,
    };

    LOG_INF("Starting SoftAP \"%s\" (%s)", CONFIG_CYBERBRICK_WIFI_SSID,
            (psk_len == 0) ? "open" : "WPA2-PSK");

    ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface,
                   &ap_params, sizeof(ap_params));
    if (ret < 0) {
        LOG_ERR("AP enable failed: %d", ret);
        return ret;
    }
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("Cyberbrick L-ONE starting");

    int ret;

    ret = servo_init();
    if (ret < 0) {
        LOG_ERR("Servo init failed: %d", ret);
        return ret;
    }

    ret = dc_motor_init();
    if (ret < 0) {
        LOG_ERR("DC motor init failed: %d", ret);
        return ret;
    }

    ret = wifi_start_ap();
    if (ret < 0) {
        LOG_ERR("WiFi AP init failed: %d — running without network", ret);
        /* Continue anyway so servo/motor still work over serial shell */
    } else {
        web_server_start();
    }

    while (true) {
        k_sleep(K_SECONDS(5));
    }

    return 0;
}
