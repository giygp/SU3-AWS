# SU3_ESP32 — 盛和养老智能床垫 ESP32 固件（AWS IoT 版）

基于原 `ESP_BC` 演进的板端工程，面向 **SU3 / 双人床睡眠传感器**：用独立组件 `components/su3` 替换旧的 `set_bc` + `pause_uart_task` 抢总线方式，云端链路为 **AWS IoT Core（mTLS，8883 端口）**。

| 项 | 内容 |
|----|------|
| 工程目录 | `SU3_ESP32` |
| CMake 工程名 | `bochuang-program` |
| 芯片 | ESP32（16MB Flash） |
| 框架 | ESP-IDF v4.4.x（当前联调：v4.4.8） |
| 云平台 | AWS IoT Core（中国区 `cn-north-1`，ATS endpoint） |
| 固件版本宏 | `SL_ESP_2026_1_1_3` |

架构说明见 [`docs/SU2_通信架构.md`](docs/SU2_通信架构.md)（文档名沿用 SU2 协议族，实现目录为 `su3`）。

---

## AWS IoT 连接

### 鉴权：x.509 双向 TLS

| 材料 | 存放位置 | 说明 |
|------|----------|------|
| Amazon Root CA 1（服务器 CA） | `nvs_certs` 分区 key `root_ca` | OTA HTTPS 下载同用此根证书（编译内置一份于 `use_ota.h`） |
| 设备证书 | `nvs_certs` 分区 key `dev_cert` | 每台设备一张，产测烧录 |
| 设备私钥 | `nvs_certs` 分区 key `dev_key` | 每台设备一把，产测烧录 |

- MQTT URI：`mqtts://<endpoint>:8883`，`client_id` = 设备ID。
- 证书在连接建立时由 `aws_certs_load()` 从 `nvs_certs` NVS 分区读入（namespace `certs`）。
- **SNTP 时间同步后才启动 MQTT**：x.509 校验依赖系统时间，时间不对 TLS 握手会失败。

### Endpoint 地址配置

默认值编译于 `common.h` 的 `AWS_IOT_ENDPOINT_DEFAULT`，运行时存 NVS（`config_cfg` 命名空间 key `aws_url`），与 WiFi 账号同生命周期：

- App 配网时可下发 `aws_url` 字段（`app_control.c` BLE 命令解析），与 ssid/passwd 一起持久化；
- 未配置时回落到默认 endpoint。

### 设备 ID

`storage` 分区 **offset 0** 起 20 字节 NUL 结尾字符串（`ALIYUN_BURN=1`）。
> 注意：旧涂鸦布局为 20(PK)+20(name)+50(secret)，旧数据残留会把 PK 当设备ID读出——产测必须按新布局重烧 `storage`。分区不存在或未烧录时回落到 `DEVEICE_ID` 宏。

### MQTT Topic（`{id}` = 设备ID）

| Topic | 方向 | 用途 |
|-------|------|------|
| `{id}/user/5s/put` | 发布 | 5s 实时数据 |
| `{id}/user/60s/put` | 发布 | 60s 数据 |
| `{id}/user/sa/put` | 发布 | SA 数据 |
| `{id}/user/sleep/put` | 发布 | 睡眠报告 |
| `{id}/user/cli/get` | 订阅 | 云端 CLI 下发（主芯片 / 床控共用） |
| `{id}/user/cli/put` | 发布 | CLI 应答 |
| `{id}/user/mccli/put` | 发布 | 床控 CLI 应答 |
| `ota/device/upgrade/{id}` | 订阅 | OTA 升级指令 |
| `ota/device/inform/{id}` | 发布 | OTA 版本上报 |
| `ota/device/progress/{id}` | 发布 | OTA 进度 |

> AWS IoT 策略需为设备证书放行以上 topic 的 connect/subscribe/publish。

---

## 证书烧录（产测流程）

证书文件放在 `components/use_wifi/certs/`：

```
AmazonRootCA1.pem                  # 公开根证书，随仓库分发
<DeviceId>.cert.pem                # 每台设备的设备证书（不入库，.gitignore 已排除）
<DeviceId>.private.key             # 每台设备的设备私钥（不入库，.gitignore 已排除）
```

一键生成 + 烧录 `nvs_certs` 分区（依赖本机 ESP-IDF，路径自动探测）：

```powershell
.\make_certs_bin.ps1 -DeviceId KSSLBED00000001              # 仅生成 certs_bin\nvs_certs_<id>.bin
.\make_certs_bin.ps1 -DeviceId KSSLBED00000001 -Flash       # 生成并烧录（默认 COM3）
.\make_certs_bin.ps1 -DeviceId KSSLBED00000001 -Flash -Port COM5
```

脚本内部用 IDF 的 `nvs_partition_gen.py` 打包（NVS v2）、`parttool.py` 按分区名烧录，分区偏移自动从设备分区表读取。

---

## OTA

- **主片 OTA**：`ota/device/upgrade/{id}` 指令 → HTTPS 下载（校验 Amazon Root CA 1）→ 写 `ota_0/ota_1` 双分区 → 重启。
- **传感器固件 OTA**：云端下发 → HTTPS 下载落盘 → UART port4 刷写（详见 `use_ota.c`）。
- **常驻启动器**（`ota_launcher_init`，开机创建）：MQTT 指令到达时零内存触发信号量；启动器先 **stop MQTT**（释放 esp-mqtt 任务栈约 20KB + TLS 内存），确保内部 RAM 足够创建 OTA 任务；任务创建失败会清标志并恢复 MQTT，成功路径设备重启。
- 下载过程每 10% 打一条进度日志。

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

## 分区表（16MB Flash）

| 分区 | 类型 | 大小 | 用途 |
|------|------|------|------|
| nvs | data/nvs | 0x4000 | 业务配置（`config_cfg`：wifiSsid / wifiPasswd / **aws_url** / snore_param） |
| otadata | data/ota | 0x2000 | OTA 启动选择 |
| phy_init | data/phy | 0x1000 | RF 校准 |
| factory | app | 2M | 出厂固件 |
| ota_0 / ota_1 | app | 各 2M | OTA 双分区 |
| storage | data/fat | 8M | **设备ID（offset 0）**、传感器固件落盘 |
| nvs_certs | data/nvs | 0x8000 | **AWS 三证书（root_ca / dev_cert / dev_key）** |

---

## 目录结构

```
SU3_ESP32/
├── main/                    # app_main：NVS → device_init → BLE/WiFi → app_control → OTA 启动器
├── components/
│   ├── SU3/                 # 传感器通信栈（对外 su3_client.h）
│   ├── app_control/         # 业务：配网(BLE+aws_url)、上报、床控、打鼾
│   ├── common/              # 配置、NVS、storage 分区设备ID读取
│   ├── protobuf/            # keesoncloud / qs_protobuf
│   ├── use_wifi/            # WiFi、AWS MQTT、OTA；certs/ 为证书源文件目录
│   ├── use_ble_server/      # BLE GATT 配网/透传
│   ├── use_uart/            # MFP 等 UART 辅助
│   ├── use_mfp/             # 床控协议
│   └── use_adc/ · use_pwm/
├── make_certs_bin.ps1       # AWS 证书打包/烧录脚本（产测用）
├── docs/                    # 架构、协议、原理图
├── my_partitions.csv
└── sdkconfig / sdkconfig.defaults
```

---

## 开发环境与编译

| 项目 | 要求 |
|------|------|
| 芯片 | ESP32（16MB Flash） |
| IDF | v4.4.x（与 `sdkconfig` 一致） |
| 工具 | VS Code + Espressif IDF 插件，或 `idf.py` |

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

---

## 主要功能一览

| 模块 | 说明 |
|------|------|
| **SU3 传感器** | UART1；1s / 1min / SA / 睡眠报告 protobuf；双路地址 03/06 |
| **WiFi + MQTT** | STA；AWS IoT Core mTLS 8883，证书来自 `nvs_certs` 分区 |
| **BLE** | 配网（ssid/passwd/**aws_url**）、透传、床控 |
| **MFP** | UART2 床控盒、打鼾干预 |
| **OTA** | 主片 HTTPS 双分区 + 传感器串口 OTA；常驻启动器，OTA 前停 MQTT 释放内存 |
| **上报** | JSON 经 MQTT / BLE（带 `side` / `sensor` 字段） |

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
| 2026-11-06 | AWS IoT 迁移：mTLS 证书分区化（nvs_certs）、endpoint 可配、设备ID offset 0、OTA 常驻启动器、证书烧录脚本 |
| 2026-07-16 | 初版 README：SU3 通信栈联调基线、硬件与进度说明 |
