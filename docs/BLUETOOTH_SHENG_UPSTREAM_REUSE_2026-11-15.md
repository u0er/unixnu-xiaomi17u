# UniXNU小米Pad6S Pro蓝牙实现与Nezha复用审计

2026-11-15线上源码实读。按用户要求只研究骁龙/UniXNU路线，不使用Switch/Tegra实现。

## 结论

**上游已有完整组件，应复用其分层实现；当前Nezha最先需要修正的是板级UART时钟。**
Pad6S Pro（sheng）在README报告WCN7850配对、重连、开关和A2DP工作；本轮核对的是源码，
没有复测它的实机。它使用SM8550，Nezha是SM8850；这不是整个驱动可原样套用的关系。

读取了`UniXNU/UniXNU-sheng`实际引用的`drivers/QcaBluetooth`子模块版本，确认本次下载的
`qbt_geni.c`、`qbt_linux.c`、`qbt_transport.c`与该固定版本一致，而不是只看一个无关分支。
选择核对结果保存在`work/bluetooth-source-audit/unixnu-upstream-20261115/source-selection.json`。

## 上游实现分层

| 上游源码 | 职责 | 对Nezha的结论 |
|---|---|---|
| `UniXNU-sheng/package/BluetoothProbe.cpp` | MMIO、SMMU/DMA映射、引脚、GIC IRQ、单worker与板级上电 | 参考/适配；地址、SID、GPIO、IRQ不能直接复制 |
| `UniXNU-sm8550-common/kits/Sm8550Platform/Sm8550GeniUart.c` | UART14源时钟及共享GCC vote | MND/HID原理可适配；RCG地址与DFS所有权需按SM8850核对 |
| `UniXNU-sheng/package/WifiProbe.cpp`的WcnPower接口 | Wi-Fi/BT共享电源所有者、锁和最后一个owner释放 | 复用设计；Nezha既有RPMh资源与Wi-Fi要保持一致，不能导入sheng rail表 |
| `QcaBluetooth/src/qbt_geni.c` | GENI SE DMA、四个RX buffer、TX buffer、流控、停止/abort/reset、背压 | 适配候选；要接Nezha SMMU、DMA同步和IRQ，不能把FIFO probe直接替换后声称完成 |
| `QcaBluetooth/src/qbt_core.c` | 有界H4 parser、IBS、单执行器、队列与超时 | 普通HCI运行阶段可适配；Nezha冷启动的PERI前置阶段需独立接入 |
| `QcaBluetooth/src/qbt_linux.c`及锁定Linux btqca | WCN7850查询、波特率切换、TLV/NVM下载 | **不适合直接用于Nezha冷启动**，本机Android已证明FD/FC+PERI/Ganges前置流程 |
| `QcaBluetooth/src/qbt_transport.c`、BlueTool、Type0 | UART.SK与原生bluetoothd、开关和能力适配 | 后续可适配；上游目标iPadOS27，当前Nezha lane为iOS16，接口/daemon补丁需另审 |

这些层均已有实现，后续无需重新写一套不明来源的H4/IBS、DMA ring或内核pipe。
先打通Nezha控制器的真实响应，再接入已有通用传输组件和原生蓝牙栈。

## 时钟：上游独立确认了当前缺陷

`Sm8550GeniUart.c:sm8550_geni_uart14_source`在115200选择：

```text
源时钟7372800Hz，M384，N15625
M_REG=384
N_REG=~(15625-384)&0xffff=0xc476
D_REG=~15625&0xffff=0xc2f6
HID=1，SRC=6，MODE=2
UART采样16，divider4，M/S_CLK=0x41
```

这与SM8850的`gcc-canoe.c`频率表、`clk-rcg2.c`编码，以及Nezha Android trace的
115200/ser_clk_cfg65吻合。旧Nezha写mode0/rawN75再报告64MHz的做法没有来源，必须纠正。
具体证据见[SM8850时钟审计](BLUETOOTH_SM8850_CLOCK_AUDIT_2026-11-15.md)。

但两者的时钟控制策略仍有区别，必须明确：

- sheng明确以该UART引擎独占为条件，读取DFS并清bit0，再读回确认，采用手动RCG；
  CFG更新还清bit20，不设置HW_CLK_CTRL。
- 本轮已准备的[level13](BLUETOOTH_LEVEL13_PLAN_2026-11-15.md)遇DFS开启只读表然后停止，
  不主动接管；普通RCG配置保留SM8850原厂`HW_CLK_CTRL_MODE`要求。
- 因此level13是按SM8850原厂常规RCG路径修正的候选，**并未完整实现sheng的手动接管方案**。
  如果后续采用手动接管，应独立说明SE5独占依据，保存原DFS/CFG，核对接管后读回及错误退出。
  不应一边保留自动时钟标志，一边宣称与sheng手动控制完全相同。
- 全部方案均不需要写PLL M/N/D；父PLL仍需只读确认或注明名义频率。

## GENI传输与上电顺序

上游`BluetoothProbe.cpp:powerOn`先持有共享电源、BT_EN低100ms，配置wrapper/serial clock、
停止旧引擎并prepare DMA；然后BT_EN高60ms，切UART pinmux、启动RX，再进入`qbt_boot`。
调试模式可在控制器复位期间做DMA内部回环，检查DMA、packing和cache同步。

Nezha当前是BT_EN高后才配置UART clock；本机Android也是btpower完成后才打开UART。
不能仅凭另一块板的顺序就认定Nezha应全部复制。实际相同的约束是：**源时钟正确、
RX准备好、流控正确、控制器复位释放后再发协议**。GPIO、PMIC、延时均需本机依据。

`qbt_geni_prepare/start`设置RX_TRANS_CFG=0、RFR_OPEN，四字节DMA packing，手动流控在
RX buffer已armed时再允许ready。IRQ只屏蔽/唤醒worker，解码和队列推进由单worker拥有；
停止有cancel→abort→DMA reset的有界流程，DMA停止未证实前不释放buffer。
这比继续堆轮询FIFO命令更适合作为正式驱动，但当前无RX问题应先修时钟，再迁移传输。

## 固件和协议不能照搬

sheng `qbt_linux.c:qbt_boot`直接以`QCA_WCN7850`调用`qca_read_soc_version`，随后用
HCI opcode0xfc48切速，再调用`qca_uart_setup`下载Hamilton TLV/NVM；成功后启用IBS并设置地址。
其`BluetoothFirmware.c`只接受`hmtbtfw*.tlv`与`hmtnv*`名。

Nezha保存的Android数据则先FD→FC，发送`31 00 f0 ff 02 05 00`，之后还有PERI、Ganges/BT/UWB
相关初始化。将sheng WCN7850启动和hmt固件直接套入，是回到已经被本机证据否定的旧假设。
可以保留PERI前端，再在进入普通HCI后复用上游H4/IBS及native transport。

## 原始线上来源与快照

```text
https://github.com/UniXNU/UniXNU-sheng/blob/main/package/BluetoothProbe.cpp
https://github.com/UniXNU/UniXNU-sheng/blob/main/package/WifiProbe.cpp
https://github.com/UniXNU/UniXNU-sheng/blob/main/package/BluetoothFirmware.c
https://github.com/UniXNU/QcaBluetooth/blob/main/src/qbt_geni.c
https://github.com/UniXNU/QcaBluetooth/blob/main/src/qbt_linux.c
https://github.com/UniXNU/QcaBluetooth/blob/main/src/qbt_core.c
https://github.com/UniXNU/QcaBluetooth/blob/main/src/qbt_transport.c
https://github.com/UniXNU/UniXNU-sm8550-common/blob/main/kits/Sm8550Platform/Sm8550GeniUart.c
```

快照保存在`work/bluetooth-source-audit/unixnu-upstream-20261115/`。
源文件许可证各自保留；尚未把GPL组件源码复制进当前蓝牙驱动或改动其接口。
只读研究没有运行上游脚本的完整性检查，也没有下载或安装其固件。

本轮[level12](BLUETOOTH_LEVEL12_RESULT_2026-11-15.md)已完成一次获准启动，BT_EN数字采样通过。
level13已准备但尚未启动。用户要求优先看Pad6S Pro上游后，保持设备释放，不追加启动。
