# 蓝牙：无需逻辑分析仪的软件排查路线（2026-11-15）

> **level10 RFR_OPEN候选已准备好，尚未启动；另确认[S8F电源范围差异](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)，未混入该候选。**

> **最新实机：[level 9 结果](BLUETOOTH_LEVEL9_RESULT_2026-11-15.md)。RX bit1 已清零并读回0，三次 FD 仍无回应，90.221秒自动返回。下一项独立软件判据为 RFR_OPEN，尚未实现新候选。**

> **最新：[level 8 已完成实机](BLUETOOTH_LEVEL8_RESULT_2026-11-15.md)：三次 FD 无 FC，
> 版本请求未发送，90.221 秒自动返回。已保留 BT_EN pad 采样和 UART 参数差异的软件排查路线。
> level 7 实机内部回环 PASS；随后读出 Android 已有 hsuart trace，
> 确认实际是 FD/FC 唤醒和 PERI 初始化，且有 Ganges 家族响应。
> 当前下一步以 [Android ROM 路线报告](BLUETOOTH_ANDROID_ROM_ROUTE_2026-11-15.md) 为准，
> 不要仅凭 Hamilton 文件存在继续套用普通 H4 / WCN7850 初始化。**

## 结论与验证边界

当前软件假设与判据汇总（下文早期候选记录保留为历史）：

| 假设 | 现有证据 | 状态 / 下一判据 |
|---|---|---|
| AP内部FIFO/收发sequencer不可用 | level7严格8字节内部回环PASS | 该内部路径可用；不覆盖外部线路 |
| 仅清RX_TRANS_CFG bit1即可唤醒 | level9读回0，三次FD仍无RX | 本次冷启动对照为阴性，勿重复作为未测假设 |
| 缺少RFR_OPEN影响接收ready | 本机Android RX命令带bit7，XNU此前未带 | [level10](BLUETOOTH_LEVEL10_PLAN_2026-11-15.md)完整构建及镜像检查通过，尚未实机 |
| S8F请求未达到BT consumer范围 | BT请求1800mV，live DT下限1876mV，Wi-Fi请求1876mV | [电源范围审计](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)已确认源码/DT差异，未测电压或独立试验 |
| BT_EN配置正确但pad未按预期变化 | LV/MV MODE/SOURCE已读回；输入采样尚未做 | 独立MODE2/RT_STS实验，必须保存与恢复，不把锁存读回当采样 |

有可继续执行的软件路线。旧交接的“软件全部对齐、两个独立信号为零，所以只能接逻辑分析仪”不成立；源码、原始 w67 日志和当前 Android DT 已足以指出多处具体错误。

已修正 `wsl/pandora_bt.c`，同步到 `xnu/patches/0476-pandora-bluetooth.patch`。
最终完整构建 43651227510 和离线打包检查均已通过；这仍是一份待试机候选，不代表蓝牙已工作。
随后经用户同意完成一次 level 2 临时启动：已读到 GPIO5 subtype=0x12、QUP=4.3，
但仍无 HCI 应答。实机结果和下一步内部回环见文末；未宣称蓝牙已恢复。

本轮手机操作仅为 Android 上读取 sysfs、DT 和复制现用内核模块到主机。没有开关蓝牙、挂载 debugfs、重启、启动 iOS、推送文件或写固件容器。Android 当前没有挂载 debugfs，也没有 `/dev/mem`，因此旧的 debugfs/devmem 采集脚本不能原样复用。当前工作区的根 Git 尚无提交；旧交接所称远端 main 不等于本地可核验的 HEAD。

## 六项决定路线的证据

### 1. PMIC GPIO 编码仍然错误，且遗漏了硬件子类型

Linux `pinctrl-spmi-gpio.c` 的 `pmic_gpio_populate` / `pmic_gpio_config_set` 区分两种布局：

| 布局 | TYPE / SUBTYPE | normal 输出低 / 高 |
|---|---|---|
| legacy | TYPE=0x10；SUBTYPE=0x01/0x05/0x09/0x0d | MODE_CTL=0x10 / 0x11；电平在 bit0 |
| LV/MV | TYPE=0x10；SUBTYPE=0x10..0x15 | MODE_CTL=1；DIG_OUT_SOURCE_CTL（0x44）=0 / 0x80 |

旧代码写 `MODE_CTL=0x12` 表示高电平。legacy 下它是 paired function、低输出值；LV/MV 下它也没有设置 0x44 的输出值。读回 0x12 只能证明写入值读回，不能证明输出 high。

从手机复制的 `pinctrl-spmi-gpio.ko` 也验证了这两个编码分支：`pmic_gpio_config_set` 的 LV/MV 分支在 0x13dc..0x13e8 写 offset 0x44，输出值左移 7；legacy 分支在 0x13f4..0x1414 把 function 左移 1、direction 左移 4，再与未移位的布尔输出值合并。这是本机模块证据，避免只套用相近硬件的实现。

同时，0x43 是输入路由 / DTEST 配置，不是电平采样。采样在 RT_STS（0x10）bit0，并且需要输入缓冲开启；纯输出配置下读到零不能证明物理低电平。候选按 TYPE/SUBTYPE 选编码，未知类型不写，检查 VIN 范围、写入返回值和配置读回。level 1 新增 GPIO5/6 的只读快照；SPMI 读事务需要提交 arbiter 命令，但不修改 PMIC 配置。

### 2. TLMM 输入采样位读反，“SW_CTRL=0”是误判

同款 SM8850 的 `pinctrl-canoe.c` 明确 `.in_bit=0, .out_bit=1`，并给出相同的 GPIO18、24..27 地址步长和 UART mux。手机 sysfs 也确认正在使用 `pinctrl_canoe`。

旧代码把 GPIO_IO 右移 1 当作输入。w67 原始日志第 4292 行是：gpio18 IO=0x1、gpio24 IO=0x1、gpio27 IO=0。正确解码为 **SW_CTRL=1、CTS=1、RX=0**。因此“SW_CTRL 与 RX 两个独立信号都低”的前提已被原日志反证。RX 的原始值确实为零，仍需解释；不能由错误解码推出整颗 BT core 没启动。

候选统一读取 bit0，并同时打印 GENI IOS（0x908）的输入采样，便于区分 pad / mux / 串口接收路径。IOS 与 TLMM 都是数字域观测，不等同于测量模拟电压或波形质量。

### 3. w67 的 level 6 实际用了 4 Mbps，还继承了等待 60 秒

旧 `bt_level >= 5` 同时控制 128 MHz RCG 和 4 Mbps；`bt_level >= 4` 又把等待实验带进了 level 6。

原日志：第 4243 行等待 60000 ms；第 4247、4269、4283 行三次均打印 `UART 4000000`。这不能证明文档描述的“115200 冷启动、三次重试”失败。

候选只让 **level 5** 使用 4 Mbps，只让 **level 4** 执行 60 秒等待；level 6 回到冷启动速率。w66 的 115200 试验仍使用错误 PMIC 编码，不能作为修正后组合的失败证据。

### 4. 波特率计算缺少 QUP v2.5 的采样倍数分支

Linux 串口源码与本机 `msm_geni_serial.ko` 的 `msm_geni_serial_config_baud_rate` 都在 QUP v2.5 及以上采用 16 倍采样，旧版本使用 32 倍。本机模块反汇编 0x95e8..0x9610 明确比较 major / minor 并选择 16 或 32。

旧代码始终用 32，并向下取整。候选在 wrapper/SE 时钟开启且读回正常后，读取 QUPV3_3 `0x019c0000 + 4` 的硬件版本，按版本选择采样倍数，依照 Android 的向上取整计算 divider，打印请求速率和计算实际速率。SE_FW_REV=0x206 是 UART 协议固件标识，不能代替 wrapper 版本。

**条件性推论**：如果本机 wrapper >=2.5，旧 64 MHz/divider17 的“115200”实际约为 235294 baud。当前尚未读出该硬件版本，不能称这个数值已在手机上测得。RCG=64/128 MHz 的读回也不排除此错误。

### 5. FMD GPIO6 被误当成 XO 输出

本轮直接读取手机 DT：

| 节点 | 引脚 | function | power-source | 方向属性 |
|---|---|---|---|---|
| `bt_en` | gpio5 | normal | 1 | input-disable、output-enable |
| `bt_fmd_clk_en` | gpio6 | normal | 1 | input-enable、output-disable |

旧代码却把 GPIO6 设置为 VIN0、强制输出高；交接称它与 Android 一致，和当前 DT 不符。同平台 SM8850 的 `btpower.c` 只解析并保存 `bt_gpio_fmd_clk_ctrl`；正常上电里的 XO helper 使用独立的 `qcom,xo-clk-gpio`，不能把两个字段混用。该源码和本机模块都包含新式 FMD / 多客户端电源路径；旧 `work/tmp_btpower.c` 不能独自代表当前设备驱动。

候选去掉 GPIO6 的强制写入，保留交接状态，level 1 可读出其配置。DT 表明预期配置；当前运行时 PMIC 寄存器值仍待采集，不把 DT 当作寄存器快照。

### 6. GPIO9 没有独立的 BT_WAKE 绑定

继续核对本机 `soc:bt_peach/of_node` 后，确认没有 `qcom,btwake_gpio` 或
`qcom,bthostwake_gpio` 属性。旧代码却把 `mpm_wake_set_gpios` 数组第二项的 9
直接当作 dev-wake GPIO，强制配置为输出高。

同平台 `btpower.c:get_gpio_dt_pinfo` 对 `mpm_wake_set_gpios` 使用
`of_property_read_u32`，保存到 SW_CTRL 的 MPM 配置字段；OOBS 的 dev-wake
则从独立的 `qcom,btwake_gpio` 读取。数组第二项不是 GPIO9 为 dev-wake 输出的依据。
最终候选删除 GPIO9 的驱动和多余映射，保留该引脚的交接配置。本轮没有据此猜测
GPIO9 的其他用途，也没有重新做没有判据的唤醒极性试验。

## 候选的其余必要修正

- SPMI / RF clock 失败立即终止当前初始化，不再吞掉返回值后打印正常上电流程。共享 rail 不擅自撤票，避免干扰 Wi-Fi。
- 部分 SPMI 窗口映射失败后保留已成功映射，后续进入只补缺失窗口，避免重复映射。映射仍按原有约定随内核存活。
- 重试重新配置串口前停止仍活跃的主 / 次 sequencer；CMD_CTRL 的 abort 命令是 bit1，不能使用 IRQ 的 abort 完成位 bit5。
- HCI 成功必须收到完整的 Reset Command Complete、opcode=0x0c03、status=0。噪声、截断数据或其他事件不再记为控制器成功应答。

改动限定在 BT 驱动、其构建补丁和蓝牙说明；未修改 Wi-Fi 驱动、固件/NVM、IPA 安装路径或设备镜像。

## 无需仪器的执行顺序与判据

| 次序 | 可执行动作 | 能区分的问题 | 状态 |
|---|---|---|---|
| A | 源码 + 原始日志 + Android 只读 DT / 模块审计 | 是否真的已经排除软件错误 | 本轮已完成，发现上述错误 |
| B | 完整构建候选；经用户明确同意，做一次 level 2 的 90 秒临时启动 | TYPE/SUBTYPE、正确 BT_EN、QUP sampling、pad / IOS、HCI | 最终完整构建和镜像准备已完成，尚未启动；level 2 单次初始化优先，避免混入重试因素 |
| C | 若正常 HCI 仍无应答，单独加入 GENI 内部 loopback 并收回原字节 | AP FIFO / sequencer / packing / 接收是否工作 | level 7 完整构建和实机均通过，8/8 匹配；不能证明外部连线或绝对波特率 |
| D | 对照 Android 的 PMIC 配置和上电事件 | kernel API 所用子类型、真实 pinconf、时钟/电源调用顺序 | 尚缺运行时寄存器快照；debugfs 未挂载，不能假称已抓到 |
| E | 按 Qualcomm 初始化顺序发 `qca_read_soc_version`，验证匹配的 vendor / Command Complete 响应 | ROM 握手与通用 HCI Reset 行为是否不同 | 未实现；Linux 并不把通用 Reset 成功作为读取版本的唯一前提 |
| F | ROM 通信成立后再移植 TLV / NVM 下载 | 固件加载、运行速率、后续栈接入 | 未开始，不能用盲灌固件解决链路问题 |

后续若需要 Android 主动开关蓝牙或使用 trace 配置，必须单独说明具体实验；本轮未执行。对 GPIO 做输入/输出同时开启并读取 RT_STS 也是一个可选的数字电平验证实验，但会改变 pad 配置，应独立设计并恢复原配置，不混入首个候选。数字回读不能覆盖模拟电气故障，但当前没有理由以仪器为继续工作的前提。

## 复用与资料入口

以下均为一手开源资料，按硬件寄存器事实与接口约定参考，未整段搬入其他系统的驱动框架：

- Linux `drivers/pinctrl/qcom/pinctrl-spmi-gpio.c`，v6.12 与 master：TYPE/SUBTYPE、MODE、SOURCE、RT_STS。用于适配编码。
- OnePlusOSS `android_kernel_oneplus_sm8850`，分支 `oneplus/sm8850_b_16.0.0_oneplus_15`，`drivers/pinctrl/qcom/pinctrl-canoe.c`：同平台 TLMM 输入/输出位和 mux。用于直接核对寄存器事实。
- OnePlusOSS `android_kernel_modules_and_devicetree_oneplus_sm8850`，同名分支，`vendor/qcom/opensource/bt-kernel/pwr/btpower.c`：FMD 与 XO 的不同字段、SW_CTRL 输入语义。可适配顺序，不能把别的整机 rail 表直接套入小米。
- Linux `drivers/tty/serial/qcom_geni_serial.c`、`drivers/soc/qcom/qcom-geni-se.c`、`include/linux/soc/qcom/geni-se.h`：采样倍数、wrapper version、sequencer abort、IOS、loopback。
- Linux `drivers/bluetooth/hci_qca.c`、`btqca.c`：冷启动初始化、版本握手、固件下载。UART/固件协议可适配，不是 XNU HCI 栈的直接替代品。

原始资料、本机模块及定点反汇编保存在工作区 `work/bluetooth-source-audit/`，入口为该目录的 `README.md`；后续接手可不依赖 debugfs 或手机重新启动来复核本轮静态结论。

固件来源也不是“只能写 280 KB C 数组”：现有 `pandora_wifi.c` 已通过 `pandora_firmware_pread` 读取外部固件。可以参考该接口设计 BT loader；本轮未改动仍由 IPA 实验占用的容器，也未声称 BT 固件已经在可读存储中。

## 本轮验证与下一步

本地使用现有 XNU DEVELOPMENT_ARM64_PANDORA 生成头和 NDK clang，以 `arm64-apple-macos11.0`、`-mkernel -O2 -Wall -Wextra` 编译单个 BT 源文件，检查真实进程返回码；构建补丁的新增文件须与 `.c` 完全一致，两个注册 / 调用上下文块保持原样。最终实际结果记在审计目录 `verification.txt`。

单文件编译之后，最终完整内核构建和镜像准备也已通过（见下方最终记录）。下一步是在明确获准的独占窗口做 B；第一次日志必须包含 TYPE/SUBTYPE、MODE/SOURCE、wrapper version / sampling 和真实 HCI 数据。未获得用户明确启动同意前，停止在主机准备阶段。

## 后续构建与映射准备

已在独立分支 `codex/bluetooth-no-analyzer-20261115` 启动完整 DEVELOPMENT/PANDORA
构建，未更新 main。初版运行编号为 43649529960；去掉无绑定依据的 GPIO9 驱动后，
最终候选需要新的完整构建结果，不能拿初版产物代替。

新增 `tools/bluetooth/prepare_trial.py` 复用现有 mkpayload / ADT / loader 构建链，
使用新输出目录。基线为上次 BT 镜像 `usb-bt-tlmm-level2-w67-ext.img`，首轮 level 2、
自动返回 90 秒。loader 使用原有 `E:/IOS16/IOS16` 源码，避免把工作区额外的
boot_args ABI / framebuffer 修改混进首轮 BT 试验。

Android FIEMAP / pin-status 的只读预检还发现：旧镜像的固件映射对应
`metal-update-0013.img` 的历史 32 MiB、8 段布局；同名现存文件是 pinned 的
64 MiB、6 段布局。新工具可显式接收当前映射快照，仅在新镜像的 ADT 更新
`pandora-fw-extents` / `pandora-fw-blocks`，不写、不重新分配容器。root 的有效
16 GiB 暴露区间及日志映射与手机当前物理布局相符，保持原样；没有把 root.img
的 64 GiB 文件长度擅自改成新的对外磁盘大小。

随后通过现有 pinfile 的只读 rawreadat 读取 root 物理视图的 HFS 卷头：HX、
blockSize=4096、totalBlocks=4194304，即卷声明容量确实仍为 16 GiB。
证据在 `android-root-volume.json`；不是仅凭旧镜像大小决定保留该值。

快照与有效范围比较结果见 `work/bluetooth-source-audit/extent-preflight.json`。
ADT 里的 extent 数组带预留零槽，比较时必须按有效区间解析；不能用 JSON 长度或
未去掉预留槽的字节长度判断是否漂移。最终内核运行、镜像路径和验证结果将在
构建完成后记录，当前仍没有启动手机。

## 最终候选已准备（2026-11-15）

- 初版完整构建 43649529960 成功；含 GPIO9 修正的最终完整构建
  **43651227510 成功**。下载产物中的 BT 诊断字符串已核对，单文件检查不能替代的
  完整内核编译/链接层现已通过。
- 最终内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/artifact/kernel.development.pandora`。
- 最终镜像：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level2/loader/nezha-loader-boot.img`，
  17,285,120 字节，level 2，`pandora_reboot_after=90`。只在主机生成，未启动。
- 镜像为 Android boot v4；payload/segment/ADT 范围、trust-cache 复制及重定位、
  root/log 映射保留、固件映射采用当前 6 段快照均已检查。
- 备份的试机前日志：`D:/turnip-local/newboot/bt-no-analyzer-20261115/before-trial/`，
  包含物理视图 `log.img` 与解码 `kernel.txt`。采集使用现有 pinfile，只读，未推送 helper。
- 记录：最终输出目录的 `trial.json`、`verification.json`，以及工作区审计目录
  `final-preparation.json`。初版镜像只用于验证打包工具，不用于设备试验。
- **设备验证仍未发生**。下一步须先取得明确的独占启动窗口同意，再临时 boot 最终镜像，
  预期 iOS 阶段 90 秒后自动返回 Android，立即只读抓取并分析新日志。启动将覆盖设备
  `log.img`；旧日志已有主机备份。不得写 firmware 容器，不得用旧镜像替代。

## Level 2 实机结果：2026-11-15 07:25 +08

用户对明确的试机请求回复“继续”后，执行一次 `fastboot boot`。设备按
`pandora_reboot_after=90` 在内核时间 90.222 秒自动复位，随后回到 Android。
未再次重启，未写固件容器，未推送 helper；日志取回后已释放设备窗口。

日志路径：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level2/result/kernel.txt`。
原始 `log.img` 同目录保存；紧邻启动前的旧日志保存在 `level2/preboot/log.img`。

| 观察 | 原始日志行 | 结论 |
|---|---|---|
| GPIO5 TYPE=0x10、SUBTYPE=0x12（LV/MV） | 249、253、426、427 | 子类型已实机确认；BT_EN 高读回 MODE=1、SOURCE=0x80、VIN1 |
| QUP version=0x40030000、sampling=16、divider=35 | 486 | v4.3，确实需要 16 倍采样；请求 115200，计算实际 114285 |
| GPIO18=1、CTS=1、RX=0；GENI IOS=0x6 | 489、490、496、497 | SW_CTRL 高的纠正得到本次实测支持；pad 与 GENI 的 RX 数字采样均为低 |
| HCI Reset sent 后 200 ms 无数据 | 491、492 | 该组合仍无 HCI 应答；AP 命令完成不等于外部字节正确到达控制器 |
| exec SpringBoard；90 秒自动返回 | 931、14484 | 系统启动和计时返回已观察；不等同于用户视觉验收 |

不能从 MODE/SOURCE 配置读回推断模拟电压；本次也尚未确认 GPIO6 的运行时配置。
现在的证据足以继续软件隔离，没有重新引入“必须逻辑分析仪”的前置条件。

接收 sequencer 的 S_IRQ=0xc00（489、500 行）也应保留：Linux UART 驱动把
GP IRQ1/2 分别放在丢弃错误接收数据 / break 处理分支。它支持继续检查接收链路，
但不是一份 HCI 包，更不能单独判定控制器已启动或损坏。内部回环会在切换到内部
接收后清除旧 IRQ，再记录本次状态，避免把外部 RX 持续低造成的状态沿用为判据。

### 下一候选：level 7 内部回环

`pandora_bt=7` 是独立诊断模式：仅打开 QUP 时钟并配置 UART，不投 BT rail、不脉冲
BT_EN、不修改 TLMM mux，也不发送 HCI Reset。使用上游驱动相同的
`SE_UART_LOOPBACK_CFG`（0x22c）值 3，在 RX/TX 与 CTS/RTS 之间内部连接。

发送固定八字节 `55 aa 00 ff 01 80 7e 81`，最多收取 16 字节并等待 200 ms。
仅在收到恰好八个匹配字节时记 PASS；旧 RX 数据先有界排空，排不完则报错。
退出时停止主/次 sequencer，恢复原 loopback 寄存器，失败也记录，不把它当成
恢复了之前 OS 的完整 UART 会话。正常 level 2 初始化明确关闭内部回环。

此测试用于判别 AP FIFO packing / sequencer / RX 数据通道是否可用。PASS 不能证明
芯片已启动、外部走线正确或绝对波特率准确；FAIL 则优先检查 AP 串口路径。
level 7 已通过 ARM64 单文件编译并同步 0476，完整构建和设备结果须单独记录。
本次用户只授权了上面的 level 2 启动，未据此执行第二次 boot。

level 7 完整构建 **43653645421 成功**，仍在独立蓝牙分支，main 未改动。
产物内已确认回环诊断字符串，主机打包已完成：
`D:/turnip-local/newboot/bt-no-analyzer-20261115/level7/loader/nezha-loader-boot.img`，
17,285,120 字节，90 秒自动返回。它继承 level 2 已实机使用的全部 root/FW/log 映射和
trust cache，仅 boot-arg 的 BT level 改为 7；内核新增独立回环代码。
记录在 `level7/trial.json` / `verification.json` 和审计目录的 `level7-preparation.json`。
后续已获准启动该回环镜像，**实机 PASS，8/8 匹配**，并在内核 90.217 秒自动返回。
结果在 `level7/result/`。返回后又读取了 Android 原有 hsuart trace；下一步改为先审计
真实 FD/FC 与 PERI 协议，详见本页顶部链接。不能将回环 PASS 当作蓝牙控制器就绪。
