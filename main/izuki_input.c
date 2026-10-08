#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "izuki_input.h"
#include "izuki_ble.h"
#include "izuki_proto.h"

static const char *TAG = "izuki_input";

#define IZUKI_QUEUE_LEN   64
#define IZUKI_WORKER_STACK 4096
#define IZUKI_WORKER_PRIO  5

typedef struct {
    uint16_t len;
    uint8_t data[IZUKI_MAX_FRAME];
} izuki_cmd_t;

static QueueHandle_t s_queue;
static esp_hidd_dev_t *s_dev;

static uint16_t s_screen_w;
static uint16_t s_screen_h;

/* Keyboard state kept across separate KEY down/up frames. */
static uint8_t s_modifier;
static uint8_t s_keys[6];

static bool s_error_active;
static uint8_t s_error_code;

/* Touch state kept across CMD_GESTURE frames so one trajectory can be split
 * into multiple frames (see IZUKI_GESTURE_KEEP_DOWN) without lifting in between.
 * Coordinates here are already mapped to the digitizer range. */
static bool s_touching;
static uint16_t s_last_x;
static uint16_t s_last_y;

/* Last coordinate actually put on the wire, used to defeat the Linux input
 * core's duplicate-EV_ABS suppression (see send_touch). */
static uint16_t s_sent_x;
static uint16_t s_sent_y;

#define IZUKI_GESTURE_STEP_MS 16

/* ---- HID readiness / failure reporting ---------------------------------- */

static bool hid_link_up(void)
{
    return s_dev != NULL && esp_hidd_dev_connected(s_dev);
}

static uint8_t hid_status_flags(void)
{
    uint8_t flags = 0;
    if (hid_link_up()) {
        flags |= IZUKI_STATUS_LINK_UP;
    }
    if (izuki_ble_is_encrypted()) {
        flags |= IZUKI_STATUS_ENCRYPTED;
    }
    if ((flags & IZUKI_STATUS_LINK_UP) && (flags & IZUKI_STATUS_ENCRYPTED) &&
        izuki_ble_is_hid_subscribed()) {
        flags |= IZUKI_STATUS_HID_READY;
    }
    if (izuki_ble_is_subscribed()) {
        flags |= IZUKI_STATUS_APP_READY;
    }
    return flags;
}

void izuki_input_send_status(void)
{
    uint8_t evt[2];
    evt[0] = EVT_STATUS;
    evt[1] = hid_status_flags();
    ESP_LOGI(TAG, "HID status flags=0x%02x (link=%d enc=%d hid_sub=%d)",
             evt[1], (evt[1] & IZUKI_STATUS_LINK_UP) ? 1 : 0,
             (evt[1] & IZUKI_STATUS_ENCRYPTED) ? 1 : 0,
             izuki_ble_is_hid_subscribed() ? 1 : 0);
    izuki_ble_notify(evt, sizeof(evt));
}

static void report_input_result(esp_err_t err)
{
    if (err == ESP_OK) {
        if (s_error_active) {
            s_error_active = false;
            izuki_input_send_status();
        }
        return;
    }

    uint8_t code;
    if (!hid_link_up()) {
        code = IZUKI_ERR_HID_NOT_LINKED;
    } else if (!izuki_ble_is_encrypted() || !izuki_ble_is_hid_subscribed()) {
        code = IZUKI_ERR_HID_NOT_READY;
    } else {
        code = IZUKI_ERR_INPUT_SET;
    }

    if (!s_error_active || s_error_code != code) {
        s_error_active = true;
        s_error_code = code;
        uint8_t evt[2] = { EVT_ERROR, code };
        ESP_LOGW(TAG, "HID input error 0x%02x (err=%d, link=%d, enc=%d, hid_sub=%d)",
                 code, err, hid_link_up() ? 1 : 0, izuki_ble_is_encrypted() ? 1 : 0,
                 izuki_ble_is_hid_subscribed() ? 1 : 0);
        izuki_ble_notify(evt, sizeof(evt));
    }
}

/* ---- HID report helpers ------------------------------------------------- */

static esp_err_t hid_input_send(size_t report_id, const uint8_t *data, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hid_link_up()) {
        ESP_LOGW(TAG, "HID report %u dropped: not connected", (unsigned)report_id);
        report_input_result(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, report_id, (uint8_t *)data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HID report %u input_set failed: %d", (unsigned)report_id, err);
    }
    report_input_result(err);
    return err;
}

static void send_touch(uint16_t raw_x, uint16_t raw_y, bool tip)
{
    /* The Linux input core silently drops EV_ABS events whose value equals the
     * axis's stored value. For a gesture that keeps one axis constant (e.g. a
     * perfectly vertical swipe), that axis is therefore never re-emitted, and
     * Android's InputReader starts the new contact from the axis's stale/zero
     * value -- the symptom is a touch that always begins at x=0.
     *
     * Nudge a repeated coordinate by one raw unit (1/32768 of the panel, well
     * under a pixel) so every report carries a change and both axes are
     * delivered.
     *
     * Only track this while the tip is down: hid-multitouch does NOT emit X/Y
     * for the tip-up frame (it only clears the tracking id), so updating the
     * tracker on release would desync it from the driver's stored value and the
     * next press frame would be dropped as a duplicate. */
    if (tip) {
        if (raw_x == s_sent_x) {
            raw_x ^= 1u;
        }
        if (raw_y == s_sent_y) {
            raw_y ^= 1u;
        }
        s_sent_x = raw_x;
        s_sent_y = raw_y;
    }

    /* Must match the multi-touch Touch Screen report map (report id 1). X/Y
     * deliberately sit at the same offsets as the older 5-byte map so a phone
     * that still has the previous Report Map cached keeps decoding correctly:
     *   byte 0 tip switch, bytes 1-2 X, bytes 3-4 Y, byte 5 contact id,
     *   byte 6 contact count. */
    uint8_t rpt[7];
    rpt[0] = tip ? 0x01 : 0x00; /* Tip Switch */
    izuki_wr_u16(&rpt[1], raw_x);
    izuki_wr_u16(&rpt[3], raw_y);
    rpt[5] = 0x00;              /* Contact Identifier (only one contact) */
    rpt[6] = tip ? 0x01 : 0x00; /* Contact Count */
    hid_input_send(IZUKI_REPORT_ID_TOUCH, rpt, sizeof(rpt));
}

static uint16_t map_coord(uint16_t v, uint16_t screen)
{
    if (screen == 0) {
        return v > IZUKI_DIGITIZER_MAX ? IZUKI_DIGITIZER_MAX : v;
    }
    uint32_t out = ((uint32_t)v * (IZUKI_DIGITIZER_MAX + 1)) / screen;
    if (out > IZUKI_DIGITIZER_MAX) {
        out = IZUKI_DIGITIZER_MAX;
    }
    return (uint16_t)out;
}

static void send_keyboard_report(uint8_t modifier, const uint8_t keys[6])
{
    uint8_t rpt[8] = {0};
    rpt[0] = modifier;
    memcpy(&rpt[2], keys, 6);
    hid_input_send(IZUKI_REPORT_ID_KEYBOARD, rpt, sizeof(rpt));
}

/* ---- Command handlers --------------------------------------------------- */

/* Drop any finger left down by an interrupted trajectory before a new gesture. */
static void gesture_release(void)
{
    if (s_touching) {
        send_touch(s_last_x, s_last_y, false);
        s_touching = false;
    }
}

/* Clear trajectory state. Called on (dis)connect so a gesture interrupted by a
 * dropped link cannot leave the next gesture starting from a stale position. */
void izuki_input_reset(void)
{
    s_touching = false;
    s_last_x = 0;
    s_last_y = 0;
}

static void do_tap(uint16_t x, uint16_t y, uint16_t duration_ms)
{
    gesture_release();

    uint16_t rx = map_coord(x, s_screen_w);
    uint16_t ry = map_coord(y, s_screen_h);

    send_touch(rx, ry, true);
    if (duration_ms < 20) {
        duration_ms = 20;
    }
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    send_touch(rx, ry, false);
}

static void do_swipe(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                     uint16_t duration_ms, uint8_t steps)
{
    gesture_release();

    if (steps < 1) {
        steps = 1;
    }
    int32_t step_delay = duration_ms / steps;
    if (step_delay < 1) {
        step_delay = 1;
    }

    uint16_t rx = map_coord(x1, s_screen_w);
    uint16_t ry = map_coord(y1, s_screen_h);
    send_touch(rx, ry, true);

    for (int i = 1; i <= steps; i++) {
        int32_t xi = (int32_t)x1 + ((int32_t)((int32_t)x2 - (int32_t)x1) * i) / steps;
        int32_t yi = (int32_t)y1 + ((int32_t)((int32_t)y2 - (int32_t)y1) * i) / steps;
        if (xi < 0) xi = 0;
        if (yi < 0) yi = 0;
        rx = map_coord((uint16_t)xi, s_screen_w);
        ry = map_coord((uint16_t)yi, s_screen_h);
        send_touch(rx, ry, true);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }
    send_touch(rx, ry, false);
}

/* Move the pressed tip from the current position to (target_x, target_y) over
 * duration_ms, emitting interpolated touch reports. Same coords = pure hold. */
static void gesture_move_to(uint16_t target_x, uint16_t target_y, uint16_t duration_ms)
{
    uint16_t rx = map_coord(target_x, s_screen_w);
    uint16_t ry = map_coord(target_y, s_screen_h);

    if (rx == s_last_x && ry == s_last_y) {
        if (duration_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(duration_ms));
        }
        return;
    }

    int32_t steps = (int32_t)((duration_ms + IZUKI_GESTURE_STEP_MS - 1) /
                              IZUKI_GESTURE_STEP_MS);
    if (steps < 1) {
        steps = 1;
    }
    int32_t step_delay = (int32_t)duration_ms / steps;
    if (step_delay < 1) {
        step_delay = 1;
    }

    int32_t x0 = s_last_x;
    int32_t y0 = s_last_y;
    for (int32_t i = 1; i <= steps; i++) {
        uint16_t xi = (uint16_t)(x0 + (((int32_t)rx - x0) * i) / steps);
        uint16_t yi = (uint16_t)(y0 + (((int32_t)ry - y0) * i) / steps);
        send_touch(xi, yi, true);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }
    s_last_x = rx;
    s_last_y = ry;
}

/* CMD_GESTURE body: pts = count * { x:u16, y:u16, dt:u16 }.
 * The first point of a stroke presses; each later point moves to it over its
 * dt (dt with unchanged coords = hold). Lifts at the end unless KEEP_DOWN. */
static void do_gesture(uint8_t flags, uint8_t count, const uint8_t *pts)
{
    /* A START frame is the first frame of a fresh trajectory. Drop any tip left
     * down by an interrupted gesture first, otherwise the first point would be
     * treated as a move from the stale (often 0,0) position. */
    if (flags & IZUKI_GESTURE_START) {
        gesture_release();
    }

    ESP_LOGI(TAG, "gesture flags=0x%02x count=%u touching=%d last=(%u,%u)",
             flags, count, s_touching, s_last_x, s_last_y);

    for (uint8_t i = 0; i < count; i++) {
        uint16_t off = (uint16_t)i * 6;
        uint16_t px = izuki_rd_u16(pts + off);
        uint16_t py = izuki_rd_u16(pts + off + 2);
        uint16_t dt = izuki_rd_u16(pts + off + 4);

        if (!s_touching) {
            s_last_x = map_coord(px, s_screen_w);
            s_last_y = map_coord(py, s_screen_h);
            ESP_LOGI(TAG, "  pt[%u] press raw=(%u,%u) dt=%u -> mapped=(%u,%u)",
                     i, px, py, dt, s_last_x, s_last_y);
            send_touch(s_last_x, s_last_y, true);
            s_touching = true;
            if (dt > 0) {
                vTaskDelay(pdMS_TO_TICKS(dt));
            }
        } else {
            ESP_LOGI(TAG, "  pt[%u] move raw=(%u,%u) dt=%u", i, px, py, dt);
            gesture_move_to(px, py, dt);
        }
    }

    if (!(flags & IZUKI_GESTURE_KEEP_DOWN) && s_touching) {
        send_touch(s_last_x, s_last_y, false);
        s_touching = false;
    }
}

static void do_key(uint8_t usage, uint8_t modifier, uint8_t down)
{
    if (usage == 0) {
        return;
    }
    if (down) {
        s_modifier |= modifier;
        bool present = false;
        for (int i = 0; i < 6; i++) {
            if (s_keys[i] == usage) {
                present = true;
                break;
            }
        }
        if (!present) {
            for (int i = 0; i < 6; i++) {
                if (s_keys[i] == 0) {
                    s_keys[i] = usage;
                    break;
                }
            }
        }
    } else {
        s_modifier &= (uint8_t)~modifier;
        for (int i = 0; i < 6; i++) {
            if (s_keys[i] == usage) {
                s_keys[i] = 0;
            }
        }
    }
    send_keyboard_report(s_modifier, s_keys);
}

static bool char_to_hid(uint8_t c, uint8_t *usage, uint8_t *mod)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') { *usage = (uint8_t)(0x04 + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'Z') { *usage = (uint8_t)(0x04 + (c - 'A')); *mod = 0x02; return true; }
    if (c >= '1' && c <= '9') { *usage = (uint8_t)(0x1E + (c - '1')); return true; }
    if (c == '0') { *usage = 0x27; return true; }

    switch (c) {
    case '\n': case '\r': *usage = 0x28; return true;
    case 0x1B:            *usage = 0x29; return true;
    case 0x08: case 0x7F: *usage = 0x2A; return true;
    case '\t':            *usage = 0x2B; return true;
    case ' ':             *usage = 0x2C; return true;
    case '-': *usage = 0x2D; return true;
    case '_': *usage = 0x2D; *mod = 0x02; return true;
    case '=': *usage = 0x2E; return true;
    case '+': *usage = 0x2E; *mod = 0x02; return true;
    case '[': *usage = 0x2F; return true;
    case '{': *usage = 0x2F; *mod = 0x02; return true;
    case ']': *usage = 0x30; return true;
    case '}': *usage = 0x30; *mod = 0x02; return true;
    case '\\': *usage = 0x31; return true;
    case '|': *usage = 0x31; *mod = 0x02; return true;
    case ';': *usage = 0x33; return true;
    case ':': *usage = 0x33; *mod = 0x02; return true;
    case '\'': *usage = 0x34; return true;
    case '"': *usage = 0x34; *mod = 0x02; return true;
    case '`': *usage = 0x35; return true;
    case '~': *usage = 0x35; *mod = 0x02; return true;
    case ',': *usage = 0x36; return true;
    case '<': *usage = 0x36; *mod = 0x02; return true;
    case '.': *usage = 0x37; return true;
    case '>': *usage = 0x37; *mod = 0x02; return true;
    case '/': *usage = 0x38; return true;
    case '?': *usage = 0x38; *mod = 0x02; return true;
    case '!': *usage = 0x1E; *mod = 0x02; return true;
    case '@': *usage = 0x1F; *mod = 0x02; return true;
    case '#': *usage = 0x20; *mod = 0x02; return true;
    case '$': *usage = 0x21; *mod = 0x02; return true;
    case '%': *usage = 0x22; *mod = 0x02; return true;
    case '^': *usage = 0x23; *mod = 0x02; return true;
    case '&': *usage = 0x24; *mod = 0x02; return true;
    case '*': *usage = 0x25; *mod = 0x02; return true;
    case '(': *usage = 0x26; *mod = 0x02; return true;
    case ')': *usage = 0x27; *mod = 0x02; return true;
    default: return false;
    }
}

static void do_text(const uint8_t *utf8, uint16_t len)
{
    uint8_t empty[6] = {0};
    for (uint16_t i = 0; i < len; i++) {
        uint8_t usage = 0;
        uint8_t mod = 0;
        if (!char_to_hid(utf8[i], &usage, &mod)) {
            continue; /* non-ASCII: skipped (HID keyboard cannot type it) */
        }
        uint8_t keys[6] = {0};
        keys[0] = usage;
        send_keyboard_report(mod, keys);
        vTaskDelay(pdMS_TO_TICKS(8));
        send_keyboard_report(0, empty);
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

static void handle_frame(const uint8_t *f, uint16_t len)
{
    if (len < 1) {
        return;
    }
    switch (f[0]) {
    case CMD_HANDSHAKE:
        izuki_input_send_handshake_ack();
        break;

    case CMD_SET_RESOLUTION:
        if (len >= 5) {
            s_screen_w = izuki_rd_u16(f + 1);
            s_screen_h = izuki_rd_u16(f + 3);
            ESP_LOGI(TAG, "screen resolution set to %ux%u", s_screen_w, s_screen_h);
        }
        break;

    case CMD_TAP:
        if (len >= 7) {
            do_tap(izuki_rd_u16(f + 1), izuki_rd_u16(f + 3), izuki_rd_u16(f + 5));
        }
        break;

    case CMD_SWIPE:
        if (len >= 12) {
            do_swipe(izuki_rd_u16(f + 1), izuki_rd_u16(f + 3),
                     izuki_rd_u16(f + 5), izuki_rd_u16(f + 7),
                     izuki_rd_u16(f + 9), f[11]);
        }
        break;

    case CMD_GESTURE:
        if (len >= 3) {
            uint8_t flags = f[1];
            uint16_t available = (uint16_t)((len - 3) / 6);
            uint8_t count = f[2];
            if (count > available) {
                count = (uint8_t)available;
            }
            if (count > IZUKI_GESTURE_MAX_POINTS) {
                count = IZUKI_GESTURE_MAX_POINTS;
            }
            do_gesture(flags, count, f + 3);
        }
        break;

    case CMD_KEY:
        if (len >= 4) {
            do_key(f[1], f[2], f[3]);
        }
        break;

    case CMD_TEXT:
        if (len >= 3) {
            uint16_t text_len = izuki_rd_u16(f + 1);
            if (text_len > (uint16_t)(len - 3)) {
                text_len = (uint16_t)(len - 3);
            }
            do_text(f + 3, text_len);
        }
        break;

    case CMD_PING: {
        uint8_t pong = EVT_PONG;
        izuki_ble_notify(&pong, 1);
        break;
    }

    default:
        ESP_LOGW(TAG, "unknown cmd 0x%02x", f[0]);
        break;
    }
}

static void izuki_worker(void *arg)
{
    (void)arg;
    izuki_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            handle_frame(cmd.data, cmd.len);
        }
    }
}

void izuki_input_init(esp_hidd_dev_t *dev)
{
    s_dev = dev;
    s_queue = xQueueCreate(IZUKI_QUEUE_LEN, sizeof(izuki_cmd_t));
    configASSERT(s_queue);
    xTaskCreate(izuki_worker, "izuki_input", IZUKI_WORKER_STACK, NULL,
                IZUKI_WORKER_PRIO, NULL);
}

void izuki_input_feed(const uint8_t *frame, uint16_t len)
{
    if (len == 0) {
        return;
    }
    if (len > IZUKI_MAX_FRAME) {
        len = IZUKI_MAX_FRAME;
    }
    izuki_cmd_t cmd;
    cmd.len = len;
    memcpy(cmd.data, frame, len);
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full; dropping 0x%02x", frame[0]);
    }
}

void izuki_input_send_handshake_ack(void)
{
    uint8_t evt[6];
    evt[0] = EVT_HANDSHAKE_ACK;
    evt[1] = IZUKI_PROTOCOL_VERSION;
    izuki_wr_u16(&evt[2], IZUKI_DIGITIZER_MAX);
    izuki_wr_u16(&evt[4], IZUKI_DIGITIZER_MAX);
    izuki_ble_notify(evt, sizeof(evt));
    izuki_input_send_status();
}
