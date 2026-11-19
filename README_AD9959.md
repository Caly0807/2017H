# AD9959 CH2 串口正弦波测试

本工程使用 STM32H743VG、SPI1 和 USART2，控制 AD9959 的硬件 CH2。
上电后自动设置正弦输出：100 kHz、幅度码 512、相位偏移 0°，其他通道关闭。
时钟按厂家资料的模块参考频率 25 MHz、PLL 倍频 20、系统时钟 500 MHz 配置；
模块 W1 跳帽应选择板载振荡器，且模块应先于控制板完成供电。

## 接线

| AD9959 模块 | STM32 / 处理方式 |
| --- | --- |
| CS | PA1 |
| UPDATE / I/O_UPDATE | PA3 |
| SCLK | PA5 / SPI1_SCK |
| SD0 / SDIO_0 | PA7 / SPI1_MOSI |
| GND | 与 STM32、串口转换器共地 |
| 模块 DC 电源座 | 按所提供模块说明使用 5 V 供电 |
| PDC / PWR_DWN_CTL | 接 GND；不能悬空，也不能让模块进入掉电状态 |
| SD3 / SDIO_3 | 接 GND；单比特串行模式下它是 SYNC_I/O，不能悬空 |
| RST / MASTER_RESET | 接 PA0；程序执行低→高→低复位，随后保持低电平 |

SPI 为 Mode 0、8 位、MSB first、80 MHz / 32 = 2.5 MHz，CS 由 GPIO 控制。
采用 AD9959 的单比特串行 2-wire 模式，只写寄存器，不做回读。
PA6 / SPI1_MISO 不需要连接；模块 SD0 在本程序中是输入数据脚，不是 MISO。
静态正弦模式关闭了调制和扫频，P0~P3 不参与选频。

USB-TTL 转换器必须使用 3.3 V 逻辑电平，接线如下：

| USB-TTL | STM32 |
| --- | --- |
| TXD | PD6 / USART2_RX |
| RXD | PA2 / USART2_TX |
| GND | GND |

注意：本工程 PA3 已用于 UPDATE，串口接收脚是 **PD6**。

## 第一次测试

1. 核对接线、参考时钟和复位状态，下载 `build/Debug/2017H.elf` 或 `.hex`。
2. 串口助手设置为 **115200、8 数据位、无校验、1 停止位、无流控**。
3. 选择 ASCII / 文本发送，关闭十六进制发送，每条命令追加 CR、LF 或 CRLF。
4. 复位 STM32，能收到启动信息；如果错过启动信息，发送 `STATUS`。
5. 示波器连接模块的硬件 CH2 输出和地，先发送 `SET 1MHz 512 0`。

程序按芯片原始编号 CH0、CH1、CH2、CH3 选择通道，CH2 对应 CSR = 0x40。
如果模块厂商把接口标成 CH1~CH4，需要对照原理图；物理第二路可能实际是芯片 CH1。
可修改 `Core/Inc/dds_config.h` 中的 `DDS_OUTPUT_CHANNEL`，允许值 0~3。

## 串口命令

命令与频率单位不区分大小写，参数间使用空格或制表符。
所有设置均保存在 RAM，STM32 重新上电后恢复配置头文件中的默认值。

| 命令 | 含义 |
| --- | --- |
| `SET 1MHz 512 0` | 输出 1 MHz，幅度码 512，相位偏移 0°，并开启输出 |
| `10MHz 800 90` | 简写：输出 10 MHz，幅度码 800，相位偏移 90° |
| `SET 100kHz` | 改为 100 kHz，保留幅度和相位，并开启输出 |
| `FREQ 2.5MHz` | 只修改频率，保持开关、幅度和相位 |
| `AMP 1023` | 满幅度码；范围 0~1023，必须为整数 |
| `AMP 0` | 幅度码设为零 |
| `PHASE 45.5` | 修改相位偏移，单位度；允许 0~359.999° |
| `OFF` | 将所选通道的 DAC 和 DDS 核置于掉电状态，保留参数 |
| `ON` | 按当前参数开启输出 |
| `STATUS` | 显示当前软件设置和时钟配置 |
| `HELP` 或 `?` | 查看命令说明 |
| `INIT` | 重新初始化寄存器并恢复当前设置 |
| `CLOCK 25MHz 20` | 按实际参考时钟重配 PLL，并重新初始化输出 |
| `CLOCK 25MHz 1` | 旁路 PLL；此时系统时钟为 25 MHz |

频率无后缀时单位为 Hz，也支持 Hz、kHz、MHz、k、M；数字和单位不能有空格。
例如 `1000000`、`1000k`、`1M`、`1.0MHz` 都表示 1 MHz。
数字最多接受 6 位小数；频率四舍五入到整数 Hz，相位输入四舍五入到 0.001°。
芯片使用 32 位频率字和 14 位相位字，实际频率分辨率为 SYSCLK / 2^32，
相位分辨率约为 0.022°；STATUS 显示请求值，寄存器值按最邻近值量化。
不接受负数、科学计数法、NaN 或 Inf。频率要求为 `1 Hz <= f < SYSCLK / 2`。
实际高频输出质量取决于模块时钟、滤波器、布线和负载；建议先在 1 MHz 验证。

幅度参数是 **10 位数字幅度码，不是 mV 或 Vpp**。输出电压还取决于模块电路和负载，
需用示波器测量后标定。相位是 DDS 内部相位偏移；单路测量的相位取决于触发参考，
设置相位不会将波形同步到串口命令，也不提供多通道同步。

错误参数不会写入 DDS。超长命令会丢弃到行尾；接收缓冲区溢出或 UART 错误时，
先发送一个空行，再重新发送完整命令。建议逐条发送并等待回复，避免持续大批量粘贴。
没有逐字符回显，需要时打开串口助手的本地回显。

## 模块参考时钟与复位

修改永久默认值的位置：`Core/Inc/dds_config.h`。

- `DDS_REFERENCE_CLOCK_HZ`：实际参考频率，默认 `25000000UL`。
- `DDS_PLL_MULTIPLIER`：默认 `20U`，`1U` 表示旁路，也接受 4~20 倍。
- `DDS_START_FREQUENCY_HZ`、`DDS_START_AMPLITUDE`、`DDS_START_PHASE_MDEG`：启动波形。

运行时 `CLOCK` 命令改变的是芯片的 PLL 配置及软件计算依据，不会改变模块晶振。
错误的参考频率会造成实际输出频率按比例偏差。启用 PLL 时，输入参考应为 10~125 MHz，
系统时钟只接受数据手册保证的 100~160 MHz 或 255~500 MHz；旁路时接受 1~500 MHz。
模块若使用无源晶体，还需满足晶体模式的 20~30 MHz 及硬件 CLK_MODE_SEL 要求。

厂家例程每次初始化均对 AD9959 执行硬件复位。当前工程也默认使用 PA0 控制模块 RST：

```c
#define DDS_RESET_PORT GPIOA
#define DDS_RESET_PIN GPIO_PIN_0
```

驱动等待 500 ms 后，保持 RST 低电平 1 ms，再给出 1 ms 高脉冲，随后保持低电平。
每次启动及发送 `INIT` 或有效的 `CLOCK` 命令时，都会执行此复位流程。
请断电后将模块 RST 接到 PA0、PDC 和 SD3 接到 GND，随后重新下载固件。
RST 不应再悬空，也不要将接 PA0 的 RST 同时直接接地，否则会与复位高脉冲冲突。
SPI 中途超时可能使模块停留在未完成的通信帧中；CS 拉高仅暂停通信，不能清除该帧。
收到 `ERR SPI transfer` 后可发送 `INIT`，通过已连接的 PA0 复位模块后重新配置。
如果将 `DDS_RESET_PORT` 改为空，程序将跳过硬件复位；此时不能依靠 `INIT` 恢复帧同步。
如果改用其他 GPIO 端口，应先在 CubeMX / `MX_GPIO_Init` 中启用对应 GPIO 时钟。
不要将驱动复位输出接到被其他器件主动驱动的 RST 网络。

`OK` 仅说明 STM32 的 SPI 传输函数返回成功。本接法没有寄存器回读或 PLL 锁定检测，
模块缺电、未接线或未锁定时也可能回复 `OK`，必须通过示波器确认实际输出。

## 构建与文件

在本机 VS Code 中按 `Ctrl+Shift+P`，选择 `Tasks: Run Task`，再选择
`STM32: Build and Flash (ST-LINK)`。该任务先编译 Debug，成功后通过 ST-LINK/SWD
下载 HEX、校验并复位运行，无需进入调试会话。请先停止其他正在占用 ST-LINK 的调试会话。
终端出现 `Download verified successfully` 表示下载及校验成功；
随后通过 USART2 发送 `STATUS`，确认应用已执行。

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

构建自动生成 `2017H.elf`、`2017H.hex`、`2017H.bin`。
BIN 的 Flash 下载地址是 `0x08000000`；ELF 和 HEX 已带地址。

本机 VS Code 工作区已通过 `.vscode/settings.json` 选择 D 盘 STM32CubeCLT 1.20.0
提供的 CMake、Ninja 和 GCC 13.3 工具链。此前插件自带 GCC 14.3 在链接阶段出现
`liblto_plugin.dll: error loading plugin`，不能把该次构建视为烧录成功。
若 VS Code 仍显示旧工具链，执行 `Developer: Reload Window`，再选择 Debug 预设，
执行 `CMake: Delete Cache and Reconfigure`，然后 Build。不要同时在终端和插件中构建。
这些工作区路径是本机配置；迁移到其他电脑时需改为该电脑的工具安装路径。

驱动在 `Core/Src/ad9959.c`，串口命令在 `Core/Src/dds_console.c`。
主循环只负责调用 `DDS_Console_Process`；接收中断只收字节，不执行 SPI 或打印回复。
`2017H.ioc` 已同步 SPI、GPIO 空闲电平和 USART2 中断设置；CubeMX 重新生成后
仍需保留 USER CODE 区域，并保留顶层 CMake 中两个新增源文件及固件导出命令。

寄存器依据：Analog Devices《AD9959 Data Sheet》，Rev. C，Tables 31~37；
SDIO_3 接地要求见 Pin Function Descriptions / Serial I/O Modes of Operation。
官方资料：https://www.analog.com/media/en/technical-documentation/data-sheets/AD9959.pdf
