/*
 * Control-frame worker: parses App commands and injects HID reports
 * (touch digitizer + keyboard) through the esp_hid device profile.
 */
#ifndef IZUKI_INPUT_H_
#define IZUKI_INPUT_H_

#include <stdint.h>
#include "esp_hidd.h"

void izuki_input_init(esp_hidd_dev_t *dev);

/* Enqueue a control frame for asynchronous processing. Non-blocking. */
void izuki_input_feed(const uint8_t *frame, uint16_t len);

/* Send a HANDSHAKE_ACK event (also used on first Event subscription). */
void izuki_input_send_handshake_ack(void);

/* Send an EVT_STATUS frame describing the current HID readiness. */
void izuki_input_send_status(void);

/* Clear touch/trajectory state (call on connect/disconnect). */
void izuki_input_reset(void);

#endif /* IZUKI_INPUT_H_ */
