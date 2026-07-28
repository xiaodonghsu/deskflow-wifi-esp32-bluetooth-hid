#include "deskflow.h"
#include "app_config.h"
#include "ble_hid.h"
#include "wifi.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "deskflow";

typedef struct {
    size_t target;
    char screen_name[32];
    char task_name[16];
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t buttons;
    int16_t last_x;
    int16_t last_y;
    bool have_position;
} deskflow_client_t;

static deskflow_client_t s_clients[APP_MAX_HID_DEVICES];

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static void mouse_move(deskflow_client_t *client, int dx, int dy)
{
    while (dx || dy) {
        int8_t sx = dx > 127 ? 127 : dx < -127 ? -127 : (int8_t)dx;
        int8_t sy = dy > 127 ? 127 : dy < -127 ? -127 : (int8_t)dy;
        ble_hid_mouse(client->target, client->buttons, sx, sy, 0);
        dx -= sx;
        dy -= sy;
    }
}

static uint8_t hid_key(uint16_t key)
{
    if (key >= 'a' && key <= 'z') return 0x04 + key - 'a';
    if (key >= 'A' && key <= 'Z') return 0x04 + key - 'A';
    if (key >= '1' && key <= '9') return 0x1e + key - '1';
    if (key == '0') return 0x27;
    switch (key) {
    case 0x20: return 0x2c;
    case '-': case '_': return 0x2d; case '=': case '+': return 0x2e;
    case '[': case '{': return 0x2f; case ']': case '}': return 0x30;
    case '\\': case '|': return 0x31;
    case ';': case ':': return 0x33; case '\'': case '"': return 0x34;
    case '`': case '~': return 0x35;
    case ',': case '<': return 0x36; case '.': case '>': return 0x37;
    case '/': case '?': return 0x38;
    case '!': return 0x1e; case '@': return 0x1f; case '#': return 0x20;
    case '$': return 0x21; case '%': return 0x22; case '^': return 0x23;
    case '&': return 0x24; case '*': return 0x25; case '(': return 0x26;
    case ')': return 0x27;
    case 0xEF08: return 0x2a; case 0xEF09: return 0x2b; case 0xEF0D: return 0x28;
    case 0xEF1B: return 0x29; case 0xEFFF: return 0x4c;
    case 0xEF50: return 0x4a; case 0xEF51: return 0x50; case 0xEF52: return 0x52;
    case 0xEF53: return 0x4f; case 0xEF54: return 0x51; case 0xEF55: return 0x4b;
    case 0xEF56: return 0x4e; case 0xEF57: return 0x4d;
    case 0xEF61: return 0x46; case 0xEF63: return 0x49;
    case 0xEF67: return 0x65; case 0xEF7F: return 0x53;
    case 0xEFE5: return 0x39; case 0xEF14: return 0x47;
    default: return 0;
    }
}

static uint8_t hid_modifiers(uint16_t deskflow_mask)
{
    uint8_t m = 0;
    if (deskflow_mask & 0x0001) m |= 0x02; /* shift */
    if (deskflow_mask & 0x0002) m |= 0x01; /* control */
    if (deskflow_mask & 0x0004) m |= 0x04; /* alt */
    if (deskflow_mask & 0x0010) m |= 0x08; /* super/meta */
    return m;
}

static void key_event(deskflow_client_t *client, uint16_t key, uint16_t mask, bool down)
{
    uint8_t code = hid_key(key);
    client->modifiers = hid_modifiers(mask);
    switch (key) {
    case 0xEFE1: if (down) client->modifiers |= 0x02; else client->modifiers &= ~0x02; break;
    case 0xEFE2: if (down) client->modifiers |= 0x20; else client->modifiers &= ~0x20; break;
    case 0xEFE3: if (down) client->modifiers |= 0x01; else client->modifiers &= ~0x01; break;
    case 0xEFE4: if (down) client->modifiers |= 0x10; else client->modifiers &= ~0x10; break;
    case 0xEFE9: if (down) client->modifiers |= 0x04; else client->modifiers &= ~0x04; break;
    case 0xEFEA: if (down) client->modifiers |= 0x40; else client->modifiers &= ~0x40; break;
    case 0xEFE7:
    case 0xEFEB: if (down) client->modifiers |= 0x08; else client->modifiers &= ~0x08; break;
    case 0xEFE8:
    case 0xEFEC: if (down) client->modifiers |= 0x80; else client->modifiers &= ~0x80; break;
    default: break;
    }
    ESP_LOGI(TAG, "[%s] key %s id=0x%04x hid=0x%02x",
             client->screen_name, down ? "down" : "up", key, code);
    if (code) {
        if (down) {
            for (int i = 0; i < 6; ++i)
                if (client->keys[i] == code) goto send;
            for (int i = 0; i < 6; ++i)
                if (!client->keys[i]) { client->keys[i] = code; break; }
        } else {
            for (int i = 0; i < 6; ++i)
                if (client->keys[i] == code) client->keys[i] = 0;
        }
    }
send:
    ble_hid_keyboard(client->target, client->modifiers, client->keys);
}

static void put_be16(uint8_t *p, int16_t value)
{
    uint16_t v = (uint16_t)value;
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static bool send_frame(int fd, const void *data, uint32_t len);

static void send_screen_info(int fd)
{
    uint8_t info[18] = {'D', 'I', 'N', 'F'};
    const int16_t values[7] = {
        0, 0,
        APP_DESKFLOW_SCREEN_WIDTH, APP_DESKFLOW_SCREEN_HEIGHT,
        0,
        APP_DESKFLOW_SCREEN_WIDTH / 2, APP_DESKFLOW_SCREEN_HEIGHT / 2
    };
    for (int i = 0; i < 7; ++i) put_be16(info + 4 + i * 2, values[i]);
    if (send_frame(fd, info, sizeof(info))) {
        ESP_LOGI(TAG, "sent screen info %dx%d",
                 APP_DESKFLOW_SCREEN_WIDTH, APP_DESKFLOW_SCREEN_HEIGHT);
    } else {
        ESP_LOGE(TAG, "failed to send screen info");
    }
}

static void parse_frame(deskflow_client_t *client, int fd, const uint8_t *p, size_t n)
{
    if (n < 4) return;
    if (!memcmp(p, "QINF", 4)) {
        send_screen_info(fd);
    } else if (!memcmp(p, "CALV", 4)) {
        send_frame(fd, "CALV", 4);
    } else if (!memcmp(p, "CINN", 4) && n >= 14) {
        client->last_x = (int16_t)be16(p + 4);
        client->last_y = (int16_t)be16(p + 6);
        client->have_position = true;
        memset(client->keys, 0, sizeof(client->keys));
        client->buttons = 0;
        client->modifiers = hid_modifiers(be16(p + 12));
        ble_hid_keyboard(client->target, client->modifiers, client->keys);
        ble_hid_mouse(client->target, 0, 0, 0, 0);
        ESP_LOGI(TAG, "[%s] cursor entered at %d,%d",
                 client->screen_name, client->last_x, client->last_y);
    } else if (!memcmp(p, "DKDL", 4) && n >= 14) {
        /* Protocol 1.8: key, modifier mask, physical button, language string. */
        key_event(client, be16(p + 4), be16(p + 6), true);
    } else if (!memcmp(p, "DKDN", 4) && n >= 10) {
        key_event(client, be16(p + 4), be16(p + 6), true);
    }
    else if (!memcmp(p, "DKUP", 4) && n >= 10) key_event(client, be16(p + 4), be16(p + 6), false);
    else if (!memcmp(p, "DKRP", 4) && n >= 12) {
        key_event(client, be16(p + 4), be16(p + 6), true);
        key_event(client, be16(p + 4), be16(p + 6), false);
    } else if (!memcmp(p, "DMDN", 4) && n >= 5) {
        unsigned b = p[4];
        if (b >= 1 && b <= 5) client->buttons |= 1u << (b - 1);
        ble_hid_mouse(client->target, client->buttons, 0, 0, 0);
    } else if (!memcmp(p, "DMUP", 4) && n >= 5) {
        unsigned b = p[4];
        if (b >= 1 && b <= 5) client->buttons &= ~(1u << (b - 1));
        ble_hid_mouse(client->target, client->buttons, 0, 0, 0);
    } else if (!memcmp(p, "DMRM", 4) && n >= 8) {
        mouse_move(client, (int16_t)be16(p + 4), (int16_t)be16(p + 6));
    } else if (!memcmp(p, "DMMV", 4) && n >= 8) {
        int16_t x = be16(p + 4), y = be16(p + 6);
        if (client->have_position) {
            int dx = x - client->last_x, dy = y - client->last_y;
            mouse_move(client, dx, dy);
        }
        client->last_x = x; client->last_y = y; client->have_position = true;
    } else if (!memcmp(p, "DMWM", 4) && n >= 8) {
        int16_t dy = be16(p + 6);
        int wheel = dy / 120;
        if (!wheel && dy) wheel = dy > 0 ? 1 : -1;
        ble_hid_mouse(client->target, client->buttons, 0, 0,
                      wheel > 127 ? 127 : wheel < -127 ? -127 : wheel);
    } else if (!memcmp(p, "COUT", 4)) {
        memset(client->keys, 0, sizeof(client->keys));
        client->modifiers = client->buttons = 0;
        client->have_position = false;
        ble_hid_keyboard(client->target, 0, client->keys);
        ble_hid_mouse(client->target, 0, 0, 0, 0);
    }
}

static bool recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        int got = recv(fd, p, len, 0);
        if (got <= 0) return false;
        p += got; len -= got;
    }
    return true;
}

static bool send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        int sent = send(fd, p, len, 0);
        if (sent <= 0) return false;
        p += sent;
        len -= sent;
    }
    return true;
}

static bool send_frame(int fd, const void *data, uint32_t len)
{
    uint32_t net_len = htonl(len);
    uint8_t packet[4 + 128];
    if (len > sizeof(packet) - 4) return false;
    memcpy(packet, &net_len, 4);
    memcpy(packet + 4, data, len);
    return send_all(fd, packet, len + 4);
}

static void serve(deskflow_client_t *client, int fd)
{
    uint8_t frame[1024];
    while (true) {
        uint32_t net_len;
        if (!recv_all(fd, &net_len, sizeof(net_len))) break;
        uint32_t len = ntohl(net_len);
        if (!len || len > sizeof(frame)) {
            ESP_LOGW(TAG, "invalid frame size: %" PRIu32, len);
            break;
        }
        if (!recv_all(fd, frame, len)) break;
        if (len >= 11 && (!memcmp(frame, "Synergy", 7) || !memcmp(frame, "Barrier", 7))) {
            uint8_t hello[128];
            const char *magic = !memcmp(frame, "Barrier", 7) ? "Barrier" : "Synergy";
            size_t m = strlen(magic), name_len = strlen(client->screen_name);
            memcpy(hello, magic, m);
            /* Echo the server's major/minor version, then a 32-bit string length. */
            memcpy(hello + m, frame + m, 4);
            hello[m + 4] = (uint8_t)(name_len >> 24);
            hello[m + 5] = (uint8_t)(name_len >> 16);
            hello[m + 6] = (uint8_t)(name_len >> 8);
            hello[m + 7] = (uint8_t)name_len;
            memcpy(hello + m + 8, client->screen_name, name_len);
            if (send_frame(fd, hello, m + 8 + name_len)) {
                ESP_LOGI(TAG, "[%s] protocol handshake (%s) sent",
                         client->screen_name, magic);
            } else {
                ESP_LOGE(TAG, "protocol handshake send failed");
                break;
            }
        } else {
            parse_frame(client, fd, frame, len);
        }
    }
}

static void client_task(void *arg)
{
    deskflow_client_t *client_ctx = arg;
    while (true) {
        wifi_wait_connected();
        int client = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        struct sockaddr_in addr = { .sin_family = AF_INET,
            .sin_port = htons(APP_DESKFLOW_PORT) };
        if (client < 0 || inet_pton(AF_INET, APP_DESKFLOW_SERVER_IP, &addr.sin_addr) != 1 ||
            connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGW(TAG, "[%s] connect to %s:%d failed: errno %d",
                     client_ctx->screen_name, APP_DESKFLOW_SERVER_IP,
                     APP_DESKFLOW_PORT, errno);
            if (client >= 0) close(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "[%s] connected to Deskflow server %s:%d",
                 client_ctx->screen_name, APP_DESKFLOW_SERVER_IP, APP_DESKFLOW_PORT);
        serve(client_ctx, client);
        shutdown(client, SHUT_RDWR);
        close(client);
        ESP_LOGI(TAG, "[%s] Deskflow server disconnected; reconnecting",
                 client_ctx->screen_name);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t deskflow_start(void)
{
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        deskflow_client_t *client = &s_clients[i];
        client->target = i;
        snprintf(client->screen_name, sizeof(client->screen_name), "%s%u",
                 APP_DESKFLOW_SCREEN_NAME_PREFIX, (unsigned)i + 1);
        snprintf(client->task_name, sizeof(client->task_name), "deskflow-%u",
                 (unsigned)i + 1);
        if (xTaskCreate(client_task, client->task_name, 6144, client, 5, NULL) != pdPASS)
            return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
