# Level 11：独立验证S8F请求1876mV

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
