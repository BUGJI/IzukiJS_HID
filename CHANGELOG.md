# 更新日志

本项目的所有重要变更都会记录在此文件。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 计划中

- 多指触摸：扩展 HID 报告并在协议中加入 pointer id。
- 硬件复位 / 出厂重置指令。
- CI 构建校验（`.github/workflows/build.yml`）。

## [0.1.0] - 2026-10-08

首个版本。

### 新增

- ESP32-C3 BLE-HID 固件：暴露**单触点多点触控数字板**（绝对坐标 `0..32767`）与
  **6KRO 键盘**，配对后以 `Izuki-HID-XXXXXX`（BT MAC 后 3 字节）广播。
- 自定义 GATT 控制通道：Control / Event 两个特征。
- 控制协议 v2：握手、分辨率设置、点击、滑动、按键、文本、Ping、以及带轨迹的复杂手势。
- 固件侧手势插值与停顿处理（`CMD_GESTURE`），每段 `dt` 内坐标不变即原地按住。

[未发布]: https://github.com/BUGJI/IzukiJS_HID/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/BUGJI/IzukiJS_HID/releases/tag/v0.1.0
