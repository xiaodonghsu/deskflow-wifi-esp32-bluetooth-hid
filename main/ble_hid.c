#include "ble_hid.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
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
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_keyboard_handle;
static uint16_t s_mouse_handle;
static uint8_t s_protocol_mode = 1;
static uint8_t s_control_point;
static uint8_t s_own_addr_type;
static ble_addr_t s_last_peer;
static bool s_have_last_peer;
static bool s_directed_advertising;

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
static void advertise_for_reconnect(void);

static void remember_peer(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return;

    s_last_peer = desc.peer_id_addr;
    static const uint8_t zero_addr[6] = {0};
    if (s_last_peer.type > BLE_ADDR_RANDOM ||
        memcmp(s_last_peer.val, zero_addr, sizeof(zero_addr)) == 0) {
        s_last_peer = desc.peer_ota_addr;
    }
    s_have_last_peer = true;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_directed_advertising = false;
            remember_peer(s_conn);
            ESP_LOGI(TAG, "HID host connected");
            ble_gap_security_initiate(s_conn);
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_directed_advertising = false;
        ESP_LOGI(TAG, "HID host disconnected, reason=%d; starting reconnect advertising",
                 event->disconnect.reason);
        advertise_for_reconnect();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscription handle=%u notify=%u",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            remember_peer(event->enc_change.conn_handle);
            ESP_LOGI(TAG, "secure link established; bond available for reconnect");
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
        if (s_directed_advertising) {
            s_directed_advertising = false;
            ESP_LOGI(TAG, "directed reconnect timed out; falling back to fast advertising");
        }
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (ble_gap_adv_active()) return;

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
        ESP_LOGI(TAG, "fast connectable advertising started");
    }
}

static void advertise_for_reconnect(void)
{
    if (!s_have_last_peer || ble_gap_adv_active()) {
        advertise();
        return;
    }

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_DIR,
        .disc_mode = BLE_GAP_DISC_MODE_NON,
        .high_duty_cycle = 1
    };
    s_directed_advertising = true;
    int rc = ble_gap_adv_start(s_own_addr_type, &s_last_peer, BLE_HS_FOREVER,
                               &p, gap_event, NULL);
    if (rc) {
        s_directed_advertising = false;
        ESP_LOGW(TAG, "directed reconnect advertising failed: %d", rc);
        advertise();
    } else {
        ESP_LOGI(TAG, "directed reconnect advertising started");
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

bool ble_hid_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static void notify(uint16_t handle, const void *data, uint16_t len)
{
    if (!ble_hid_connected()) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        int rc = ble_gatts_notify_custom(s_conn, handle, om);
        if (rc != 0) ESP_LOGW(TAG, "notification failed handle=%u rc=%d", handle, rc);
    }
}

void ble_hid_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    uint8_t report[8] = {modifiers, 0};
    memcpy(report + 2, keys, 6);
    notify(s_keyboard_handle, report, sizeof(report));
}

void ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    const uint8_t report[4] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    notify(s_mouse_handle, report, sizeof(report));
}
