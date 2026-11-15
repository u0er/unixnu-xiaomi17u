# Level 11：独立验证S8F请求1876mV

> **已完成实机：[level11结果](BLUETOOTH_LEVEL11_RESULT_2026-11-15.md)。S8F1876电压/使能请求均成功，但三次FD仍无回应；90.218秒自动返回。下文为试验设计和准备记录。**

## 当前状态与依据

蓝牙仍没有恢复。[level10实机](BLUETOOTH_LEVEL10_RESULT_2026-11-15.md)确认RX配置0、
RFR_OPEN命令0x08000080读回成功，三次FD仍无接收，未发送版本请求。

[电源审计](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)已沿本机live DT的consumer配置和
supply phandle确认：BT的S8F_E0要求1876..2000mV，当前BT代码请求1800mV。
现有Wi-Fi驱动对相同资源已经使用1876mV。本次使用该有来源的值，不猜其他电压或rail。

## 最小改动与失败处理

`pandora_bt=11`保持level10的RX配置0、RFR_OPEN、115200/16x、FIFO、BT_EN时序、
FD重试和严格版本询问，仅把BT的S8F_E0电压请求改成1876mV。
旧level1..10的电源请求保持原值；实测source/patch快照独立保留。

`bt_power`逐条发送既有rail请求，仅S8F选择新值，分别打印：

- `level 11 S8F_E0 voltage request 1876 mV -> <返回值>`；
- 电压请求成功后才发送并打印`level 11 S8F_E0 enable request -> <返回值>`。

任一步失败会报告实际请求值和错误，停止后续BT_EN/UART操作。
不关闭或回滚共享rail，不修改Wi-Fi驱动或RPMh接口。RPMh命令使用原有序列化锁；
没有新增分配、映射或资源生命周期。请求成功不是模拟电压测量。

已有模式范围判断随最大level扩到11，level9及以上保持RX bit1清零，level10及以上保持
RFR_OPEN，level8及以上走独立PERI分支；入口仍拒绝所有超出1..11的值。
这样level11不会误入普通HCI或power-cycle循环，旧level行为不变。

不混入MODE2采样、UART break、其他rail/mode/load修改、baud切换、固件或NVM下载。
改动仅蓝牙源文件、同步0476patch、host-only packer的level上限和说明。
差异保存在`work/bluetooth-source-audit/level11-from-level10.diff`。

## 验证状态

ARM64单文件编译与Clang static analyzer均exit0且无诊断；0476新文件内容与源码一致，
registration/caller hunks不变，git patch解析通过。临时对象/plist已删除，记录保留。
完整构建和镜像状态在后续更新；当前没有level11实机结果。

下一次实机必须只使用支持level11的完整内核，保留90秒自动返回。启动前重查映射/pinned，
保存原log.img，再在新的明确许可窗口临时boot；不改共享固件容器。

## 判据

1. 先确认S8F1876mV的电压和使能请求都返回0，否则只报告配置失败。
2. 确认RX配置0、S_CMD0=0x08000080，比较SW_CTRL/CTS/RX和GENI输入状态。
3. 有FC才发送既有版本请求；完整匹配才称ROM probe成功，不称蓝牙已恢复。
4. 仍无RX时结论仅限“请求值对齐后这一路径仍未通信”，不能由投票成功推断实际电压。
   下一独立观测为GPIO5 low/high的INPUT_OUTPUT MODE2/RT_STS数字采样，需保存、恢复并验证。

本轮实现/准备未授权设备启动，没有追加手机状态切换。

## 完整候选已就绪

完整构建43668443548已成功，运行于既有候选分支`codex/bluetooth-no-analyzer-20261115`，
main未更新。状态记录`work/bluetooth-source-audit/level11-build-status.json`。
准备期间只读预检确认level10镜像的boot v4、payload/ADT、trust cache及90秒参数有效，
exposed root16GiB、FW64MiB、log64MiB仍与手机实际映射相符且均为pinned。
详见`level11-baseline-preflight.json`；真正启动前仍须重查。
上述预检时未生成level11镜像；后续已完成主机打包，仍未启动手机。

## Android运行态读数的限制

准备期间只读sysfs报告S8F当前1800mV，同时UART处于suspended，dmesg已在12.110973秒
进入Power Retention；蓝牙开关仍为1。其他共享轨也低于启动consumer范围。
详见[电源审计补充](BLUETOOTH_POWER_RANGE_AUDIT_2026-11-15.md)。这不是反证启动请求范围，
也不是实际电压测量，但意味着不能声称Android全程使用1876或1800必然无效。
level11只验证冷启动请求对齐是否改变通信结果，不预设成功。

## 最终主机准备结果

完整内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/s8f1876-artifact/kernel.development.pandora`，
17,738,112字节。S8F电压/使能诊断标记已核对。
临时镜像：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level11/loader/nezha-loader-boot.img`，
17,285,120字节。`level11/trial.json`及`verification.json`记录未启动。

boot v4、payload/ADT边界、root/FW/log映射、trust cache、内存基线与已实测level10一致，
启动参数只从`pandora_bt=10`改为`11`，保留自动返回90秒。临时下载归档已删除。
没有写固件容器、切换蓝牙或改变手机运行状态。

下一步在新的明确独占许可窗口，重查映射/pinned并备份设备当前log.img，临时boot上述镜像，
等90秒自动复位及Android返回后立即取回日志。启动会覆盖设备log.img；旧level2/7/8/9/10
原始证据已经分别保存。没有level11结果前不得更新蓝牙恢复状态。
