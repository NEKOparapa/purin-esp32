# Wintermute ESP32 — Metalio E-Ink 4

基于 ESP-IDF 5.5.4 的录音和 Agent 设备。主页可进入对话，BOOT 长按录音，短按选择并确认上传。
硬件为 ESP32-S3、16 MB Flash、Octal PSRAM、SSD1677 / GDEM0397T81 800×480 墨水屏。
图形库为 LVGL 9.5.0，触摸使用官方 CST816S 1.0.6 组件。

## 启动与主页

启动顺序：`app_main()` → `board_init()` → UI 任务 → `lvgl_port_init()` → `home_screen_create()`。
主页采用 **480×800 竖屏**。顶栏显示录音和上传状态，底部单行导航可进入「对话」「日程」「阅读」「设置」。待机时，表盘按分钟更新指针，下方显示数字时间、日期和星期。设备启动时从板载 PCF8563 读取北京时间，联网后通过 SNTP 校时并写回芯片。时钟掉电且尚未同步时在表盘中央显示「等待校时」，校时成功后自动显示指针和日期。

## 屏上对话与信息采集

后端分层重构保留了固件使用的聊天、通知、感知、提问、设备工具和音频上传协议，本端不使用旧 `/v1/profile`。`WINTERMUTE_SERVER_URL` 填服务根地址，不添加 `/v1`。相邻 Wintermute 后端仓库的 `uv run python tools/check_clients.py` 可验证固件请求字段、响应结构、音频重试去重和设备隔离；该检查不连接或烧录实体设备，硬件功能仍需实机验证。

- 触摸输入框，用屏幕键盘输入英文，点上方 **EN / 拼音** 切换输入法后可点选汉字，按 **Send** 发送。收到任务回执后，页面每 5 秒检查回复。上下滑动可阅读长文本。屏上使用 Noto Sans SC 的 GB2312 汉字子集，单次输入最多 192 字符。
- **Retry** 重查最近一条问题。问题正文和客户端消息 ID 在发送前存入 NVS，任务 ID 收到后写回。断线或重启后再次进入可沿用原 ID 重试或读取最近任务。屏幕保留本次开机最近 16 条正文，跨重启保留最近问题和服务器任务引用。
- **Inbox** 取本设备的一条未读输出；后台通知标注为 **Notification**。读完按 **Read / next** 显式确认并取下一条。查询或滚动本身保留服务端的未读状态。超过设备响应上限（单次 HTTP JSON 128 KiB）的内容会报告错误并保留在服务器。
- **Share state** 读取点按时的实际录音器状态（空闲、启动、录制、保存或故障），以 `recorder.state` 提交至 `POST /v1/perceptions`。设备没有可信日历时钟，由后端记录接收时间。该操作只提交录音状态，会议声音沿用 BOOT 确认上传的音频切片路径。
- 网络请求使用下文的设备配置和同一设备凭据。录音上传和聊天在独立后台任务上共享网络连接锁。录音中短按 BOOT 可从聊天页返回上传确认面板。

### 主页连接器响应

提示卡片显示标题和正文预览，点「查看并确认」打开全文。倒计时显示剩余时分秒。卡片和倒计时并存时，最近启动的项目在主区域显示，另一项可从下方打开。

Agent 调用震动器后，主页显示 10 秒震动提醒。实际震动时长取决于调用参数。收书或收章成功后，主页显示书名或章名，点「打开这本书」即可阅读。播放请求显示音频名称和播放状态，调用失败时显示原因。短暂提醒结束后恢复未读卡片、正在运行的计时器或待机时钟。状态查询只在响应区下方短暂显示结果提示。

启动不挂载或写入 SD 卡，不触发震动；马达初始化保持关闭。
上传确认页通过触摸选择时长和取消；确认上传仍需再次短按 BOOT。

「设置 → 信息采集」分为录音采集和阅读采集。录音上传默认手动，自动上传选项暂只保存设置。阅读进度上传默认关闭。

首页首帧全刷，静止时不反复刷屏。LVGL 界面操作在同一个 UI 任务执行，
实际 SPI/BUSY 等待由独立刷新任务完成。显示适配层将竖屏 I1 画布旋转为控制器
原生 800×480 帧后发送；屏幕驱动和波形仍使用原生寻址。
串口 `Startup complete: home screen ready` 表示主页已创建；
`Frame ... FULL complete` 表示面板刷新流程完成，实际画面仍需实物确认。

## 离线阅读

- 主页的「阅读」直接打开书架。有书时点书名开始阅读，书架没有内容时显示「书架为空」。将 UTF-8 编码的 `.txt` 文件复制到 SD 卡的 `Wintermute/Library/` 即可加入书架。TXT 支持 UTF-8 BOM 和 Windows 换行，长文件分段读取。
- 正文按 480×800 竖屏排版。音量减／加键、底部翻页按钮、轻点正文左右两侧或横向滑动均可翻页。Agent 推送的书可连续翻到相邻章节，也可用「上一章」「下一章」跳转。
- 书签自动保存在设备中，重新打开书目会恢复阅读位置。Agent 书页显示章内页码，TXT 显示全书进度。Agent 工具继续使用每页 160 字的接口分页。
- 开启「设置 → 信息采集 → 阅读进度上传」后，退出正文时会对比同一本书上次已上传或待上传的位置。位置变化时尝试向切片层发送阅读进度更新，断网后重试，离线期间按书保留最新待上传位置。上报包含书名和进度；TXT 使用文件内的字节位置，Agent 书目使用章节和屏幕页码。

分页和音量键交互参考 [Metalio E-Ink 4 阅读器](https://github.com/CloudZao/Metalio-E-INK4/blob/d12f481f4a42420634bb56a928f3959a977367d4/README.md#113-reader-book)。

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
  ui/                    录音主页、触屏对话、键盘和收件箱
  agent/                 网络选择、HTTP 对话、状态采集和 NVS 恢复
  recording/             BOOT 判定、录音状态与模块协调
  upload/                不可变分段读取与后台音频上传
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
| 麦克风准备 | 持有音频独占权时调用 `board_microphone_init()` 获取借用句柄，停止采集后调用 `board_microphone_deinit()` 释放 I2S 通道 |
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
# 编辑 upload_config.local.h，填写服务端地址、设备 token 和 WINTERMUTE_DEVICE_NAME
idf.py build
```

`upload_config.local.h` 已忽略提交；凭据会编入固件。服务地址不带末尾 `/`，例如 `http://192.168.1.10:8080`。
服务端 HTTP 必须监听设备可达的地址，并配置匹配的 `device_id` 和 token；用户、会话由服务端设备绑定决定。

在设备上打开「设置 → 连接」可查看固件当前使用的设备名称、设备平台、设备 ID 和服务地址。更改设备名称或服务配置后需重新构建并刷写固件。

### 网络设置

在设备上打开「设置 → 网络」，选择 **Wi-Fi**、**移动网络**或**关闭**。选择保存在 NVS，切换会等当前网络操作结束再生效。默认选择 Wi-Fi。点选联网方式会尝试连接，成功后对话、连接器和上传共用该连接。

选择 **Wi-Fi → 扫描 Wi-Fi**，在列表中选择附近的 2.4 GHz 网络。加密网络输入密码后点「连接」，开放网络可直接连接。取得 IP 地址后设备保存该网络，重启时会自动连接上次成功保存的 Wi-Fi。换网失败会保留之前的网络。已有编译期 Wi-Fi 配置的设备升级后仍可使用原配置，屏上成功配网后以新配置为准。

移动网络使用板上的 NT26 4G 模块及 SIM 卡，由 UART1 和 MRDY/SRDY 握手接入 ESP-IDF 网络栈，沿用模组的默认 APN。板级接线和波特率参照 [Metalio 的 NT26 实现](https://github.com/CloudZao/Metalio-E-INK4/blob/d12f481f4a42420634bb56a928f3959a977367d4/main/boards/common/board/nt26_board.cc)。

Wi-Fi 等待 IP 的超时为 30 秒，移动网络为 60 秒，单次 HTTP 操作的超时为 15 秒。如果服务地址是家中局域网地址，改用移动网络时需要换成移动网络可达的服务地址。

HTTPS 使用 ESP-IDF 内置 CA 证书包验证主机证书，不跟随重定向；私有 CA 需自行加入证书包。
未填写上传所需配置时显示「待配置上传」。

每段以原始 Ogg 字节 `POST /v1/audio/slices`，包含 `Content-Type: audio/ogg`、`Content-Length` 及以下头：

| 请求头 | 内容 |
| --- | --- |
| `X-Device-ID` / `Authorization` | 设备 ID 和独立的 Bearer 令牌。设备 ID 留空时使用 `esp32-<出厂 MAC>`，可从启动串口日志读取并登记到后端。 |
| `X-Recording-ID` | 每次录音独立生成的 128-bit 随机 ID；同一场录音重试保持不变 |
| `X-Segment-Sequence` | SD 索引的连续正整数分段号 |
| `X-Start-Sample` / `X-Sample-Count` | 分段起点 / 精确真实样本数 |
| `X-Sample-Rate` | `16000` |
| `X-Window-Minutes` | 选择的 15/30/60/120，校验窗口上限，不参与去重 |
| `X-Recording-Elapsed-Samples` | 快照累计样本加等待和发送前的单调时钟差，供服务端估算录音时间 |

服务器按设备、录音 ID、分段号幂等接收；相同内容重复发送返回 202，不一致返回 409。
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
首次构建会下载锁定版本的 LVGL、触摸及 Opus 组件。字体和对话功能使 factory 分区从 2 MiB 扩为 5 MiB。首次升级应使用 `idf.py flash` 同步更新分区表和应用；NVS、PHY 起始位置保持原布局，无需擦除整片 Flash。

## 主 Agent 查询设备状态

在 `config/upload_config.local.h` 中填入 HTTPS 服务地址和设备凭据，设置 `#define WINTERMUTE_DEVICE_STATUS_ENABLED 1` 后编译、刷写。设备在 Agent 网络任务中约每 6 秒领取一次请求，提供 `device_status`，参数为 `{}`。返回的 JSON 包含板型、芯片、核心数、运行秒数和空闲堆内存字节数。设备在 NVS 中暂存待确认的结果，断网或重启后使用同一份结果重试。

## ESP 设备连接器

主页进入「设置 → 连接器」，点开应用即可本机试用。五个应用各有一个保存在设备上的远程连接开关，首次启动默认关闭。使用远程调用时，在上述本地配置中填写 HTTPS 服务地址和已绑定的设备凭据，构建并刷写后在设备上选择联网方式，再点所需应用右上角的「远程已关闭」开启远程调用。设备会向 Wintermute 同步应用名称和开关，各应用独立于 `WINTERMUTE_DEVICE_STATUS_ENABLED`。

| 应用 | Agent 可调用的动作 | 设备表现与返回内容 |
| --- | --- | --- |
| 震动器 `esp_vibration` | `pulse`、`stop`、`status` | 一次短震 1–1000 毫秒，默认 100 毫秒。返回马达当前的开关状态。 |
| 播放器 `esp_player` | `play`、`stop`、`status`、`list` | 省略文件名可试听内置双音，音量 1–100。将 16 kHz、16-bit PCM 单声道或双声道 WAV 放入 SD 卡的 `/Wintermute/Audio/`。文件名使用英文字母、数字、空格、点、下划线和连字符，以字母、数字、下划线或连字符开头。播放异步进行，查询可得到文件名、进度状态及错误原因。录音和播放共用音频硬件。 |
| 提示卡片 `esp_card` | `show`、`status` | 发送标题（最多 48 字）和正文（最多 240 字）。最新卡片显示在墨水屏上，用户点「我看过了」后，Agent 可查询卡片 ID、标题及确认状态。重启保留最近一张卡片。 |
| 计时器 `esp_timer` | `start`、`stop`、`status` | 单次倒计时 1 秒至 24 小时，再次启动会替换当前倒计时。返回名称、剩余秒数和状态。断网继续走时，重启清零。到时在屏幕提示，开启震动器连接后还会短震一次。 |
| 阅读书库 `esp_library` | `add`、`append`、`list`、`read` | 将纯文本保存到 SD 卡的 `/Wintermute/Library/`。先用 `add` 建书，长文用 `append` 从第 2 章起依次追加，每章最多 2500 字，共可有 999 章。可列出 Agent 书目和按章、页读取。Agent 查阅页不会改变设备上的阅读位置。 |

例如，对指定设备调用 `Mcp` 并传入下列参数，可启动 25 分钟专注计时。将 `<设备ID>` 替换为连接器列表中的设备 ID。

```json
{"action":"call","provider":"esp_timer@<设备ID>","tool":"esp_timer","args":{"action":"start","seconds":1500,"label":"专注"}}
```

音频与书目列表每次最多返回四项。`has_more` 为真时将 `offset` 加四继续查询。书库返回的当前页、总页数和书签页码均从 1 开始。同一书目同一章可重试发送相同文本，提交不同文本会收到冲突错误。

## 录音诊断

串口 `Saved session ... segment ...` 只在音频和索引均保存成功后输出；停止日志给出
编码/写卡耗时及任务剩余栈，启动日志给出内部 RAM/PSRAM 可用量。

## 硬件说明

接线定义集中在 `config/board_config.h`。`BOARD_IO_*_MASK` 表示扩展器端口位，不能当作 ESP32 GPIO。
屏幕初始化失败时查看串口中的 I2C、BUSY、供电错误；CST816S 休眠后可能需要触摸唤醒。
触摸原生为 480×800，与竖屏主页直接对应，不交换或镜像坐标；Y=900 的盖板虚拟键尚未接入应用。

移植源为本地 `D:\Github\Metalio-E-INK4-main`，来源与许可见 `drivers/ssd1677/NOTICE.md`。
