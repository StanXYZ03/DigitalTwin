# M0 数字孪生网页按键前后端开发手册

## 1. 目标与边界

网页按面板丝印完整复刻 F1～F10，并与十个实体键同步按下/松开状态。实现不能要求用户修改 XC7A 实验 HDL。对 exp2-01，只有 F2～F6 具有实验功能，依次转换为 `PI[8]`～`PI[12]` 的一次翻转；F1、F7～F10 仍完整采集、传输和显示，只是该实验不消费它们。

实体按键和网页按键是并行输入源：

```text
实体 F1..F10 -> Lattice 消抖/状态 ----------------------> 网页同步显示
实体 F2..F6  -> Lattice 按下沿翻转 -> physical_pi_requested --+
                                                                 XOR -> MCP23017 -> XC7A PI[8..12]
网页 F2..F6  -> down 按下沿 -> STM32 virtual_toggle -----------+
```

合并公式为：

```text
pi_applied = pi_requested_from_lattice XOR pi_virtual_toggle
```

任一输入源发生一次有效按下沿，都只会令对应 PI 位翻转一次。网页会分别发送 down 和 up，使长按状态能够持续同步；up 只清除网页保持状态，不产生第二个实验 PI 翻转。

## 2. 运行环境

- STM32 地址：`192.168.100.50/24`
- 数字孪生服务器建议地址：`192.168.100.100/24`
- 遥测与控制 UDP 端口：`5005`
- 网页与 REST API 端口：`8080`
- Node.js：无需第三方 npm 包

启动后端：

```powershell
cd D:\CubeMX\STM32_ETH_M0_Demo\DigitalTwin
node .\server.js
```

浏览器打开 `http://192.168.100.100:8080/`。

## 3. 前端实现

页面文件为 `public/index.html`，完整显示 F1～F10：

- 实体按键灯来自遥测 `key_state`，表示 Lattice 实际采集的按键电平。
- `pointerdown` 调用 down API，按住期间不重复发送；`pointerup`、取消或页面失焦调用 up API。
- 网页按钮在实体或网页任一来源按住时高亮，悬停提示分别列出实体、网页和 XOR 状态。
- 后端返回 HTTP `202 Accepted` 只代表命令已排队。
- 页面通过 WebSocket 中的 `control.status` 判断 STM32 是否真正确认执行。
- `acknowledged` 表示 STM32 已接收该命令，并在该次遥测前更新了 PI 输出目标。

调用示例：

```javascript
await fetch('/api/digital-twin/devices/board-001/keys/F2/down', {
  method: 'POST'
});
```

按键编号和默认实验功能：

| 面板/网页键 | PI 位 | exp2-01 功能 |
|---|---:|---|
| F1 | — | 未使用 |
| F2 | PI[8] | 暂停/继续（旧逻辑名 KEY3） |
| F3 | PI[9] | 清零（旧逻辑名 KEY4） |
| F4 | PI[10] | 递减（旧逻辑名 KEY5） |
| F5 | PI[11] | 递增（旧逻辑名 KEY6） |
| F6 | PI[12] | LED 滚动方向（旧逻辑名 KEY7） |
| F7～F10 | — | 未使用，但状态和控制完整保留 |

“当前示例实验功能”只是页面提示；平台传递的是通用按键事件，其他用户实验可自行定义各 PI 位用途。

## 4. REST 与 WebSocket 接口

### 4.1 获取快照

```http
GET /api/digital-twin/devices/board-001/snapshot
```

设备在线时返回 `200`，尚未收到遥测时返回 `503`。

### 4.2 下发按键

```http
POST /api/digital-twin/devices/board-001/keys/F{1..10}/{down|up}
```

正常返回：

```json
{"status":"queued","commandId":305419896,"key":"F2","action":"down"}
```

常见状态码：

- `202`：已排队。
- `400`：按键参数无效。
- `429`：控制队列已满。
- `503`：设备离线，无法确定 STM32 的 UDP 回程地址。

### 4.3 实时快照

```text
ws://服务器:8080/ws/digital-twin/devices/board-001
```

快照中的控制字段示例：

```json
{
  "control": {
    "status": "acknowledged",
    "commandId": 305419896,
    "key": 2,
    "action": "down",
    "attempts": 1,
    "queued": 0
  }
}
```

状态包括 `idle`、`pending`、`acknowledged` 和 `failed`。

## 5. 后端控制队列

后端入口仍为 `server.js`，双向实现位于 `server_control.js`。后端从合法 STM32 遥测包记录其 UDP 源地址和临时源端口，再把控制帧发回同一端点。因此不需要在 STM32 额外开放固定控制端口，也能穿过常见的主机防火墙会话规则。

控制队列一次只允许一个在途命令：

1. 为每个 down/up 动作分配非零 32 位 `commandId`。
2. 立即发送控制帧。
3. 250 ms 内未在遥测 `control_ack` 中看到相同 ID，则重发。
4. 最多发送 8 次；仍无确认则标记 `failed`。
5. 收到确认后再发送下一条排队命令，避免连续点击覆盖中间确认。

## 6. UDP 控制帧

控制帧固定为 16 字节，所有多字节整数均采用网络字节序（大端）：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `M0KC` |
| 4 | 1 | version | 当前为 `1` |
| 5 | 1 | key | 面板 F 编号 `1`～`10` |
| 6 | 1 | action | `1`=down，`2`=up |
| 7 | 1 | reserved | 必须为 `0` |
| 8 | 4 | command_id | 非零 32 位命令号 |
| 12 | 2 | reserved2 | 当前必须为 `0` |
| 14 | 2 | crc16 | 对字节 0～13 计算 CRC-16/CCITT-FALSE |

CRC 参数：初值 `0xFFFF`，多项式 `0x1021`，不反射，无最终异或。

## 7. STM32 接收与执行

`ETHDefaultTask.c` 使用发送遥测的同一个非阻塞 UDP socket 轮询控制帧。接收端依次检查：

1. 帧长度必须为 16 字节。
2. 源 IP 和源端口必须与配置的数字孪生服务器一致。
3. magic、版本、按键范围、动作和保留字段必须合法。
4. CRC16 必须正确。
5. `command_id` 必须是新的序号；重发的相同命令只确认、不重复执行。

`M0_DataSource_VirtualKeySet()` 保存十位网页保持状态。F2～F6 的新 down 边沿会立即调用 `FPGA_PI_Write()` 并翻转对应 `virtual_pi_toggle`；重复 down 和 up 均不产生额外实验边沿。下一次成功的 FMC 快照继续按 XOR 公式与实体翻转状态对齐。遥测增加：

- `pi_virtual_toggle`：网页事件累计形成的翻转掩码。
- `virtual_key_state`：网页当前保持按下的 F1～F10 位图，bit0 对应 F1。
- `virtual_key_count`：STM32 接受的网页按键事件计数。
- `control_ack`：最后一个已经成功写入 PI 扩展器的命令 ID。

Keil 调试时可观察：

- `eth_task_dbg.control_rx_count`
- `eth_task_dbg.control_accept_count`
- `eth_task_dbg.control_duplicate_count`
- `eth_task_dbg.control_reject_count`
- `eth_task_dbg.last_control_id`
- `m0_fmc_dbg.pi_virtual_toggle`
- `m0_fmc_dbg.virtual_key_state`
- `m0_fmc_dbg.virtual_key_count`
- `m0_fmc_dbg.last_control_id`
- `m0_fmc_dbg.pi_applied`

## 8. 测试方法

后端单元测试：

```powershell
cd D:\CubeMX\STM32_ETH_M0_Demo\DigitalTwin
node .\test_server.js
```

测试覆盖十六进制显示、LED 映射、遥测序号、控制帧字段、CRC、控制重发和确认。

硬件联调建议按以下顺序：

1. 确认网页显示“在线”，遥测继续更新。
2. 按住网页 F2，确认按钮持续高亮且 `virtual_key_state` bit1 保持为 1；松开后恢复 0。
3. 观察计数暂停；再次按下 F2，计数继续。
4. 分别验证 F3～F6 的实验功能，以及 F1、F7～F10 的状态同步。
5. 交替或同时使用实体键和网页键，确认两路按 XOR 规则独立生效。
6. Wireshark 使用 `udp.port == 5005`；JSON 是 STM32→服务器遥测，16 字节 `M0KC` 是服务器→STM32 控制帧。

## 9. 二次开发注意事项

- 必须在真实 pointerdown/pointerup 时分别发送 down/up，不能用 click 模拟长按。
- up 只清除保持状态，不得再次翻转 PI；实验接口仍以按下沿引起的 PI 位翻转表示一次按键事件。
- 不要用网页状态覆盖 Lattice 的 `pi_requested`；必须使用 XOR 合并，才能保留实体按键控制。
- 新增动作时应提升协议版本或分配新的 `action`，并在 STM32 严格拒绝未知值。
- 当前方案适合受控实验局域网。若跨公网部署，应在 HTTP 前增加身份认证和 TLS，并通过 VPN 隔离 UDP 控制网络。
