# Level12实机：BT_EN低/高数字采样通过，串口仍无回应

用户同意的单次临时启动完成。本地增量内核实际执行，90.217秒自动复位并返回Android，
原始和解码日志已立即保存，设备窗口已释放。没有追加启动或固件容器写入。

进入bootloader后曾短时未枚举fastboot，因此首次等待超时，没有发送镜像。
用户确认FASTBOOT/USB连接后重新读到设备，仅发送了一次`fastboot boot`，没有重复重启。
PnP重扫描因权限不足未执行成功；没有安装或更换Windows驱动。

| `level12/result/kernel.txt`行 | 实际结果 |
|---|---|
| 153 | thread level12执行 |
| 429 | low预期0，RT_STS三次0，sample error0、restore error0 |
| 486 | high预期1，RT_STS三次1，sample error0、restore error0 |
| 499、500、502、503 | 三次FD TX完成，无RX，未发版本请求 |
| 509、515 | probe error60、UART stop0，最终SE status0 |
| 951、4904、7221 | SpringBoard执行记录，90.217秒自动复位 |

原始目录`D:/turnip-local/newboot/bt-no-analyzer-20261115/level12/`，
包含`preboot/log.img`、`result/{log.img,kernel.txt,bluetooth.txt,summary.json}`及实际更新的
`trial.json`、`verification.json`、`device-preflight.json`。
本次源文件和补丁保存在`work/bluetooth-source-audit/level12-device-source.c`与
`level12-device-0476.patch`，结构化结果为`level12-device-result.json`。

BT_EN低/高数字状态与预期一致，恢复已核对。它只证明PMIC pad数字采样，不代表
控制器端模拟电压或完整电源就绪。无panic/Kernel data abort/Restart Type匹配，未作视觉验收。

**重要：本次日志仍沿用旧时钟代码。[SM8850时钟审计](BLUETOOTH_SM8850_CLOCK_AUDIT_2026-11-15.md)
已发现其M/N/D编码、mode和频率解码不符合原厂clk-rcg2，且没有检查DFS。**
日志的“64MHz/actual114285”不是已验证的物理频率，不能用于排除时钟或外部波特率问题。

下一项是按本SoC原厂RCG原理实现/验证时钟路径，不再重复已通过的BT_EN采样或盲换协议。
