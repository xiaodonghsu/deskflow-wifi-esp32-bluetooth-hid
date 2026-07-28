#include "ble_hid.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* NimBLE's store/config header exposes the backend APIs but not its initializer. */
void ble_store_config_init(void);

static const char *TAG = "ble_hid";
static uint16_t s_keyboard_handle;
static uint16_t s_mouse_handle;
static uint8_t s_protocol_mode = 1;
static uint8_t s_control_point;
static uint8_t s_own_addr_type;

typedef struct {
    bool connected;
    bool has_peer;
    uint16_t conn_handle;
    ble_addr_t peer_addr;
} hid_peer_t;

static hid_peer_t s_peers[APP_MAX_HID_DEVICES];
static portMUX_TYPE s_peers_lock = portMUX_INITIALIZER_UNLOCKED;

static const uint8_t s_report_map[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65,
    0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00, 0xC0,
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x02, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
    0x95,0x05, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x03, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38,
    0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03, 0x81,0x06, 0xC0,0xC0
};
static const uint8_t s_hid_info[] = {0x11, 0x01, 0x00, 0x02};
static const uint8_t s_keyboard_ref[] = {1, 1};
static const uint8_t s_mouse_ref[] = {2, 1};

static int read_bytes(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    uintptr_t kind = (uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        if (kind == 1) return read_bytes(ctxt, s_hid_info, sizeof(s_hid_info));
        if (kind == 2) return read_bytes(ctxt, s_report_map, sizeof(s_report_map));
        if (kind == 3) return read_bytes(ctxt, &s_protocol_mode, 1);
        if (kind == 5) return read_bytes(ctxt, s_keyboard_ref, 2);
        if (kind == 6) return read_bytes(ctxt, s_mouse_ref, 2);
        if (kind == 7) { const uint8_t report[8] = {0}; return read_bytes(ctxt, report, 8); }
        if (kind == 8) { const uint8_t report[4] = {0}; return read_bytes(ctxt, report, 4); }
        return 0;
    }
    if (kind == 3) return ble_hs_mbuf_to_flat(ctxt->om, &s_protocol_mode, 1, NULL);
    if (kind == 4) return ble_hs_mbuf_to_flat(ctxt->om, &s_control_point, 1, NULL);
    return 0;
}

static struct ble_gatt_dsc_def s_keyboard_dsc[] = {
    { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
      .access_cb = gatt_access, .arg = (void *)5 }, {0}
};
static struct ble_gatt_dsc_def s_mouse_dsc[] = {
    { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
      .access_cb = gatt_access, .arg = (void *)6 }, {0}
};
static const struct ble_gatt_chr_def s_hid_chrs[] = {
    { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = gatt_access, .arg = (void *)1,
      .flags = BLE_GATT_CHR_F_READ },
    { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = gatt_access, .arg = (void *)2,
      .flags = BLE_GATT_CHR_F_READ },
    { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = gatt_access, .arg = (void *)3,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = gatt_access, .arg = (void *)4,
      .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = gatt_access, .arg = (void *)7,
      .val_handle = &s_keyboard_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .descriptors = s_keyboard_dsc },
    { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = gatt_access, .arg = (void *)8,
      .val_handle = &s_mouse_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .descriptors = s_mouse_dsc },
    {0}
};
static const struct ble_gatt_svc_def s_services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
      .characteristics = s_hid_chrs }, {0}
};

static void advertise(void);

static bool addr_equal(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

static ble_addr_t resolved_peer_addr(const struct ble_gap_conn_desc *desc)
{
    static const uint8_t zero_addr[6] = {0};
    ble_addr_t addr = desc->peer_id_addr;
    if (addr.type > BLE_ADDR_RANDOM ||
        memcmp(addr.val, zero_addr, sizeof(zero_addr)) == 0) {
        addr = desc->peer_ota_addr;
    }
    return addr;
}

static void save_peer(size_t target)
{
    uint8_t data[7];
    char key[8];
    nvs_handle_t nvs;

    taskENTER_CRITICAL(&s_peers_lock);
    data[0] = s_peers[target].peer_addr.type;
    memcpy(data + 1, s_peers[target].peer_addr.val, 6);
    taskEXIT_CRITICAL(&s_peers_lock);

    snprintf(key, sizeof(key), "peer%u", (unsigned)target);
    if (nvs_open("hid_slots", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, key, data, sizeof(data));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void load_peers(void)
{
    nvs_handle_t nvs;
    if (nvs_open("hid_slots", NVS_READONLY, &nvs) != ESP_OK) return;

    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        uint8_t data[7];
        size_t len = sizeof(data);
        char key[8];
        snprintf(key, sizeof(key), "peer%u", (unsigned)i);
        if (nvs_get_blob(nvs, key, data, &len) == ESP_OK && len == sizeof(data)) {
            s_peers[i].peer_addr.type = data[0];
            memcpy(s_peers[i].peer_addr.val, data + 1, 6);
            s_peers[i].has_peer = true;
            s_peers[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
    }
    nvs_close(nvs);
}

static int target_for_handle(uint16_t conn_handle)
{
    int target = -1;
    taskENTER_CRITICAL(&s_peers_lock);
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        if (s_peers[i].connected && s_peers[i].conn_handle == conn_handle) {
            target = (int)i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_peers_lock);
    return target;
}

static unsigned connected_count(void)
{
    unsigned count = 0;
    taskENTER_CRITICAL(&s_peers_lock);
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i)
        if (s_peers[i].connected) ++count;
    taskEXIT_CRITICAL(&s_peers_lock);
    return count;
}

static int assign_peer(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return -1;
    ble_addr_t addr = resolved_peer_addr(&desc);
    int target = -1;

    taskENTER_CRITICAL(&s_peers_lock);
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
        if (!s_peers[i].connected && s_peers[i].has_peer &&
            addr_equal(&s_peers[i].peer_addr, &addr)) {
            target = (int)i;
            break;
        }
    }
    if (target < 0) {
        for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
            if (!s_peers[i].connected && !s_peers[i].has_peer) {
                target = (int)i;
                break;
            }
        }
    }
    if (target < 0) {
        for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i) {
            if (!s_peers[i].connected) {
                target = (int)i;
                break;
            }
        }
    }
    if (target >= 0) {
        s_peers[target].connected = true;
        s_peers[target].has_peer = true;
        s_peers[target].conn_handle = conn_handle;
        s_peers[target].peer_addr = addr;
    }
    taskEXIT_CRITICAL(&s_peers_lock);

    return target;
}

static void update_peer_identity(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int target = target_for_handle(conn_handle);
    if (target < 0 || ble_gap_conn_find(conn_handle, &desc) != 0) return;
    ble_addr_t addr = resolved_peer_addr(&desc);
    taskENTER_CRITICAL(&s_peers_lock);
    s_peers[target].peer_addr = addr;
    s_peers[target].has_peer = true;
    taskEXIT_CRITICAL(&s_peers_lock);
    save_peer((size_t)target);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            int target = assign_peer(event->connect.conn_handle);
            if (target < 0) {
                ESP_LOGW(TAG, "no free HID target; disconnecting extra host");
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                ESP_LOGI(TAG, "HID host connected as target %d", target + 1);
                ble_gap_security_initiate(event->connect.conn_handle);
                advertise();
            }
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        int target = target_for_handle(event->disconnect.conn.conn_handle);
        if (target >= 0) {
            taskENTER_CRITICAL(&s_peers_lock);
            s_peers[target].connected = false;
            s_peers[target].conn_handle = BLE_HS_CONN_HANDLE_NONE;
            taskEXIT_CRITICAL(&s_peers_lock);
        }
        ESP_LOGI(TAG, "HID target %d disconnected, reason=%d",
                 target + 1, event->disconnect.reason);
        advertise();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "target %d subscription handle=%u notify=%u",
                 target_for_handle(event->subscribe.conn_handle) + 1,
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            update_peer_identity(event->enc_change.conn_handle);
            ESP_LOGI(TAG, "target %d secure link established",
                     target_for_handle(event->enc_change.conn_handle) + 1);
        } else {
            ESP_LOGW(TAG, "link security failed: %d", event->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGW(TAG, "peer requested pairing again; replacing old bond");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (ble_gap_adv_active() || connected_count() >= APP_MAX_HID_DEVICES) return;

    struct ble_hs_adv_fields fields = {0};
    const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)APP_BLE_DEVICE_NAME;
    fields.name_len = strlen(APP_BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t *)&hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,
        .itvl_max = 0x30
    };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) {
        ESP_LOGE(TAG, "advertise failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "connectable advertising started (%u/%u targets connected)",
                 connected_count(), APP_MAX_HID_DEVICES);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "unable to determine BLE identity address");
        return;
    }
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_init(void)
{
    for (size_t i = 0; i < APP_MAX_HID_DEVICES; ++i)
        s_peers[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    load_peers();

    int rc = nimble_port_init();
    if (rc) return ESP_FAIL;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(APP_BLE_DEVICE_NAME);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    rc = ble_gatts_count_cfg(s_services);
    if (!rc) rc = ble_gatts_add_svcs(s_services);
    if (rc) return ESP_FAIL;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool ble_hid_connected(size_t target)
{
    if (target >= APP_MAX_HID_DEVICES) return false;
    bool connected;
    taskENTER_CRITICAL(&s_peers_lock);
    connected = s_peers[target].connected;
    taskEXIT_CRITICAL(&s_peers_lock);
    return connected;
}

static void notify(size_t target, uint16_t handle, const void *data, uint16_t len)
{
    if (target >= APP_MAX_HID_DEVICES) return;
    uint16_t conn_handle;
    taskENTER_CRITICAL(&s_peers_lock);
    conn_handle = s_peers[target].connected
        ? s_peers[target].conn_handle : BLE_HS_CONN_HANDLE_NONE;
    taskEXIT_CRITICAL(&s_peers_lock);
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        int rc = ble_gatts_notify_custom(conn_handle, handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "target %u notification failed handle=%u rc=%d",
                     (unsigned)target + 1, handle, rc);
        }
    }
}

void ble_hid_keyboard(size_t target, uint8_t modifiers, const uint8_t keys[6])
{
    uint8_t report[8] = {modifiers, 0};
    memcpy(report + 2, keys, 6);
    notify(target, s_keyboard_handle, report, sizeof(report));
}

void ble_hid_mouse(size_t target, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    const uint8_t report[4] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    notify(target, s_mouse_handle, report, sizeof(report));
}
