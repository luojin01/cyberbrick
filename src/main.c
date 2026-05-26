#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "servo.h"
#include "dc_motor.h"
#include "web_server.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── WiFi management ────────────────────────────────────────────────────────── */

static K_SEM_DEFINE(wifi_disconnected_evt, 0, 1);

static struct net_mgmt_event_callback wifi_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);

    switch (event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *status =
            (const struct wifi_status *)cb->info;
        if (status && status->status == 0) {
            LOG_INF("WiFi connected");
        } else {
            LOG_ERR("WiFi connect failed (status %d)",
                    status ? status->status : -1);
        }
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        LOG_WRN("WiFi disconnected — will reconnect");
        k_sem_give(&wifi_disconnected_evt);
        break;
    default:
        break;
    }
}

/* Poll the interface until DHCP gives us a usable IPv4 address. */
static int wait_for_ipv4(struct net_if *iface, k_timeout_t timeout)
{
    int64_t deadline = k_uptime_get() + k_ticks_to_ms_ceil64(timeout.ticks);

    while (true) {
        if (iface->config.ip.ipv4 != NULL) {
            ARRAY_FOR_EACH(iface->config.ip.ipv4->unicast, i) {
                struct net_if_addr *if_addr =
                    &iface->config.ip.ipv4->unicast[i].ipv4;

                if (if_addr->is_used &&
                    if_addr->addr_type == NET_ADDR_DHCP) {
                    char ip_str[NET_IPV4_ADDR_LEN];
                    net_addr_ntop(AF_INET, &if_addr->address.in_addr,
                                  ip_str, sizeof(ip_str));
                    LOG_INF("IP address: %s", ip_str);
                    LOG_INF("Open http://%s in your browser", ip_str);
                    return 0;
                }
            }
        }

        if (k_uptime_get() >= deadline) {
            return -ETIMEDOUT;
        }
        k_sleep(K_MSEC(250));
    }
}

static int wifi_do_connect(struct net_if *iface)
{
    struct wifi_connect_req_params params = {
        .ssid        = (const uint8_t *)CONFIG_CYBERBRICK_WIFI_SSID,
        .ssid_length = strlen(CONFIG_CYBERBRICK_WIFI_SSID),
        .psk         = (const uint8_t *)CONFIG_CYBERBRICK_WIFI_PASSWORD,
        .psk_length  = strlen(CONFIG_CYBERBRICK_WIFI_PASSWORD),
        .channel     = WIFI_CHANNEL_ANY,
        .security    = WIFI_SECURITY_TYPE_PSK,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
        .mfp         = WIFI_MFP_OPTIONAL,
    };

    LOG_INF("Connecting to WiFi SSID: %s", CONFIG_CYBERBRICK_WIFI_SSID);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
                       &params, sizeof(params));
    if (ret < 0) {
        LOG_ERR("WiFi connect request failed: %d", ret);
        return ret;
    }

    /* ESP32 driver auto-starts DHCPv4 once associated. */
    ret = wait_for_ipv4(iface, K_SECONDS(45));
    if (ret < 0) {
        LOG_ERR("Timeout waiting for IPv4 address");
        return ret;
    }
    return 0;
}

static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No network interface");
        return -ENODEV;
    }

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
        NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    return wifi_do_connect(iface);
}

/* Background thread: re-runs connect whenever the driver reports a drop. */
static void wifi_supervisor_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("supervisor: no iface");
        return;
    }

    while (true) {
        /* Block until the event callback signals a disconnect. */
        k_sem_take(&wifi_disconnected_evt, K_FOREVER);

        /* Drain any duplicate disconnect events queued during reconnect. */
        k_sem_reset(&wifi_disconnected_evt);

        unsigned int backoff_ms = 2000;
        while (true) {
            LOG_INF("Reconnect attempt (backoff %u ms)…", backoff_ms);
            int ret = wifi_do_connect(iface);
            if (ret == 0) {
                LOG_INF("Reconnect succeeded");
                /* Idempotent: starts web thread only if not already running. */
                web_server_start();
                break;
            }
            LOG_WRN("Reconnect failed (%d), retrying", ret);
            k_sleep(K_MSEC(backoff_ms));
            if (backoff_ms < 30000) {
                backoff_ms *= 2;
            }
        }
    }
}

K_THREAD_DEFINE(wifi_sup_tid, 4096,
                wifi_supervisor_thread, NULL, NULL, NULL,
                7, 0, 0);

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

    ret = wifi_connect();
    if (ret < 0) {
        LOG_ERR("WiFi init failed: %d — running without network", ret);
        /* Continue anyway so servo/motor still work over direct serial/debug */
    } else {
        web_server_start();
    }

    /* Main loop: nothing to poll — control happens via web server thread */
    while (true) {
        k_sleep(K_SECONDS(5));
    }

    return 0;
}
