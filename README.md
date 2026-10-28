# Wintermute ESP32 — Metalio E-Ink 4

基于 ESP-IDF 5.5.4 的显示、触摸、SD 卡与震动测试固件。
硬件为 ESP32-S3、16 MB Flash、Octal PSRAM，屏幕为 SSD1677 / GDEM0397T81，800×480。
显示接入 LVGL 9.5.0；触摸使用官方 CST816S 1.0.6 组件；SD 使用 IDF 的 SDMMC + FatFS。

## 目录与职责

```text
config/
  board_config.h          板型配置：引脚、地址、时钟，中文注释

drivers/                  驱动层：每种硬件独立文件夹，可复用
  ssd1677/                墨水屏协议与波形
  cst816s_touch/          触摸原生采样、IRQ、事件队列
  tca9555/                IO 扩展器寄存器操作
  sdcard/                 SDMMC 1-bit 与 FAT 文件系统
  vibration/              GPIO 马达脉冲、自动关闭与停止

board/                    板级层：本板接线、上电顺序、设备创建
  include/board.h         统一板级入口
  board.c                 电源、触摸复位与初始化、BOOT
  board_i2c.c             共享 I2C 的内部初始化与恢复
  board_display.c         屏幕 SPI 和面板创建
  board_sdcard.c          本板 SD 卡挂载配置
  board_vibration.c       本板马达引脚、极性和脉冲配置

middleware/lvgl_port/     适配层：对应用统一提供 LVGL 初始化与全刷
  include/lvgl_port.h
  display/                墨水屏刷屏任务
  input/                  触摸坐标变换、LVGL 输入适配

main/                     应用层：不配置引脚、不拼装驱动
  app_main.c              固件入口，仅选择应用模式
  apps/                   硬件测试流程、原始屏幕诊断
  ui/                     LVGL 测试页面
  tests/                  SD 文件校验、显示测试图

tests/metalio/            无需设备的主机测试
```

边界约定：

- **配置只由 `board` 读取**。应用不包含 `board_config.h`，不直接配置 GPIO、I2C 或 SPI。
- **驱动不依赖板型、LVGL 或应用**。通过参数传入引脚和总线；换板时仍可复用。
- **板级层组装硬件**。只处理设备与资源，不创建页面，不包含测试流程。
- **LVGL 适配层接收板级设备**，使用驱动完成显示和输入适配；不包含业务页面。
- **应用调用 `lvgl_port` 和 `board`**，负责按键行为、测试流程和结果显示。
- 原始屏幕诊断为了验证波形/位平面，明确直接依赖 `ssd1677`；普通 LVGL 应用不依赖这些接口。

独立文件夹表示独立职责，不要求每个小文件都注册成一个组件。
I2C 初始化、引脚配置、显示板级初始化均收在一个 `board` 组件；
显示与触摸适配收在一个 `lvgl_port` 组件。工程共 5 个驱动组件、1 个板级组件、1 个适配组件。
根 `CMakeLists.txt` 用 `EXTRA_COMPONENT_DIRS` 注册各目录。

## 常用入口

| 需求 | 接口/位置 |
| --- | --- |
| 修改引脚、频率 | `config/board_config.h` |
| 初始化板卡电源 | `board_init()` |
| 初始化 LVGL 显示和触摸 | UI 任务内调用 `lvgl_port_init()` |
| 请求全刷 | `lvgl_port_request_full_refresh()` |
| 读取 BOOT 状态 | `board_boot_pressed()`，消抖和长短按由应用处理 |
| 请求短震 / 停止 | `board_vibration_pulse(ms)` / `board_vibration_stop()` |
| 读取马达输出状态 | `board_vibration_is_active()`，不代表检测到实际震动 |
| 挂载 SD 并获取容量 | `board_sdcard_mount()` |
| 访问文件 | `board_sdcard_path()` 获取路径，再使用标准文件 API |
| 卸载 SD | 关闭所有文件后调用 `board_sdcard_unmount()` |
| 修改测试页面 | `main/ui/lvgl_demo_ui.c` |

普通应用无需知道共享 I2C 句柄或 TCA9555 电源位。
新增外设时，先在 `drivers/` 添加驱动，再由 `board` 配置接线，最后由应用使用。
驱动复用于其他 ESP-IDF 5.5 工程时，复制所需目录并在构建中注册；第三方依赖版本随组件 manifest 声明。

## 编译与烧录

在 ESP-IDF PowerShell 中运行：

```powershell
cd D:\Github\wintermute-esp32
idf.py build
idf.py -p COM9 flash monitor
```

普通 PowerShell 先加载本机环境：

```powershell
$env:Path = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;' + $env:Path
. D:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
```

`COM9` 来自当前 VS Code 配置，变化时替换为实际串口。退出监视器按 `Ctrl+]`。
工程提供 `sdkconfig`、`sdkconfig.defaults` 和 `partitions.csv`；新配置先执行 `idf.py set-target esp32s3`。
首次构建会下载锁定版本的 LVGL 和官方触摸组件。烧录会替换当前应用和分区表，无需 `erase-flash`。

## 默认测试固件

- **NEXT +1 / RESET**：增加或清零计数，并改变进度条。
- **FULL REFRESH**：强制全刷；四角 **TL/TR/BL/BR** 用于检查触摸方向。
- 松手后显示最后的原生坐标与屏幕坐标。
- **BOOT 短按**增加计数并触发 35 ms 短震，**长按 1 秒**请求全刷。
- **SD 按钮**显示测试结果与卡容量，点击可重测。
- **VIBE 35ms / VIBE 200ms / STOP**：短震、长一点的震动、立即停止；下方显示请求与输出状态。

显示首帧全刷，后续变化局刷；10 次成功局刷后的下一次变化会全刷清残影。
静止页面不反复刷新。刷屏、触摸采样、SD 测试各自执行，所有 LVGL API 都在 UI 任务调用。
全刷出现黑白闪烁属于波形过程，完成以串口 `Frame ... complete` 为准。
当前 LVGL 页面使用 1-bit 黑白，未加载中文字库。

CST816S 可能休眠后不响应 I2C；初始 `NO TOUCH` 时点击屏幕唤醒，驱动在 IRQ 后重试。
原生坐标 480×800 转为横向 800×480：`x = raw_y`，`y = 479 - raw_x`。
原生 Y=900 的盖板虚拟键不属于有效屏区，过滤后不会误触屏幕边缘。

SD 测试使用 FAT 卡，建议 FAT32；当前不启用 exFAT，不会自动格式化。
自动挂载后，以 `O_CREAT | O_EXCL` 创建新的 `Wxxxxxxx.TST` 文件，写入 4096 字节、同步、
关闭、重读校验，再删除自建文件并卸载。已有同名文件会跳过，不覆盖。

- **SD: PASS**：挂载、读写校验、清理和卸载全部成功。
- **SD: NOT READY**：未插卡、连接异常或不支持的文件系统；看串口错误。
- **SD: TEST FAILED**：文件操作或卸载失败；串口会说明步骤和可能遗留的测试文件。

测试期间不要拔卡。成功卸载后可更换卡再点击重测；未实现热插拔检测。
没有触摸或 SD 卡时，显示和 BOOT 测试仍可使用。

## 震动测试与复用

马达采用原项目的 **GPIO44、高有效、35 ms 默认脉冲**。配置集中在
`config/board_config.h`；板级初始化在外设电源打开前尝试将马达输出置为关闭，启动不自动震动。
驱动通过 `esp_timer` 自动关断，调用不等待脉冲结束，不阻塞 LVGL、触摸或 SD 任务。
连续点击会从新请求时刻重新计时；单次上限为 1000 ms，超限返回错误，不提供无限常开接口。

验收方法：点击 35 ms，应感到一次短震；点击 200 ms，时长应明显增加；在 200 ms 期间点 STOP，
应提前停止。重复点击后应在最后一次请求结束时停止。无触摸时可短按 BOOT 验证 35 ms。
墨水屏显示比马达动作慢，短脉冲可能结束后才显示状态，以手感和串口日志一起判断。
`OUTPUT OFF` 只表示软件已关闭 GPIO，板上没有震动反馈传感器，不能据此判定马达硬件通过。

驱动不依赖板型、LVGL 或测试代码，复用时通过 `vibration_config_t` 传入引脚、有效极性、
默认脉宽和上限；直接驱动 API `vibration_pulse(handle, 0)` 表示使用默认脉宽，停止使用
`vibration_stop()`。当前实例与定时器常驻到重启，同一引脚只创建一个实例。
默认固件增加震动测试；原始屏幕诊断仍只测试显示，马达保持关闭。

## 原始屏幕诊断

`idf.py menuconfig` → `Wintermute display` → `Display application`，选择
`Raw panel diagnostic (black/white/gray/partial)`。
此模式依次显示白屏、黑屏、四灰阶、黑白测试图、5 次局刷计数，然后让面板休眠。
短按 BOOT 重跑；结束日志为 `Display test complete. Press BOOT to run again.`。
该模式不启动触摸、SD 或震动测试；切回 `LVGL display, touch, SD card and vibration test` 恢复默认固件。

## 接线与排查

| 信号 | 连接 |
| --- | --- |
| 屏幕 MOSI / SCLK / CS | GPIO8 / GPIO14 / GPIO45；SPI3、10 MHz |
| 屏幕 DC / RST / BUSY | GPIO13 / GPIO18 / GPIO9（BUSY 高有效） |
| 共享 I2C SDA / SCL | GPIO41 / GPIO42 |
| TCA9555 / CST816S | 0x20、100 kHz / 0x15、400 kHz |
| 触摸 INT / RST | GPIO1 / TCA9555 P1.1 |
| 总电源 / 屏卡座电源 | TCA9555 P0.6 / P0.5 |
| SD CLK / CMD / D0 / DAT3 | GPIO38 / GPIO40 / GPIO39 / GPIO46；1-bit、20 MHz |
| BOOT | GPIO0，低有效 |
| 马达开关 | GPIO44，高有效；默认 35 ms，最大单次 1000 ms |

`BOARD_IO_*_MASK` 是 TCA9555 扩展口位，不能当成 ESP32 GPIO。
GPIO46 仅输入上拉；USB MUX 保持调试通路；关机脉冲脚 P1.3 保持高。

`TCA9555 missing` 检查电源与 I2C；`BUSY ... 15 seconds` 检查屏幕排线、电源、BUSY/RST。
日志完成但屏幕不变时仍需检查实物，SPI 不会读回面板身份。
触摸持续不可用时检查 GPIO1、共享 I2C、触摸排线及 P1.1 复位。

## 验证

在已配置 C 编译器、CMake 和 Ninja 的主机终端中：

```powershell
cmake -S tests/metalio -B build/host-tests -G Ninja
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

震动主机测试使用模拟 GPIO 和定时器，直接链接实际驱动，检查脉冲、自动关闭、停止、重触发、
有效极性、参数与错误处理。硬件测试入口保留在默认 LVGL 测试页中。
主机测试不代替实际震动手感、触摸响应、SD 电气连接和屏幕视觉验收。

移植源为本地 `D:\Github\Metalio-E-INK4-main`；来源与许可见 `drivers/ssd1677/NOTICE.md`。
