# Level13：按骁龙SM8850原厂实现修正UART时钟

依据：[SM8850时钟原理审计](BLUETOOTH_SM8850_CLOCK_AUDIT_2026-11-15.md)。
旧RCG编码与自定义解码均错误，64MHz结论撤回；不是继续无来源试参数。

## 行为

沿用level12已通过的BT_EN数字采样、S8F1876请求、RX配置0、RFR_OPEN和PERI协议。
时钟路径改为先打印普通RCG和DFS状态：

- DFS bit0开启：只读输出8档CFG/M/N，然后返回ENOTSUP，不直接重写RCG，不发FD。
- DFS关闭：确认wrapper/SE时钟已开启、停止旧sequencer后，按原厂SE5频率表的
  `F(7372800,GPLL0_EVEN,1,384,15625)`设置M384、N0xc476、D0xc2f6、
  mode2/HID1/HW_CLK_CTRL，保留无关位，不写PLL。
- 等待UPDATE清零，核对CFG/M/N/D读回；任何超时/不匹配均停止，不继续算一个“成功频率”。
- 用原厂HID/MND规则重新解码，结果须为名义7372800Hz；否则停止。
- UART115200/16x得到divider4，与本机Android trace的ser_clk_cfg0x41一致。
  日志明确为computed/nominal，不声称测得波形或父PLL频率。
- 正常路径继续既有FD/FC，仅收到FC后才发送版本询问。无固件、baud切换或额外power-cycle。

旧level的错误估算明确标为legacy unverified，原注释“bit20是parked”已纠正。
原实机版本已单独保存，旧镜像不被更新或改写。
本候选不把vendor DMA模式的IO_MACRO0x431c盲拷到FIFO模式。

## 验证

Windows ARM64单文件编译、Clang static analyzer、0476/source一致性通过，无诊断。
原registration/caller hunks未改。原厂频率表各GPLL0_EVEN项经正确编码/解码核对。
本地以已实测level12的构建路线重编BT、链接1159个匹配CI43668443548对象，无未解析符号。
这是本地完整内核重链接，不是新CI完整构建。Clang14归档头文件两条警告及ld64搜索目录警告保留。
本地链接器已经在level12执行到采样、SpringBoard和自动返回；level13自身仍未实机。

完成镜像准备后在文末记录最终路径/结构检查。本轮只获准并完成level12一次启动，
level13没有启动许可，也没有再次改变手机状态。

## 镜像已准备

内核：`D:/turnip-local/newboot/bt-no-analyzer-20261115/sm8850-clock-artifact/kernel.development.pandora`，
17,115,624字节。
镜像：`D:/turnip-local/newboot/bt-no-analyzer-20261115/level13/loader/nezha-loader-boot.img`，
17,285,120字节。`trial.json`及`verification.json`记录未启动。
boot v4、payload/ADT、root/FW/log映射、trust cache和内存基线均检查通过；除BT级别12->13
外启动参数保持相同，90秒自动返回。没有新CI构建，使用已在level12实机启动的本地链接路线。

下一次需要新的独占许可：先重查映射/pinned并备份当前log.img，临时boot此镜像，
自动返回后立即取日志。会覆盖设备log.img，旧level12和之前原始证据已分别保存。
首要判据是DFS状态和原厂RCG配置/读回，而不是把“TX完成”写成通信成功。

## Pad6S Pro上游复用审计补充

用户随后要求优先研究sheng，已完成[逐层复用审计](BLUETOOTH_SHENG_UPSTREAM_REUSE_2026-11-15.md)。
它的实际时钟helper独立确认M/N/D、mode2、7.3728MHz/divider4。
但其手动接管会清DFS和HW_CLK_CTRL，当前level13选择DFS开启时先读表停止，
普通路径设置SM8850原厂HW_CLK_CTRL_MODE；两者不完全相同，不得混称已直接复用。
当前没有继续启动level13，设备窗口保持释放；不导入sheng的Hamilton固件启动流程。
