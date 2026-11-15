# 骁龙SM8850蓝牙路线复核：UART时钟的编码与验证存在实质错误

## 结论

**旧驱动的“RCG64MHz、UART实际114285”不能成立为已验证事实。**
在同款SM8850的Qualcomm开源`gcc-canoe.c`、`clk-rcg2.c`中，普通RCG与DFS是不同路径。
当前驱动直接写`CFG=0x600,M=16,N=75,D=0`，随后又用自定义错误公式算出64MHz，
这只是写入和解码错误相互吻合，内部回环也不能证明外部波特率。
该问题是确定的源码缺陷；是否就是无回应的根因，仍需实机验证。

本次查的是骁龙SM8850本平台实现。未采用Switch/Tegra方案，也没有引入其他芯片驱动。

## 第一手来源与本机对应关系

来源仓库：`OnePlusOSS/android_kernel_oneplus_sm8850`，
分支`oneplus/sm8850_b_16.0.0_oneplus_15`，2026-11-15通过GitHub公开contents API读取。

| 原厂开源路径 | 使用的事实 | 本地快照 |
|---|---|---|
| `drivers/clk/qcom/gcc-canoe.c` | SE5 RCG地址0xa8640，MND16位/HID5位，父源0/1/6，频率表及DFS注册 | `gcc-canoe.sm8850.c` |
| `drivers/clk/qcom/clk-rcg2.c` | M/N/D编码、mode2、half-divider、HW_CLK_CTRL和DFS检测 | `clk-rcg2.sm8850.c` |
| `drivers/tty/serial/msm_geni_serial.c` | 波特率按源时钟/16x采样/整数divider；非console端口setup | `msm_geni_serial.sm8850.c` |
| `drivers/soc/qcom/qcom-geni-se-msm.c` | GENI wrapper、主/从sequencer的角色和资源路径 | `qcom-geni-se-msm.sm8850.c` |

快照均在`work/bluetooth-source-audit/`。本机只读sysfs确认GCC绑定`gcc_canoe`，
compatible=`qcom,canoe-gcc`，UART绑定`msm_geni_serial`。见`android-clock-module-inventory.json`。
`clk_qcom`等模块在/proc/modules中加载，但本次未找到可读取的时钟模块文件；
时钟编码的证据来自同SoC原厂源码，没有声称完成该模块的二进制交叉核对。

本机Android UART trace已保存：第165行`ser_clk_cfg=65 (0x41)`，177行设置115200。
GENI串口配置字段为`(divider<<4)|1`，故Android实际采用divider4。
结合16x采样，所需名义源时钟为115200×16×4=7372800Hz，与本SoC SE5频率表首项一致。
这是独立于XNU错误计算的交叉依据，仍不是测得引脚上的波形频率。

## 普通RCG的正确原理

`clk-rcg2.c:__clk_rcg2_configure`及`calc_rate`规定：

- M寄存器写M；N写`~(N-M)&0xffff`，D写`~N&0xffff`（本频率下D的clamp结果为N）。
- 分数模式为`MODE=2`；mode0不执行M/N分频，不能仍按M/N计算频率。
- HID是half-divider：HID非零时乘`2/(HID+1)`，不是除以`HID+1`。
- CFG bit20是`HW_CLK_CTRL`，不是旧注释所称的“parked bit”。SE5设置了HW_CLK_CTRL_MODE。
- 父源6为GPLL0_EVEN，表中的名义父率300MHz；父源1为GPLL0_MAIN，名义600MHz。
  本轮不写PLL，也没有测量父PLL实际频率。

| 表项 | CFG关键字段（含HW_CTRL） | M | N寄存器 | D寄存器 |
|---|---|---|---|---|
| 7.3728MHz，M384/N15625 | 0x102601 | 384 | 0xc476 | 0xc2f6 |
| 64MHz，M16/N75 | 0x102601 | 16 | 0xffc4 | 0xffb4 |
| 旧驱动所谓64MHz | 0x600（mode0） | 16 | 75 | 0 |

如果DFS关闭且父源名义为300MHz，旧CFG0x600按原厂算法是300MHz，而非64MHz。
如果DFS开启，则普通CFG/M/N根本不足以代表当前源时钟。因此无论哪一种，原64MHz断言都未获证。
对原厂频率表各GPLL0_EVEN表项的编码/解码核对结果保存在`sm8850-clock-encoding-check.json`。

## DFS路径不能猜

同SoC把SE5列在`gcc_dfs_clocks[]`中。`clk_rcg2_enable_dfs`只有在
`CMD_DFSR(+0x14).bit0=1`时才改用DFS ops；当前level1..12从未记录该字段。
DFS开启时从选中的perf表读取CFG/M/N，不能把普通RCG寄存器读回当作实际频率。

新候选先读并打印CMD/CFG/M/N/D/DFS。DFS开启则保存8档perf表并明确返回ENOTSUP，
不写普通RCG、不发FD；拿到实际表和控制状态后才能实现该路径。
DFS关闭才按原厂编码设置7.3728MHz，检查UPDATE完成及M/N/D/CFG读回，然后使用divider4。

## 另一处GENI差异：暂未擅自合并

原厂`msm_geni_serial_port_setup`在非console的SE_DMA路径写`SE_GENI_CFG_REG80(+0x240)=0x431c`；
本机模块0x758c..0x7594也写相同值。后续还有依赖配置的局部路由调整，已保留完整函数反汇编
`msm_geni_serial_port_setup.disasm.txt`，不能只取一个立即数就宣称全流程等价。

XNU使用FIFO，当前读回IO_MACRO=0x31c。由于原厂写入发生在DMA路径，尚不能断言
把0x431c复制到FIFO一定正确；本轮时钟候选不合并这项修改。下一步应沿GENI固件和
对应模式核对信号路由，而不是无来源轮换寄存器值。

## 本轮实机对原理排查的帮助

[level12实机](BLUETOOTH_LEVEL12_RESULT_2026-11-15.md)首次直接获得PMIC BT_EN数字样本：
low三次0、high三次1，sample/restore错误均0；仍无FD回应。这样可把“PMIC输出根本未变化”
与当前UART时钟缺陷区分。它不证明控制器接收端电压或模拟电气裕量。

后续以本报告和新的时钟实现为准，旧报告中的64MHz/actual114285仅保留为当时错误驱动的输出。
无需逻辑分析仪即可继续，但不能再用内部回环或自算频率替代外部时钟链路的验证。
