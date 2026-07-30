#include "usb_network.h"
#include "app_settings.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_br_glue.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "apps/dhcpserver/dhcpserver.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#define USB_ATTACHED_BIT BIT0

static const char *TAG = "usb_network";
static EventGroupHandle_t s_usb_events;
static esp_netif_t *s_usb_netif;
static esp_netif_t *s_bridge_netif;
static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_pair_mac_ip_t s_usb_peer;
static bool s_usb_peer_known;

static esp_err_t usb_receive(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_usb_netif) return ESP_ERR_INVALID_STATE;

    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    /*
     * esp_netif_receive() takes ownership of the buffer once it is passed
     * to the network stack.  The registered driver_free_rx_buffer callback
     * releases it, including on input errors, so freeing it again here would
     * corrupt the heap.
     */
    return esp_netif_receive(s_usb_netif, copy, len, NULL);
}

static void usb_rx_buffer_free(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

static esp_err_t usb_transmit(void *handle, void *buffer, size_t len)
{
    (void)handle;
    if (len > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    return tinyusb_net_send_sync(buffer, (uint16_t)len, NULL,
                                 pdMS_TO_TICKS(100));
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        xEventGroupSetBits(s_usb_events, USB_ATTACHED_BIT);
        ESP_LOGI(TAG, "USB NCM host attached");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        xEventGroupClearBits(s_usb_events, USB_ATTACHED_BIT);
        taskENTER_CRITICAL(&s_peer_lock);
        s_usb_peer_known = false;
        taskEXIT_CRITICAL(&s_peer_lock);
        ESP_LOGI(TAG, "USB NCM host detached");
    }
}

static void assigned_ip_event(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_assigned_ip_to_client_t *event = data;
    if (event->esp_netif != s_bridge_netif || !usb_network_attached()) return;

    wifi_sta_list_t stations;
    if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
        for (int i = 0; i < stations.num; ++i)
            if (memcmp(stations.sta[i].mac, event->mac, sizeof(event->mac)) == 0)
                return;
    }

    char expected[16];
    char assigned[16];
    if (usb_network_peer_ip(expected, sizeof(expected)) != ESP_OK) return;
    snprintf(assigned, sizeof(assigned), IPSTR, IP2STR(&event->ip));
    if (strcmp(expected, assigned) != 0) return;

    taskENTER_CRITICAL(&s_peer_lock);
    memcpy(s_usb_peer.mac, event->mac, sizeof(s_usb_peer.mac));
    s_usb_peer.ip = event->ip;
    s_usb_peer_known = true;
    taskEXIT_CRITICAL(&s_peer_lock);
}

esp_err_t usb_network_resolve_clients(esp_netif_pair_mac_ip_t *clients,
                                      size_t count)
{
    if (!s_bridge_netif || !clients || count == 0)
        return ESP_ERR_INVALID_STATE;
    return esp_netif_dhcps_get_clients_by_mac(s_bridge_netif, (int)count,
                                               clients);
}

bool usb_network_peer_info(char *ip, size_t ip_capacity,
                           char *mac, size_t mac_capacity)
{
    if (!usb_network_attached()) return false;
    esp_netif_pair_mac_ip_t peer;
    bool known;
    taskENTER_CRITICAL(&s_peer_lock);
    peer = s_usb_peer;
    known = s_usb_peer_known;
    taskEXIT_CRITICAL(&s_peer_lock);

    if (ip && ip_capacity > 0) {
        if (known)
            snprintf(ip, ip_capacity, IPSTR, IP2STR(&peer.ip));
        else
            strlcpy(ip, "等待 DHCP", ip_capacity);
    }
    if (mac && mac_capacity > 0) {
        if (known)
            snprintf(mac, mac_capacity, "%02X:%02X:%02X:%02X:%02X:%02X",
                     peer.mac[0], peer.mac[1], peer.mac[2],
                     peer.mac[3], peer.mac[4], peer.mac[5]);
        else
            strlcpy(mac, "尚未获取", mac_capacity);
    }
    return true;
}

static esp_err_t create_usb_port(uint8_t mac[6])
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    /*
     * This USB port has no esp_eth driver to emit ETHERNET_EVENT_CONNECTED.
     * Mark it up when esp_netif_action_start() is called; otherwise
     * ethernetif_input() drops every received frame while the netif is down,
     * including DHCP Discover packets.
     */
    base.flags = ESP_NETIF_FLAG_AUTOUP;
    base.ip_info = NULL;
    base.if_key = "USB_NCM";
    base.if_desc = "usb ncm bridge port";
    base.route_prio = 0;

    esp_netif_driver_ifconfig_t driver = {
        .handle = (void *)1,
        .transmit = usb_transmit,
        .driver_free_rx_buffer = usb_rx_buffer_free,
    };
    struct esp_netif_netstack_config stack = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    esp_netif_config_t config = {
        .base = &base,
        .driver = &driver,
        .stack = &stack,
    };
    s_usb_netif = esp_netif_new(&config);
    if (!s_usb_netif) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_usb_netif, mac), TAG,
                        "failed to set USB port MAC");
    esp_netif_action_start(s_usb_netif, 0, 0, NULL);
    return ESP_OK;
}

esp_err_t usb_network_peer_ip(char *address, size_t capacity)
{
    ip4_addr_t server;
    if (!ip4addr_aton(app_settings_get()->usb_dhcp_server_ip, &server))
        return ESP_ERR_INVALID_ARG;
    uint32_t host_order = ntohl(server.addr) + 1;
    ip4_addr_t peer = { .addr = htonl(host_order) };
    if (!ip4addr_ntoa_r(&peer, address, (int)capacity))
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

bool usb_network_attached(void)
{
    return s_usb_events &&
        (xEventGroupGetBits(s_usb_events) & USB_ATTACHED_BIT) != 0;
}

static void bridge_started(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)event_data;

    ESP_ERROR_CHECK(esp_netif_bridge_add_port(s_bridge_netif, s_usb_netif));
    esp_netif_dhcps_stop(s_bridge_netif);

    ip4_addr_t server;
    ESP_ERROR_CHECK(ip4addr_aton(app_settings_get()->usb_dhcp_server_ip,
                                 &server) ? ESP_OK : ESP_ERR_INVALID_ARG);
    uint32_t base_address = ntohl(server.addr);
    dhcps_lease_t leases = {
        .enable = true,
        .start_ip.addr = htonl(base_address + 1),
        .end_ip.addr = htonl(base_address + 5),
    };
    ESP_ERROR_CHECK(esp_netif_dhcps_option(
        s_bridge_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
        &leases, sizeof(leases)));

    /*
     * The bridge glue only marks the bridge up after a physical Ethernet
     * link connects or a station joins the SoftAP.  USB NCM emits neither
     * event, so make the bridge operational explicitly before starting
     * DHCP.  esp_netif_dhcps_start() otherwise returns ESP_OK while leaving
     * the server in ESP_NETIF_DHCP_INIT when the netif is down.
     */
    esp_netif_action_connected(s_bridge_netif, WIFI_EVENT,
                               WIFI_EVENT_AP_START, NULL);
    ESP_ERROR_CHECK(esp_netif_is_netif_up(s_bridge_netif)
                        ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_bridge_netif));

    esp_netif_dhcp_status_t dhcp_status;
    ESP_ERROR_CHECK(esp_netif_dhcps_get_status(s_bridge_netif, &dhcp_status));
    ESP_ERROR_CHECK(dhcp_status == ESP_NETIF_DHCP_STARTED
                        ? ESP_OK : ESP_ERR_INVALID_STATE);

    char peer[16];
    ESP_ERROR_CHECK(usb_network_peer_ip(peer, sizeof(peer)));
    ESP_LOGI(TAG, "USB/SoftAP bridge up at %s/24, DHCP started, USB host %s",
             app_settings_get()->usb_dhcp_server_ip, peer);
}

esp_err_t usb_network_start(esp_netif_t *wifi_ap_netif)
{
    if (!wifi_ap_netif) return ESP_ERR_INVALID_ARG;
    s_usb_events = xEventGroupCreate();
    if (!s_usb_events) return ESP_ERR_NO_MEM;

    uint8_t usb_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(usb_mac, ESP_MAC_WIFI_SOFTAP), TAG,
                        "failed to derive USB MAC");
    usb_mac[0] |= 0x02;
    usb_mac[5] ^= 0x40;

    tinyusb_config_t tusb = TINYUSB_DEFAULT_CONFIG(usb_event);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb), TAG,
                        "failed to install TinyUSB");
    tinyusb_net_config_t net = {
        .on_recv_callback = usb_receive,
    };
    memcpy(net.mac_addr, usb_mac, sizeof(net.mac_addr));
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net), TAG,
                        "failed to initialize USB NCM");
    ESP_RETURN_ON_ERROR(create_usb_port(usb_mac), TAG,
                        "failed to create USB network port");

    esp_netif_ip_info_t bridge_ip = {0};
    ip4_addr_t parsed_bridge_ip;
    if (!ip4addr_aton(app_settings_get()->usb_dhcp_server_ip,
                      &parsed_bridge_ip))
        return ESP_ERR_INVALID_ARG;
    bridge_ip.ip.addr = parsed_bridge_ip.addr;
    bridge_ip.gw = bridge_ip.ip;
    IP4_ADDR(&bridge_ip.netmask, 255, 255, 255, 0);

    bridgeif_config_t bridge_options = {
        .max_fdb_dyn_entries = 12,
        .max_fdb_sta_entries = 2,
        .max_ports = 2,
    };
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS();
    base.ip_info = &bridge_ip;
    base.bridge_info = &bridge_options;
    base.if_key = "USB_AP_BR";
    base.if_desc = "USB NCM and SoftAP bridge";
    base.route_prio = 80;
    ESP_RETURN_ON_ERROR(esp_read_mac(base.mac, ESP_MAC_WIFI_SOFTAP), TAG,
                        "failed to derive bridge MAC");

    esp_netif_config_t bridge_config = {
        .base = &base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_BR,
    };
    s_bridge_netif = esp_netif_new(&bridge_config);
    if (!s_bridge_netif) return ESP_ERR_NO_MEM;

    esp_netif_br_glue_handle_t glue = esp_netif_br_glue_new();
    if (!glue) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_br_glue_add_wifi_port(glue, wifi_ap_netif),
                        TAG, "failed to add SoftAP bridge port");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_bridge_netif, glue), TAG,
                        "failed to attach bridge");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, assigned_ip_event, NULL),
        TAG, "failed to register DHCP client event");
    return esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                      bridge_started, NULL);
}
