# Level 11实机：S8F1876请求成功，FD仍无回应

2026-11-15，Asia/Shanghai。用户明确同意的单次临时启动已完成，设备窗口已释放。

## 结果

**S8F_E0的1876mV电压请求和使能请求都返回0；RX配置0、RFR_OPEN命令0x08000080
读回成功，但三次FD仍无FC或其他接收字节，版本请求未发送。蓝牙尚未恢复。**

这说明仅把请求值对齐到live DT蓝牙consumer下限，没有恢复本次冷启动通信。
RPMh请求成功不是实际电压测量，不能据此排除实际供电、BT_EN或外部UART信号问题。

## 操作与原始证据

完整内核构建43668443548。10:56:38发出临时boot，10:58:30观察到Android返回；
内核在90.218秒执行预设自动复位。启动前重查root/FW/log映射和pinned状态，并保存旧日志。
没有额外复位、helper替换或固件容器写入。

证据目录：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level11/`。
`result/log.img`是立即取回的物理视图原始日志，`result/kernel.txt`是解码结果，
`result/bluetooth.txt`保留原始行号，`result/summary.json`是结构化结果。
`trial.json`、`verification.json`已更新执行状态，`device-preflight.json`保存启动前检查。
`work/bluetooth-source-audit/level11-preparation.json`仍是历史准备记录。

| kernel.txt行 | 记录 | 结论边界 |
|---|---|---|
| 152 | thread start level=11 | 本次分支实际运行 |
| 215、227 | S8F voltage request1876mV->0，enable request->0 | 请求成功，不代表模拟电压已测量 |
| 263、430 | GPIO5 subtype0x12，MODE1，SOURCE0->0x80 | BT_EN输出配置读回，尚非pad采样 |
| 488..490 | QUP4.3/16x/div35，RX配置0，RFR_OPEN0x8000080读回成功 | 沿用level10串口基线 |
| 496、497、499 | FD TX完成三次 | AP sequencer完成，不证明对端收到 |
| 500 | wake timed out; version request not sent | 未进入版本询问 |
| 493、503、510 | RX FIFO0，SW_CTRL1、CTS1、RX0 | 输入状态仍与level10一致 |
| 494、495、505 | IOS0x6、loopback0、IO_MACRO0x31c、manual_rfr0 | 路由诊断状态不变 |
| 506、512、513 | error60、UART stop0、最终SE status0 | ETIMEDOUT，sequencer停止成功 |
| 942、15321 | exec SpringBoard，90.218秒自动复位 | 有系统执行及返回证据，未作视觉验收 |

日志未匹配到`panic(`、`Kernel data abort`、`Restart Type`。
本次实测源文件和补丁另存`work/bluetooth-source-audit/level11-device-source.c`、
`level11-device-0476.patch`，结果另存`level11-device-result.json`。

## 对照与解释

| 项目 | level10 | level11 |
|---|---|---|
| S8F请求 | 1800mV | 1876mV，电压/使能返回均0 |
| RX配置 / S_CMD0 | 0 / 0x08000080 | 0 / 0x08000080 |
| FD完成 / RX字节 | 3 / 0 | 3 / 0 |
| FC / 版本请求 | 无 / 未发 | 无 / 未发 |
| SW_CTRL/CTS/RX、IOS | 1/1/0、0x6 | 1/1/0、0x6 |

修正请求值仍有配置依据，但不是已经定位的根因或功能修复。
[电源审计](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)里的Android保持态sysfs读数也不能
代替冷启动电压或pad测量。不要把这次阴性结果表述成“所有电源问题已排除”。

## 下一步：BT_EN数字电平，而非再盲改参数

GPIO5已确认LV/MV subtype0x12。下一项无需逻辑分析仪的有判据观测是：在已有low/high
阶段临时采用正常INPUT_OUTPUT（MODE2），读取RT_STS bit0，验证分别为0/1。
保持SOURCE锁存、VIN、驱动等不变，不使用MODE3（那是analog pass-through）。

新实现必须保存实际MODE/DIG_IN，核对类型与当前输出模式，再按需设置正常输入路由和MODE2；
无论读写/采样是否失败，都尝试恢复已修改配置并核对恢复结果。未知类型、配置/采样不匹配、
恢复失败均应停止后续协议并明确报告。纯输出MODE1时RT_STS=0没有所需采样意义。
BT_EN纯数字采样也不等于模拟电气裕量或证明控制器端电压。

该采样候选尚未实现、构建或获准启动。本次授权只覆盖已完成的level11单次启动，
没有追加MODE2写入、UART break、固件、baud变化或额外power-cycle。
