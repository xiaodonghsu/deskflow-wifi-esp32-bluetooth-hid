#include "deskflow.h"
#include "app_config.h"
#include "ble_hid.h"
#include "wifi.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "deskflow";
static uint8_t s_modifiers, s_keys[6], s_buttons;
static int16_t s_last_x, s_last_y;
static bool s_have_position;

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static void mouse_move(int dx, int dy)
{
    while (dx || dy) {
        int8_t sx = dx > 127 ? 127 : dx < -127 ? -127 : (int8_t)dx;
        int8_t sy = dy > 127 ? 127 : dy < -127 ? -127 : (int8_t)dy;
        ble_hid_mouse(s_buttons, sx, sy, 0);
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

static void key_event(uint16_t key, uint16_t mask, bool down)
{
    uint8_t code = hid_key(key);
    s_modifiers = hid_modifiers(mask);
    switch (key) {
    case 0xEFE1: if (down) s_modifiers |= 0x02; else s_modifiers &= ~0x02; break;
    case 0xEFE2: if (down) s_modifiers |= 0x20; else s_modifiers &= ~0x20; break;
    case 0xEFE3: if (down) s_modifiers |= 0x01; else s_modifiers &= ~0x01; break;
    case 0xEFE4: if (down) s_modifiers |= 0x10; else s_modifiers &= ~0x10; break;
    case 0xEFE9: if (down) s_modifiers |= 0x04; else s_modifiers &= ~0x04; break;
    case 0xEFEA: if (down) s_modifiers |= 0x40; else s_modifiers &= ~0x40; break;
    case 0xEFE7:
    case 0xEFEB: if (down) s_modifiers |= 0x08; else s_modifiers &= ~0x08; break;
    case 0xEFE8:
    case 0xEFEC: if (down) s_modifiers |= 0x80; else s_modifiers &= ~0x80; break;
    default: break;
    }
    ESP_LOGI(TAG, "key %s id=0x%04x mask=0x%04x hid=0x%02x modifiers=0x%02x",
             down ? "down" : "up", key, mask, code, s_modifiers);
    if (code) {
        if (down) {
            for (int i = 0; i < 6; ++i)
                if (s_keys[i] == code) goto send;
            for (int i = 0; i < 6; ++i)
                if (!s_keys[i]) { s_keys[i] = code; break; }
        } else {
            for (int i = 0; i < 6; ++i)
                if (s_keys[i] == code) s_keys[i] = 0;
        }
    }
send:
    ble_hid_keyboard(s_modifiers, s_keys);
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

static void parse_frame(int fd, const uint8_t *p, size_t n)
{
    if (n < 4) return;
    if (!memcmp(p, "QINF", 4)) {
        send_screen_info(fd);
    } else if (!memcmp(p, "CALV", 4)) {
        send_frame(fd, "CALV", 4);
    } else if (!memcmp(p, "CINN", 4) && n >= 14) {
        s_last_x = (int16_t)be16(p + 4);
        s_last_y = (int16_t)be16(p + 6);
        s_have_position = true;
        ESP_LOGI(TAG, "cursor entered at %d,%d", s_last_x, s_last_y);
    } else if (!memcmp(p, "DKDL", 4) && n >= 14) {
        /* Protocol 1.8: key, modifier mask, physical button, language string. */
        key_event(be16(p + 4), be16(p + 6), true);
    } else if (!memcmp(p, "DKDN", 4) && n >= 10) {
        key_event(be16(p + 4), be16(p + 6), true);
    }
    else if (!memcmp(p, "DKUP", 4) && n >= 10) key_event(be16(p + 4), be16(p + 6), false);
    else if (!memcmp(p, "DKRP", 4) && n >= 12) {
        key_event(be16(p + 4), be16(p + 6), true);
        key_event(be16(p + 4), be16(p + 6), false);
    } else if (!memcmp(p, "DMDN", 4) && n >= 5) {
        unsigned b = p[4];
        if (b >= 1 && b <= 5) s_buttons |= 1u << (b - 1);
        ble_hid_mouse(s_buttons, 0, 0, 0);
    } else if (!memcmp(p, "DMUP", 4) && n >= 5) {
        unsigned b = p[4];
        if (b >= 1 && b <= 5) s_buttons &= ~(1u << (b - 1));
        ble_hid_mouse(s_buttons, 0, 0, 0);
    } else if (!memcmp(p, "DMRM", 4) && n >= 8) {
        mouse_move((int16_t)be16(p + 4), (int16_t)be16(p + 6));
    } else if (!memcmp(p, "DMMV", 4) && n >= 8) {
        int16_t x = be16(p + 4), y = be16(p + 6);
        if (s_have_position) {
            int dx = x - s_last_x, dy = y - s_last_y;
            mouse_move(dx, dy);
        }
        s_last_x = x; s_last_y = y; s_have_position = true;
    } else if (!memcmp(p, "DMWM", 4) && n >= 8) {
        int16_t dy = be16(p + 6);
        int wheel = dy / 120;
        if (!wheel && dy) wheel = dy > 0 ? 1 : -1;
        ble_hid_mouse(s_buttons, 0, 0, wheel > 127 ? 127 : wheel < -127 ? -127 : wheel);
    } else if (!memcmp(p, "COUT", 4)) {
        memset(s_keys, 0, sizeof(s_keys)); s_modifiers = s_buttons = 0;
        s_have_position = false;
        ble_hid_keyboard(0, s_keys); ble_hid_mouse(0, 0, 0, 0);
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

static void serve(int fd)
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
            size_t m = strlen(magic), name_len = strlen(APP_DESKFLOW_SCREEN_NAME);
            memcpy(hello, magic, m);
            /* Echo the server's major/minor version, then a 32-bit string length. */
            memcpy(hello + m, frame + m, 4);
            hello[m + 4] = (uint8_t)(name_len >> 24);
            hello[m + 5] = (uint8_t)(name_len >> 16);
            hello[m + 6] = (uint8_t)(name_len >> 8);
            hello[m + 7] = (uint8_t)name_len;
            memcpy(hello + m + 8, APP_DESKFLOW_SCREEN_NAME, name_len);
            if (send_frame(fd, hello, m + 8 + name_len)) {
                ESP_LOGI(TAG, "protocol handshake (%s) sent", magic);
            } else {
                ESP_LOGE(TAG, "protocol handshake send failed");
                break;
            }
        } else {
            parse_frame(fd, frame, len);
        }
    }
}

static void client_task(void *arg)
{
    (void)arg;
    while (true) {
        wifi_wait_connected();
        int client = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        struct sockaddr_in addr = { .sin_family = AF_INET,
            .sin_port = htons(APP_DESKFLOW_PORT) };
        if (client < 0 || inet_pton(AF_INET, APP_DESKFLOW_SERVER_IP, &addr.sin_addr) != 1 ||
            connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGW(TAG, "connect to %s:%d failed: errno %d",
                     APP_DESKFLOW_SERVER_IP, APP_DESKFLOW_PORT, errno);
            if (client >= 0) close(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "connected to Deskflow server %s:%d",
                 APP_DESKFLOW_SERVER_IP, APP_DESKFLOW_PORT);
        serve(client);
        shutdown(client, SHUT_RDWR);
        close(client);
        ESP_LOGI(TAG, "Deskflow server disconnected; reconnecting");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t deskflow_start(void)
{
    return xTaskCreate(client_task, "deskflow", 6144, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
