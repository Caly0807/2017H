# 开发与复现

## 改动在哪

| 内容 | 看这里 |
| --- | --- |
| ADC 通道与 DRDY 采样 | [ADC 配置](../Core/Inc/adc_config.h)、[ADS1256 驱动](../Core/Src/ads1256.c)。DRDY 仍接 PB0，改为 GPIO 轮询；同一次低电平只读取一笔。 |
| 自动扫频、100 kHz 对照 | [采集状态机](../Core/Src/auto_sweep.c)。扫频模式在复位后逐点采样，数据留在 RAM。对照模式用关、开、关判断检波直流是否随幅度变化。 |
| DRDY 诊断 | [定时器诊断](../Core/Src/drdy_diag.c)。这是单独的构建模式。按代码核对探测引脚，不能用它替代 PB0 的正常接线。 |
| 构建、SWD 读回和报告 | [工具目录](../tools)。烧录工具不随构建自动运行。 |

## 先不接板跑一遍

在仓库根目录打开 PowerShell，安装 Python 3 后运行：

```powershell
python .\tools\sweep_report.py .\examples\simulation\capture --output .\build\demo-report --calibration .\examples\simulation\calibration-simulation.csv --xy-full-scale-vpp 1
```

这会生成 CSV、SVG 和示波器输出用的数字 `xy.csv`。示例数据是模拟数据，不能当作硬件标定。两份实测摘要放在 `examples/bench`，记录 100 kHz 对照的原始码。两类数据不能混用。

`sweep_net.py` 提供电脑端发送和接收。只做过本机回环验证，不代表板卡 Wi-Fi 已实现。数字 X–Y 码也不代表示波器输出电路已经接通。

## 编译和读回

将 `$clt` 改为你安装的 STM32CubeCLT 根目录。本分支用 STM32CubeCLT 1.18.0 验证。以下命令只编译，不接触板卡：

```powershell
$clt = 'C:\ST\STM32CubeCLT_1.18.0'
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-dev.ps1 -CubeCltRoot $clt -Mode Detector -DetectorChannel 0
```

`-Mode Console` 为日常串口；`-Mode Sweep -AmplitudeCode 16` 为扫频流程测试；`-Mode Drdy` 为定时器诊断。编译后命令会打印 `build/dist` 下的包路径。每个包有固件文件、哈希和构建信息。

板上运行对应的对照固件后，复位并等约 23 s，再填入实际下载器序列号和板上对应的包路径：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\read-sweep-swd.ps1 -CubeCltRoot $clt -ProbeSerial '<ST-Link SN>' -FirmwareDirectory '<对应固件包>' -DetectorTest
```

扫频固件读回时去掉 `-DetectorTest`，等待采集结束再读。读回脚本检查固件类型和记录是否齐全，不能把其他构建的 ELF 当作板上正在运行的版本。旧版 `read-adc-swd.ps1` 仅用于脚本内固定哈希的原固件，不能用于本分支新编译的固件。

需要烧录时，先看接线、确认目标板和供电，再使用 `flash.ps1`。只给 `-CubeCltRoot` 时列出下载器；执行实际烧录还需显式提供 `-ProbeSerial`、`-FirmwareDirectory` 和 `-ConfirmTeammateBoard`。脚本检查固件哈希和设备信息，并在写入前备份两组 Flash。不需要烧录即可阅读和复现软件示例。
