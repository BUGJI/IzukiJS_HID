/*
 * Izuki JS BLE-HID control protocol (v2).
 *
 * Mirrors docs/BLE_HID_PROTOCOL.md in the Android app repo
 * (https://github.com/BUGJI/IzukiJS). All multi-byte integers are
 * little-endian.
 */
#ifndef IZUKI_PROTO_H_
#define IZUKI_PROTO_H_

#include <stdint.h>
#include <stdbool.h>

#define IZUKI_PROTOCOL_VERSION 2

/* App -> dongle (Control characteristic) */
#define CMD_HANDSHAKE        0x01
#define CMD_SET_RESOLUTION   0x02
#define CMD_TAP              0x03
#define CMD_SWIPE            0x04
#define CMD_KEY              0x05
#define CMD_TEXT             0x06
#define CMD_PING             0x07
/* Timed single-finger trajectory: flags:u8, count:u8,
 * count * { x:u16, y:u16, dt:u16 }. Dongle interpolates each segment and may
 * hold (dt with unchanged coords). Serves complex paths / press-and-hold. */
#define CMD_GESTURE          0x08

/* CMD_GESTURE flags */
#define IZUKI_GESTURE_KEEP_DOWN 0x01  /* keep tip pressed after this frame */
#define IZUKI_GESTURE_START     0x02  /* first frame of a new trajectory: force a
                                       * fresh press (lifts any stale tip left
                                       * down by an interrupted gesture). */

/* Dongle -> App (Event characteristic) */
#define EVT_HANDSHAKE_ACK    0x81
#define EVT_ACK              0x82
#define EVT_STATUS           0x83
#define EVT_ERROR            0x84
#define EVT_PONG             0x87

/* EVT_STATUS payload byte: link/readiness flags.
 * HID_READY now requires link + encryption AND the phone's HID host having
 * subscribed to a HID input report CCCD (otherwise injected reports are
 * silently dropped by the host). */
#define IZUKI_STATUS_LINK_UP      0x01
#define IZUKI_STATUS_ENCRYPTED    0x02
#define IZUKI_STATUS_HID_READY    0x04
#define IZUKI_STATUS_APP_READY    0x08

/* EVT_ERROR payload byte: reason codes. */
#define IZUKI_ERR_HID_NOT_LINKED  0x01
#define IZUKI_ERR_HID_NOT_READY   0x02
#define IZUKI_ERR_INPUT_SET       0x03

/* HID report IDs (see report map in main.c) */
#define IZUKI_REPORT_ID_TOUCH    1
#define IZUKI_REPORT_ID_KEYBOARD 2

/* Digitizer logical range (must match report map Logical Maximum). */
#define IZUKI_DIGITIZER_MAX  32767

/* Largest accepted control frame. */
#define IZUKI_MAX_FRAME      260

/* Max timed points per CMD_GESTURE frame: 1(cmd)+1(flags)+1(count)+6*n <= MAX_FRAME */
#define IZUKI_GESTURE_MAX_POINTS  ((IZUKI_MAX_FRAME - 3) / 6)

static inline uint16_t izuki_rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void izuki_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

#endif /* IZUKI_PROTO_H_ */
