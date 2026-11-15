# Level 9 实机：清除 RX bit1 后仍无唤醒回应

2026-11-15，Asia/Shanghai。用户明确同意的单次临时启动已完成，设备窗口已释放。

## 结论

**RX_TRANS_CFG 的 bit1 成功清零并读回 0，但三次 FD 仍没有 FC 或其他接收字节。
版本请求未发送；单独清除此位不足以恢复本次冷启动的 PERI 唤醒。蓝牙尚未恢复。**

level 9 完整构建编号为 43663459546。09:52:05 发出临时 boot，09:53:56 观察到
Android 自动返回；内核在 90.221 秒执行预设复位。启动前重新核对 root/FW/log 的
实际映射与 pinned 状态，并保存原 log.img。没有额外复位、helper 替换或固件容器写入。

## 实际证据

目录：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level9/`。
`result/log.img` 是立即取回的物理视图原始日志，`result/kernel.txt` 是解码结果，
`result/bluetooth.txt` 保留原始行号，`result/summary.json` 保存结构化结果。
`trial.json`、`verification.json`、`device-preflight.json` 记录本次实际操作。
主机准备记录 `work/bluetooth-source-audit/level9-preparation.json` 仍是试验前状态，
不能用它代替已更新的设备 trial/verification。

| kernel.txt 行 | 实际记录 | 含义 |
|---|---|---|
| 152 | thread start level=9 | 本次分支实际运行 |
| 253、428 | GPIO5 subtype=0x12，MODE=1，SOURCE 从 0 到 0x80 | 配置读回符合 LV/MV 输出；不是 pad 数字采样 |
| 486 | QUP4.3、16x、divider35 | 沿用 level 8 的串口速率 |
| 487、488、498 | requested0/readback0，rx_cfg=0 | 目标位实际清除，探测后仍为 0 |
| 493、494、496 | 三次 FD TX 完成 | AP sequencer 完成，尚不证明对端收到 |
| 497 | wake timed out; version request not sent | 没有进入版本询问 |
| 490、500、507 | RX FIFO=0，SW_CTRL=1、CTS=1、RX=0 | 输入状态仍与 level 8 一致 |
| 491、492、502 | IOS=0x6、loopback=0、IO_MACRO=0x31c、manual_rfr=0 | 路由诊断值未改变 |
| 503、509、510 | error60、UART stop0、最终 SE status0、S_CMD0=0x08000000 | ETIMEDOUT，收尾停止成功；仍未使用 RFR_OPEN |
| 939、5722 | exec SpringBoard；90.221秒自动复位 | 系统执行和返回有日志证据，未作视觉验收 |

日志未匹配到 `panic(`、`Kernel data abort` 或 `Restart Type`。
本次对应源代码和补丁另存 `work/bluetooth-source-audit/level9-device-source.c`、
`level9-device-0476.patch`，结果另存 `level9-device-result.json`。

## 与 level 8 的对照

| 项目 | level 8 | level 9 |
|---|---|---|
| RX_TRANS_CFG | 2 | 0，写后读回确认 |
| RX S_CMD0 | 0x08000000 | 0x08000000 |
| FD 完成次数 / RX 字节 | 3 / 0 | 3 / 0 |
| FC / 版本请求 | 无 / 未发 | 无 / 未发 |
| SW_CTRL / CTS / RX | 1 / 1 / 0 | 1 / 1 / 0 |
| 自动复位时刻 | 90.221 秒 | 90.221 秒 |

这排除了“只清 RX bit1 就能恢复当前冷启动唤醒”的假设；没有证明该位在所有模式下无影响，
也没有证明整个 UART、电源或外部线路已正确。

## 下一步软件判据

下一项可独立验证 **RX 启动的 RFR_OPEN bit7**：保持本次 RX_TRANS_CFG=0、电源和
PERI 协议不变，仅让 UART_START_READ 带 bit7（从 0x08000000 到 0x08000080），
并打印/核对启动命令和输入。它来自已保存的同 SoC 源码，以及本机
`start_rx_sequencer` 0x8588..0x8594 的 0x080000e0 写入。
本机同时设置的 bit5/6 是丢弃 framing/break 错误字符，需与 bit7 区分；
不要把三位一起改动后宣称验证了单独的 RFR_OPEN。

若仍无回应，另一项独立软件实验是 GPIO5 临时 INPUT_OUTPUT（LV/MV MODE=2）
配合 RT_STS 对 BT_EN low/high 做数字采样，并恢复全部改动。
不得将当前输出配置读回当作已完成这项采样。

新候选尚未实现或构建，本次许可仅覆盖已完成的 level 9 单次启动。
没有追加启动、RFR 改写、UART break、盲选固件或 power-cycle。
