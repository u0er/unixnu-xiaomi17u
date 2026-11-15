# Level 10：独立验证接收命令 RFR_OPEN

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
- 同时比较 RTS pad input bit0、GENI IOS、RX FIFO 和 FC 是否出现，不把 TX 完成当外部波形证据。
- 无 FC 时不发送版本请求；有 FC 才执行既有严格版本 probe，不下载固件。
- 若仍无 RX，下一独立实验是 BT_EN INPUT_OUTPUT/MODE2 配合 RT_STS 数字采样和完整恢复。
  当前没有执行该采样，输出配置读回不是其替代品。

新实机窗口必须明确同意；本轮只实现并准备候选，没有操作设备运行状态。
