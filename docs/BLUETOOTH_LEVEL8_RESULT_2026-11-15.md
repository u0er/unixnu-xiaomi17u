# Level 8：PERI 唤醒实机结果与后续软件判据

2026-11-15，Asia/Shanghai。用户同意后的单次临时启动已完成。

## 实机结果

**三次 FD TX 完成，但没有收到 FC 或其他 RX；版本请求未发送。蓝牙尚未恢复。**

完整构建 43656524051 的镜像于 09:13:55 发出 boot 命令，09:15:49 观察到 Android 返回。
内核在 90.221 秒按预设自动复位。没有额外复位、固件容器写入或 helper 替换。
启动前已保存旧日志，启动前 root/FW/log 的 pinned 状态和镜像暴露映射均核对通过。

证据目录：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level8/`。
`result/log.img` 为物理视图原始日志，`result/kernel.txt` 为解码结果，
`result/bluetooth.txt` 保留原始行号，`result/summary.json` 为结构化结果。
`trial.json`、`verification.json`、`device-preflight.json` 记录实际启动和映射检查。

| kernel.txt 行 | 记录 | 结论边界 |
|---|---|---|
| 152 | thread start level=8 | 本次分支实际运行 |
| 253、427 | GPIO5 TYPE=0x10、SUBTYPE=0x12；MODE=1；SOURCE 从 0 变为 0x80 | LV/MV 配置和输出锁存读回成立；尚非 pad 电平测量 |
| 486 | QUP=0x40030000，16x，div35，计算 114285 baud | 沿用修正后的采样倍数 |
| 491、501 | loopback=0，io_macro=0x31c，manual_rfr=0 | 外部模式，无 manual not-ready；Android IO_MACRO 运行值仍未知 |
| 492、493、495 | 三次 FD TX 完成 | AP sequencer 完成不等于对端收到 |
| 496 | wake timed out; version request not sent | 失败在版本帧解析之前 |
| 499、500、506 | RX FIFO=0；GENI IOS=0x6；SW_CTRL=1、CTS=1、RX=0 | 输入状态与 Android 握手前不同 |
| 502、508 | error=60，UART stop=0；最终 SE status=0 | ETIMEDOUT，sequencer 已停止 |
| 939、5698 | exec SpringBoard；90.221 秒自动复位 | 系统执行和有界返回有日志证据；未作视觉验收 |

日志未匹配到 `panic(`、`Kernel data abort`、`Restart Type`。
level 7 内部回环 PASS 仍成立，但它不覆盖外部 TX/RX。

## Android 对照和剩余软件边界

返回后只读保存既有 trace 和 DT，没有更改 trace 配置、挂载 debugfs、
操作 `/dev/ttyHS0` 或开关蓝牙。`work/bluetooth-source-audit/` 新增证据：

- `android-level8-hsuart-trace.txt`：第二份 trace，CPU ring 有覆盖，不能代替第一份 FD/FC 前缀。
- `android-uart-pinctrl-refs.json`、`android-uart-pinctrl-states.json`：live DT 状态引用和 UART 引脚定义。
- `android-level8-readonly-inventory.json`：查询命令、退出码和输出。
- `level8-device-source.c`、`level8-device-0476.patch`、`level8-device-result.json`：实测版本快照和结果。

第一份 `android-hsuart-trace.txt` 第 195 行（9.763551 秒）在 FD 发送前记录
GENI IOS=0x1；199 行发送 FD，215 行收到 FC，173 行 RX_TRANS_CFG=0。
本次 XNU 为 IOS=0x6、RX_TRANS_CFG=2。两者有差异，但尚不能直接认定哪一项是根因。

live DT active 仍为 GPIO24/25/26/27 对应 qup3_se5_l0/l1/l2/l3，drive=2mA，
分别 no-pull、pull-down、pull-up、no-pull；RX 在 sleep 变为 GPIO。
UART 节点未见 RX/TX 或 CTS/RTS swap 属性。这确认绑定，不确认 Android 当前寄存器值。

同 SoC 源码另存 `msm_geni_serial.sm8850.c`，来源 OnePlusOSS
`android_kernel_oneplus_sm8850`，分支 `oneplus/sm8850_b_16.0.0_oneplus_15`，
路径 `drivers/tty/serial/msm_geni_serial.c`。RX_TRANS_CFG 仅定义 bit4 parity 和 bit2 status，
未将 bit1 定义为 CTS mask。XNU 对 RX 无依据地 OR bit1 需要纠正并单独验证。
该上游 RX 启动带 RFR_OPEN bit7，本次 XNU S_CMD0 参数为零。
已进一步交叉核对本机模块：`msm_geni_serial_config_baud_rate` 的
0x9774..0x97ac 读取 RX_TRANS_CFG，非 parity 分支只清 bit4；0x98c0..0x98cc 写回，
没有对 bit1 OR。`start_rx_sequencer` 的 0x8588..0x8594 将 0x080000e0 写到 SE_S_CMD0
（UART_START_READ 加 bit5/6/7，其中 bit7 为 RFR_OPEN）。对应反汇编已经保存。
`msm_geni_serial_termios_cfg` 被编译合并，不是本机模块里的独立符号；不要对空反汇编作结论。
这些是可审计的软件差异，新配置尚未试机。

## 下一项有判据的实验

下一候选尚未实现、构建或获准启动。保留 level 8 对照，分开验证：

1. **BT_EN 数字 pad 采样**：GPIO5 已确认 LV/MV subtype=0x12。上游
   DIGITAL_INPUT_OUTPUT 是 MODE=2，MODE=3 是 analog pass-through，不能混用。
   在既定 low/high 阶段保存 MODE/DIG_IN，临时启用正常输入和输出，读取 RT_STS bit0，
   恢复保存值并核对。任何 SPMI/恢复错误须报告并停止后续协议。
   low/high 应分别采样 0/1；失败先查 PMIC pad/VIN，而不是声称 BT_EN 已验证。
   该数字结果不代表控制器接收端的模拟电气裕量。
2. **UART 参数对齐**：本机 `msm_geni_serial_config_baud_rate` 和
   `start_rx_sequencer` 已核对 RX_TRANS_CFG、RFR_OPEN，后续分别做最小变化。
   FD/FC 出现后才进入已实现的版本询问；无 FC 仍停止、不下载固件。
3. **外部 TX 输出观测**：独立 pad 诊断保持 UART mux，依据同 SoC
   UART_START_BREAK/UART_STOP_BREAK 定义，采样 GPIO26 input bit0 是否随输出低/恢复高变化。
   命令必须有界，确保 STOP/abort 清理；不要混入版本 probe。

暂不猜 IO_MACRO 值、不强推 GPIO6/9、不重复无条件 HCI Reset、盲换波特率或固件家族。
无需逻辑分析仪仍能缩小边界；本轮没有证明外围故障或 PERI 已通。

设备窗口已释放。后续一次 Android 只读查询返回 device not found，未再切换设备状态。
