# M0 数字孪生接收展示模块

此模块接收 STM32 的真实 `source=fmc16` UDP 快照，并可将网页 KEY3～KEY7 操作作为可靠、可确认的 UDP 控制命令下发给 STM32。它不生成模拟计数值，也不修改用户的 Xilinx 实验逻辑。

运行：

```powershell
node .\server.js
```

默认监听 UDP `5005` 和 HTTP `8080`，只接受源地址 `192.168.100.50`。浏览器打开 `http://服务器地址:8080/`。需要调整时可设置 `UDP_PORT`、`HTTP_PORT`、`STM32_SOURCE_IP` 和 `DEVICE_ID` 环境变量。

前后端接口、控制帧、STM32 合并语义及联调方法见 [WEB_KEY_CONTROL_DEVELOPMENT_GUIDE.md](WEB_KEY_CONTROL_DEVELOPMENT_GUIDE.md)。

测试：

```powershell
node .\test_server.js
node .\test_control_e2e.js
```
