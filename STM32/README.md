# STM32H743 FMC16 M0 数字孪生

本工程从 Lattice 的 FMC Bank1/NE1 接口读取实验 FPGA 的真实 `PO[31:0]`、`PIO[15:0]` 和实体按键状态，完成 FMC16 长度、版本、事务号、CRC16/CRC32 及有效位校验后，才通过 UDP 上报 `source=fmc16` 快照。读取失败时不会上传模拟数据。

## 平台启动约束

STM32 在释放 Xilinx `PROGRAM_B` 之前，先通过 MCP23017 将 `PI[15:0]` 驱动为确定的空闲值 0。这样 Xilinx 可以运行任意用户实验，无需在用户 HDL 中加入平台专用启动延时、按键基线学习或复位补丁。配置完成后，STM32 隔离共享桥接支路，再开始 FMC/LCD/UDP 运行。

## 网络与报文

- STM32：`192.168.100.50/24`
- 服务器：`192.168.100.100`
- UDP：`5005`
- 周期：`100 ms`

上报字段包括 `sequence`、`timestamp_ms`、`mode`、`po`、`pio`、`pi_applied`、`pi_requested`、`key_state` 和 `key_event_count`。数值均为无符号 JSON number。F10 实体按键在 M0 计数器与 M11 点阵屏之间切换；网页模式按钮通过 FMC16 `SET_MODE(0x0004)` 直接选择目标模式。M0 保留翻转事件 PI 编码，M11 使用按住/松开电平并保持实体与网页 XOR 仲裁，因此不需要修改原点阵实验的上升沿检测逻辑。

## 环境监测

PB7/PB8 软件 I2C 总线由 `Bsp/bsp_i2c_ui.c` 统一管理并通过 FreeRTOS
互斥锁串行访问。低优先级 `BoardMonitorTask` 在启动 8 秒后每秒采集：

- SHT40（`0x44`）：温度和相对湿度；
- INA226 U10（`0x41`、100mΩ）：过桥板 3.3V 支路电流；
- INA226 U44（`0x40`、20mΩ）：`VCC5V -> VCC5V_CORE` 核心板 5V 支路电流。

原有 FMC 遥测保持 100ms 周期，环境数据通过同一 UDP socket 每秒发送
`type: "module.data"`。Keil 可查看全局 `board_monitor_dbg`，其中包含原始值、
有效状态、失败连续计数及各器件成功/失败次数。

## PCAL6524 与声光报警

`BoardPanelTask` 驱动地址 `0x22` 的 PCAL6524。B1～B9、B11～B14 共 13 路
开关经 P0/P1 输出到 FPGA 多路复用器，B10 因硬件未连接固定不可用；P1.5
保持高电平，不破坏 Xilinx Master-SPI 启动模式。P2.0～P2.4 分别驱动
YDS1～YDS5（低电平点亮）。两档实验模式会自动接管拨码路由：M0 全部断开，
M11 先断开全部支路，再选择 BSW-A/B3（物理拨码 B1、B2、B5、B6、B8、B9 为 ON，B3、B4、B7、B10 为 OFF，对应位图 0x01B3）。
控制采用带命令号、CRC16、确认与重试的 `M0PC` 帧。

声光报警取 U10/U44 电流等级与 SHT40 温度等级的最大值。YDS1 显示电流
告警，YDS2 表示两路电流正常，YDS3 表示温度正常，YDS4 显示温度告警，
YDS5 表示最近 5 秒有 USB CDC 活动。蜂鸣器由 U10 INA226 ALERT 输出驱动，
按三级告警采用不同鸣响节奏，网页支持静音/恢复。湿度只上报，不参与报警。
Keil 可查看全局 `board_panel_dbg` 诊断初始化、写入、控制命令和当前输出状态。

模式切换负责平台外设路由、按键映射与遥测口径，不替换 Xilinx 内部
bitstream。实验管理端选择 M0 或 M11 时，应先烧写对应学生实验镜像；学生
原始 RTL 无需加入任何平台专用模式逻辑。

## 数字孪生接收展示

零依赖 Node.js 接收端和网页位于 `DigitalTwin`。它提供 UDP 接收、校验与去重、设备重启识别、在线/延迟/离线状态、REST 快照、WebSocket 推送，以及 8 位十六进制数码管、12 个 LED、F1～F10 双向按键和环境监测状态。

```powershell
cd DigitalTwin
node .\test_server.js
node .\server.js
```

浏览器访问 `http://192.168.100.100:8080/`。

Keil 工程：`MDK-ARM/STM32H743_XC7A100_PS_Configuration.uvprojx`。
