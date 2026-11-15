# Level 9：隔离 RX_TRANS_CFG bit1 差异

## 问题与修改

level 8 已实机运行，三次 FD 无 FC，未发送版本请求；详见
[level 8 结果](BLUETOOTH_LEVEL8_RESULT_2026-11-15.md)。内部回环已通过，不覆盖外部路径。

当前 XNU 把 TX CTS_MASK（bit1）也 OR 到 RX_TRANS_CFG。Android 启动 trace
明确记录 RX_TRANS_CFG=0；本机模块 `msm_geni_serial_config_baud_rate` 的
0x9774..0x97ac 只处理 RX parity bit4，0x98c0..0x98cc 写回，没有置 RX bit1。
同 SoC 源码也未定义 RX bit1 为 CTS mask。尚不知道误置该位是否造成无响应。

`pandora_bt=9` 重复 level 8，只把 RX_TRANS_CFG bit1 清零，并在启动接收和发送 FD
之前核对整个寄存器读回。其余位保留；读回不一致返回 EIO，由现有外层停止 sequencer。
level 8 保留此前行为，作为可重现对照；不是把新内核的行为冒充旧实测结果。

电源、BT_EN、GPIO6/9、pinmux、115200/16x、FIFO packing、RX S_CMD0 参数、
FD 重试次数/期限、PERI 版本请求和收尾路径均沿用 level 8。
本轮不混入 RFR_OPEN、BT_EN 输入输出采样、UART break 或固件下载。

改动限定于 `wsl/pandora_bt.c`、对应 0476 patch、host-only packer 的 level 上限及说明。
没有修改其他驱动或调用接口。source diff 存于
`work/bluetooth-source-audit/level9-from-level8.diff`。

## 验证与判据

ARM64 单文件编译及 Clang analyzer 已通过，无诊断。
完整构建和镜像准备状态需以文末更新和实际 trial.json 为准；当前没有 level 9 实机结果。

一次新试验应仅使用带 level 9 支持的完整内核，保留 `pandora_reboot_after=90`。
启动前再次只读核对映射/pinned 状态并保存原 log.img；不得改写共享固件容器。

- RX bit1 清零和读回成功后，观察 FC 是否出现。若出现，仅继续既有版本询问。
- 没有 FC 则仍停在 wake 阶段，不发送版本请求或固件，不追加 power-cycle。
- 有 FC 不代表蓝牙栈恢复；只有完整版本响应匹配才称 ROM probe 成功。
- 若仍无 RX，下一候选可独立验证本机模块已确认的 RFR_OPEN 参数，或先做 BT_EN pad 数字采样。

后续启动需要新的明确独占窗口。本轮“继续”用于候选实现和主机准备，未执行设备启动。

## 完整候选已就绪

候选分支 `codex/bluetooth-no-analyzer-20261115` 的完整构建编号为
43663459546 已成功；main 未更新。源码、0476 patch 和主机工具/说明已保存到该候选分支。
`level9-build-status.json` 记录成功状态；产物中的 level 9 诊断标记已核对。

准备期间只读预检确认 level 8 的 exposed root16GiB、FW64MiB、log64MiB 映射仍匹配
手机且为 pinned；时间和结果在 `work/bluetooth-source-audit/level9-baseline-preflight.json`。
这不是对未来启动窗口的预检替代，启动前需重读。

完整内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/rx-bit1-artifact/kernel.development.pandora`，
17,738,024 字节。临时镜像：
`D:/turnip-local/newboot/bt-no-analyzer-20261115/level9/loader/nezha-loader-boot.img`，
17,285,120 字节。`level9/trial.json` 与 `verification.json` 均记录未启动。

主机结构验证通过：boot v4、payload/ADT 边界，root/FW/log 全部映射、trust cache 和
内存基线与 level 8 一致，启动参数仅 `pandora_bt=8 -> 9`。保留自动返回90秒。
没有写容器或操作设备运行状态；临时下载归档、编译对象及 analyzer 输出已删除，检查记录保留。

下一步只需用户明确同意这一次独占窗口：保存现有日志，再临时 boot 上述镜像，
等待内核90秒自动复位并在 Android 返回后立即取回日志。该启动会覆盖设备 log.img；
旧 level2/7/8 原始证据已分别保存。仍需在真正启动前重做只读映射/pinned 预检。
