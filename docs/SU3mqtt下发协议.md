# SU3 MQTT 下发 / 上报协议

> 本文档在《盛和养老系统下发协议》（原 `SU3mqtt下发协议.docx`）基础上修订，适配 **SU3 双人床双传感器**。  
> UART 传感器协议参照：`SU3传感器通信协议20260714`。  
> **文档版本：** SU3-MQTT v1.3（2026-07-28）

---

## 1. 适用范围

| 项 | 说明 |
|----|------|
| 平台 | 涂鸦 IoT（阿里云版）MQTT |
| 设备 | 双人床 BC 模组（ESP32）+ 双路 SU3 气囊传感器 |
| 变更要点 | **左右侧通过 JSON `id` 区分**，不再使用 `sensor` 字段；应答带 `type`；双侧指令固定 **先 L 后 R** 应答 |

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

设备 ID 为固定长度字符串，格式：

```text
KSPSBED + SS + NNNNNN
         ^^
      侧别码（2 位）
```

| 侧别码 `SS` | 含义 | 示例（后缀 `002828`） |
|-------------|------|------------------------|
| `00` | **板端基座**（烧录 / 三元组 DeviceName） | `KSPSBED00002828` |
| `03` | **左路传感器**（UART 逻辑地址 3） | `KSPSBED03002828` |
| `06` | **右路传感器**（UART 逻辑地址 6） | `KSPSBED06002828` |

后缀 `NNNNNN` 与基座 ID 相同，仅中间两位侧别码不同。

**固件派生规则（示例）：**

```text
基座 id = KSPSBED00002828
左路 id = 将第 8～9 字符（1-based）由 "00" 改为 "03" → KSPSBED03002828
右路 id = 将第 8～9 字符（1-based）由 "00" 改为 "06" → KSPSBED06002828
```

### 3.2 公共 JSON 字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | string | 是 | 见 3.1；**上报与下发均用 `id` 区分侧别** |
| `ts` | number | 是 | UTC 时间戳（秒） |
| `type` | string | 视场景 | `sensorCli` / `deviceCli` / `mcCli`；实时数据为数字 type |
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

### 3.4 与旧版兼容

| 旧版 | SU3 v1.2 |
|------|----------|
| 单 ID，无左右 | 传感器业务用 `03`/`06` ID |
| 可选 `sensor: 3/6` | **废弃**，改由 `id` 表达 |
| 应答无 `type` | 控制类应答 **必须带 `type`** |

---

## 4. 业务控制指令（cli/get → cli/put）

### 4.1 下发格式（`sensorCli`）

**左路：**

```json
{
  "id": "KSPSBED03002828",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "list"
}
```

**右路：**

```json
{
  "id": "KSPSBED06002828",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "list"
}
```

**双侧（基座 ID）：**

```json
{
  "id": "KSPSBED00002828",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "set rtc 1735689600"
}
```

### 4.2 应答格式（`sensorCli`）

每条应答的 `id` **与目标侧一致**（不再共用基座 ID）：

```json
{
  "id": "KSPSBED03002828",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "list",
  "back": "0 2021-08-24 17:15:39 009\n1 2021-08-24 20:07:34 006"
}
```

```json
{
  "id": "KSPSBED06002828",
  "ts": 123456789,
  "type": "sensorCli",
  "cmd": "list",
  "back": "0 2021-08-25 06:32:23 521"
}
```

| 规则 | 说明 |
|------|------|
| 单侧下发（`03` 或 `06`） | 上报 **1 条** `cli/put`，`id` 与下发一致 |
| 基座 ID 双侧下发 | 上报 **2 条**，**固定顺序**：先 `KSPSBED03002828`，再 `KSPSBED06002828` |
| `back` 内容 | 与传感器 CLI 文本应答一致 |
| 某侧离线/无应答 | **仍按正常流程上报**；APP 可根据数据为 0 等自行区分，板端无额外 offline 逻辑 |

### 4.3 特殊命令说明

| `cmd` | 基座 ID 双侧 | 说明 |
|-------|-------------|------|
| `list` | 是 | 各路独立列表，分别应答 |
| `report N …` | 是 | 双侧时两路各拉取（**串行**） |
| `set rtc …` | 是 | 两侧对时 |
| `set mode 4` | 是 | 两侧进入工作模式 |
| `reboot` | 是 | 两侧重启 |
| `version` | 是 | 两侧版本可能不同，必须分 ID 应答 |

---

## 5. `sensorCli` 命令表

以下 `cmd` 与传感器 UART CLI 一致；路由遵守 **第 3 节 ID 规则**。

| cmd | back 示例 | 说明 |
|-----|-----------|------|
| `version` | `SU2-2.0.0,Board:2.0` | 软件/硬件版本 |
| `reboot` | `ok` | 重启传感器 |
| `get mode` | `1` / `2` / `4` 等 | 查询模式 |
| `set mode 0` | `ok` | 强制生成报告并停止 |
| `set mode 2` | `ok` | 1s + 1min 伴随 |
| `set mode 3` | `ok` | 固件升级模式 |
| `set mode 4` | `ok` | 立即工作 |
| `set calibration manual` / `auto` | `ok` | 校准模式 |
| `get calibration` | `manual` / `auto` | 查询校准模式 |
| `list` | 多行列表 | 睡眠报告索引 |
| `report 5 2023-10-26 14:18:03 525` | （报告走 sleep/put） | 拉取指定报告 |
| `set sbp 127` / `set dbp 83` / `set bpn 40` | `ok` | 血压参数 |
| `get sbp` / `get dbp` / `get bpn` | 数值 | 查询血压参数 |
| `set param_o …` / `get param_o …` / `get param` | 见原协议 | 扩展参数 |

---

## 6. `deviceCli`（板端，基座 ID）

面向 ESP32 板端，**仅使用基座 ID**（侧别码 `00`），不涉及某一路传感器。

**下发：**

```json
{
  "id": "KSPSBED00002828",
  "ts": 123456789,
  "type": "deviceCli",
  "cmd": "dataUpSwitch1"
}
```

**应答（必须带 `type`）：**

```json
{
  "id": "KSPSBED00002828",
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

`/user/5s/put` 载荷为 **5 个 1s 采样点的聚合**：`heart` / `breath` / `status` / `sdata` / `pdata` / `sign` 为 **长度 5 的 JSON 数组**（下标 0～4 对应时间先后）；**`sensor_ts` 为标量**，取本窗口内**最后 1 条** 1s 传感器时间戳（即聚合数组下标 `[4]`）。

**左路示例：**

```json
{
  "id": "KSSLBED03000001",
  "ts": 1735689605,
  "type": 1,
  "data": {
    "sensor_ts": 1735689604,
    "heart": [72, 73, 74, 75, 76],
    "breath": [16, 16, 15, 16, 17],
    "status": [1, 1, 1, 1, 1],
    "sdata": [0, 0, 0, 0, 0],
    "pdata": [0, 0, 0, 0, 0],
    "sign": [0, 0, 0, 0, 0]
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
    "heart": [68, 69, 68, 70, 69],
    "breath": [15, 15, 16, 15, 15],
    "status": [1, 1, 1, 0, 1],
    "sdata": [0, 0, 0, 0, 0],
    "pdata": [0, 0, 0, 0, 0],
    "sign": [0, 0, 0, 0, 0]
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | string | 左路 `…03…`，右路 `…06…` |
| `ts` | number | 板端 UTC（秒），上报时刻 `device_info->utc.time_stamp` |
| `data.sensor_ts` | number | **标量**：本 5s 窗口内最后 1 条 1s 的传感器时间戳 |
| `data.heart` | number[5] | 各 1s 心率 |
| `data.breath` | number[5] | 各 1s 呼吸率 |
| `data.status` | number[5] | 各 1s 状态位（与原协议 `status:[0,0,0,0,0]` 语义一致） |
| `data.sdata` / `pdata` | number[5] | 各 1s 原始/处理数据 |
| `data.sign` | number[5] | 各 1s Sign（床级融合标志，非左右编号） |
| 聚合规则 | — | 板端每收满 **5 帧 1s** 组包上报一次；不足 5 帧的缺位 **填 0** |
| 离线/无数据 | — | 仍按 5s 节奏上报；对应秒位为 **0** 时由 APP 判定 |

### 7.3 1 分钟 / 呼吸暂停 / 睡眠报告

| Topic | type | `id` 规则 |
|-------|------|-----------|
| `/user/60s/put` | `2` | 左路 `03` / 右路 `06` |
| `/user/sa/put` | `15` | 左路 `03` / 右路 `06` |
| `/user/sleep/put` | `5` 等 | 与触发侧一致，`03` 或 `06` |

**睡眠报告示例（左路）：**

```json
{
  "id": "KSPSBED03002828",
  "ts": 123456789,
  "type": 5,
  "report": "2023-10-26 14:18:03 525",
  "data": { }
}
```

---

## 8. `mcCli`（床控，基座 ID）

床控面向 MFP 主控盒，使用基座 ID，格式保持原协议：

```json
{
  "id": "KSPSBED00002828",
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
  |<-- sleep/put id=…030… -----|                         |              |
  |                            |                         |              |
  |<-- 5s/put id=…030… --------|  （每 5s，先 L）        |              |
  |<-- 5s/put id=…060… --------|  （每 5s，后 R）        |              |
```

---

## 10. 已定约定（v1.2）

| 序号 | 项 | 结论 |
|------|-----|------|
| 1 | 侧别区分 | **JSON `id` 侧别码**：`00` 基座 / `03` 左 / `06` 右；**废弃 `sensor` 字段** |
| 2 | MQTT Topic | 路径仍用 **基座 DeviceName**（`00`）；载荷 `id` 带侧别 |
| 3 | 双侧下发 | 基座 `00` ID → 固件双侧执行；应答 **先 03 后 06** |
| 4 | 5s 上报 | Topic 不改名；1s 采集、**5s 聚合**；每侧独立 `id` |
| 5 | 某侧离线 | 正常上报，APP 靠 0 数据区分 |
| 6 | `deviceCli` | 仅基座 `00` ID；应答带 `type` |

---

## 11. 修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| SU3-MQTT v1.3 | 2026-07-28 | 5s：`data.sensor_ts` 改为**标量**（窗口末条 1s 时间）；其余字段仍为长度 5 数组；示例 ID 改为 `KSSLBED` |
| SU3-MQTT v1.2 | 2026-07-20 | 侧别改由 `id` 编码（03/06）；5s 载荷为 **5×1s 数组聚合** |
| SU3-MQTT v1.1 | 2026-07-20 | sensor 3/6、5s 聚合、双侧顺序 |
| SU3-MQTT v1.0 | 2026-07-20 | 初稿 |
| 原 docx | — | 《盛和养老系统下发协议V2.4》 |

---

## 附录 A：与原 docx / v1.2 差异摘要

| 原协议 / v1.2 | SU3-MQTT v1.3 |
|---------------|----------------|
| 单 ID | 传感器业务 **03 / 06 派生 ID** |
| 可选 `sensor: 3/6` | **废弃**，并入 `id` |
| 可选 `side: L/R` | **废弃**，由 `id` 推断 |
| 5s：`sensor_ts` 为 number[5] | **`sensor_ts` 为标量**（最后 1 条 1s 时间）；heart 等仍为 [5] |
| deviceCli | 基座 `00` ID + 应答带 `type` |
