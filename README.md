# IzukiJS_HID

使用 **ESP32-C3** 为 [Izuki JS](https://github.com/BUGJI/IzukiJS) Android App 开发的
BLE-HID 固件（虚拟触摸屏 + 键盘）。手机连接后，App 通过自定义 GATT 通道下发
指令，固件在手机侧注入触摸 / 键盘事件，可用于自动化操作、远程控制等场景。

## 特性

- **BLE HID 设备**：同时暴露单触点多点触控**触摸屏**（绝对坐标 `0..32767`，单 contact）
  和 **6KRO 键盘**。
- **自定义 GATT 控制通道**：App → 固件下发指令，固件 → App 上报状态与错误。
- **复杂手势轨迹**：支持带时间戳的路径插值、曲线、滑到终点后按住（长按）。
- **文本输入**：将 ASCII 字符串转换为 HID 按键序列。
- **配对与安全**：Just Works 配对 + 绑定，链路加密（HOGP 要求），陈旧 bond 自动清理。
- **每台设备唯一名称**：`Izuki-HID-XXXXXX`（取 BT MAC 后 3 字节）。

## 硬件要求

| 项目 | 说明 |
| --- | --- |
| 芯片 | ESP32-C3（内置 BLE 5.0） |
| Flash | 4 MB |
| SDK | ESP-IDF v5.x（NimBLE + esp_hid） |

## 快速开始

### 编译

```bash
# 使用 ESP-IDF 环境
idf.py set-target esp32c3
idf.py build
```

Windows 下可使用仓库内的 `build.bat`（转发给 `idf.py` 前会清掉 Git Bash 设置的
`MSYSTEM`）。脚本先按 `IDF_PATH` 环境变量定位 `export.bat`，未设置时回退到
`%USERPROFILE%\esp\esp-idf`，可按本机实际安装位置调整脚本或设置 `IDF_PATH`：

```bat
build.bat build
```

### 烧录

```bash
idf.py -p COMx flash monitor
```

### 发布固件（Release）

Release 中的合并整包 `IzukiJS_HID_full_<version>.bin` 已包含 bootloader、分区表和
应用，可直接一条命令烧录到 `0x0`：

```bash
esptool.py --chip esp32c3 -p COMx -b 460800 \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 IzukiJS_HID_full_<version>.bin
```

也可分别烧录：

| 地址 | 文件 |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0x10000` | `Izuki_js_hid.bin` |

## BLE 接口

### 设备信息

| 字段 | 值 |
| --- | --- |
| 名称 | `Izuki-HID-XXXXXX`（后 3 字节 BT MAC） |
| VID / PID | `0x303A` / `0x0001` |
| 广播 | HID 服务 `0x1812` + 自定义服务 UUID |
| 外观 | HID Generic (`0x03C0`) |

### GATT 服务

| 特征 | UUID | 属性 | 方向 |
| --- | --- | --- | --- |
| Service | `7d8a0001-9a1e-4b2a-8f3c-1d2e3f4a5b6c` | — | — |
| Control | `7d8a0002-9a1e-4b2a-8f3c-1d2e3f4a5b6c` | Write / Write NR / Enc | App → 固件 |
| Event | `7d8a0003-9a1e-4b2a-8f3c-1d2e3f4a5b6c` | Notify / Enc | 固件 → App |

Control 与 Event 均要求**加密链路**，未完成加密 / 配对前无法读写。

### HID 报告

| Report ID | 类型 | 长度 | 布局 |
| --- | --- | --- | --- |
| `1` | 触摸屏 | 7 字节 | `tip(1) \| X(2) \| Y(2) \| contact_id(1) \| contact_count(1)` |
| `2` | 键盘 | 8 字节 | `modifier(1) \| reserved(1) \| keys(6)` |

X/Y 逻辑范围为 `0..32767`。报告 ID 1 使用 `Touch Screen` + `Contact Identifier`
描述符，使 Android 将其识别为**触摸屏**而非触摸板。

## 控制协议（v2）

所有多字节整数为**小端**。指令写在 Control 特征，事件从 Event 特征推送。

### App → 固件（指令）

| 指令 | 值 | 负载 |
| --- | --- | --- |
| `CMD_HANDSHAKE` | `0x01` | `ver:u8`（固件当前忽略该字节，仅用于兼容 App 侧握手帧） |
| `CMD_SET_RESOLUTION` | `0x02` | `w:u16, h:u16` |
| `CMD_TAP` | `0x03` | `x:u16, y:u16, duration:u16` |
| `CMD_SWIPE` | `0x04` | `x1:u16, y1:u16, x2:u16, y2:u16, duration:u16, steps:u8` |
| `CMD_KEY` | `0x05` | `usage:u8, modifier:u8, down:u8` |
| `CMD_TEXT` | `0x06` | `len:u16, utf8[len]` |
| `CMD_PING` | `0x07` | 无 |
| `CMD_GESTURE` | `0x08` | `flags:u8, count:u8, count * { x:u16, y:u16, dt:u16 }` |

`CMD_GESTURE` 由固件负责每段插值与停顿（`dt` 内坐标不变即原地按住）。
`flags`：

- `0x01` `IZUKI_GESTURE_KEEP_DOWN`：本帧结束后不抬手，供长轨迹分帧续传。
- `0x02` `IZUKI_GESTURE_START`：新轨迹首帧，强制重新按下（清理中断留下的手指）。

### 固件 → App（事件）

| 事件 | 值 | 负载 |
| --- | --- | --- |
| `EVT_HANDSHAKE_ACK` | `0x81` | `ver:u8, max_x:u16, max_y:u16` |
| `EVT_ACK` | `0x82` | — |
| `EVT_STATUS` | `0x83` | `flags:u8` |
| `EVT_ERROR` | `0x84` | `code:u8` |
| `EVT_PONG` | `0x87` | 无 |

`EVT_STATUS` flags：

| 位 | 名称 | 含义 |
| --- | --- | --- |
| `0x01` | `LINK_UP` | HID 链路已连接 |
| `0x02` | `ENCRYPTED` | 链路已加密 |
| `0x04` | `HID_READY` | 链路 + 加密 + 手机已订阅 HID Report |
| `0x08` | `APP_READY` | App 已订阅 Event 通知 |

`EVT_ERROR` code：`0x01` HID 未连接、`0x02` HID 未就绪、`0x03` 注入失败。

> 协议版本号为 **2**。App 解析 `EVT_HANDSHAKE_ACK` 中的 `ver`，低于 2 时不使用
> `CMD_GESTURE`，回退到分段 `CMD_SWIPE`。

## 目录结构

```
.
├── .github/workflows/      # CI：ESP-IDF 容器内构建并产出固件 artifact
├── CMakeLists.txt          # 顶层工程定义
├── build.bat               # Windows 编译辅助脚本
├── partitions.csv          # 分区表（4MB，单 factory 分区）
├── sdkconfig.defaults      # 默认 Kconfig（NimBLE / HID / 安全）
├── CHANGELOG.md            # 版本变更记录
├── LICENSE                 # MIT
└── main/
    ├── main.c              # 启动流程 + HID 报告描述符
    ├── izuki_ble.c/.h      # 自定义 GATT 服务、广播、配对与安全
    ├── izuki_input.c/.h    # 指令解析、触摸/键盘注入、手势插值
    └── izuki_proto.h       # 控制协议定义
```

## 已知问题

- Android 按**已配对设备缓存 HID Report Map**。更换描述符后需在系统设置中
  “忽略/忘记设备”再重新配对，否则可能仍按旧布局解析。
- `HID_READY` 基于 esp_hid API 的 connected 判定，真正是否送达以注入返回值 /
  `EVT_ERROR` 为准。
- 触摸仅支持**单指**；多指需扩展报告并在协议中加入 pointer id。

更多细节见 [TODO.md](TODO.md)。

## License

[MIT](LICENSE)
