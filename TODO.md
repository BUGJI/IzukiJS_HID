# Izuki BLE-HID 固件 TODO

背景：App 总显示"已连接"，但实际经常操控不了。
"已连接"只代表自定义 GATT 通道（Control/Event）通了，不代表手机作为 HID Host
已加密并订阅了 HID Report 特征。

## 已完成（v1）

- [x] `esp_hidd_dev_input_set()` 返回值检查 + 失败日志（`izuki_input.c`）。
- [x] `esp_hidd_dev_connected()` 判定 + `EVT_STATUS` / `EVT_ERROR` 上报。
- [x] 连接后主动 `ble_gap_security_initiate`（HOGP 加密前提）。
- [x] `BLE_GAP_EVENT_ENC_CHANGE` / `REPEAT_PAIRING` 处理、陈旧 bond 清理。
- [x] `CONFIG_BT_NIMBLE_SM_LVL=2`、`HANDLE_REPEAT_PAIRING_DELETION=y`。
- [x] 断开时清零加密/订阅状态。
- [x] App 侧解析 `EVT_STATUS` flags、`EVT_ERROR` 错误码；
      `HidGattClient.isReady` 改为要求 `HID_READY`；UI 区分"未就绪"。

## 触摸板 / 指针停在 (0,0) 修复（报告描述符）

背景：Android 把设备当成"触摸板"，指针停在 (0,0)。根因是内核输入设备缺少
`INPUT_PROP_DIRECT`。Linux 只有两条途径设置它：

- 通用 `hid-input` 只对单点 `Pen`(0x02) 应用设置，且仅 **内核 ≥ 5.0** 才有；
  4.x 内核这类设备会退化成触摸板。之前的 Pen 方案因此不可靠。
- `hid-multitouch` 对 `Touch Screen`(0x04) 应用在出现 **Contact Identifier(0x51)**
  时设置 `INPUT_MT_DIRECT`，从而 `hid-core` 归为 `HID_GROUP_MULTITOUCH` 并绑定
  驱动；自 4.x 起都可用。

已改：报告 ID 1 改为单点多点触控触摸屏描述符（Touch Screen + Contact Identifier
+ Contact Count + Contact Count Maximum），报告 7 字节：
`tip(1) | X(2) | Y(2) | contact_id(1) | count(1)`；`send_touch()` 同步改为 7 字节。

**关键：X/Y 字节偏移与旧的 5 字节描述符保持一致**（X 在第 1 字节、Y 在第 3
字节），Contact Identifier/Contact Count 追加在 Y 之后。原因：Android **按已配对
设备缓存 Report Map**，改了描述符但没重新配对时，手机仍用旧的 5 字节布局解析新
7 字节报文；若 X 放在第 2 字节，旧解析会把第 1 字节（contact id=0）当成 X 低位，
表现为「X 坐标恒为 0」。保持偏移一致后，无论手机用新还是旧描述符，X/Y 都能正确
解析。

- [ ] 真机验证：`dumpsys input` 中该设备 `Sources` 应为 `0x00005001`
      （TOUCHSCREEN），而非 `0x00001002`（TOUCHPAD）；点按/滑动应落在触点处。
- [ ] **换描述符后必须"忽略/忘记设备"再重配对**：Android 会按 bond 缓存 HID
      报告描述符，不重新配对可能仍用旧描述符。
- [ ] 若仍被识别为触摸板，抓 `getevent -lp` 确认是否有 `ABS_MT_POSITION_X/Y`
      与 `INPUT_PROP_DIRECT`。

## 剩余 / 待办

- [ ] **陈旧 bond 自动清除基本不会触发**：`s_enc_fail_count` 在
      `BLE_GAP_EVENT_DISCONNECT` 被清零（`izuki_ble.c`），而加密失败通常立刻断链，
      计数归零，永远到不了 `IZUKI_MAX_ENC_FAILURES`。建议只在加密成功时清零，
      或首次失败即清 bond + 断开。
- [ ] **`HID_READY` 不等于 Report CCCD 已订阅**：`esp_hidd_dev_connected()`
      在 `BLE_GAP_EVENT_CONNECT` 即置 true（`nimble_hidd.c`）。受 esp_hid API
      限制可接受，但真正是否送达以 `input_set` 返回值为准。
- [ ] 协议版本：固件仍报 `IZUKI_PROTOCOL_VERSION=1`，但 `EVT_STATUS` 语义已改。
      若需严格版本兼容，考虑升到 2（需同步 App 与文档）。
- [ ] 真机验证：
      - [ ] 点不动时串口出现明确的未就绪/未加密日志，而非静默。
      - [ ] 删除手机配对后重连，不出现"假连接"。
      - [ ] `HidSetupScreen` 在未加密时显示"HID 通道未就绪…"，加密完成后变"已就绪"。

## v2：复杂手势轨迹（滑动 + 停顿 / 曲线）

背景：App 的 `gesture(strokes)` 支持带时间戳的复杂轨迹，但 v1 协议只有
`CMD_SWIPE`（两点直线 + steps 插值），无法表达"滑到终点后按住"或曲线路径，
只能降级为逐段独立滑动（段间抬手），效果错误。

方案：新增 `CMD_GESTURE`（`0x08`）批量轨迹帧，由固件负责插值与停顿：

```
CMD_GESTURE: flags:u8, count:u8, count * { x:u16, y:u16, dt:u16 }
```

- `flags` bit0 = `IZUKI_GESTURE_KEEP_DOWN`：本帧结束后不抬手，供 App 把长轨迹拆帧续传。
- 每点 `dt` = "从上一个点移动到本点"所花的毫秒；坐标不变的段即"原地按住"。
- 固件维护 `s_last_x/s_last_y/s_touching`，跨帧续传同一根手指。
- 仅单指（触摸报告只有一路）；多指手势不支持。
- 协议版本升到 **2**；App 解析 `HANDSHAKE_ACK` 的 `ver`，`< 2` 时不走 `CMD_GESTURE`。

### 已完成

- [x] `izuki_proto.h`：新增 `CMD_GESTURE`、`IZUKI_GESTURE_KEEP_DOWN`、
      `IZUKI_GESTURE_MAX_POINTS`，`IZUKI_PROTOCOL_VERSION` 升到 2。
- [x] `izuki_input.c`：`do_gesture()` + `gesture_move_to()` 插值/停顿实现，
      跨帧触摸状态，`TAP/SWIPE` 前调用 `gesture_release()` 兜底抬手。
- [x] App `HidProtocol.kt`：`CMD_GESTURE` 编码、`gesture()` 组帧、版本升 2。
- [x] App `HidGattClient.kt`：从 `HANDSHAKE_ACK` 记录固件版本并按连接重置。
- [x] App `HidController.kt`：`supportsGesture()` / `gesture()`（按 42 点分帧）。
- [x] 文档：`docs/BLE_HID_PROTOCOL.md` 更新 v2 与 `CMD_GESTURE`。

### 待办 / 验证

- [ ] **真机验证滑动并停顿**：`basic_ops.cleanJunk()` 能稳定打开最近任务
      （上滑 ~260ms 后在终点按住 ~700ms）。
- [ ] 曲线路径：多个 waypoint + 变化的 `dt`，确认插值平滑、无跳点。
- [ ] 长轨迹分帧：超过 42 点的轨迹用多帧 `KEEP_DOWN` 续传，中间不抬手。
- [ ] 分帧中断兜底：App 中途断开后，下一次 `TAP/SWIPE` 能通过
      `gesture_release()` 恢复（当前每次 `TAP/SWIPE` 会先抬手）。
- [ ] 时序精度：`vTaskDelay` 的 tick 粒度（默认 100Hz → 10ms）会带来抖动，
      必要时把 `CONFIG_FREERTOS_HZ` 提高到 1000，或改用更细的等待。
- [ ] 多指：如需多指，需扩触摸报告为多个 Contact 并在帧里加 pointer id。
- [ ] 协议版本兼容回归：旧固件（v1）下 App 仍应正常 `swipe/tap`，
      `gesture()` 回退到分段。（App 侧 `supportsGesture()` 已按版本判断。）
