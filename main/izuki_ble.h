/*
 * Custom GATT service + advertising for the Izuki JS dongle.
 *
 * Service 7d8a0001-...  Control (write)  Event (notify)
 * Advertises HID (0x1812) and the custom service UUID so the app's scan
 * filter matches, plus the device name "Izuki-HID" in the scan response.
 */
#ifndef IZUKI_BLE_H_
#define IZUKI_BLE_H_

#include <stdint.h>
#include <stdbool.h>

/* Register the custom service (call after esp_hidd_dev_init, before the
 * NimBLE host task is started). */
void izuki_ble_service_init(void);

/* Build advertising + scan-response fields (call before host start).
 * `device_name` must stay valid for the lifetime of advertising. */
void izuki_ble_adv_config(const char *device_name);

/* Start (or restart) advertising. Safe to call after host sync. */
void izuki_ble_adv_start(void);

/* Notify the Event characteristic with `len` bytes. Returns 0 on success. */
int izuki_ble_notify(const uint8_t *data, uint16_t len);

/* Whether the app has subscribed to Event notifications. */
bool izuki_ble_is_subscribed(void);

/* Whether the phone's HID host has subscribed to at least one HID input report
 * CCCD. Reports are only received by the host after this becomes true. */
bool izuki_ble_is_hid_subscribed(void);

/* Whether the current link has completed pairing/encryption. */
bool izuki_ble_is_encrypted(void);

/* Delete all stored bonding keys (clears stale/conflicting pairings). */
void izuki_ble_clear_bonds(void);

#endif /* IZUKI_BLE_H_ */
