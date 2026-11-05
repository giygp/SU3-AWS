# SU3 MQTT 下发 / 上报协议

> 本文档在《盛和养老系统下发协议》（原 `SU3mqtt下发协议.docx`）基础上修订，适配 **SU3 双人床双传感器**。  
> UART 传感器协议参照：`SU3传感器通信协议20260714`。  
> **文档版本：** SU3-MQTT/BLE v1.10（2026-08-20）

---

## 1. 适用范围

| 项 | 说明 |
|----|------|
| 平台 | 涂鸦 IoT（阿里云版）MQTT + 板端 BLE GATT |
| 设备 | 双人床 BC 模组（ESP32）+ 双路 SU3 气囊传感器 |
| 变更要点 | **左右侧通过 JSON `id` 区分**；MQTT 5s 聚合；**睡眠报告 type4~12 与实时数据同一套 `03`/`06` 规则**；**BLE：1s `type:16` + SU3 CLI `type:17` + deviceCli `type:18`**；最多 2 手机同连 |

---

## 2. Topic 列表

`${productKey}`、`${deviceName}` 为设备三元组中的 ProductKey、DeviceName（**烧录基座 ID**，见 3.1）。

| 方向 | Topic | 说明 |
|------|-------|------|
| 下发 | `/${productKey}/${deviceName}/user/cli/get` | 业务控制指令下发 |
| 上报 | `/${productKey}/${deviceName}/user/cli/put` | 业务控制指令反馈 |
| 上报 | `/${productKey}/${deviceName}/user/5s/put` | **实时数据**（Topic 名沿用 5s；板端由 **1s 缓存聚合成 5s** 再上报） |
| 上报 | `/${productKey}/${deviceName}/user/60s/put` | 1 分钟数据 |
| 上报 | `/${productKey}/${deviceName}/user/sa/put` | 呼吸暂停事件 |
| 上报 | `/${productKey}/${deviceName}/user/sleep/put` | 睡眠报告 |
| 上报 | `/${productKey}/${deviceName}/user/mccli/put` | 床控（MFP）指令反馈 |
| 上报 | `/ota/device/inform/${productKey}/${deviceName}` | OTA 版本 inform |
| 下发 | `/ota/device/upgrade/${productKey}/${deviceName}` | OTA 固件下发 |

> **说明：** MQTT Topic 路径中的 `${deviceName}` **始终为烧录基座 ID**（侧别码 `00`）。JSON 载荷内的 `id` 可带 `03`/`06` 表示具体传感器侧。

---

## 3. 设备 ID 编码（核心规则）

### 3.1 ID 格式

本产品（固件 `DEVEICE_ID` / 三元组 DeviceName）前缀为 **`KSSLBED`**（共 15 字符）：

```text
KSSLBED + SS + NNNNNN
^^^^^^^   ^^   ^^^^^^
产品前缀  侧别  设备序号
（7 位） （2） （6 位）
```

| 侧别码 `SS`（第 8～9 字符，1-based） | 含义 | 示例（序号 `004958`） |
|--------------------------------------|------|------------------------|
| `00` | **板端基座**（烧录 / Topic 中的 `${deviceName}`） | `KSSLBED00004958` |
| `03` | **左路传感器**（UART 逻辑地址 3） | `KSSLBED03004958` |
| `06` | **右路传感器**（UART 逻辑地址 6） | `KSSLBED06004958` |

后缀 `NNNNNN` 与基座 ID 相同，仅中间两位侧别码不同。

**固件派生规则（与 `su3_make_side_id` 一致）：**

```text
基座 id = KSSLBED00004958
左路 id = 将第 8～9 字符由 "00" 改为 "03" → KSSLBED03004958
右路 id = 将第 8～9 字符由 "00" 改为 "06" → KSSLBED06004958
```

> **注意：** 旧文档 / 旧产品曾用 `KSPSBED…`。当前固件只认本机烧录的 `KSSLBED…`；JSON `id` 前缀或序号不一致会被 **直接丢弃且不回包**（见 3.4）。

### 3.2 公共 JSON 字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | string | 是 | 见 3.1；**上报与下发均用 `id` 区分侧别** |
| `ts` | number | 是 | UTC 时间戳（秒） |
| `type` | string / number | 视场景 | MQTT 控制类为字符串 `sensorCli` / `deviceCli` / `mcCli`；实时数据、睡眠报告、BLE 多为数字 type |
| `cmd` | string | 是 | CLI 文本（控制类） |
| `back` | string | 应答 | 指令执行结果或查询内容 |

> **v1.2 起不再使用 `sensor`、`side` 字段。** 侧别信息完全由 `id` 中的 `03`/`06` 表达。

### 3.3 下发路由规则

| JSON `id` 侧别码 | 固件行为 |
|------------------|----------|
| `03` | 仅向 **左路** 传感器下发 `cmd` |
| `06` | 仅向 **右路** 传感器下发 `cmd` |
| `00`（基座 ID） | **双侧** 各执行一次相同 `cmd`（如 `set rtc`、`list`、`version`） |

云端若只需操作单侧，应使用对应的 `03` 或 `06` ID；若需双侧，使用基座 `00` ID 一条下发即可。

### 3.4 `id` 校验（固件 `su3_mqtt_id_belong`）

下发到 `cli/get` 的 JSON，在进入 `sensorCli` / `deviceCli` 处理前会校验 `id`：

| 条件 | 要求 |
|------|------|
| 长度 | 与本机基座 ID 相同（当前为 15） |
| 除第 8～9 位外 | **必须与本机基座 ID 逐字符相同**（含 `KSSLBED` 前缀与序号） |
| 第 8～9 位 | 仅允许 `00` / `03` / `06` |

不满足时：**静默丢弃，不向 `cli/put` 回任何内容**。  
Topic 路径里的 `${deviceName}` 始终是基座 `00` ID；不能用错误前缀的 JSON `id`「冒充」本机。

### 3.5 与旧版兼容

| 旧版 | 当前 |
|------|------|
| 前缀 `KSPSBED…` | **改为 `KSSLBED…`**（与烧录 DeviceName 一致） |
| 单 ID，无左右 | 传感器业务用 `03`/`06` ID |
| 可选 `sensor: 3/6` | **废弃**，改由 `id` 表达 |
| 应答无 `type` | 控制类应答 **必须带 `type`** |

---

## 4. 业务控制指令（cli/get → cli/put）

### 4.1 下发格式（`sensorCli`）

Topic：`/${productKey}/${deviceName}/user/cli/get`（`${deviceName}` = 基座 `00`，如 `KSSLBED00004958`）。

**左路：**

```json
{
  "id": "KSSLBED03000072",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "set param_o 6 2.0"
}
```

**右路：**

```json
{
  "id": "KSSLBED06004958",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "list"
}
```

**双侧（基座 ID）：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "set rtc 1735689600"
}
```

### 4.2 应答格式（`sensorCli`）

Topic：`/${productKey}/${deviceName}/user/cli/put`。

每条应答的 `id` **为实际执行侧**（由固件 `su3_make_side_id` 生成，**不是**简单回显下发基座 `00`）：

**左路应答示例：**

```json
{
  "id": "KSSLBED03004958",
  "ts": 1786252850,
  "type": "sensorCli",
  "cmd": "list",
  "back": "0 2021-08-24 17:15:39 009\n1 2021-08-24 20:07:34 006"
}
```

**右路应答示例：**

```json
{
  "id": "KSSLBED06004958",
  "ts": 1786252850,
  "type": "sensorCli",
  "cmd": "list",
  "back": "0 2021-08-25 06:32:23 521"
}
```

| 规则 | 说明 |
|------|------|
| 单侧下发（`03` 或 `06`） | 上报 **1 条** `cli/put`，`id` 为该侧 `03`/`06` |
| 基座 ID 双侧下发 | 上报 **2 条**，**固定顺序**：先 `KSSLBED03004958`，再 `KSSLBED06004958` |
| `back` 内容 | 与传感器 CLI 文本应答一致；换行在 JSON 中转义为 `\n` |
| 传感器栈未就绪 | 回 1 条：`"back":"su3 not ready"`（`id` 为下发原 `id`） |
| CLI 队列满 | 回 `"back":"busy"` |
| `id` 不属于本机 | **不回包**（见 3.4） |
| 某侧离线/超时 | **仍按正常流程上报**该侧一条；`back` 可能为空或超时结果，板端无额外 offline 字段 |

### 4.3 特殊命令说明

完整 command 定义见 **第 5 节**。常用双侧规则：

| `cmd` | 基座 ID 双侧 | 说明 |
|-------|-------------|------|
| `list` | 是 | 各路独立列表，分别应答 |
| `report N …` | 是 | 双侧时两路各拉取（**串行**） |
| `set rtc …` | 是 | 两侧对时 |
| `set mode 2` / `4` | 是 | 默认伴随 / 立即工作 |
| `Set Single Mode` / `Set Dual Mode ` | 是 | 单人/双人输出 |
| `reboot` | 是 | 两侧重启 |
| `version` | 是 | 两侧版本可能不同，必须分 ID 应答 |

---

## 5. `sensorCli` / CLI 命令表（command 定义）

以下 `cmd` 与传感器 UART CLI 一致；经 **MQTT `sensorCli`** 或 **BLE `type:17`** 下发时，路由遵守 **第 3 节 ID 规则**（`00` 双侧 / `03` 左 / `06` 右）。  
通用应答：`ok` = 成功，`failed` = 失败。

### 5.1 传感器主动输出（非云端/App 下发）

| command | 反馈（back） | 说明 |
|---------|--------------|------|
| `hello` | （周期输出） | 传感器 MCU 上电后约每 **500ms** 输出；当前 ID/地址由传感器自定。板端收到后只做 **`set mode X` + 可选读 ID**，**不再 `set addr`** |
| `list updata …` | 如 `7 2021-08-25 16:35:16 065` | 新睡眠报告生成、list 有更新时传感器主动上报索引与时间；源地址为设备地址，目的地址为 0 |

### 5.2 控制 / 查询命令

| command | 反馈（back） | 说明 |
|---------|--------------|------|
| `set addr 3` / `set addr 6` | `ok` | 设置传感器 MCU 地址（03=传感器1，06=传感器2）。**保留**：以传感器 MCU 为准；蓝牙/业务 MCU **仅解析，设置不作数** |
| `factory_reset` | `settings reset...` | 恢复出厂设置 |
| `version` | `SU2-1.0.0,Board:1.0` | 读取软/硬件版本 |
| `reboot` | `ok` | 传感器重新启动 |
| `get mode` | `0`～`10` 等 | 查询状态/故障，见 **5.3** |
| `set mode 0` | `ok` | 停止工作，掉电不保存，进入空闲；2 分钟无指令则自动进入工作 |
| `set mode 2` | `ok` | 开启 1 分 / 1 秒数据伴随（默认模式） |
| `set mode 3` | `ok` | 固件升级模式，掉电不保存；2 分钟无指令则自动回默认工作 |
| `set mode 4` | `ok` | 立即工作（可打断 mode 0）；端口配置完成且待机后方可响应 |
| `Set Single Mode` | `ok` | 单人模式，双侧输出同样数据 |
| `Set Dual Mode` | `ok` | 双人模式，两侧单独输出数据 |
| `Set Without Press Judge` | `ok` | 无气压检测模式，入床不再依赖气压 |
| `Get Sleep Mode` | `Single Mode` / `Dual Mode` | 查询单人/双人输出模式 |
| `Get Press Mode` | `Without Press Mode` / `With Press Mode` | 查询是否使用压力判断 |
| `set calibration manual` | `ok` | 进入手动校准 |
| `set calibration auto` | `ok` | 进入自动校准（默认） |
| `get calibration` | `manual` / `auto` | 查询校准模式 |
| `get rtc` | 如 `1622234876` | 查询时间（Unix 秒） |
| `set rtc 1622234876` | `ok` | 设置时间（长期不断电对时） |
| `set devid SU20000001` | `ok` | 设置传感器设备 ID |
| `get devid` | `SU20000001` | 查询传感器设备 ID |
| `list` | 多行索引列表 | 查询睡眠报告列表（见示例） |
| `report 0` | （走 sleep/put 分包） | 按索引拉取睡眠报告：基础/周期/心率/呼吸/体动/打鼾/收缩压/舒张压等 |

**`list` 反馈示例：**

```text
0 2021-08-24 17:15:39 009
1 2021-08-24 20:07:34 006
2 2021-08-24 21:16:46 001
3 2021-08-25 06:32:23 521
…
```

**`report` 说明：** `report x` 应答索引 `x` 的睡眠报告数据；传感器分别发送基础数据、睡眠周期、心率、呼吸、体动、打鼾、收缩压、舒张压等，板端经 `/user/sleep/put` 转发云端。  
**侧别强制规则（v1.7）：** 该次 `report` 若对左路（`03`）执行，则后续本报告全部 `sleep/put`（type **4、5、6~12**）的 JSON `id` **必须为左路 `03`**；右路同理为 `06`。**禁止**再用基座 `00` ID 上报任一侧报告分包（与 5s/60s 实时数据规则一致）。详见 **§7.3 / §7.4**。

### 5.3 `get mode` 返回值

| 值 | 含义 |
|----|------|
| `0` | 空闲（约 2 分钟无指令后进入默认工作） |
| `1` | 开启 1 分钟数据伴随（掉电保存） |
| `2` | 开启 5 秒 + 1 分钟数据伴随（默认模式） |
| `3` | 固件升级（约 2 分钟无指令后自动回默认工作） |
| `4` | 正在工作 |
| `5` | 传感器1（dest_addr=3）故障 |
| `6` | 传感器2（dest_addr=6）故障 |
| `7` | ADC 故障 |
| `8` | FLASH 故障 |
| `9` | LSE 故障 |
| `10` | HSE 故障 |

**优先级：** 有故障优先返回故障码；无故障返回工作状态码。

### 5.4 其它扩展命令（可选）

| command | 说明 |
|---------|------|
| `set sbp` / `get sbp`、`set dbp` / `get dbp`、`set bpn` / `get bpn` | 血压相关参数（若传感器固件支持） |
| `set param_o …` / `get param_o …` / `get param` | 扩展参数（见传感器原厂协议） |

---

## 6. `deviceCli`（板端，基座 ID）

面向 ESP32 板端，**仅使用基座 ID**（侧别码 `00`），不涉及某一路传感器。

**下发：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "dataUpSwitch1"
}
```

**应答（必须带 `type`）：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "dataUpSwitch1",
  "back": "ok"
}
```

| cmd | back | 说明 |
|-----|------|------|
| `dataUpSwitch1` | `ok` | 开启数据上传 |
| `dataUpSwitch0` | `ok` | 关闭数据上传 |
| `currentReport` | 报告名 | 查询当前报告 |
| `otaRollback0` / `otaRollback1` | `ok` | OTA 回滚分区 |
| `su3GpioQuery` | `high` / `low` | 查询板端 **GPIO12**（SU3 供电 EN）当前输出电平 |
| `su3PowerOff` | `ok` | **仅拉低** GPIO12，关闭 SU3 供电（诊断用，会停报） |
| `su3PowerOn` | `ok` | **仅拉高** GPIO12，打开 SU3 供电（不自动 re-setup） |
| `su3Enable` | `ok` | **冷启动**：GPIO12 先关约 500ms 再开，并触发整机 re-setup |

**SU3 供电诊断命令示例：**

查询 GPIO：

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "su3GpioQuery"
}
```

应答：

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "su3GpioQuery",
  "back": "high"
}
```

仅关电 / 仅开电：

```json
{ "id": "KSSLBED00004958", "ts": 123456789, "type": "deviceCli", "cmd": "su3PowerOff" }
{ "id": "KSSLBED00004958", "ts": 123456789, "type": "deviceCli", "cmd": "su3PowerOn" }
```

应答均为 `"back":"ok"`。

> **说明：** `back` 为 `high`/`low` 表示 ESP32 **GPIO 输出寄存器**读数（高=供电使能开）；与模块端 5V 实测可能因硬件链路略有差异。`su3PowerOff`/`On` 不触发 re-setup；完整恢复请用 `su3Enable`。BLE 见 **§7A.8a**。

**`su3Enable` 下发示例：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "su3Enable"
}
```

**`su3Enable` 应答示例：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "su3Enable",
  "back": "ok"
}
```

> 说明：`"back":"ok"` 表示供电脉冲已执行；传感器 hello/setup 为异步后续过程，APP 可观察实时数据恢复或 `setupLR`。BLE 通道见 **§7A `type:18`**。

---

## 7. 实时数据上报（`/user/5s/put`）

### 7.1 上报策略

| 项 | 约定 |
|----|------|
| Topic | **沿用** `/user/5s/put`（路径用基座 DeviceName） |
| 传感器侧 | UART topic 16，约 **1s** 一帧 |
| 板端策略 | 缓存 1s，**每 5s 聚合** 再 MQTT 上报 |
| 双侧 | 每 5s 最多 2 条，顺序：**先 `03` ID，后 `06` ID** |

### 7.2 JSON 格式（`type: 1`）

`/user/5s/put` 兼容**后端旧字段**，并扩展 5 点数组：

- **旧字段（兼容）**：`sensor_ts` / `heart` / `breath` 为**标量**（取本窗口最后 1 条 1s）；`status` 为长度 5 数组  
- **新字段（5s 聚合）**：`sensor_ts_5s` / `heart_5s` / `breath_5s` / `sdata_5s` / `pdata_5s` / `sign_5s` 为长度 5 数组（下标 0～4 时间先后）

**左路示例：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1735689605,
  "type": 1,
  "data": {
    "sensor_ts": 1735689604,
    "heart": 76,
    "breath": 17,
    "status": [1, 1, 1, 1, 1],
    "sensor_ts_5s": [1735689600, 1735689601, 1735689602, 1735689603, 1735689604],
    "heart_5s": [72, 73, 74, 75, 76],
    "breath_5s": [16, 16, 15, 16, 17],
    "sdata_5s": [0, 0, 0, 0, 0],
    "pdata_5s": [0, 0, 0, 0, 0],
    "sign_5s": [0, 0, 0, 0, 0]
  }
}
```

**右路示例：**

```json
{
  "id": "KSSLBED06000001",
  "ts": 1735689605,
  "type": 1,
  "data": {
    "sensor_ts": 1735689604,
    "heart": 69,
    "breath": 15,
    "status": [1, 1, 1, 0, 1],
    "sensor_ts_5s": [1735689600, 1735689601, 1735689602, 1735689603, 1735689604],
    "heart_5s": [68, 69, 68, 70, 69],
    "breath_5s": [15, 15, 16, 15, 15],
    "sdata_5s": [0, 0, 0, 0, 0],
    "pdata_5s": [0, 0, 0, 0, 0],
    "sign_5s": [0, 0, 0, 0, 0]
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | string | 左路 `…03…`，右路 `…06…` |
| `ts` | number | 板端 UTC（秒） |
| `data.sensor_ts` | number | **旧/标量**：窗口末条 1s 传感器时间 |
| `data.heart` | number | **旧/标量**：窗口末条心率 |
| `data.breath` | number | **旧/标量**：窗口末条呼吸率 |
| `data.status` | number[5] | **旧**：各 1s 状态位 |
| `data.sensor_ts_5s` | number[5] | **新**：5 个 1s 传感器时间戳 |
| `data.heart_5s` | number[5] | **新**：5 个 1s 心率 |
| `data.breath_5s` | number[5] | **新**：5 个 1s 呼吸率 |
| `data.sdata_5s` / `pdata_5s` / `sign_5s` | number[5] | **新**：5 个 1s 对应字段 |
| 聚合规则 | — | 满 5 帧 1s 组包；缺位填 0 |

### 7.3 1 分钟 / 呼吸暂停 / 睡眠报告（总表）

| Topic | type | `id` 规则 |
|-------|------|-----------|
| `/user/60s/put` | `2` | 左路 `03` / 右路 `06`（与实时数据相同） |
| `/user/sa/put` | `15` | 左路 `03` / 右路 `06` |
| `/user/sleep/put` | **4 / 5 / 6~12** | **与触发侧一致：左 `03` / 右 `06`（与实时数据相同）** |

> **硬性约定（v1.7）：** `/user/sleep/put` 上**所有**数字 type（含分包 type6~12）的 `id` 规则，与 `/user/5s/put`、`/user/60s/put` **完全一致**：  
> - 左路传感器产生的报告 → 一律 `…03…`  
> - 右路传感器产生的报告 → 一律 `…06…`  
> - **不得**使用基座 `…00…` 作为睡眠报告业务载荷的 `id`（Topic 路径仍用基座 DeviceName，见 §2）。

---

### 7.4 睡眠报告 `/user/sleep/put` 详细约定

#### 7.4.1 type 与传感器 UART topic 对应

一次 `report N`（或传感器主动推送）后，板端按侧转发到同一 Topic `/user/sleep/put`：

| MQTT `type` | UART topic | 含义 | 载荷形态 |
|-------------|------------|------|----------|
| **4** | （板端主动） | 即将拉取/已选定报告索引通知 | 无 `data` 分包，仅 `report` 名 |
| **5** | 5 | 睡眠周期 / 基础摘要 | 结构化 `data` 字段 |
| **6** | 6 | 睡眠状态原始序列 | 分包 `data`（见 7.4.3） |
| **7** | 7 | 心率原始序列 | 分包 `data` |
| **8** | 8 | 呼吸率原始序列 | 分包 `data` |
| **9** | 9 | 体动原始序列 | 分包 `data` |
| **10** | 10 | 打鼾原始序列 | 分包 `data` |
| **11** | 11 | 收缩压原始序列 | 分包 `data` |
| **12** | 12 | 舒张压原始序列 | 分包 `data` |

同一份报告内：`id`、`report`（报告名）在 type4/5/6~12 各条消息中保持一致；仅 `type` 与 `data` 不同。

#### 7.4.2 公共字段（与实时数据对齐）

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | string | 是 | **侧别 ID**：左 `03` / 右 `06`（派生规则见 §3.1），**禁止 `00`** |
| `ts` | number | 是 | 板端 UTC 秒（上报时刻） |
| `type` | number | 是 | 4 / 5 / 6~12，见上表 |
| `report` | string | 是 | 报告名（与 `list` 行一致，如 `0 2023-10-26 14:18:03 525`） |
| `data` | object | type≥5 | type5 为摘要对象；type6~12 为分包对象 |

**左路 type5 示例：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1786004880,
  "type": 5,
  "report": "0 2023-10-26 14:18:03 525",
  "data": {
    "calResult": 1,
    "startTime": 1698300000,
    "totalSleepTime": 420,
    "sleepEfficiency": 90,
    "sleepQuality": 80,
    "turnoverTimes": 12,
    "sleepLatency": 15,
    "offBedTimes": 1,
    "cRSD": 0,
    "slop1": 0,
    "slop2": 0,
    "osaTimes": 0,
    "avgSA": 0,
    "maxSA": 0
  }
}
```

**右路同一字段，仅 `id` 为 `KSSLBED06000001`。**

**type4（报告索引通知，左路）示例：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1786004880,
  "type": 4,
  "report": "0 2023-10-26 14:18:03 525"
}
```

#### 7.4.3 type6~12 分包格式（与实时数据同一 `id` 规则）

原始序列可能较长，板端按固定块切分后多条上报；**每条分包的 `id` 仍为该侧 `03`/`06`**，不得因分包改成基座 `00`。

| 字段 | 类型 | 说明 |
|------|------|------|
| `data.len` | number | 本报告该 type 原始总字节数 |
| `data.allPacket` | number | 总分包数 |
| `data.packet` | number | 当前包序号，从 **0** 起 |
| `data.packetLen` | number | 本包字节数 |
| `data.value` | number[] | 本包字节数组（0~255） |

**左路 type6 分包示例（第 0 包）：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1786004880,
  "type": 6,
  "report": "0 2023-10-26 14:18:03 525",
  "data": {
    "len": 450,
    "allPacket": 3,
    "packet": 0,
    "packetLen": 200,
    "value": [1, 2, 3]
  }
}
```

**右路示例：仅将 `id` 换为 `KSSLBED06000001`，其余字段含义相同。**

云端组装规则：

1. 按 `(id, report, type)` 聚合分包；  
2. 校验 `packet` 覆盖 `0 .. allPacket-1`，且各包 `packetLen` 之和等于 `len`；  
3. 用 `id` 的侧别码区分左右床，**不得**再假设 sleep 分包一定是基座 `00`。

#### 7.4.4 双侧与顺序

| 场景 | 约定 |
|------|------|
| 仅左路有报告 | 只出现 `03` 的 type4/5/6~12 |
| 仅右路有报告 | 只出现 `06` 的 type4/5/6~12 |
| 双侧各有报告 | 两侧各自完整一套；**不要**把右路数据挂在左路 `id` 上 |
| 与实时数据并存 | 5s/60s 与 sleep 可交错上报；靠 `id`+`type`+Topic 区分 |

#### 7.4.5 兼容说明（给云端 / 测试）

| 版本 | type6~12 的 `id` 行为 |
|------|------------------------|
| **v1.7 协议（目标）** | 与触发侧一致：`03` 或 `06` |
| 历史错误实现 | 可能把 type6~12 打成基座 `00`，而 type5 仍为 `03`/`06` → 云端侧别错乱 |

联调验收：**同一侧同一 `report` 名下，type5 与 type6~12 的 `id` 必须完全相同（同为 03 或同为 06）。**

---

## 7A. 蓝牙协议（BLE GATT）

> 本章独立描述手机 App ↔ ESP32 的 BLE JSON 协议。  
> **侧别规则与 MQTT 相同**（见第 3 节）：`id` 中 `00` 基座 / `03` 左 / `06` 右。  
> 载荷一律 UTF-8 JSON；经 GATT Char **Write** 下发、**Notify** 上报。

### 7A.1 链路与服务

| 项 | 约定 |
|----|------|
| 角色 | ESP32 = Peripheral（GATT Server）；手机 = Central |
| 广播名 | **基座 DeviceName**（侧别码 `00`），如 `KSSLBED00000014`；**不按左右拆成两个名字** |
| 多连接 | 最多 **2** 台手机同时连接同一 ESP；未满 2 路时继续广播 |
| Service UUID | `0x00EE` |
| 上报特征 UUID | `0xFF02`（Read / Write / **Notify**） |
| 下发特征 UUID | `0xFF04`（**Write**） |
| 下发 | App → Write **`0xFF04`** |
| 上报 | ESP → Notify **`0xFF02`**（有连接且已开 Notify 的手机会都收到） |
| MTU | App 连接后应申请较大 MTU（建议 ≥247）；默认 23 时单包仅 20 字节，大 JSON 会被截断 |

### 7A.2 `type` 一览（BLE 数字 type）

| type | 方向 | 用途 | `id` 规则 |
|------|------|------|-----------|
| **3** | 下发 + 状态 Notify | WiFi 一键配网 | 基座 `00` |
| **4** | 下发 / 应答 | 查询板端版本 `sensorVersion` | 基座 `00` |
| **5** | 下发 / 应答 | 传感器升级占位（暂未实现） | 基座 `00` |
| **12** | 下发 / 应答 | 床控 MFP（`cmd` 为 HEX 字符串） | 基座 `00` |
| **16** | **仅上报** | SU3 **1s** 实时数据（不上 MQTT） | 左 `03` / 右 `06` |
| **17** | **下发 / 应答** | SU3 **sensorCli**（JSON 字段同 MQTT `sensorCli`） | `00`/`03`/`06` |
| **18** | **下发 / 应答** | 板端 **deviceCli**（当前支持 `su3Enable`，字段同 MQTT `deviceCli`） | 基座 `00` |

> MQTT 侧 `type` 多为字符串（`sensorCli` / `deviceCli` / `mcCli`）；BLE App 历史使用**数字 type**。  
> **`type:17` = 蓝牙通道上的 sensorCli**，`cmd` / `back` / 侧别路由与第 4 节 MQTT 完全一致。  
> **`type:18` = 蓝牙通道上的 deviceCli**，`cmd` / `back` 与第 6 节 MQTT 对齐（至少 `su3Enable`）。

---

### 7A.3 配网（`type: 3`）

**下发：**

```json
{
  "id": "KSSLBED00000018",
  "type": 3,
  "cmd": {
    "ssid": "7788",
    "passwd": "77885522"
  }
}
```

**状态 Notify（示例）：**

```json
{"id":"KSSLBED00000014","ts":1735689605,"type":3,"state":"wifiOk"}
```

`state` 可能值：`wifiOk` / `wifiFail` / `mqttOk` / `mqttFail`。

---

### 7A.4 版本查询（`type: 4`）

**下发：**

```json
{
  "id": "KSSLBED00000014",
  "type": 4,
  "cmd": "sensorVersion"
}
```

**应答 Notify：**

```json
{
  "id": "KSSLBED00000014",
  "ts": 1735689605,
  "type": 4,
  "version": "SL_ESP_2026_1_0_2"
}
```

---

### 7A.5 传感器升级占位（`type: 5`）

**下发：** `cmd` = `"sensorUpgrade"`（基座 `00`）。  
当前固件仅回告未实现，不执行真实 OTA。

---

### 7A.6 床控 MFP（`type: 12`）

与 MQTT `mcCli` 同类业务；BLE 用数字 type，`cmd` 为 **HEX 字符串**（`AA…`）。

**下发：**

```json
{
  "id": "KSSLBED00000014",
  "ts": 1735689605,
  "type": 12,
  "cmd": "AA020B080100000078010F5030xxxx"
}
```

应答经 Notify 上报（格式沿用既有床控 JSON，`id` 为基座）。

---

### 7A.7 SU3 1s 实时上报（`type: 16`）

| 项 | 约定 |
|----|------|
| 通道 | **仅 BLE Notify**，**不上 MQTT** |
| 触发 | UART topic16，约每 1s 一帧 → 立即 Notify |
| 双侧 | 左右各报一条，`id` 为 `03` / `06` |
| 多连接 | 同一条 Notify 发给所有已连接手机 |
| 开关 | 需 `data_up_switch` 开启且 BLE 已连接 |

MQTT 路径不变：1s 仍缓存，满 5 帧走 `/user/5s/put`（`type:1`）。

**左路示例：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1735689605,
  "type": 16,
  "data": {
    "sensor_ts": 1735689604,
    "heart": 72,
    "breath": 16,
    "status": 1,
    "sdata": 0,
    "pdata": 0,
    "sign": 0
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | string | 左 `…03…` / 右 `…06…` |
| `ts` | number | 板端 UTC（秒） |
| `type` | number | 固定 **16** |
| `data.*` | number | 本帧标量（非数组） |

---

### 7A.8 SU3 CLI 下发 / 应答（`type: 17`）★

与 MQTT **`sensorCli`**（第 4 节）对齐：**同一套 `id` 路由 + 同一套 `cmd` 文本**（见第 5 节命令表）。  
区别仅在于：通道为 BLE，且 `type` 使用数字 **17**（MQTT 仍为字符串 `"sensorCli"`）。

#### 下发（App → Write）

**左路：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1735689605,
  "type": 17,
  "cmd": "list"
}
```

**右路：**

```json
{
  "id": "KSSLBED06000001",
  "ts": 1735689605,
  "type": 17,
  "cmd": "list"
}
```

**双侧（基座 ID）：**

```json
{
  "id": "KSSLBED00000001",
  "ts": 1735689605,
  "type": 17,
  "cmd": "set rtc 1735689600"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | string | `00` 双侧 / `03` 仅左 / `06` 仅右（同 MQTT） |
| `ts` | number | UTC 秒 |
| `type` | number | 固定 **17** |
| `cmd` | string | 与 MQTT `sensorCli` 相同的 CLI 文本 |

#### 应答（ESP → Notify）

每条应答的 `id` 与目标侧一致；双侧下发时 **先 03 后 06** 各一条（同 MQTT）。

```json
{
  "id": "KSSLBED03000001",
  "ts": 1735689605,
  "type": 17,
  "cmd": "list",
  "back": "0 2021-08-24 17:15:39 009\n1 2021-08-24 20:07:34 006"
}
```

| 规则 | 说明 |
|------|------|
| 通道 | 仅 BLE Notify（不强制再发一份 MQTT `cli/put`，除非业务另定） |
| 多连接 | 应答 Notify 发给当前所有已连接手机 |
| `back` | 传感器 CLI 文本原样 |
| 命令集 | 同第 5 节（`list` / `report` / `set rtc` / `version` 等） |

#### 与 MQTT `sensorCli` 对照

| 项 | MQTT | BLE |
|----|------|-----|
| 通道 | `cli/get` → `cli/put` | Write **`0xFF04`** → Notify **`0xFF02`** |
| `type` | 字符串 `"sensorCli"` | 数字 **`17`** |
| `id` / `cmd` / `back` | 相同 | 相同 |
| 侧别路由 | 00/03/06 | 00/03/06 |

---

### 7A.8a 板端 deviceCli（`type: 18`）

与 MQTT **`deviceCli`**（第 6 节）对齐；BLE 用数字 **18**。

| cmd | back | 说明 |
|-----|------|------|
| `su3GpioQuery` | `high` / `low` | 查询 GPIO12 电平 |
| `su3PowerOff` | `ok` | 仅拉低 GPIO12 |
| `su3PowerOn` | `ok` | 仅拉高 GPIO12 |
| `su3Enable` | `ok` | 关 500ms 再开 + 整机 re-setup |

**查询下发 / 应答 Notify：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": 18,
  "cmd": "su3GpioQuery"
}
```

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": 18,
  "cmd": "su3GpioQuery",
  "back": "high"
}
```

**`su3Enable` 下发 / 应答：**

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": 18,
  "cmd": "su3Enable"
}
```

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": 18,
  "cmd": "su3Enable",
  "back": "ok"
}
```

| 项 | 说明 |
|----|------|
| `id` | 仅基座 `00` |

---

### 7A.9 App 对接注意

1. 扫到广播名后连接，打开 **`0xFF02`** 的 Notify（写 CCCD）。  
2. 业务下发（配网 / 床控 / type:17 / type:18 等）Write 到 **`0xFF04`**。  
3. **先协商 MTU**，再收 `type:16` / 较长的 `type:17` `back`。  
4. 实时曲线看 BLE `type:16`；云端历史/聚合仍看 MQTT `type:1`（5s）。  
5. SU3 控制：`type:17` + `cmd`；床控：`type:12`。

---

## 8. `mcCli`（床控，基座 ID）

床控面向 MFP 主控盒，使用基座 ID，格式保持原协议：

```json
{
  "id": "KSSLBED00004958",
  "ts": 123456789,
  "type": "mcCli",
  "cmd": "AA020B080100000078010F5030xxxx"
}
```

应答 Topic：`/user/mccli/put`。

---

## 9. 双人床交互时序（建议）

```
云端                         ESP32                  传感器 L(3)    传感器 R(6)
  |                            |                         |              |
  |-- cli/get id=…000… list ---|                         |              |
  |   （基座 ID，双侧）         |-- list ---------------->|              |
  |                            |-- list ------------------------------>|
  |<-- cli/put id=…030… -------|  （先 L）               |              |
  |<-- cli/put id=…060… -------|  （后 R）               |              |
  |                            |                         |              |
  |-- cli/get id=…030… report -|                         |              |
  |                            |-- report N ------------>|              |
  |<-- sleep/put id=…030… -----|  type4/5/6~12 均为 03   |              |
  |                            |                         |              |
  |-- cli/get id=…060… report -|                         |              |
  |                            |-- report N -------------------------->|
  |<-- sleep/put id=…060… -----|  type4/5/6~12 均为 06   |              |
  |                            |                         |              |
  |<-- 5s/put id=…030… --------|  （每 5s，先 L）        |              |
  |<-- 5s/put id=…060… --------|  （每 5s，后 R）        |              |
```

---

## 10. 已定约定（v1.2 / v1.7 / v1.8 / v1.9 / v1.10 补充）

| 序号 | 项 | 结论 |
|------|-----|------|
| 1 | 侧别区分 | **JSON `id` 侧别码**：`00` 基座 / `03` 左 / `06` 右；**废弃 `sensor` 字段** |
| 2 | MQTT Topic | 路径仍用 **基座 DeviceName**（`00`）；载荷 `id` 带侧别 |
| 3 | 双侧下发 | 基座 `00` ID → 固件双侧执行；应答 **先 03 后 06** |
| 4 | 5s 上报 | Topic 不改名；1s 采集、**5s 聚合**；每侧独立 `id` |
| 5 | 某侧离线 | 正常上报，APP 靠 0 数据区分 |
| 6 | `deviceCli` | 仅基座 `00` ID；含 **`su3GpioQuery` / `su3PowerOff` / `su3PowerOn` / `su3Enable`** |
| 7 | **睡眠报告（v1.7）** | `/user/sleep/put` 的 type **4/5/6~12** 与实时数据相同：`id` 必须为触发侧 `03`/`06`；**禁止** type6~12 使用基座 `00` |
| 8 | **产品前缀（v1.8）** | 本固件 DeviceName / JSON `id` 前缀为 **`KSSLBED`**；旧示例 `KSPSBED` 无效且不回包 |
| 9 | **SU3 供电（v1.9～v1.10）** | `su3Enable`=冷启动+re-setup；v1.10 增 GPIO 查询与分步 On/Off 诊断 |

---

## 11. 修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| SU3-MQTT/BLE v1.9 | 2026-08-12 | **deviceCli `su3Enable`**：GPIO12 先关后开使能/冷启动 SU3；BLE 新增 **`type:18`** |
| SU3-MQTT/BLE v1.8 | 2026-08-09 | **ID 前缀对齐固件**：全文示例由 `KSPSBED` 改为 `KSSLBED`；§3 补 `su3_mqtt_id_belong` 校验说明；§4.1/4.2 `sensorCli` 示例与应答规则与代码一致 |
| SU3-MQTT/BLE v1.7 | 2026-08-08 | **睡眠报告对齐实时数据侧别规则**：明确 type4/5/6~12 一律 `03`/`06`；新增 **§7.4**（type 表、分包字段、验收口径）；修正历史「分包可能打成 00」的歧义 |
| SU3-MQTT/BLE v1.6 | 2026-07-31 | 整理完整 **BLE 协议（7A）**；新增 SU3 CLI **`type:17`**；**§5 补全 CLI command 定义**（hello/mode/单双人/list/report 等） |
| SU3-MQTT/BLE v1.5 | 2026-07-29 | 5s `type:1`：恢复旧标量 `heart`/`breath`/`sensor_ts` + `status[5]`；5 点数组改到 `heart_5s` 等新字段 |
| SU3-MQTT/BLE v1.4 | 2026-07-29 | 新增 BLE **1s 实时 `type:16`**（仅 Notify，不上 MQTT）；MQTT 5s 仍为 `type:1` |
| SU3-MQTT v1.3 | 2026-07-28 | 5s：`data.sensor_ts` 改为**标量**（窗口末条 1s 时间）；其余字段仍为长度 5 数组；示例 ID 改为 `KSSLBED` |
| SU3-MQTT v1.2 | 2026-07-20 | 侧别改由 `id` 编码（03/06）；5s 载荷为 **5×1s 数组聚合** |
| SU3-MQTT v1.1 | 2026-07-20 | sensor 3/6、5s 聚合、双侧顺序 |
| SU3-MQTT v1.0 | 2026-07-20 | 初稿 |
| 原 docx | — | 《盛和养老系统下发协议V2.4》 |

---

## 附录 A：与原 docx / 前版差异摘要

| 原协议 / 前版 | SU3-MQTT/BLE v1.9 |
|---------------|-------------------|
| 示例 ID 前缀 `KSPSBED`（旧产品） | **统一为 `KSSLBED`**，与烧录 DeviceName / `su3_mqtt_id_belong` 一致 |
| sleep type6~12 `id` 未强制 / 实现易打成基座 `00` | **强制与实时数据一致**：触发侧 `03`/`06`；见 §7.4 |
| sleep 仅有 type5 简例 | 补全 type4 + type5 + **type6~12 分包字段**与验收规则 |
| BLE 仅散落描述 type:16（v1.5） | **7A 完整 BLE 协议**（自 v1.6）：链路、type 表、3/4/5/12/16/17/18 |
| 无 BLE 下发 SU3 CLI | 新增 **`type:17`**（自 v1.6），字段对齐 MQTT `sensorCli` |
| 无板端供电冷启动命令 | **v1.9**：MQTT `deviceCli`/`su3Enable` + BLE **`type:18`** |
| 5s 字段 / deviceCli | 保持 v1.5，并扩展 `su3Enable` |
