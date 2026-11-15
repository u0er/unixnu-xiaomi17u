# Level 10 实机：RFR_OPEN 已生效，FD 仍无回应

2026-11-15，Asia/Shanghai。用户明确同意的单次临时启动已完成，设备窗口已释放。

## 结论

**RX_TRANS_CFG=0、RX命令0x08000080均读回成功，三次FD仍没有FC或其他接收字节。
版本请求未发送。单独增加RFR_OPEN在本次现有电源配置下未恢复唤醒，蓝牙尚未恢复。**

本次S8F请求仍为1800mV；[电源范围审计](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)
已确认它低于live DT蓝牙consumer的1876mV下限。因此本结果不能把电源或RFR与电源的
组合影响排除，也不能声称所有UART配置已正确。

## 实际操作与证据

完整内核构建43665623570。10:19:54发出临时boot，10:21:44观察到Android返回；
内核在90.214秒执行预设自动复位。启动前核对root/FW/log实际映射和pinned状态，
并保存当前log.img。没有额外复位、helper替换或固件容器写入。

证据目录：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level10/`。
`result/log.img`为立即取回的物理视图原始日志，`result/kernel.txt`为解码结果，
`result/bluetooth.txt`保留原始行号，`result/summary.json`为结构化结果。
`trial.json`、`verification.json`已更新实际启动和返回状态，`device-preflight.json`保存启动前检查。
`work/bluetooth-source-audit/level10-preparation.json`仍是准备期记录，不代表当前执行状态。

| kernel.txt行 | 实际记录 | 解释边界 |
|---|---|---|
| 152 | thread start level=10 | 本次分支实际执行 |
| 251、428 | LV/MV subtype0x12，MODE1，SOURCE从0到0x80 | BT_EN配置读回，仍非pad数字采样 |
| 486、487 | QUP4.3、16x、divider35；RX requested0/readback0 | 保留level9的串口配置修正 |
| 488 | RFR_OPEN requested0x8000080/readback0x8000080 | 新增bit7实际生效，未混入bit5/6过滤 |
| 494、495、497 | 三次FD TX完成 | AP sequencer完成，不等于外部对端收到 |
| 498 | wake timed out; version request not sent | 没有进入版本询问 |
| 491、501、508 | RX FIFO0，SW_CTRL1、CTS1、RX0 | 无RX，输入状态与level9一致 |
| 492、493、503 | IOS0x6，loopback0、IO_MACRO0x31c、manual_rfr0 | 其余诊断状态不变 |
| 504、510、511 | error60、UART stop0、最终SE status0，S_CMD0仍0x8000080 | ETIMEDOUT且停止成功，接收命令值保留 |
| 940、5973、8371 | 两次exec SpringBoard；90.214秒自动复位 | 记录系统执行与返回，不作界面稳定或视觉验收结论 |

日志未匹配到`panic(`、`Kernel data abort`、`Restart Type`。
本次源代码与补丁另存`work/bluetooth-source-audit/level10-device-source.c`、
`level10-device-0476.patch`，结果另存`level10-device-result.json`。
RTS GPIO_IO是在sequencer停止后的总报告中记录，不能把它当作接收期间的RTS波形证据。

## 与level9对照

| 项目 | level9 | level10 |
|---|---|---|
| RX_TRANS_CFG | 0 | 0 |
| S_CMD0 | 0x08000000 | 0x08000080，读回确认 |
| S8F请求 | 1800mV | 1800mV |
| FD完成次数 / RX字节 | 3 / 0 | 3 / 0 |
| FC / 版本请求 | 无 / 未发 | 无 / 未发 |
| GENI IOS、SW_CTRL/CTS/RX | 0x6、1/1/0 | 0x6、1/1/0 |
| UART停止 | 成功 | 成功 |

## 下一步

下一候选应以本次实测level10为基线，**仅将BT的S8F请求1800改为1876mV**，
保持RFR_OPEN、RX配置、电源时序与PERI协议不变，并保留原level10供对照。
1876来自live DT蓝牙consumer下限，且现有已验证Wi-Fi驱动使用相同资源和值。
须明确记录RPMh请求及返回值，不能把命令成功写成模拟电压已达标。

这次不再把历史“电源全部排除”作为依据；不同时添加BT_EN MODE2采样、UART break、
固件下载、baud变化或其他rail修改。若电源修正后仍无回应，再做具有保存/恢复流程的
BT_EN low/high数字采样。

新S8F候选尚未实现或构建；本次许可仅覆盖已经完成的level10单次启动，没有追加设备操作。
