# 蓝牙：Android 现有 UART trace 揭示的 ROM 路线（2026-11-15）

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

**上述下一 probe 尚未实现、构建或启动。** 已完成的 level 7 试验不授权新的启动，
也不证明这个下一实验已成功。需要的全部现有证据都在主机，后续可先离线完成解析器
与小范围实现，再提交完整候选供用户确认。
