# SU3_ESP32 — 盛和养老智能床垫 ESP32 固件（SU3）

基于原 `ESP_BC` 演进的板端工程，面向 **SU3 / 双人床睡眠传感器** 通信重构：用独立组件 `components/su3` 替换旧的 `set_bc` + `pause_uart_task` 抢总线方式，保留 WiFi / BLE / MFP / MQTT / OTA 等业务能力。

| 项 | 内容 |
|----|------|
| 工程目录 | `SU3_ESP32` |
| CMake 工程名 | `bochuang-program` |
| 芯片 | ESP32 |
| 框架 | ESP-IDF v4.4.x（当前联调：v4.4.8） |
| 版本宏 | `SL_ESP_2026_1_0_0`（涂鸦新产品 KSSLBED） |
| 基线标签 | `su3-bringup-20260716` |

架构说明见 [`docs/SU2_通信架构.md`](docs/SU2_通信架构.md)（文档名沿用 SU2 协议族，实现目录为 `su3`）。

---

## 当前进度（联调基线）

### 已完成

- `components/su3`：帧层 / CRC / CliCommand / 单 RX 任务 / `cli_exec`·`list`·`fire`
- 业务接入：`hello` → `set addr` → `set mode 4`；topic 回调与双路缓存骨架
- 硬件：`GPIO12` 传感器电源使能（高电平上电），上电后延时 500ms 再启 BLE/WiFi
- 实机验证：UART 收包稳定（`crc_fail=0`），hello / 设址应答 `ok`

### 未完成 / 待验收

- 双路（左+右）实时数据上云稳定性验收
- `list` / `report` 睡眠报告全流程验收
- 云端 CLI 指定侧（`side` L/R）
- 传感器固件 OTA（`su3_sensor_ota` 未实现）
- 供电 brownout 长稳（建议加强电源；软件仅错峰延时）

**说明：** 串口通信栈可作阶段性验收；整机产品替换尚未结项。

---

## 软件架构

```
业务层  app_control / use_wifi / BLE
           │  只依赖 su3_client.h
           ▼
事务层  su3_client     单 RX、pending、cli_exec / list / fire
           ▼
协议层  su3_proto      CliCommand (topic=4) 编解码
           ▼
帧层    su3_frame + su3_crc
           │  AA55、addr|port、CRC、多帧重组
           ▼
UART1   TX=GPIO22  RX=GPIO26  115200
```

旧路径 `uart_data_parser_task` 已停用。外部仍可调用 `set_bc()`，内部桥接到 `su3_cli_*`（默认发左侧，部分命令双侧）。

开关：`common.h` 中 `SU3_USE_NEW_STACK`（当前为 `1`）。

---

## 硬件要点

| 信号 | GPIO | 说明 |
|------|------|------|
| 传感器 UART TX | 22 | ESP → 传感器 |
| 传感器 UART RX | 26 | 传感器 → ESP |
| 传感器电源使能 | 12 | 高电平上电；复位默认关 |
| MFP UART | 19 / 25 | 床控盒（UART2） |
| MFP 方向 | 33 | `UART_CTR` |

帧内地址为 **`(port<<4)|addr`**，业务端口 `port=3`：

| 角色 | 逻辑 addr | 帧字节 |
|------|-----------|--------|
| 未设址 / hello | 0 | `0x30` |
| 左传感器 | 3 | `0x33` |
| 右传感器 | 6 | `0x36` |
| ESP32 本机 | 7 | `0x37` |

CLI 文本仍用逻辑地址，例如 `set addr 3`（不是 `set addr 51`）。

原理图：[`docs/SCH_睡眠传感器双人床控制板_2026-07-16.pdf`](docs/SCH_睡眠传感器双人床控制板_2026-07-16.pdf)

---

## 目录结构

```
SU3_ESP32/
├── main/                    # app_main：NVS → device_init → BLE/WiFi → app_control
├── components/
│   ├── SU3/                 # 传感器通信栈（对外 su3_client.h）
│   ├── app_control/         # 业务：hello/上报/床控/打鼾等
│   ├── common/              # 配置、NVS、GPIO12 供电
│   ├── protobuf/            # keesoncloud / qs_protobuf
│   ├── use_wifi/            # WiFi、MQTT、OTA
│   ├── use_ble_server/      # BLE GATT
│   ├── use_uart/            # MFP 等 UART 辅助
│   ├── use_mfp/             # 床控协议
│   ├── use_adc/ · use_pwm/
├── docs/                    # 架构、协议、原理图
├── my_partitions.csv
└── sdkconfig / sdkconfig.defaults
```

---

## 开发环境与编译

| 项目 | 要求 |
|------|------|
| 芯片 | ESP32 |
| IDF | 建议 4.4.x（与现有 `sdkconfig` 一致） |
| 工具 | VS Code / Cursor + Espressif IDF 插件，或 `idf.py` |

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

分区表：根目录 [`my_partitions.csv`](my_partitions.csv)。

### Git 基线

```bash
git checkout su3-bringup-20260716   # 联调基线标签
# 或
git log -1                          # 查看当前提交
```

---

## 主要功能一览

| 模块 | 说明 |
|------|------|
| **SU3 传感器** | UART1；1s / 1min / SA / 睡眠报告等 protobuf；双路地址 03/06 |
| **WiFi + MQTT** | STA；涂鸦 IoT（阿里云版）动态签名 |
| **BLE** | 配网、透传、床控 |
| **MFP** | UART2 床控盒、打鼾干预 |
| **OTA** | ESP32 应用 HTTPS 双分区；传感器 OTA 未实现 |
| **上报** | JSON 经 MQTT / BLE（带 `side` / `sensor` 字段） |

---

## 联调日志参考

正常上电片段：

```text
I (...) common: SU3 power EN GPIO12 = 1
I (...) common: SU3 power settle 500ms done
I (...) su3_client: su3_init ok ... self=0x37 L=0x33 R=0x36 pins tx=22 rx=26
I (...) su3_client: cli ... cmd="hello" ...
I (...) control: hello side=0 src=0x33
I (...) control: setup side=0 dest=0x33: set addr 3
I (...) control: setup addr rsp=ok
I (...) su3_client: rx stats: bytes=... frame_ok=... crc_fail=0 overflow=0
```

| 现象 | 含义 |
|------|------|
| `bytes` 持续增加 | 累计收字节（传感器在持续发） |
| `crc_fail=0` | 帧校验正常 |
| `Brownout detector` | 供电瞬时掉压（WiFi 发射 + 传感器上电） |

---

## 对外 API（业务侧）

只包含 [`components/SU3/include/su3_client.h`](components/SU3/include/su3_client.h)：

- `su3_init` / `su3_set_handlers`
- `su3_cli_exec` / `su3_cli_list` / `su3_cli_fire`
- `su3_side_to_addr` / `su3_addr_to_side`

不要在业务里直接拼 AA55 帧。

---

## 相关文档

- [`docs/SU2_通信架构.md`](docs/SU2_通信架构.md) — 分层设计
- [`docs/SU2_V2.0_协议答疑.md`](docs/SU2_V2.0_协议答疑.md) — 双路 / hello / Sign
- [`docs/SU3传感器通信协议20260714.docx`](docs/SU3传感器通信协议20260714.docx) — 协议原文

---

## 修订记录

| 日期 | 说明 |
|------|------|
| 2026-07-16 | 初版 README：SU3 通信栈联调基线、硬件与进度说明 |
