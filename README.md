# Wintermute ESP32 — Metalio E-Ink 4

基于 ESP-IDF 5.5.4 的项目基础固件。启动后进入竖屏主页，支持 BOOT 长按持续录音、短按确认音频上传。
硬件为 ESP32-S3、16 MB Flash、Octal PSRAM、SSD1677 / GDEM0397T81 800×480 墨水屏。
图形库为 LVGL 9.5.0，触摸使用官方 CST816S 1.0.6 组件。

## 启动与主页

启动顺序：`app_main()` → `board_init()` → UI 任务 → `lvgl_port_init()` → `home_screen_create()`。
主页采用 **480×800 竖屏**，显示 WINTERMUTE、HOME、Ready 等静态占位内容，使用已有英文字体。
后续页面从 `main/ui/home_screen.c` 扩展。

旧硬件测试页面、原始屏幕诊断、测试模式菜单及按键测试动作已移除。
当前没有计数器、坐标展示、SD 读写测试或震动测试；BOOT 长按用于开始/停止录音，录音时短按进入上传确认。
启动不挂载或写入 SD 卡，不触发震动；马达初始化保持关闭。
上传确认页通过触摸选择时长和取消；确认上传仍需再次短按 BOOT。

首页首帧全刷，静止时不反复刷屏。所有 LVGL 调用在同一个 UI 任务执行，
实际 SPI/BUSY 等待由独立刷新任务完成。显示适配层将竖屏 I1 画布旋转为控制器
原生 800×480 帧后发送；屏幕驱动和波形仍使用原生寻址。
串口 `Startup complete: home screen ready` 表示主页已创建；
`Frame ... FULL complete` 表示面板刷新流程完成，实际画面仍需实物确认。

## 分层目录

```text
config/board_config.h      中文注释的引脚、地址、频率、方向配置

drivers/                  独立、可复用的硬件驱动
  ssd1677/                墨水屏协议与波形
  cst816s_touch/          触摸采样和事件队列
  tca9555/                IO 扩展器
  sdcard/                 SDMMC 1-bit + FatFS
  vibration/              马达脉冲与自动关断
  i2s_microphone/         I2S 从机双路原始采集、短读与丢帧检测

board/                    本板供电、接线、总线和设备初始化
  include/board.h         板级公开接口

middleware/lvgl_port/     LVGL 显示与触摸适配
  include/lvgl_port.h
  display/
  input/

middleware/audio_capture/ 双麦混音、采集任务与 PCM 队列
  include/audio_capture.h
  include/audio_pcm.h     可独立使用的纯 PCM 混音运算

middleware/ogg_opus/      音频编码与容器，一个组件内按职责拆分
  encoder/               PCM → 原始 Opus 包，无文件操作
  muxer/                 Opus 包 → Ogg 页，通过回调输出字节
  ogg_opus.c             连接编码/容器，负责普通文件打开、同步与关闭

middleware/recording_storage/ PCM 分段保存与顺序索引
  include/recording_writer.h
  recording_writer.c     切片、周期同步、正常收尾与异常中止
  recording_store.c      内部目录/文件命名、改名与索引提交

main/                     应用层
  app_main.c              初始化与 UI 任务
  ui/home_screen.c        占位主页与静态录音状态
  ui/home_screen.h
  recording/             BOOT 判定、录音状态与模块协调
  upload/                按需 Wi-Fi、不可变分段读取与后台 HTTP 上传
```

只有 `board` 的实现读取 `board_config.h`。驱动不依赖板型、LVGL 或应用；
板级层不创建页面；适配层不包含业务界面。应用使用板级与图形接口。
根 `CMakeLists.txt` 注册 6 个驱动、1 个板级和 4 个中间件组件。

录音数据流为：

```text
I2S 麦克风驱动
    ↓ 原始左右 32-bit 样本
audio_capture：等权混音 → PCM16 → 采集队列
    ↓ 单声道 PCM 块
recording_storage：按真实样本数分段
    ↓
ogg_opus：Opus 编码 → Ogg 封装 → 文件输出
    ↓ 音频收尾、同步关闭
recording_storage：改名 → 追加并同步顺序索引
    ↓
已挂载文件系统（由 board + SD 驱动提供）
```

`main/recording/recorder.c` 只协调板级准备、启停、PCM 块传递、同步时机与页面状态，
五分钟是应用传给存储模块的策略。它不做混音、不计算切片、不接触 Ogg 或文件路径。
`audio_capture` 接收借用的麦克风句柄，独占其 start/read/stop；不依赖 board、SD 或页面。
`recording_storage` 接收已挂载路径和 PCM，可独立用于其他采样来源或文件系统，
不依赖 I2S、采集队列、UI 或 SD 挂载。编码器和容器可通过回调连接其他输出，
没有为这些内部模块各自增加任务或组件。

底层接口继续保留，供后续业务使用：

| 功能 | 接口 |
| --- | --- |
| 请求全刷 | `lvgl_port_request_full_refresh()`，仅 UI 任务调用 |
| AI / BOOT 键状态 | `board_boot_pressed()`，GPIO0，应用消抖后长按 2 秒切换录音 |
| 麦克风准备 | `board_microphone_init()`，停止采集后调用，返回借用句柄 |
| 录音控制 | `recorder_start()`、`recorder_stop()`，非阻塞；`recorder_get_status()` 获取快照 |
| SD 挂载与路径 | `board_sdcard_mount()`、`board_sdcard_path()` |
| SD 卸载 | 关闭所有文件后调用 `board_sdcard_unmount()` |
| 震动与停止 | `board_vibration_pulse(ms)`、`board_vibration_stop()` |

SD 固定 1-bit，不自动格式化，挂载后使用标准文件 API。
震动默认 35 ms、单次上限 1000 ms，定时关断，不阻塞 UI。
`board_vibration_is_active()` 只表示软件输出状态，无法检测马达是否实际震动。

## 持续录音

主页就绪后，BOOT 稳定按下满 **2 秒**开始；松手后再次长按 2 秒停止。
按键消抖为 30 ms，每次按住只触发一次；开机残留的按住状态需先松手。
录音中的短按在稳定松手时触发上传确认；长按后的松手不会误触发短按。
准备期间显示 `Starting recording`，实际采集并编码成功后显示右上角 **● REC**；
停止时显示 `Saving`，末段及索引同步完成后图标消失，恢复 `Ready`。
录音图标不闪烁，不显示持续变化的计时器。

按已确认的硬件配置，左右 I2S 通道均作为独立麦克风：先用 64-bit 整数等权平均，
再按板级 `BOARD_AUDIO_PCM_SHIFT=12` 缩放并限幅为 PCM16。
不沿用原项目的麦克风/回声参考通道语义，不加 AGC、降噪或静音跳过。
I2S0 使用从机 Philips 32-bit stereo RX，外置音频模块提供 16 kHz WS；
每次录音前通过 UART2 重发原板模式命令，PA 保持关闭。

采集任务与编码/写卡任务分开，约 2 秒 PCM 队列放在 PSRAM，任务栈使用内部 RAM。
编码为 16 kHz 单声道、24 kbps CBR Opus、20 ms 帧，complexity 0，关闭 DTX/FEC。
每段独立编码器并补偿编码延迟；以真实样本数切片，完整分段恰好 300 秒。
文件切换不停止采集；队列或 DMA 丢帧、持续 1 秒收不到输入时明确停止并报错。

```text
/sdcard/recordings/
  000001/
    index.jsonl
    000001.ogg
    000002.ogg
    000003.ogg.part
  000002/
    index.jsonl
    000001.ogg
```

每次开始扫描并建立新的六位编号目录，分段编号从 1 开始，不依赖系统时间且不覆盖旧文件。
录音持续写入 `.ogg.part`，每秒同步；每五分钟写入 Ogg EOS、同步并关闭，然后改名为 `.ogg`，
最后追加并同步索引。停止时使用同样流程保存不足五分钟的末段，不生成空的完成分段。
每五分钟音频约 0.9 MB，加上少量 Ogg 页开销。

`index.jsonl` 首行 `type=session` 记录版本、会话编号、采样率、声道数和码率；
后续 `type=segment` 记录 `sequence`、`file`、`start_sample`、`samples`、`duration_ms`、`bytes`。
先按会话目录编号、再按 `sequence` 即可确定音频保存顺序；`samples` 是精确时长依据。
索引只接收以换行结尾的完整 JSON 记录；未来消费者必须忽略残缺尾行，并确认对应文件存在。

无卡、卡满、音频或写入异常会停止本次录音并在主页显示错误；修复后可重新长按开始。
正常停止后关闭所有文件并卸载 SD 卡；若上传尚未结束，先停止录音并保存末段，待上传关闭读文件后再卸载。
意外断电不自动恢复录音，不修复 `.part`，
也不自动收录已改名但未提交索引的文件；这些文件会保留供人工检查。

依赖锁定为 `78/esp-opus` 1.0.5；许可说明见 `middleware/ogg_opus/NOTICE.md`。

## 音频上传

录音时短按 BOOT 打开确认页，默认选择 **30 分钟**，可触摸改为 **15 分钟 / 1 小时 / 2 小时**；
再次短按 BOOT 提交。这里的“前 N 分钟”指**本次录音起点开始的 N 分钟**，不足时上传确认时已录好的全部音频。
触摸 Cancel 取消，长按 BOOT 仍可停止录音。上传准备、连接和发送期间忽略重复短按。

上传不会停止采集：录音任务先排空接到请求时已经排队的 PCM（不追逐后续新样本），
将当前末段收尾为完整 Ogg，再继续处理采集队列；
下一段仍在本次录音的绝对五分钟边界结束，因此反复提前截取不会使 15/30/60/120 分钟边界漂移。
独立低优先级任务只读已完成的 `.ogg`，按顺序发送，每次最多读取 2 KiB，不把两小时音频放入 RAM。
读取期间录音任务继续写新分段，SD 最多同时打开四个文件。

连接或上传失败不影响已保存音频；仍在录音时，再短按两次 BOOT 可重试。
选择相同时长会重发**原失败快照**，成功接受的分段由服务端去重；换时长会建立新快照。
界面 `Audio accepted for processing` 仅表示全部分段被服务端 HTTP 202 接收，ASR、总结和记忆写入在服务端异步进行。
当前不在重启后自动重传，也不提供停止录音后的历史文件选择。

先复制本地配置并填写独立的设备上传凭据，再构建：

```powershell
Copy-Item config/upload_config.example.h config/upload_config.local.h
# 编辑 upload_config.local.h，填写 Wi-Fi、服务端地址、终端 ID、终端 token
idf.py build
```

`upload_config.local.h` 已忽略提交；凭据会编入固件。服务地址不带末尾 `/`，例如 `http://192.168.1.10:8080`。
服务端 HTTP 必须监听设备可达的地址，并配置匹配的 `terminal_id` 和 token；用户、会话由服务端终端绑定决定。
Wi-Fi 只在确认上传后启动，连接超时 30 秒，HTTP 每次网络操作超时 15 秒；完成或失败后关闭 Wi-Fi。
HTTPS 使用 ESP-IDF 内置 CA 证书包验证主机证书，不跟随重定向；私有 CA 需自行加入证书包。
未填写配置时显示 `Configure Wi-Fi and upload server`，不会尝试上传。

每段以原始 Ogg 字节 `POST /v1/audio/slices`，包含 `Content-Type: audio/ogg`、`Content-Length` 及以下头：

| 请求头 | 内容 |
| --- | --- |
| `X-Terminal-ID` / `Authorization` | 配置的终端 ID / `Bearer <token>` |
| `X-Recording-ID` | 每次录音独立生成的 128-bit 随机 ID；同一场录音重试保持不变 |
| `X-Segment-Sequence` | SD 索引的连续正整数分段号 |
| `X-Start-Sample` / `X-Sample-Count` | 分段起点 / 精确真实样本数 |
| `X-Sample-Rate` | `16000` |
| `X-Window-Minutes` | 选择的 15/30/60/120，校验窗口上限，不参与去重 |
| `X-Recording-Elapsed-Samples` | 快照累计样本加等待和发送前的单调时钟差，供服务端估算录音时间 |

服务器按终端、录音 ID、分段号幂等接收；相同内容重复发送返回 202，不一致返回 409。
任一分段非 202 即停止本次上传，不删除 SD 文件。固件保持单段不超过五分钟、4 MiB。

## 编译与烧录

在 ESP-IDF PowerShell 中：

```powershell
cd D:\Github\wintermute-esp32
idf.py build
idf.py -p COM9 flash monitor
```

普通 PowerShell 可先加载本机工具链：

```powershell
$env:Path = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;' + $env:Path
. D:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
```

串口变化时替换 COM9；退出监视器按 `Ctrl+]`。
工程提供 `sdkconfig`、`sdkconfig.defaults` 和 `partitions.csv`，全新目标配置使用 `idf.py set-target esp32s3`。
首次构建会下载锁定版本的 LVGL、触摸及 Opus 组件。烧录替换当前应用和分区表，无需 `erase-flash`。

## 验证

本次新增的上传主机测试直接编译生产按键逻辑、索引读取校验和 `recording_writer.c`，
用可注入故障的存储/编码替身验证短长按隔离、快照不漂移、空段避免、卡满错误锁存和残缺索引拒绝：

```powershell
cmake -S tests/audio/upload -B build/audio-upload -G Ninja
cmake --build build/audio-upload
ctest --test-dir build/audio-upload --output-on-failure
```

实机还需验证：录音不足 15 分钟时上传全量；超过 30 分钟后只上传所选前缀；
上传中继续录音及长按停止；错误 token / 断网后的重试；四个时长的触摸选择及取消。
主机测试和固件构建不等于已验证实机采集、Wi-Fi 并发性能或服务端真实 ASR。

主机检查直接编译实际生产代码，输出放在 `build/`。在 Windows 的 Visual Studio 开发者终端中，
确保 CMake/Ninja 可用，然后从项目根目录执行：

```powershell
foreach ($suite in @('logic', 'microphone', 'capture', 'storage', 'writer')) {
    cmake -S "tests/audio/$suite" -B "build/audio-$suite" -G Ninja
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $suite" }
    cmake --build "build/audio-$suite"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $suite" }
    ctest --test-dir "build/audio-$suite" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $suite" }
}
```

- `logic`：启动按住屏蔽、消抖、2 秒阈值、单次触发、独立 PCM 混音/限幅。
- `microphone`：实际驱动配合模拟 I2S，验证半帧拼接、超时、DMA 丢帧和重复启停。
- `capture`：实际采集模块配合模拟任务与 I2S，验证短读、停止后排空、溢出、无音频及故障重试。
- `storage`：目录递增、碰撞、索引顺序和文件大小校验；Windows 额外注入部分写入、卡满、同步失败。
- `writer`：直接调用分段存储模块，验证一批 PCM 跨多段、两个五分钟整段和末段、空段避免、
  异常中止，以及只有索引提交成功才推进保存计数；测试编码器用计数假实现，文件/索引执行实际代码。
- `ogg`：实际 Opus 固定点源码及生产封装器，独立解析 CRC/页号并使用 FFmpeg 解码，
  验证 300 秒、非整帧末段和极短音频时长及切片交界。运行方法见 `tests/audio/ogg/README.md`。

实机验收：分别对左右麦克风发声确认两路均有效，连续录音超过 10 分钟后停止，
应产生两个 300 秒分段及一个末段，索引样本位置连续，图标正确切换。
另验证无卡/卡满、故障后重试和断电后已完成分段保留。
串口 `Saved session ... segment ...` 只在音频和索引均保存成功后输出；停止日志给出
编码/写卡耗时及任务剩余栈，启动日志给出内部 RAM/PSRAM 可用量。
主机测试和固件编译不能替代实物接线、实际采样率、持续录音性能与供电异常验证。

## 硬件说明

接线定义集中在 `config/board_config.h`。`BOARD_IO_*_MASK` 表示扩展器端口位，不能当作 ESP32 GPIO。
屏幕初始化失败时查看串口中的 I2C、BUSY、供电错误；CST816S 休眠后可能需要触摸唤醒。
触摸原生为 480×800，与竖屏主页直接对应，不交换或镜像坐标；Y=900 的盖板虚拟键尚未接入应用。

移植源为本地 `D:\Github\Metalio-E-INK4-main`，来源与许可见 `drivers/ssd1677/NOTICE.md`。
