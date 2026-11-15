# 蓝牙：Android 现有 UART trace 揭示的 ROM 路线（2026-11-15）

> **最新实机：[level 8 结果与后续软件判据](BLUETOOTH_LEVEL8_RESULT_2026-11-15.md)。三次 FD 无 FC，未发送版本请求；90.221 秒自动返回。以下实现/就绪部分为试验前记录。**

## 当前结论

**无需逻辑分析仪的路线已获得两类实机证据：GENI 内部回环通过，Android 既有 UART trace
包含真实启动收发。后者显示 PERI 初始化发生在普通 HCI 运行前，旧交接中的
“HAMILTON 固件存在，因此本机只需普通 H4 HCI Reset / WCN7850 初始化”不能沿用。**

当前还没有让 UniXNU 的蓝牙控制器应答。不能把下面 Android 的成功交互当作 XNU 成功，
也不能仅据固件名字重新宣称芯片的精确 SKU。

## Level 7 回环实机：已通过

经用户对明确的试机请求回复“继续”，只执行一次 level 7 `fastboot boot`。
系统在内核 90.217 秒执行计时复位并自动回到 Android；未追加重启或写容器。

日志：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level7/result/kernel.txt`。
原始日志、BT 摘录和结果分别为同目录 `log.img`、`bluetooth.txt`、`summary.json`。

| 原始日志行 | 结果 | 可支持的结论 |
|---|---|---|
| 191 | loopback config=3，discarded=0 | 测试前没有沿用 FIFO 旧字节 |
| 194、481 | 发 8 字节，收 `55 aa 00 ff 01 80 7e 81` | 内部收发、FIFO packing 和两条 sequencer 数据通道工作 |
| 486 | PASS，error=0，received=8/8 | 严格长度与逐字节匹配成立 |
| 492 | 最终 SE status=0 | 清理后收发 sequencer 已停止 |
| 17023 | 90.217 秒自动返回 | 有界启动已验证 |

测试没有驱动 BT_EN、没有投 BT rail、没有配置外部 UART pinmux。
因此结果不证明外部 TX/RX 连线、电压、绝对波特率或控制器 readiness。
level 2 的“外部 RX 低，无 HCI”现象仍成立；它与内部回环 PASS 不冲突。

## 不新增 trace 配置，直接读取 Android 已有数据

返回 Android 后发现 `/sys/kernel/tracing/instances/hsuart/trace` 已存在并有内容。
本轮只读 `trace`，没有读取会消费数据的 `trace_pipe`，没有挂载 debugfs、清空 trace、
添加 probe、开关蓝牙或再次重启。串口为同一个 `1994000.qcom,qup_uart`。

原始快照保存于工作区 `work/bluetooth-source-audit/android-hsuart-trace.txt`。
该日志的 CPU ring 有起始覆盖，且 vendor 字节 dump 常常只记前缀，不能声称拥有完整
启动字节流或完整固件内容。

已新增离线工具 `tools/bluetooth/uart_trace.py`，在这份真实 trace 上提取到 731 个 UART
chunk，其中 280 个被日志截断。它保留时间、原始行号、reported/captured 长度；不把
DMA chunk 自动当协议帧，也不补造被截断字节。

重现：

```powershell
python outputs/data-a-live-nezha/tools/bluetooth/uart_trace.py `
  work/bluetooth-source-audit/android-hsuart-trace.txt `
  --out work/bluetooth-source-audit/android-uart-chunks.json
```

## Android 实际启动序列

下表行号指 **Android 原始 trace**，不是 XNU kernel.txt。时间为 Android uptime。

| 时间 / 行 | 实际观测 | 含义与限制 |
|---|---|---|
| 9.763247 / 177 | 设置 115200 | 首轮通信速率；之前 9600 是串口打开过程，不是待猜的 ROM 探测速率 |
| 9.763625 / 199 | TX `fd` | 有实际的 in-band wake，不是“ROM 阶段等效禁用 IBS” |
| 9.765449 / 215 | RX `fc` | wake acknowledgement；这两条都是完整单字节 chunk |
| 9.766328 / 223 | TX `31 00 f0 ff 02 05 00` | 完整 7 字节 PERI version request |
| 9.770886 / 239 | reported RX=25，prefix=`34 00 ff 15 f0 01 01 f0 ff 00 05 00 1e 00 00 00 58 6d 00 02 00` | 已实际有响应；日志只记到 21 字节，末 4 字节未知 |
| 9.773776 / 253 | TX `31 00 f1 ff 02 02 0f` | 后续速率请求；不要把这个改变控制器状态的命令混进首个只问版本的 probe |
| 9.800225 / 308 | AP 改为 4 Mbps | 是握手后的速率切换，不是冷启动直接使用 4 Mbps |
| 9.801908 / 336 | TX `31 00 f0 ff 03 08 00 00` | PERI 配置过程继续 |
| 9.810119 / 358 | TX prefix=`31 00 f0 ff f6 06 00 f3 7f 45 4c 46 ...` | 开始 ELF 内容下载；只记录前缀，不重建下载文件 |
| 9.927708 / 1528 | RX reported=115，prefix=`34 00 ff 6f f0 01 01 f0 ff 00 09 00 66 42 54 46 57 2e 47 41 4e` | 可见 build 字符串前缀 `BTFW.GAN`，不是 `BTFW.HAM` |
| 9.930829 / 1552 | TX `31 00 f0 ff 02 05 03` | 同类 version request 的子系统字节变为 3；与先前 0 不可混用 |

后来 trace 有普通 `01` 命令、`04` 事件。它们是初始化后的运行阶段，不能倒推冷启动
就能直接接受普通 HCI Reset。没有把 DMA chunk 的分包边界当 HCI frame 边界。

Android dmesg 的 `android-bt-startup.txt` 同时显示 BT_EN low/high 与 SW_CTRL=1，
与 level 2 纠正后的配置方向一致；这不是 XNU GPIO 的模拟电压测量。

## 本机 HAL 二进制给出的独立交叉证据

只读复制本机实际映射的 `/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so`
到审计目录。同目录包含定点反汇编，未把其他系统整个 HAL 搬进内核。

- `PeriPatchDLManager::PeriSocInit()` 调用 `WakeUpSoC()`，随后进入 activation / version /
  baud / configure / patch 的流程。确切调用顺序可在相应 `.disasm.txt` 审计。
- `WakeUpSoC()` 的 0x12a4d0 构造十进制 253，即 `0xfd`；之后调用其读取事件方法。
- `PeriPatchVerReq()` 的 0x12ac9c..0x12ad3c 构造 `31 00 f0 ff 02 05 <subsystem>`，
  并以长度 7 发送。默认子系统为 0，另一条分支为 3。这和 Android trace 中完整请求一致。
- `FramePeriHciPkt()` 的 0x130090..0x1300b4 在 offset 0 写 16-bit 值 0x0031，
  offset 2 写 16-bit opcode，offset 4 写 payload length，offset 5 写子命令，
  offset 6 写 subsystem。不能把 `31 00` 擅自解释成普通 H4 的 type + SSID。
- HAL 有明确的 `PeriPatchDLManager`、`FramePeriHciPkt`、`gngperifw20.*`、
  `gngbtfw20.*` 和 Nezha 的 UWB/PERI NVM 分支。

这些是已检查过的本机二进制事实。完整 event parser 的长度、状态、子系统、版本字段
布局还须沿 `ReadHciEvent` / `ReadHciPeriEvent` 审计，不靠响应前缀猜完结构体。

## 固件基线更正

`/vendor/bt_firmware/image` 同时有 Hamilton、Ganges、其他家族文件。
旧 agent 仅找到 `hmtbtfw20.tlv` 和 `hmtbtfw20.ver`，不能据此认定 Android 选择它们。

本机 `gngbtfw20.ver` 内容为 `BTFW.GANGES.2.0.1-00363-PATCHZ-1.164416.13_DEF`。
当前目录还有 Ganges 的 PERI / BT / UWB、TLV / MBN / NVM 文件。
这与实际 PERI 下载序列和 `BTFW.GAN` 响应共同支持当前走 Ganges 多子系统路线，
但 trace 截断的完整 build 后缀与准确芯片 SKU 仍不下结论。

下一步只做 ROM 询问，不下载任何固件。真正需要下载时，须依据完整版本响应和 HAL
选择规则确定 PERI、BT、校准/NVM 的文件与变体；不能盲用早先提取的 Hamilton 文件。

## 下一个最小、可判别的软件实验

先实现独立的 **wake + PERI version probe**，不做固件、NVM、UWB、4 Mbps 切换：

1. 沿已实测的 GPIO5 LV/MV 上电与 QUP4.3/16x UART 设置；增加外部 UART
   `LOOPBACK`、`IO_MACRO_CTRL`、manual RFR 的只读快照，区分内部通过与外部路由。
2. 在 115200 请求速率发送单字节 FD，有限等待 FC。若对端发 FD，按已审计的握手
   行为处理；未拿到匹配 ACK 时停止本轮，不把其他数据当成功。
3. ACK 后仅发送从本机 trace 和 HAL 双重确认的 7 字节版本请求
   `31 00 f0 ff 02 05 00`。按新帧格式和边界接收，保存原始字节。
4. 只有完整、匹配 opcode/subcommand/subsystem 且状态成功的响应才能记为
   “PERI ROM version probe 成功”；不能记成蓝牙栈恢复。

若 FD 仍完全无回应，继续用 AP 数字观测和 Android 对照检查外部线路/电源/唤醒，
不是回头重复无条件 HCI Reset。若 FD/FC 通但版本帧失败，则边界已缩小到新协议。

**上述 probe 已实现为 level 8，单文件 ARM64 编译通过，完整构建和实机结果另行记录。**
已完成的 level 7 试验不授权新的启动，也不证明 level 8 已成功。

## Level 8 实现与失败边界

`pandora_bt=8` 复用 level 2 已实机验证的 rail / BT_EN / UART 设置，设置外部传输，
先打印 LOOPBACK、IO_MACRO_CTRL、manual RFR，再进入独立协议分支。
从该分支直接退出，不会落入普通 HCI Reset 或 level 6 的 power-cycle 重试循环。

- `bt_peri_wake` 最多三次 FD，每次等 ACK 200 ms。FC 才是本机请求的成功确认。
  收到对端 FD 时回复 FC，FE 记为 sleep indication；其他字节报协议错误。
  总控制字节数也有上限，不用对端流量无限延长等待。
- `bt_rx_exact` 在同一个 absolute deadline 内逐字段读取，恰好读取需要的字节，
  不靠 DMA chunk 边界，也不读走下一字段。版本帧整体期限 1 秒。
- 只在帧边界识别 FD/FC/FE；帧 payload 内的同值字节作为普通数据。
- 首个版本响应仅接受本机 HAL/trace 对应的 25 字节形式：header=`34 00 ff 15`，
  payload 起始 `f0 01 01 f0 ff`，offset 9 为状态，offset 10 为子命令 05，
  offset 11 为 subsystem 0。完整长度和匹配字段都成立、status=0 才记 probe PASS。
  版本号内容暂时只以原始字节记录，不把未审计字段命名成芯片 SKU。
- 使用 32 字节栈缓冲。header 声明长度超过容纳范围时返回 EMSGSIZE；短帧、
  未知 event 或字段不匹配返回错误；不会把截断的 Android dump 补成“成功样本”。
- 完成后停止 UART sequencer，打印停止结果；不降共享 rail、不写固件/NVM，
  也不切到 Android 后续的 4 Mbps 阶段。

进一步交叉检查过本机 HAL 的 `ReadHciPeriEvent`：0x129680..0x129724 识别 FC/FD/FE
并对 FD 写 FC；0x129924..0x129988 读取 extended event 的两字节尾头和声明长度。
`GetVsHciEvent` 从 raw offset 10 分派子命令，`ProcessPeriEdlPatchEvt` 以原始 buffer
读取版本信息，支持当前 probe 对帧结构的限定。没有复用 HAL 中把超时最终返回 0
等宽松结果路径，probe 必须有实际匹配响应。

改动文件为 `wsl/pandora_bt.c`、同步生成的 0476 patch、prepare_trial 的 level 范围
和蓝牙说明。原 level 2/7 设备证据保留。完整候选准备好后，再向用户报告启动目标、
90 秒期限和日志覆盖范围，取得这一次的明确同意。

## Level 8 完整候选已就绪

- 完整 DEVELOPMENT/PANDORA 构建 **43656524051 成功**；产物内的 wake ACK / PERI
  请求和响应诊断标记已核对。ARM64 单文件编译、Clang static analyzer 均无诊断。
- 内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/peri-artifact/kernel.development.pandora`。
- 镜像：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level8/loader/nezha-loader-boot.img`，
  17,285,120 字节。`pandora_bt=8`，`pandora_reboot_after=90`。
- 继承 level 7 已实机使用的 root/FW/log 映射、memory bases、trust cache 和其他启动参数。
  主机结构检查通过；level 7 → 8 之外无其他 boot-arg 变化。
- `level8/trial.json` / `verification.json` 和审计目录 `level8-preparation.json` 保留
  准备记录。没有再写容器，没有试机，也没有把已有 level 7 PASS 当成 PERI 结果。
- 下一步仅在明确同意的新独占窗口临时 boot 此镜像。iOS 阶段约 90 秒后自动返回，
  立即只读取回日志；会覆盖设备 `log.img`，既有 level 2/7 原始证据已分别保存。
