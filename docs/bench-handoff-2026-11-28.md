# 2026 年 11 月 28 日联调交接

## 这次做了什么

- 现场在 AD8367 的 `OUTPUT` 测到 **1.22 Vpp**。这是放大器输出，不是 DDS 直出，也不是 ADC 读数。现场报告按 `RESET` 后才出现波形，开机偶发无波的原因未定位。
- 电脑通过串口完成 DDS 开关、两档幅度对照和 1～40 MHz 扫频。对 37 MHz 单独复测，保留了首次异常和复测记录。
- 用 ST-Link 把 40 点回放程序加载到 STM32 的 RAM。读回检查了 DAC 码和帧周期；现场接示波器看到了台阶。没有刷写 Flash。

## 接线与数据

本轮用队友仓库 `6c53155` 的串口固件，`AD9959 CH2 → AD8367 → AD835 两路输入 → OUTPUT1 → 低通 → ADS1256 模块 AIN2`。模块端子 `AIN2` 对应芯片通道 `1`，串口显示 `AIN1-AINCOM`。ADC 设为 `100 SPS`、`PGA=1`、`VREF=2500mV`、`CAL=ON`。串口是 `115200 8N1`。

ADC 曾配置为 `100 SPS`，但 `N` 实际每秒增加约 9,830，期间报出的 1.029 V 不用于分析。通过 `ADC RATE 10` 再回到 `ADC RATE 100`，复核 `N`、`DRDY_EDGES` 和样本新鲜度后，确认恢复约每秒 100 笔。扫频时不要只看 `ADC OK`。

| 检查 | 结果 | 原始记录 |
| --- | --- | --- |
| 100 kHz 幅度对照 | 关断底值约 151.0 mV；幅度码 `256` 约 163.5 mV；恢复 `512` 后约 201.4 mV。两档扣底值后约为 12.5 mV 和 50.4 mV。 | [数据](../examples/bench/2026-11-28/100k-live-ab-20261128-160356.csv) · [串口记录](../examples/bench/2026-11-28/100k-live-ab-20261128-160356.log) |
| 1～40 MHz，幅度码 `256` | 首次有 39 点读到响应，范围约 210.79～216.12 mV。37 MHz 首次回到底值，单独复测 5 次均为约 215.63～215.65 mV，不能当作固定频响凹点。 | [40 点](../examples/bench/2026-11-28/band-live-256-20261128-160445.csv) · [原始曲线](../examples/bench/2026-11-28/band-live-256-20261128-160445-detector.svg) · [37 MHz 复测](../examples/bench/2026-11-28/37mhz-retest-20261128-160617.csv) |
| 早期无有效响应的对照 | DDS 关闭后约 150.30 mV；打开后 40 点仍在 150.23～150.35 mV。后续有响应的扫频与这一轮分开归档。 | [早期 40 点](../examples/bench/2026-11-28/uart-sweep.csv) · [档位对照](../examples/bench/2026-11-28/uart-spotcheck.csv) |

`U` 是低通后的检波电压。频率来自 DDS 命令，未独立测频。幅度码不等于实际 `Vpp`，这组数据支持检波通路有响应，不构成逐频幅度标定。

## 示波器回放

只用一根探头：尖端接 STM32 的 `PA4`，地夹接同一块板的 `GND`。不要把 `PA5` 当 DAC，它用于 DDS 时钟。示波器选 `CH1`、DC 耦合、`1 MΩ`，探头与菜单都设为 `×10`。时基先用 `5 ms/div`、垂直档先用 `500 mV/div`，选择 `CH1` 上升沿触发，电平约 `0.4 V`，触发位置靠左。不用 `X-Y` 模式。

触发后第一个台阶是 1 MHz，之后每 1 ms 对应下一个频点，39 ms 处是 40 MHz。每帧前 40 ms 是数据，后 10 ms 拉低用于触发下一帧。后半段较平，可用 `100 mV/div` 放大查看。37 MHz 的回放值来自 5 次复测的中位数；[回放点表](../examples/bench/2026-11-28/display-points.csv) 同时保留了首次值。纵轴把 210～217 mV 的检波读数线性映射到标称 0.35～2.8 V 的 DAC 显示窗口，不代表放大器的绝对幅度。

### 从仓库复现

在仓库根目录打开 PowerShell，将 `$clt` 改为本机的 STM32CubeCLT 安装目录。下面的构建不连接板卡。已在队友串口固件运行于 64 MHz 的 STM32H743VGT6 上验证回放；其他时钟或占用 `PA4` 的固件不适用。

```powershell
$clt = 'C:\ST\STM32CubeCLT_1.18.0'
python .\tools\build-scope-replay.py --cube-clt "$clt" --output .\build\scope-replay
```

确定板卡、探头和 ST-Link 后，在另一个终端启动 GDB Server，把 `PROBE_SN` 换成实际序列号。等待服务器监听，再回到第一个终端运行 GDB。

```powershell
& "$clt\STLink-gdb-server\bin\ST-LINK_gdbserver.exe" -d -g -i PROBE_SN -p 61234 -cp "$clt\STM32CubeProgrammer\bin"
```

```powershell
Push-Location .\build\scope-replay
& "$clt\GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe" -nx -batch .\replay.elf -x .\replay.gdb
Pop-Location
```

GDB 脚本仅加载 RAM，确认首末 DAC 码和帧时长后断开，板子继续回放。回放期间原串口任务暂停；按板上复位后回到 Flash 中的固件。看图前不要复位。若 GDB 报错而串口无响应，复位回原固件再检查。

## 发挥项验收状态

| 项目 | 实测状态 |
| --- | --- |
| （1）+5 V、600 Ω、1 Vrms 无明显失真 | 未验收。1.22 Vpp 不是此项所需的约 2.83 Vpp，也没有带载记录。 |
| （2）1.5 m 双绞线、独立测频及示波器显示 | 示波器已看到 RAM 回放，双绞线与接收端实测未完成。 |
| （3）自建 Wi-Fi 局域网与笔记本曲线 | 旧实测包和模拟包通过本机 HTTP 收包；绑定本机 WLAN 地址也通过。同一台电脑的测试不算跨设备 Wi-Fi，装置无线发送未验收。 |

下一步先在固定增益、稳定激励下，用实际 `Vpp` 和检波电压做多档标定。留一档独立检查，确认能反算幅度，再做实时显示和发挥项整机验收。
