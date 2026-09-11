# DigitalTwin

基于 STM32H743、Lattice MachXO2 和 FMC16 的远程 FPGA 数字孪生平台，实现 Xilinx 实验 PO/PIO 实时采集、F1～F10 实体与网页按键同步控制、LCD 状态显示及 UDP/Web 数据交互。

## 目录结构

- `STM32/`：STM32H743 固件、FMC16 通信、LCD、以太网和按键控制代码。
- `Lattice/`：MachXO2 FMC16 协议、PO/PIO 采集及实体按键转发工程。
- `DigitalTwin/`：Node.js 后端、浏览器数字孪生界面、测试和协作文档。

## 默认网络配置

- STM32：`192.168.100.50`
- 服务器：`192.168.100.100`
- UDP 遥测与控制端口：`5005`
- Web 服务端口：`8080`

Xilinx FPGA 用于承载用户实验，本仓库的平台代码只负责采集和转发实验 I/O，不要求用户为数字孪生功能修改原有实验设计。
