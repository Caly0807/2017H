# 2017H 工程重命名与 GitHub 初始化计划

> **For agentic workers:** 使用 executing-plans 按任务执行，并在提交及上传前验证结果。

**Goal:** 将用户的完整 STM32 工程更名为 2017H，上传到其 GitHub 新建仓库以支持团队协作。

**Architecture:** 保留现有硬件配置和应用行为，同步 CubeMX、CMake、烧录任务及文档中的项目名称。所有源码和依赖驱动纳入版本控制，本机 build 缓存和备份保留在本地。

**Tech Stack:** STM32H743、CubeMX、CMake、ARM GCC、Git、GitHub。

**Spec:** 用户请求：将 D:\STM\DDS_Test 上传到自己的 GitHub，新建名为 2017H 的仓库，并将项目名称改为 2017H。

## 全局约束

- 保留 DDS、ADC 和串口的现有行为及接线配置。
- 使用已登录账号 T-Darren，新仓库默认设为私有。
- 不上传登录凭据、本机构建缓存和历史备份。
- 用户已授权创建仓库、必要的初始提交和上传；不执行固件烧录。

## 执行任务

- [x] 同步 CMake、CubeMX 文件名与工程名、VS Code 构建及烧录目标。
- [x] 补充 README 与协作流程，检查待上传文件。
- [x] 原目录被其他进程占用，保留为备份，将源码复制至 D:\STM\2017H，并在新路径重新配置、编译。
- [x] 初始化 Git、创建私有仓库、提交并推送 main。
- [x] 核对远程提交与本地提交一致；仓库地址及使用说明已写入 README。

## 验证记录

- 在 D:\STM\2017H 全新配置及编译成功；VS Code 配置与构建任务再次执行成功。
- 生成 2017H.elf、2017H.hex、2017H.bin；Flash 使用 51656B，DTCMRAM 使用 3704B。
- 132 个工程文件纳入版本控制；其中 111 个 Core / Drivers 文件与原目录逐字节一致。
- build 目录未暂存；常见私钥及 GitHub token 格式扫描未发现匹配。
- GitHub 私有仓库：https://github.com/T-Darren/2017H，默认分支 main。
- 原目录因 Windows 进程占用而保留，后续开发使用 D:\STM\2017H。
