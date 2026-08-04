#include "usb_network.h"
#include "app_settings.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#define USB_ATTACHED_BIT BIT0

static const char *TAG = "usb_network";
static EventGroupHandle_t s_usb_events;
static esp_netif_t *s_usb_netif;
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
    if (event->esp_netif != s_usb_netif || !usb_network_attached()) return;

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

static esp_err_t create_usb_netif(uint8_t mac[6])
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    /*
     * This USB port has no esp_eth driver to emit ETHERNET_EVENT_CONNECTED.
     * Mark it up when esp_netif_action_start() is called; otherwise
     * ethernetif_input() drops every received frame while the netif is down,
     * including DHCP Discover packets.
     */
    esp_netif_ip_info_t usb_ip = {0};
    ip4_addr_t parsed_ip;
    if (!ip4addr_aton(app_settings_get()->usb_dhcp_server_ip, &parsed_ip))
        return ESP_ERR_INVALID_ARG;
    usb_ip.ip.addr = parsed_ip.addr;
    usb_ip.gw = usb_ip.ip;
    IP4_ADDR(&usb_ip.netmask, 255, 255, 255, 252);

    base.flags = ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_DHCP_SERVER;
    base.ip_info = &usb_ip;
    base.if_key = "USB_NCM";
    base.if_desc = "usb ncm";
    base.route_prio = 80;

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
                        "failed to set USB netif MAC");
    esp_netif_action_start(s_usb_netif, 0, 0, NULL);
    return ESP_OK;
}

esp_err_t usb_network_bind_socket(int socket_fd)
{
    if (!s_usb_netif || socket_fd < 0) return ESP_ERR_INVALID_STATE;
    struct ifreq interface = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_netif_impl_name(
        s_usb_netif, interface.ifr_name), TAG,
        "failed to get USB netif name");
    return setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE,
                      &interface, sizeof(interface)) == 0 ? ESP_OK : ESP_FAIL;
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

esp_err_t usb_network_start(void)
{
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
    ESP_RETURN_ON_ERROR(create_usb_netif(usb_mac), TAG,
                        "failed to create USB network interface");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, assigned_ip_event, NULL),
        TAG, "failed to register DHCP client event");
    char peer[16];
    ESP_RETURN_ON_ERROR(usb_network_peer_ip(peer, sizeof(peer)), TAG,
                        "failed to calculate USB peer address");
    ESP_LOGI(TAG, "USB NCM up at %s/30, DHCP host %s",
             app_settings_get()->usb_dhcp_server_ip, peer);
    return ESP_OK;
}
