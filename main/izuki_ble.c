#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "os/os_mbuf.h"

#include "izuki_ble.h"
#include "izuki_proto.h"
#include "izuki_input.h"

static const char *TAG = "izuki_ble";

/*
 * Custom service UUIDs (little-endian byte order, i.e. reversed from the
 * canonical string form):
 *   Service 7d8a0001-9a1e-4b2a-8f3c-1d2e3f4a5b6c
 *   Control 7d8a0002-...
 *   Event   7d8a0003-...
 */
static const ble_uuid128_t izuki_svc_uuid =
    BLE_UUID128_INIT(0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x3c, 0x8f,
                     0x2a, 0x4b, 0x1e, 0x9a, 0x01, 0x00, 0x8a, 0x7d);
static const ble_uuid128_t izuki_ctl_uuid =
    BLE_UUID128_INIT(0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x3c, 0x8f,
                     0x2a, 0x4b, 0x1e, 0x9a, 0x02, 0x00, 0x8a, 0x7d);
static const ble_uuid128_t izuki_evt_uuid =
    BLE_UUID128_INIT(0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x3c, 0x8f,
                     0x2a, 0x4b, 0x1e, 0x9a, 0x03, 0x00, 0x8a, 0x7d);

#define IZUKI_MAX_ENC_FAILURES 3

static uint16_t s_event_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile bool s_encrypted;
static uint8_t s_enc_fail_count;

/* HID input report CCCD subscriptions done by the phone's HID host. Injected
 * reports are only received after the host subscribes, so readiness must not
 * depend on `esp_hidd_dev_connected()` alone (that turns true for ANY BLE
 * connection, including our own custom GATT client). The custom Event
 * characteristic is tracked separately (s_event_val_handle); every other
 * notify subscription is a HID report. */
#define IZUKI_MAX_HID_SUBS 8
static uint16_t s_hid_sub_handles[IZUKI_MAX_HID_SUBS];
static volatile uint8_t s_hid_sub_count;

static struct ble_hs_adv_fields s_adv;
static struct ble_hs_adv_fields s_rsp;
static ble_uuid16_t s_hid_uuid = BLE_UUID16_INIT(0x1812);

static int izuki_gap_event(struct ble_gap_event *event, void *arg);

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t buf[IZUKI_MAX_FRAME];
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out_len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* Hand off to the worker task; must not block the NimBLE host task. */
    izuki_input_feed(buf, out_len);
    return 0;
}

static int event_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* Notify-only characteristic. */
    return 0;
}

static const struct ble_gatt_svc_def izuki_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &izuki_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Control: App -> dongle. ENC so the App cannot drive HID
                 * before the link is secured (matches HOGP expectations). */
                .uuid = &izuki_ctl_uuid.u,
                .access_cb = control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                /* Event: dongle -> App. ENC gates the CCCD subscription too. */
                .uuid = &izuki_evt_uuid.u,
                .access_cb = event_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &s_event_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

void izuki_ble_service_init(void)
{
    int rc = ble_gatts_count_cfg(izuki_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(izuki_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "custom service registered (event val handle=%u)", s_event_val_handle);
}

void izuki_ble_adv_config(const char *device_name)
{
    memset(&s_adv, 0, sizeof(s_adv));
    s_adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv.uuids16 = &s_hid_uuid;
    s_adv.num_uuids16 = 1;
    s_adv.uuids16_is_complete = 1;
    s_adv.uuids128 = &izuki_svc_uuid;
    s_adv.num_uuids128 = 1;
    s_adv.uuids128_is_complete = 1;

    memset(&s_rsp, 0, sizeof(s_rsp));
    s_rsp.name = (uint8_t *)device_name;
    s_rsp.name_len = (uint8_t)strlen(device_name);
    s_rsp.name_is_complete = 1;
    s_rsp.appearance = 0x03C0; /* HID Generic */
    s_rsp.appearance_is_present = 1;
}

void izuki_ble_adv_start(void)
{
    struct ble_gap_adv_params adv_params;

    int rc = ble_gap_adv_set_fields(&s_adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
    }
    rc = ble_gap_adv_rsp_set_fields(&s_rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, izuki_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as Izuki-HID");
    }
}

int izuki_ble_notify(const uint8_t *data, uint16_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_event_val_handle == 0) {
        return -1;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat((void *)data, len);
    if (om == NULL) {
        return -1;
    }
    return ble_gatts_notify_custom(s_conn_handle, s_event_val_handle, om);
}

bool izuki_ble_is_subscribed(void)
{
    return s_subscribed;
}

bool izuki_ble_is_encrypted(void)
{
    return s_encrypted;
}

static void hid_sub_add(uint16_t handle)
{
    for (uint8_t i = 0; i < s_hid_sub_count; i++) {
        if (s_hid_sub_handles[i] == handle) {
            return;
        }
    }
    if (s_hid_sub_count < IZUKI_MAX_HID_SUBS) {
        s_hid_sub_handles[s_hid_sub_count++] = handle;
    }
}

static void hid_sub_remove(uint16_t handle)
{
    for (uint8_t i = 0; i < s_hid_sub_count; i++) {
        if (s_hid_sub_handles[i] == handle) {
            s_hid_sub_handles[i] = s_hid_sub_handles[--s_hid_sub_count];
            return;
        }
    }
}

bool izuki_ble_is_hid_subscribed(void)
{
    return s_hid_sub_count > 0;
}

void izuki_ble_clear_bonds(void)
{
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_clear rc=%d", rc);
    } else {
        ESP_LOGW(TAG, "bond store cleared");
    }
}

static int izuki_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                s_conn_handle != event->connect.conn_handle) {
                ESP_LOGW(TAG, "new conn=%d while conn=%d tracked",
                         event->connect.conn_handle, s_conn_handle);
            }
            s_conn_handle = event->connect.conn_handle;
            s_encrypted = false;
            s_hid_sub_count = 0;

            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn_handle, &desc) == 0 &&
                desc.sec_state.encrypted) {
                ESP_LOGI(TAG, "connected (conn=%d); link already encrypted",
                         s_conn_handle);
                s_encrypted = true;
            } else {
                ESP_LOGI(TAG, "connected (conn=%d); requesting encryption",
                         s_conn_handle);
                int rc = ble_gap_security_initiate(s_conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "ble_gap_security_initiate rc=%d", rc);
                }
            }
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            izuki_ble_adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; conn=%d reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_encrypted = false;
        s_hid_sub_count = 0;
        izuki_input_reset();
        /* NOTE: s_enc_fail_count is deliberately NOT reset here. Encryption
         * failures almost always drop the link immediately; resetting on every
         * disconnect would make IZUKI_MAX_ENC_FAILURES unreachable and leave a
         * stale bond wedged forever. It is cleared on successful encryption. */
        izuki_ble_adv_start();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc change; conn=%d status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        if (event->enc_change.status == 0) {
            s_encrypted = true;
            s_enc_fail_count = 0;
        } else {
            s_encrypted = false;
            if (s_enc_fail_count < 0xFF) {
                s_enc_fail_count++;
            }
            if (s_enc_fail_count >= IZUKI_MAX_ENC_FAILURES) {
                s_enc_fail_count = 0;
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                    ESP_LOGW(TAG, "encryption failed %u times; deleting stale bond for peer",
                             IZUKI_MAX_ENC_FAILURES);
                    ble_store_util_delete_peer(&desc.peer_id_addr);
                } else {
                    ESP_LOGW(TAG, "encryption failed %u times; clearing bond store",
                             IZUKI_MAX_ENC_FAILURES);
                    izuki_ble_clear_bonds();
                }
                ble_gap_terminate(event->enc_change.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            izuki_input_send_status();
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ESP_LOGW(TAG, "repeat pairing; deleting old bond (conn=%d rc=%d)",
                 event->repeat_pairing.conn_handle, rc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_event_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "event subscription=%d", s_subscribed);
            if (s_subscribed) {
                izuki_input_send_handshake_ack();
            } else {
                izuki_input_send_status();
            }
        } else {
            /* HID input report CCCD (touch/keyboard). This is what actually
             * makes injected reports reach the phone's HID host. */
            if (event->subscribe.cur_notify) {
                hid_sub_add(event->subscribe.attr_handle);
            } else {
                hid_sub_remove(event->subscribe.attr_handle);
            }
            ESP_LOGI(TAG, "HID report sub handle=%u notify=%d (total=%u)",
                     event->subscribe.attr_handle, event->subscribe.cur_notify,
                     s_hid_sub_count);
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                izuki_input_send_status();
            }
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete; restarting");
        izuki_ble_adv_start();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated: conn=%d value=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}
