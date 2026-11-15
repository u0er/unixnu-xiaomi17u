# 蓝牙电源范围复核：S8F 请求值与 DT 不一致

## 已确认的差异

**当前蓝牙代码请求 `S8F_E0=1800` mV，而本机 live DT 的蓝牙 consumer 配置为
1876000..2000000 µV。现有 Wi-Fi 驱动对同一资源使用1876 mV。**

这否定旧交接里“S8F1800与Android完全对齐”的说法，但尚未证明这就是冷启动无回应的原因。
本轮只读 Android DT、审计主机源码和已复制的模块；没有改电源投票或当前 level10 候选。

## 证据链

1. Android 实际 `bt_power` 驱动绑定 `soc:bt_peach`，其 of_node 是 live DT 的
   `/soc/bt_peach`，compatible=`qcom,peach-bt`。
2. `qcom,bt-vdd-rfa1p8-config` 的五个 big-endian u32 解码为
   `(1876000, 2000000, 0, 1, 0)`，其 supply phandle=0x450。
3. phandle0x450 对应 `/soc/rsc@18900000/drv@2/rpmh-regulator-s8f-e0/`
   `regulator-pmh0110-f-s8`。该 provider 的全局允许范围1800000..2000000µV，
   不能用 provider 的宽范围代替蓝牙 consumer 的下限1876000µV。
4. 同 SoC `btpower.sm8850.c:dt_parse_vreg_info` 将 config 的前两项读入 min/max；
   本机 `dt_parse_vreg_info` 的0x56b4..0x56d4也进行相同的转换与保存。
   `vreg_configure` 的0x3bac..0x3bd0读取这两个字段并传递给配置调用；同 SoC
   源码对应 `regulator_set_voltage(reg, min_vol, max_vol)`。
5. `wsl/pandora_bt.c:bt_power` 的静态表仍为 `{"S8F_E0",1800}`。
   `pandora_rpmh_resource` 在 `wsl/pandora_adsp.c`，将传入值直接写入 CMD-DB
   解析出的资源+offset；调用本身不做 Linux regulator consumer 范围聚合。
6. `wsl/pandora_wifi.c:wifi_power` 的同一资源值为1876。Wi-Fi验证结论不受本次审计改变，
   此处仅用于共享资源对应关系的交叉核对。

| 资源 | live DT 蓝牙 consumer 范围（mV） | 当前 BT 请求（mV） |
|---|---|---|
| L2G_E0 | 1800..1800 | 1800 |
| L3G_E0 | 1200..1200 | 1200 |
| S2J_E1（aon/dig/rfa0p75共享） | 892..1036 | 892 |
| S7F_E0 | 1328..1340 | 1328 |
| S8F_E0 | 1876..2000 | **1800** |
| L6K_E1 | 1800..1860 | 1800 |
| S1J_E1（wlan-aon） | 892..1000 | 892 |

表格只比较声明范围与源码请求，不能据此声称全部 rail 在实机都已满足要求，
也不能推断可选 consumer 是否应当启用或其他 mode/load/retention 设置正确。

## 时序与未知

level9 日志中 BT在0.202秒报告 rail vote完成，0.345秒拉高BT_EN，1.099秒结束失败探测；
Wi-Fi最早诊断出现在20.218秒，其源码的board power阶段会请求S8F1876。
这支持“BT探测早于Wi-Fi共享轨设置”的源码/日志时序判断，**没有量到模拟电压**。
不能因为后来的Wi-Fi工作，就认定0.2秒时BT用的电源配置已相同。

也不能把历史60秒等待当作排除证据：此前的PMIC编码、UART采样和协议基线曾错误；
而已完成的level8/9都在约1.1秒结束探测，没有重新投票后重做正确PERI唤醒。

原始记录在 `work/bluetooth-source-audit/`：
`android-btpower-live-dt.json`、`android-btpower-supply-nodes.json`、
`bt-power-range-audit.json`、`dt_parse_vreg_info.disasm.txt`、`vreg_configure.disasm.txt`。
供应节点只通过读取DT归档到主机内存解析，未向手机写文件。

## 后续实验边界

level10仍仅测试RFR_OPEN，**没有修正S8F请求值**。如果后续单独测试此电源差异，
应保持一个已记录的UART/PERI基线，仅将蓝牙S8F请求改为已验证Wi-Fi使用的1876，
检查RPMh返回值，保留90秒自动返回，并在新的明确许可窗口试机。
不要同时改BT_EN采样、共享rail关闭、固件或baud；不要把DT范围符合写成实测电压恢复。

## 后续level10结果

[level10已完成实机](BLUETOOTH_LEVEL10_RESULT_2026-11-15.md)，RFR_OPEN读回成功，
三次FD仍无回应，S8F仍请求1800mV。因此下一项应独立验证S8F1876mV修正；
当前尚未实现新候选，没有将源码请求差异宣称为实测电压或已定位根因。

## Level11准备期间的运行态对照

只读采集`/sys/class/regulator/`的相关name/microvolts/state/opmode/num_users，
保存在`work/bluetooth-source-audit/android-bt-regulator-sysfs.json`。
当前Android的S8F报告1800000µV，S7F1224000、S2J500000、S1J400000、
L6K1700000；L2G1800000、L3G1200000。不能忽略这些与启动consumer范围不同的值。

同次只读上下文`android-bt-regulator-sysfs-context.json`确认：
settings的bluetooth_on=1，但UART runtime_status为suspended；Android dmesg在
10.077876秒请求Power ON，在12.110973秒请求Power Retention，采集uptime约975秒。
同SoC源码`vreg_configure(...,retention=true)`会把consumer的最低请求设为0，
允许provider或其他consumer采用不同值。因此当前休眠/保持态读数不等同于启动要求。
这些只是驱动报告值，仍不代表模拟电气测量。

上述对照进一步限制结论为“BT冷启动源码请求与active配置范围不一致”，
不能写成“Android总是1876mV”或“1800mV必然无法通信”。
[1876mV的level11试验](BLUETOOTH_LEVEL11_RESULT_2026-11-15.md)已完成实机：电压/使能请求均返回0，但三次FD仍无回应。

## Level11实机回填

[level11](BLUETOOTH_LEVEL11_RESULT_2026-11-15.md)的1876mV电压请求与使能请求均返回0，
RX0、RFR_OPEN0x08000080读回成功；三次FD仍无接收。请求值对齐没有恢复该次冷启动通信，
不能将范围差异宣称为已定位根因，也不能把请求完成当实际电压测量或电源问题全部排除。
下一项独立软件观测为BT_EN低/高的MODE2/RT_STS数字采样，尚未实现新候选。
