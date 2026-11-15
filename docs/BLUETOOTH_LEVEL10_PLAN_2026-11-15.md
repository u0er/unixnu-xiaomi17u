# Level 10：独立验证接收命令 RFR_OPEN

> **已完成实机：[level10结果](BLUETOOTH_LEVEL10_RESULT_2026-11-15.md)。RX命令读回0x08000080，三次FD仍无回应；90.214秒自动返回。下文保留候选设计及准备记录。**

## 实验依据

[level 9 实机](BLUETOOTH_LEVEL9_RESULT_2026-11-15.md) 已确认 RX_TRANS_CFG=0，
但三次 FD 无接收，未发送版本请求。上一候选的 RX bit1 修正不足以恢复唤醒。

本机 Android 串口模块 `start_rx_sequencer` 的 0x8588..0x8594 将
0x080000e0 写入 SE_S_CMD0。同 SoC `msm_geni_serial.sm8850.c` 定义参数 bit7 为
RFR_OPEN，bit5/6 为跳过 framing/break 错误字符。对应原始反汇编保存在
`work/bluetooth-source-audit/start_rx_sequencer.disasm.txt`。

假设：缺少 RFR_OPEN 可能使对端受到 AP ready 信号限制；成功 TX 不能证明控制器已收到。
这仍是假设，level 9 的 manual_rfr=0 并不足以证明 RFR_OPEN 无影响。

## 最小修改

`pandora_bt=10` 复用 level 9：保留 RX_TRANS_CFG bit1 清零及读回检查，
只把 UART_START_READ 从 0x08000000 改为 0x08000080。写后打印 requested/readback；
不匹配返回 EIO，由外层现有路径停止 sequencer，不发送 FD。
错误字符过滤 bit5/6 保持为零，避免把三个参数同时改变。

电源、GPIO、115200/16x、FIFO packing、FD/FC 重试/期限、版本请求和清理均沿用 level 9。
不改 IO_MACRO、manual RFR、BT_EN 采样模式，不加 UART break、固件下载或 power-cycle。
level 8/9 的实验行为保留，实测源文件快照独立保存。

改动限定于蓝牙源文件、同步 0476 patch、host-only packer 的 level 上限及说明。
没有修改其他模块或调用接口。差异见 `work/bluetooth-source-audit/level10-from-level9.diff`。

## 验证状态与判据

ARM64 单文件编译、Clang static analyzer 均 exit0 且无诊断。
0476 新文件内容与源码一致，registration/caller hunks 未改变，git patch 解析通过。
临时编译对象和 analyzer 输出已删除，命令及诊断记录保留。
完整构建和镜像准备状态在后续更新；当前没有 level 10 实机结果。

- 命令读回必须为 0x08000080，RX 配置目标应仍为 0；不符则停止并报告错误。
- 比较 GENI IOS、RX FIFO 和 FC 是否出现，不把 TX 完成当外部波形证据。
  现有 bt_report 在 sequencer 停止后记录 RTS GPIO_IO，不能将该值当接收期间的 RTS 电平。
- 无 FC 时不发送版本请求；有 FC 才执行既有严格版本 probe，不下载固件。
- 若仍无 RX，下一独立实验是 BT_EN INPUT_OUTPUT/MODE2 配合 RT_STS 数字采样和完整恢复。
  当前没有执行该采样，输出配置读回不是其替代品。

新实机窗口必须明确同意；本轮只实现并准备候选，没有操作设备运行状态。

## 完整候选已就绪

完整构建43665623570已成功，运行于既有候选分支 `codex/bluetooth-no-analyzer-20261115`。
main未更新；`work/bluetooth-source-audit/level10-build-status.json`记录构建状态。

准备期间只读核对 level9 镜像：boot v4、payload/ADT、trust cache 和90秒参数有效；
其 exposed root16GiB、FW64MiB、log64MiB 与当前手机实际映射相符且均为 pinned。
详见 `level10-baseline-preflight.json`。这份准备期预检不替代真正启动前的重查。
上述预检时尚未生成level10镜像；后续已完成主机打包，仍未启动手机。

## 构建等待期间的独立电源审计

只读DT/源码复核发现S8F_E0的BT请求1800mV低于蓝牙consumer下限1876mV。
详见[电源范围复核](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)。这尚不是实测电压或根因。
该差异未混入level10；本候选仍仅更改RFR_OPEN，S8F请求仍为1800mV。

如果level10结果为阴性，其结论仅限现有电源条件下单加RFR_OPEN未恢复通信；
不能排除RFR_OPEN与正确电源条件的组合影响。S8F配置差异应作为下一项独立修正/验证，
不能再因历史交接声称电源排除而略过。

完整内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/rfr-open-artifact/kernel.development.pandora`，
17,738,024字节，level10诊断标记已核对。
临时镜像：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level10/loader/nezha-loader-boot.img`，
17,285,120字节。`level10/trial.json`和`verification.json`均记录未启动。

主机结构检查通过：boot v4、payload/ADT边界、root/FW/log映射、trust cache、内存基线
与已实测level9一致；启动参数只把`pandora_bt=9`改为`10`，保留自动返回90秒。
没有写固件容器或改变设备状态；下载归档、编译对象和analyzer输出已清理。

下一步需用户明确同意本次level10独占窗口：启动前重查映射/pinned并保存当前log.img，
临时boot此镜像，等90秒自动复位及Android返回后立即取日志。会覆盖设备log.img；
旧level2/7/8/9原始证据保留。此镜像不测试S8F电压修正或BT_EN采样。
