/*
 * Izuki JS BLE-HID dongle firmware for ESP32-C3 (4MB).
 *
 * Presents a HID device (absolute-coordinate touch screen + keyboard) and a
 * custom GATT control channel that mirrors docs/BLE_HID_PROTOCOL.md from the
 * Izuki JS Android app.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "store/config/ble_store_config.h"

#include "esp_hidd.h"

#include "izuki_ble.h"
#include "izuki_input.h"

/* Provided by NimBLE's store/config; not declared in its header. */
void ble_store_config_init(void);

static const char *TAG = "izuki";

/*
 * HID report map: report id 1 = single-contact multi-touch touchscreen
 * (X/Y 0..32767), report id 2 = boot-style keyboard (6KRO + modifiers).
 */
static const uint8_t izuki_report_map[] = {
    /* ---- Multi-touch Touch Screen, Report ID 1 ----
     *
     * Android only treats a HID device as a *touchscreen* (not an indirect
     * touchpad whose pointer is parked at 0,0) when the kernel input device
     * carries INPUT_PROP_DIRECT. The Linux HID stack sets that property in two
     * mutually exclusive ways:
     *
     *   1. Generic hid-input sets it only for a single-touch "Pen" (0x02)
     *      application -- and only since ~v5.0. Kernels 4.x never set it.
     *   2. hid-multitouch sets it (INPUT_MT_DIRECT via input_mt_init_slots)
     *      for a "Touch Screen" (0x04) application as soon as a Contact
     *      Identifier (0x51) is present. That usage makes hid-core assign
     *      HID_GROUP_MULTITOUCH, which binds hid-multitouch on every kernel
     *      since v4.x.
     *
     * So we expose a single-contact multi-touch digitizer ("Touch Screen" +
     * Contact Identifier + Contact Count). One contact is plenty because the
     * firmware only ever drives one finger.
     *
     * IMPORTANT: Android caches the Report Map per bonded device, so after
     * this change the phone may still decode reports with the *previous*
     * 5-byte map (TipSwitch:1, X:16, Y:16) until it is unpaired. To stay
     * correct even in that case, X/Y are placed at exactly the same bit
     * offsets as the old map (X at byte 1, Y at byte 3); the Contact
     * Identifier / Contact Count bytes are appended after Y instead of being
     * inserted before it.
     *
     * Report layout (7 bytes):
     *   byte 0     Tip Switch (bit0)
     *   bytes 1-2  X (0..32767)
     *   bytes 3-4  Y (0..32767)
     *   byte 5     Contact Identifier (always 0)
     *   byte 6     Contact Count (0 or 1) */
    0x05, 0x0D,             /* Usage Page (Digitizers) */
    0x09, 0x04,             /* Usage (Touch Screen) -> INPUT_PROP_DIRECT */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    0x09, 0x22,             /*   Usage (Finger) */
    0xA1, 0x02,             /*   Collection (Logical) */
    0x09, 0x42,             /*     Usage (Tip Switch) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x75, 0x01,             /*     Report Size (1) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) -> byte 0 bit 0 */
    0x75, 0x07,             /*     Report Size (7) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x03,             /*     Input (Const) -> pad to 1 byte */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) -> bytes 1-2 X, 3-4 Y */
    0x05, 0x0D,             /*     Usage Page (Digitizers) */
    0x09, 0x51,             /*     Usage (Contact Identifier) -> MT bind */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) -> byte 5 */
    0x09, 0x54,             /*     Usage (Contact Count) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) -> byte 6 */
    0xC0,                   /*   End Collection (Logical) */
    0x09, 0x55,             /*   Usage (Contact Count Maximum) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0xB1, 0x02,             /*   Feature (Data,Var,Abs) -> maxcontacts = 1 */
    0xC0,                   /* End Collection (Application) */

    /* ---- Keyboard, Report ID 2 ---- */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x06,             /* Usage (Keyboard) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x02,             /*   Report ID (2) */
    0x05, 0x07,             /*   Usage Page (Key Codes) */
    0x19, 0xE0,             /*   Usage Minimum (0xE0) */
    0x29, 0xE7,             /*   Usage Maximum (0xE7) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x08,             /*   Report Count (8) */
    0x81, 0x02,             /*   Input (Data,Var,Abs) -> modifiers */
    0x95, 0x01,             /*   Report Count (1) */
    0x75, 0x08,             /*   Report Size (8) */
    0x81, 0x03,             /*   Input (Const) -> reserved byte */
    0x05, 0x07,             /*   Usage Page (Key Codes) */
    0x19, 0x00,             /*   Usage Minimum (0) */
    0x29, 0x65,             /*   Usage Maximum (0x65) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x65,             /*   Logical Maximum (0x65) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x06,             /*   Report Count (6) */
    0x81, 0x00,             /*   Input (Data,Ary,Abs) -> 6 keys */
    0xC0,                   /* End Collection */
};

static esp_hid_raw_report_map_t izuki_report_maps[] = {
    {
        .data = izuki_report_map,
        .len = sizeof(izuki_report_map),
    },
};

static esp_hid_device_config_t izuki_hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x0001,
    .version = 0x0100,
    .device_name = "Izuki-HID",
    .manufacturer_name = "Izuki",
    .serial_number = "0001",
    .report_maps = izuki_report_maps,
    .report_maps_len = 1,
};

static esp_hidd_dev_t *s_hid_dev;

/* Per-board BLE name: "Izuki-HID-XXXXXX" from the last 3 bytes of the BT MAC. */
static char s_device_name[24];

static void build_device_name(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_device_name, sizeof(s_device_name), "Izuki-HID-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

static void hid_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID device started");
        izuki_ble_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID connected (dev_connected=%d)",
                 s_hid_dev != NULL && esp_hidd_dev_connected(s_hid_dev) ? 1 : 0);
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "HID disconnected (dev_connected=%d)",
                 s_hid_dev != NULL && esp_hidd_dev_connected(s_hid_dev) ? 1 : 0);
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "HID protocol mode changed");
        break;
    default:
        break;
    }
}

static void izuki_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

/* Boot-time sanity check: confirm the HID parser found our reports/lengths. */
static void izuki_log_report_map(void)
{
    esp_hid_report_map_t *map =
        esp_hid_parse_report_map(izuki_report_map, sizeof(izuki_report_map));
    if (map == NULL) {
        ESP_LOGE(TAG, "report map failed to parse!");
        return;
    }
    ESP_LOGI(TAG, "report map: usage=%s appearance=0x%04x reports=%u",
             esp_hid_usage_str(map->usage), map->appearance, map->reports_len);
    for (uint8_t i = 0; i < map->reports_len; i++) {
        ESP_LOGI(TAG, "  report id=%u type=%s len=%u",
                 map->reports[i].report_id,
                 esp_hid_report_type_str(map->reports[i].report_type),
                 map->reports[i].value_len);
    }
    esp_hid_free_report_map(map);
}

static void izuki_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    /* Host configuration. */
    ble_hs_cfg.reset_cb = izuki_on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    /* Just Works pairing + bonding (no passkey UI required). */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    build_device_name();
    izuki_hid_config.device_name = s_device_name;
    ESP_LOGI(TAG, "device name: %s", s_device_name);

    izuki_log_report_map();

    /* HID device profile (digitizer + keyboard). */
    ESP_ERROR_CHECK(esp_hidd_dev_init(&izuki_hid_config, ESP_HID_TRANSPORT_BLE,
                                      hid_event_cb, &s_hid_dev));

    /* Custom control/event GATT service must be added before host start. */
    ble_svc_gap_device_name_set(s_device_name);
    izuki_ble_service_init();
    izuki_ble_adv_config(s_device_name);
    izuki_input_init(s_hid_dev);

    ESP_LOGI(TAG, "starting NimBLE host");
    nimble_port_freertos_init(izuki_host_task);
}
