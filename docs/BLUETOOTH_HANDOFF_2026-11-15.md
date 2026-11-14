# Bluetooth 攻坚 Handoff（2026-11-15 06:30）

> **2026-11-15 后续审计更正：此页下文保留为历史记录。当前结论和候选修改见
> [无需逻辑分析仪的软件排查路线](BLUETOOTH_NO_ANALYZER_2026-11-15.md)。**
> §2.1 把 MODE_CTL bit1 当电平的写法仍然错误：legacy 电平是 bit0；LV/MV
> 使用 MODE=1、0x44 bit7，必须先读 TYPE/SUBTYPE。0x43 是输入路由配置，不是电平。
> §3/§4/§5 的已排除项因此不能一概沿用：w67 的 level 6 实际继承了 4 Mbps 和 60 秒等待；
> canoe TLMM 输入是 GPIO_IO bit0，w67 的 gpio18 raw=0x1 表示高，不是 SW_CTRL=0。
> 当前 Android DT 还把 FMD GPIO6 定义为输入/VIN1，旧代码强推输出/VIN0 与之不符；
> 串口采样倍数也应按 QUP 硬件版本选择，不能固定为 32。
> 后续还删除了从 MPM 数组第二项猜测 GPIO9 为 BT_WAKE 的驱动，当前 DT 没有独立的 dev-wake GPIO 绑定。
> 这些错误已在候选中修正，完整构建 43651227510 和离线打包检查通过；实机结果见下一行。
> 后续已完成 level 2 实机：GPIO5 subtype=0x12、QUP=4.3、SW_CTRL=1，仍无 HCI；
> level 7 内部回环镜像已由完整构建 43653645421 生成，待新的启动许可。详见更正报告末尾。

> 交接对象：任何接手 UniXNU 蓝牙的 agent。
> 读这份文档可以**避免重复踩我在 2026-11-13 ~ 11-15 踩过的所有坑**。
> 详细过程见同目录 `BLUETOOTH_W61B_W67_2026-11-14.md`。

---

## 0. 一句话状态

**历史试验未收到 HCI 应答，但软件配置和输入电平解码仍存在错误，不能认定 BT 子系统未启动，
也不能认定逻辑分析仪是继续工作的前提。优先执行上方链接中的软件路线。**

---

## 1. 硬件事实（已核对，可直接采信）

| 项 | 值 | 来源 |
|---|---|---|
| 平台 | SM8850 "canoe"/"nezha"（小米 17 Ultra） | `live.dts` |
| BT 控制器 | QCA **WCN7850/786x（HAMILTON）**，UART 传输 | 固件 `hmtbtfw20.tlv` 版本串 `BTFW.HAMILTON.2.0.3-00035-PATCHZ-3` |
| HCI 传输 | **GENI SE5 UART**，`qcom,qup_uart@1994000`（QUPV3_3，base `0x19c0000`） | `live.dts:22436`、Android `ttyHS0` |
| `/dev/ttyHS0` | 确认属于 `1994000.qcom,qup_uart`；Android 上 `speed 4000000 crtscts` | `readlink` + `stty`（只读采集） |
| BT_RESET | **PMH0104（SPMI bus1 SID9）GPIO5**，DT 名 `qcom,bt-reset-gpio`，flags=0 | `live.dts` |
| XO/FMD clk | PMH0104 GPIO6，`qcom,bt-fmd-clk-gpio` | 同上 |
| SW_CTRL | TLMM **gpio18**，`qcom,bt-sw-ctrl-gpio` | 同上；Android 上是 `wcn_sw_ctrl`（func1），**WiFi 驱动会自己把它配成 func1** |
| WLAN reset | TLMM **gpio16**（`qcom,wl-reset-gpio`） | 同上；`pandora_wifi.c` 的 `W_EN` |
| UART 引脚 | gpio24=CTS(l0)、25=RTS(l1)、26=TX(l2)、27=RX(l3)，**func1** | Android pinctrl/`gpio` debugfs |
| BT_WAKE | TLMM gpio9（`mpm_wake_set_gpios = <0x12 0x9>`） | 同上 |
| 电源轨 | 8 条 `bt-vdd-*` 对应 6 个 RPMh 资源 | DT phandle 逐条核对，见 §4 |

**关键结构性事实**：Android 的 BT 与 WiFi 共享同一颗芯片，但 **BT 走 UART（SE5）、WiFi 走 PCIe**；
WLAN_EN（gpio16）由 `pandora_wifi.c` 在 **~20 s** 才拉高，而 BT 驱动在 **0.2 s** 就开始工作。
w63 已证明：即使等到 60 s（WiFi 完全跑起来、WMI 扫描正常），BT TX 依然是 0 —— 所以**不是时序先后问题**。

---

## 2. 两个 Root Cause 级知识（务必先读，能省几天）

### 2.1 PMIC GPIO 的电平不在 `0x45`

mainline `drivers/pinctrl/qcom/pinctrl-spmi-gpio.c`：

```c
#define PMIC_GPIO_REG_MODE_CTL    0x40   // (dir<<4) | (function<<1) | (level<<1)
#define PMIC_GPIO_REG_DIG_VIN_CTL 0x41   // 电源源选择（不是输入！）
#define PMIC_GPIO_REG_DIG_PULL_CTL 0x42   // 5 = pull disabled
#define PMIC_GPIO_REG_DIG_IN_CTL  0x43   // 输入在这里
#define PMIC_GPIO_REG_DIG_OUT_CTL 0x45   // 只有 strength(1:0) + buffer type(5:4)
#define PMIC_GPIO_REG_EN_CTL      0x46   // bit7 = master enable
```

**w44–w65 期间我们把电平写进 `0x45`、把 `MODE_CTL` 写成 0（= 数字输入）→ BT_EN 一直是高阻输入，
芯片从未被使能。** 这段历史解释了此前所有"HCI 无应答"的现象。

**正确写法（`0524ac15`）**：

```
0x40 = (1<<4) | (level<<1)   // direction=output, function=normal, level
0x41 = vin                    // GPIO5 用 vin-1
0x42 = 0x05                   // pull disabled
0x45 = 0x03                   // strength high, CMOS
0x46 = 0x80                   // master enable
```

w66/w67 读回 `mode 0x12 vin 0x1 out 0x3 en 0x80`，与 Android 的
`out high / vin-1 / no pull / push-pull / strength 3` **逐位一致**。

### 2.2 SPMI v8 命令编码

对照 mainline `drivers/spmi/spmi-pmic-arb.c`：

| 项 | 正确值 |
|---|---|
| 通道偏移 | `chnls + 0x200 * apid` |
| 写命令 | `(reg << 4)` —— **bc=0 表示 1 字节**（写 1 会变成 2 字节事务） |
| 读命令 | `(1 << 27) \| (reg << 4)`（EXT_READL，opcode 在 bit31:27） |
| 读数据 | `RDATA0 = 0x18`（不是 0x14） |
| 状态 | `0x08`：bit0=DONE，bit1=fail，bit2=denied，bit3=dropped |
| 命令后 | `delay(10)` 再轮询，避免读到上一次的 DONE |

PMH0104 = SID9（bus1）；GPIO5 PPID `0x98c` → apid 2234；GPIO6 PPID `0x98d` → apid 2235。
`pandora_bt.c` 里 `bt_spmi_write/bt_spmi_read8/bt_spmi_gpio` 已是正确实现（`1464ef3f` + `0524ac15`）。

---

## 3. 代码现状

### 3.1 文件

| 文件 | 说明 |
|---|---|
| `wsl/pandora_bt.c` | 驱动本体（~870 行），含 SPMI、power、时钟、UART、HCI、状态机 |
| `xnu/patches/0476-pandora-bluetooth.patch` | 由 `work/tmp_regen_bt.py` 从 `.c` 重新生成（**改完 .c 必须跑它**） |
| `bsd/conf/files` | 已注册（patch 内 hunk） |
| `bsd/dev/sweet_ufs.c` | 调用 `pandora_bt_init()`（patch 内 hunks） |

### 3.2 boot-arg level 语义（`pandora_bt=N`）

| level | 行为 |
|---|---|
| 1 | 只读：map 窗口、读 RCG/pin 状态，不写任何寄存器 |
| 2 | 完整上电：rail vote → BT_RESET low 100 ms → FMD 高 → BT_RESET high 150 ms → UART 115200 → HCI Reset |
| 3 | 跳过 BT_EN 脉冲，只配 UART + HCI（用于判定 iBoot 交接状态） |
| 4 | level 3 + 轮询 WCN TX 最长 60 s，变高后才开 UART |
| 5 | RCG 128 MHz（M=32/N=75，divider 1）→ **4 Mbps**，用于跨状态接管 |
| **6** | **level 2 + 3 次重试（幂等 power/reset 状态机）** ← 当前最新 |

### 3.3 最近的提交（main）

```
184f8652  bt: idempotent power/reset state machine with 3 retries (level 6)
0524ac15  bt: PMIC GPIO MODE_CTL carries direction+level; DIG_OUT_CTL is only strength
f14e71d9  bt: read DIG_IN_CTL back (EN=0xc0), ant-ldo rail 1800 mV
8f6e9fb3  bt: level 5 4 Mbps takeover (128 MHz RCG, divider 1)
09b24518  bt: level 4 waits for the combo chip before opening the UART
fbf22059  bt: level 3 skips the BT_EN reset pulse
1464ef3f  bt: SPMI EXT_WRITEL bc=0, EXT_READL opcode 1, RDATA0 0x18
```

---

## 4. 已排除的假设（**不要再重复验证**）

| 假设 | 结论 | 证据 |
|---|---|---|
| TLMM 物理地址写错导致 SEA | ❌ 已修复（`97794c79`），基址 `0x0f1xxxxx` | w44 |
| RCG 分频不对 | ❌ 64 MHz 与 128 MHz 都读回精确 | w45 / w64 |
| CTS/RX 的 OE 配置 | ❌ 与 Android 一致（func1、no pull） | w46 / Android pinctrl |
| 时序不够长（150 ms、RTS、BT_WAKE） | ❌ 全部无效 | w48–w60 |
| RF clock 资源名 | ❌ `rfclka1` 正确并投票成功 | w57 |
| FMD/XO 引脚 | ❌ PMH0104 GPIO6 已配 `mode 0x12 vin 0` | w58 / w66 |
| **QMP / PDC** | ❌ **只服务 `vreg_ipa`**（`btpower_aop_set_vreg_param` 唯一调用者） | `tmp_btpower.c:1574` |
| **PMIC GPIO 电平寄存器** | ✅ **是根因之一，已修复**（见 §2.1） | w66 |
| 跨状态继承（Android 4 Mbps） | ❌ `reboot bootloader` 本身复位 WCN，RX 在 0.2 s 就是 0 | w64 |
| WiFi 未起导致 BT 不启动 | ❌ 等 60 s（WiFi 已跑）仍是 0 | w63 |
| 电源轨缺失 | ❌ 8 条 supply / 6 个资源全部投到且成功 | DT phandle 核对 |
| 重试次数不足 | ❌ 3 次幂等重试全部无声 | w67 |

**电源轨映射（DT phandle → RPMh 资源）**，供后续核对：

```
bt-vdd-rfa1p25 → pmh0110_f_s7 → S7F_E0 (1328 mV)
bt-vdd-rfa1p8  → pmh0110_f_s8 → S8F_E0 (1800)
bt-vdd-rfa0p75/aon/dig → pmh0104_s2 → S2J_E1 (892)
bt-vdd12-io    → pmh0110_g_l3 → L3G_E0 (1200)
bt-vdd18-aon   → pmh0110_g_l2 → L2G_E0 (1800)
bt-ant-ldo     → pmr735d_l6   → L6K_E1 (1800)
bt-vdd-wlan-aon→ pmh0104_s1   → S1J_E1 (892，**Android 是 off**，我们多投了，无害)
```

---

## 5. 当前卡点（分层，勿混淆）

| 层 | 状态 | 判据 |
|---|---|---|
| 电源配置 | ✅ | `mode 0x12 vin 0x1 en 0x80 out 0x3` 读回一致 |
| 电源轨 | ✅ | 7 条投票全部返回成功，无 `rail ... failed` |
| 时钟 | ✅ | FMD `0x12`、rfclka1、SE5 64/128 MHz 读回 |
| UART（AP 侧） | ✅ | `M_CMD_DONE`、`tx_fifo 0`、`tx io 0x3` |
| **控制器启动** | ❌ | **gpio27（TX）全程 0；gpio18（SW_CTRL）也是 0** |
| 固件 / NVM | ⏸ | 见 §6 |

**两个独立信号（TX、SW_CTRL）都为 0 —— 这不是"UART 不通"，而是"芯片的 BT 子系统没有从复位中出来"。**

---

## 6. 固件（已提取，暂无法使用）

已从 `/vendor/bt_firmware/image/`（需 root + `chmod 644` 才能 adb pull）提取到本地：

| 文件 | 大小 | 说明 |
|---|---|---|
| `hmtbtfw20.tlv` | 280,368 B | RAMPATCH，TLV tag `01` |
| `hmtnv20.bin` / `hmtnv20.b33` | 9,612 B | NVM，tag `02`，含版本串 |
| `hmtbtfw20.ver` | 35 B | `BTFW.HAMILTON.2.0.3-00035-PATCHZ-3` |

**为什么现在不能用**：QCA 的 EDL 下载（`qca_download_firmware` → `qca_send_vendor_command`）
依赖 **HCI-VS 命令 + 等 Command Complete**，第一步 `qca_read_soc_version` 就会超时。
ROM 连 HCI Reset 都不回，固件一个字节也灌不进去。

**且**：Android 的 `by-name` 里**没有** pandora/metal 分区，UniXNU 的固件容器是裸 LBA 区域，
写入属于 raw write（高风险，且 AGENTS.md 要求不动那块 —— IPA 侧载实验在用）。
要把固件放进内核需要 280 KB 的 C 数组（patch 会暴涨），也不现实。

---

## 7. 工具与脚本（都在 `D:/turnip-local/newboot`）

| 脚本 | 用途 |
|---|---|
| `build_w67.ps1` | 从 CI 产物打包启动镜像（改 `$KERNEL` / `$CMDFILE` 即可复用） |
| `trial_bt_level6_w67.py` | 上机：`reboot bootloader` → `fastboot boot` → 等 adb 回来 → `fetch_log.py` 抓 `log.img` |
| `monitor_w67.py` | 轮询 CI run、下载 `xnu-kernel` artifact、解压、grep 编译标记 |
| `dl_w67.ps1` | **分块下载**（8 MB/Range），CI artifact 单次下载经常卡住时用它（实测有效） |
| `push_w61.py` | 用 Git Data API 单 commit 推送 `.c` + patch（推送后需手动 `workflow_dispatch`，`xnu.yml` 没有 push 触发） |
| `probe_android_se5.ps1` / `probe_android_bt2.ps1` / `probe_android_se5b.ps1` | Android 侧只读采集（pinctrl / gpio / regulator / dmesg） |
| `syntax_check_w61.sh` | 用 NDK clang 做 `-fsyntax-only`（WSL 下跑，路径已适配 `C:/...`） |

**注意**：`push_w61.py` 内含 GitHub token，**不要把内容抄进任何文档或提交**。

**日志解码**：`E:/IOS16/IOS16/tools/fetch_log.py --adb <adb> --serial 2d611f9f --out <dir>`，
产出 `kernel.txt`；蓝牙行一律以 `PANDORA BT:` 开头，`grep 'PANDORA BT'` 即可。

---

## 8. 设备纪律（AGENTS.md，最高优先级）

- **`fastboot boot` 前必须得到用户明确同意**；
- **不写分区**、不 `fastboot flash`、不擅自重启到 iOS；
- 同一时刻只能有一个 agent 操作设备 —— 由于 `xnu.yml` 有 `concurrency`，**推送/dispatch CI 会与另一位 agent 的构建排队**，且 `ndroid reboot bootloader` 会把对方正在进行的采集打断；
- 协作文件：`D:/turnip-local/newboot/COORDINATION.md`（**动手前先读，动完手补一段**）。

---

## 9. 建议的下一步（按优先级）

### A. 逻辑分析仪（**最有价值，能一次定论**）

抓三个点：

| 点 | 判据 | 若异常说明 |
|---|---|---|
| PMH0104 GPIO5 | 写 `0x12/vin-1/en` 后是否出现 1.8 V 高 | PMIC 输出级/电源域问题，与芯片无关 |
| gpio26（AP→WCN TX） | HCI Reset 4 字节是否有波形 | GENI/TLMM 引脚映射问题 |
| gpio27（WCN→AP TX） | 上电后是否出现 idle 高 | 芯片未启动（需固件/32 kHz） |

### B. 若逻辑分析仪证明 BT_EN 正常

按顺序排查：

1. **`susclk`（SUSCLK_32KHZ）** —— DT binding 里是可选 clocks，本板 DT 未提供；但 WCN7850 的 `qca_regulator_enable` 会 `clk_prepare_enable(susclk)`。可查 `live.dts` 是否有 32 kHz 源（`rpmhcc` 的 `s XO` 资源）并投票。
2. **`sw_ctrl` 语义** —— Android 用 `SYNC_GPIO_SOURCE_CURRENT(bt_sw_ctrl)` 读回判断芯片是否起振；我们的 gpio18 目前是下拉输入（读 0）。若芯片依赖 SW_CTRL 的电平选择 **UART vs USB 传输**，则需要在 BT_RESET 拉起前把它设为正确的极性。
3. **固件下载** —— 只有在 ROM 能回 HCI 后才有意义；届时优先实现 `qca_read_soc_version` + EDL，再按 `hmtbtfw20.tlv` / `hmtnv20.bin` 顺序灌入。

### C. 已知的"不必再试"

- 换分频（64/128 MHz 都正确）
- 加长等待（150 ms、60 s 都试过）
- 改 RTS/CTS/BT_WAKE
- 加 rail（已全覆盖）
- 幂等重试（3 次无声）

---

## 10. 一句话给接手的人

**先读后续审计报告，验证 TYPE/SUBTYPE、TLMM bit0、冷启动波特率和 QUP 采样倍数；
旧报告中的“软件全部对齐”和“必须先用逻辑分析仪”已被源码与原始日志证据推翻。**
