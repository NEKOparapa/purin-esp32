#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "cst816s_touch.h"
#include "i2s_microphone.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 板级入口：只在这里组合具体引脚、电源、总线和驱动。
 * 应用无需访问 board_config.h 或操作 IO 扩展器。
 * 初始化接口由调用方串行执行，已成功的初始化可重复调用；
 * 板级总线、面板、触摸资源持续到重启，不由上层删除借用句柄。
 */
esp_err_t board_init(void);
// Save the synchronized system clock to the board's battery-backed local clock.
esp_err_t board_clock_store(void);

// Waits for the NT26 to obtain an IP address. Start/stop run on a serialized
// network worker after esp_netif and the default event loop are initialized.
esp_err_t board_cellular_connect(void);
void board_cellular_stop(void);
bool board_cellular_connected(void);

// 创建屏幕 SPI 和面板；面板 reset/init 由 LVGL 适配或原始诊断执行。
esp_err_t board_display_init(esp_lcd_panel_handle_t *panel);

// BOOT 为低有效；board_init 成功后读取，消抖和按键动作由应用处理。
bool board_boot_pressed(void);

// -1 for volume down, +1 for volume up, 0 when neither or both are pressed.
esp_err_t board_volume_key(int *direction);

// 在持有音频独占权的工作任务中配置外置模块和双声道 I2S RX，不开始采集。
// 返回借用句柄。停止采集后调用 board_microphone_deinit() 释放通道。
esp_err_t board_microphone_init(i2s_microphone_handle_t *microphone);
esp_err_t board_microphone_deinit(void);
// 原板 32-bit 采样转换成 PCM16 的缩放位数；混音后再缩放/限幅。
uint8_t board_microphone_pcm_shift(void);
bool board_audio_reserve(void);
void board_audio_release(void);
// Caller holds the audio reservation for the complete record/play operation.
// Stop playback to release the TX channel and its DMA buffers.
esp_err_t board_speaker_start(void);
esp_err_t board_speaker_write(const int32_t *stereo, size_t frames);
esp_err_t board_speaker_stop(void);

// 初始化马达输出为关闭状态，不产生启动震动；board_init 会调用，可串行重试。
esp_err_t board_vibration_init(void);
// 异步短震，到时自动关闭；0 使用板级默认时长，新脉冲替换当前脉冲。
// 在 board_init 成功后调用；不允许超过板级单次时长上限。
esp_err_t board_vibration_pulse(uint32_t duration_ms);
esp_err_t board_vibration_stop(void);
// 仅为驱动输出状态，不能判断马达是否实际转动或连接正常。
bool board_vibration_is_active(void);

typedef struct {
    uint16_t native_width;
    uint16_t native_height;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} board_touch_orientation_t;

// 执行触摸复位并启动采样，返回借用句柄及本板安装方向。
// ESP_OK 表示输入通道就绪；休眠芯片在下一次触摸中断后继续探测。
esp_err_t board_touch_init(cst816s_touch_handle_t *touch, board_touch_orientation_t *orientation);

typedef struct {
    char name[8];
    uint64_t capacity_bytes;
    uint32_t sector_size;
} board_sdcard_info_t;

// 按本板引脚挂载 FAT 卡并读取信息，不格式化；重复挂载返回 INVALID_STATE。
// SD 生命周期由一个任务串行管理，不得与文件读写或其它 SD 初始化并发。
esp_err_t board_sdcard_mount(board_sdcard_info_t *info);

// 所有文件关闭后再卸载；有效挂载无论释放结果如何，调用后均不可继续访问。
esp_err_t board_sdcard_unmount(void);

// 仅挂载成功后返回 VFS 路径；未挂载返回 NULL，卸载后路径借用失效。
const char *board_sdcard_path(void);

// Temporarily mounts the card through the same exclusive owner as recording and playback.
// Run on a worker. ESP_ERR_INVALID_STATE means the card is already in use.
esp_err_t board_sdcard_read_usage(uint64_t *total_bytes, uint64_t *free_bytes);

// Powers CX25601N 5V OTG (when present), switches USB D+/D- mux to the onboard UVC camera,
// and starts an MJPEG capture stream. Run on a worker task; close before restarting or idling.
esp_err_t board_camera_open(void);
// Copies the latest MJPEG frame into a newly allocated buffer (caller frees with free()).
esp_err_t board_camera_capture_jpeg(uint8_t **jpeg_out, size_t *len_out, uint32_t timeout_ms);
// Stops the UVC stream, switches USB mux back to debug/flash, and restores charging.
void board_camera_close(void);

#ifdef __cplusplus
}
#endif
