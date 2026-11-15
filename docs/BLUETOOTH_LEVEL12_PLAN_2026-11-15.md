# Level12：直接观测BT_EN数字状态，采用本地增量构建

## 为什么改变排查方式

level8..11都在FD阶段没有收到任何字节；RX bit1、RFR_OPEN和S8F请求的修正
均未恢复通信。此前逐个改参数再等待完整CI，耗时较多，而BT_EN一直只有输出配置读回，
尚无有效输入采样。这份候选先补足这一直接观测，不预设蓝牙会恢复。

## 新增观测

`pandora_bt=12`沿用已实测level11的电源请求、UART和PERI设置，在既有BT_EN低态
100ms等待结束、高态150ms等待结束后分别调用`bt_en_sample`。

- 只接受设备已确认的TYPE0x10/SUBTYPE0x12、输出MODE1、VIN1、正常SOURCE以及
  预期驱动/使能配置，保存真实状态后才写。未知或不匹配状态直接失败。
- 将DIG_IN设为正常输入（0，仅当不同才写），将MODE临时设为2（INPUT_OUTPUT）。
  SOURCE、VIN、PULL、OUT、EN不修改；MODE3是analog pass-through，绝不使用。
- 配置完整读回匹配后，进行三次有界SPMI RT_STS读取，只判断bit0。
  三次样本必须全部等于预期low0或high1。
- 对所有尝试过的配置写入执行恢复，即便原写入报错；恢复MODE后恢复DIG_IN，
  单独执行避免短路跳过，最后重新读回全部配置与保存值比较。
- 分别打印实际样本数、三个原始RT_STS、sample error和restore error。
  任一采样、配置或恢复失败均停止后续上电/协议，不再把无FC当唯一结果。

低态失败时不会继续拉高BT_EN；高态失败不会启动UART。始终保留90秒自动返回。
本次不是恢复进入本轮之前的Android会话，而是恢复采样前的既定低/高输出配置。
MODE1时的RT_STS0不计为有效观测。通过也只证明PMIC数字输入状态，不代表模拟裕量。

没有新增rail修改、GPIO6/9强推、UART break、baud切换、固件或额外power-cycle。
新增函数不分配内存、映射资源或创建并发事务；使用现有单BT线程和有界SPMI函数。

## 验证状态

- ARM64单文件编译、Clang static analyzer通过，无诊断；0476/source一致，
  原registration/caller hunks保持不变。
- 使用level11完整构建43668443548的**匹配buildtree和objects**，本地重编BT对象，
  其余1158个列表对象保留，合计1159个对象完成整核链接，无未解析符号。
- 本地ld64链接约3秒完成。此为完整内核的本地重链接，不是新的CI完整构建。
  本地Clang14保留归档BSD宏/配置及优化参数，只做路径、target和Clang14零初始化参数适配，
  没有生成ABI或类型桩。实际命令、日志和filelist保留。
- 本地编译有两条归档XNU `EXTERNAL_HEADERS/stddef.h`的null-pointer-subtraction警告；
  链接有一条本地不存在Apple framework搜索目录的警告。它们均已保存，未隐藏。
  Windows单文件静态检查与Clang analyzer无诊断。
- 用未改BT的原CI对象也做过本地链接，确认链接工具能完成该对象集合。
  **本地链接器改变了入口位置和部分布局/调试信息，产物不与CI二进制等价**。
  它生成静态unslid绝对指针，没有dyld bind/rebase或chained-fixup命令；loader继续用Mach-O入口。
  候选仍未实机，不能由链接成功声称能启动；本次观察包含本地链接器变化这一限制。
- boot v4、payload/ADT边界、入口/segment、root/FW/log映射、trust cache和内存基线已检查。
  映射及trust cache与level11一致，启动参数仅BT级别11->12，保留90秒自动返回。

内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/pmic-sample-artifact/kernel.development.pandora`，
17,115,592字节。
镜像：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level12/loader/nezha-loader-boot.img`，
17,285,120字节。`trial.json`和`verification.json`记录未启动。

## 可复现的增量入口

现有`tools/relink_nezha_kernel.py`新增`--replace NAME=OBJECT`模式，拒绝重复/缺失对象，
保留旧驱动包模式。部署版本现在从归档LDFLAGS读取，不再硬编码14.0。
`tools/bluetooth/compile_driver.py`从匹配buildtree的BSD `.CFLAGS`编译，记录完整命令。
使用方法见`tools/bluetooth/README.md`。

构建缓存：`/opt/nezha-bt-incremental/ci-43668443548/`，
候选输出：`/opt/nezha-bt-incremental/level12/`。归档副本在
`D:/turnip-local/newboot/bt-no-analyzer-20261115/local-baseline-43668443548/`。
不要换用工作区原来较旧的buildtree/objects；其配置和内容不代表level11。

工作区证据：`work/bluetooth-source-audit/level12-from-level11.diff`、
`level12-{compile,static-analysis,source-check}.txt`、`level12-local-build.json`、
`level12-incremental-{compile,link}.{log,json}`、`local-baseline-comparison.json`、
`level12-preparation.json`。Mach-O静态指针和布局差异记录在local-build及baseline-comparison中。

## 下一步实机判据

在用户同意的新独占窗口，先重查映射/pinned并保存当前log.img，再临时boot上述镜像，
等待90秒自动返回并立即取日志。旧试验原始证据保留，不写固件容器。

1. 若不能启动，先处理本地链接器/镜像问题，不对BT_EN作结论。
2. 低/高样本0/1且恢复成功：数字BT_EN边界得到直接证据，再观察既有FD/FC路径。
3. 样本不匹配：优先查PMIC pad、VIN和外部使能路径，不能再称BT_EN已验证。
4. 采样/恢复错误：记录SPMI错误，停止协议，不把失败伪装成无回应。

本轮没有启动手机；设备状态切换仍需新许可。
