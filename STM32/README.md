# STM32H743 FMC16 M0 数字孪生

本工程从 Lattice 的 FMC Bank1/NE1 接口读取实验 FPGA 的真实 `PO[31:0]`、`PIO[15:0]` 和实体按键状态，完成 FMC16 长度、版本、事务号、CRC16/CRC32 及有效位校验后，才通过 UDP 上报 `source=fmc16` 快照。读取失败时不会上传模拟数据。

## 平台启动约束

STM32 在释放 Xilinx `PROGRAM_B` 之前，先通过 MCP23017 将 `PI[15:0]` 驱动为确定的空闲值 0。这样 Xilinx 可以运行任意用户实验，无需在用户 HDL 中加入平台专用启动延时、按键基线学习或复位补丁。配置完成后，STM32 隔离共享桥接支路，再开始 FMC/LCD/UDP 运行。

## 网络与报文

- STM32：`192.168.100.50/24`
- 服务器：`192.168.100.100`
- UDP：`5005`
- 周期：`100 ms`

上报字段包括 `sequence`、`timestamp_ms`、`po`、`pio`、`pi_applied`、`pi_requested`、`key_state` 和 `key_event_count`。数值均为无符号 JSON number。

## 数字孪生接收展示

零依赖 Node.js 接收端和网页位于 `DigitalTwin`。它提供 UDP 接收、校验与去重、设备重启识别、在线/延迟/离线状态、REST 快照、WebSocket 推送，以及 8 位十六进制数码管、12 个 LED 和 KEY3~KEY7 的只读镜像。

```powershell
cd DigitalTwin
node .\test_server.js
node .\server.js
```

浏览器访问 `http://192.168.100.100:8080/`。

Keil 工程：`MDK-ARM/STM32H743_XC7A100_PS_Configuration.uvprojx`。
