#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i2s_microphone_device *i2s_microphone_handle_t;

typedef struct {
    i2s_port_t port;
    uint32_t sample_rate_hz; // 外部 WS 必须与此值一致；从机不会生成采样时钟。
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t din_gpio;
} i2s_microphone_config_t;

// 通用 I2S RX：从机、Philips 格式、32-bit 左右双槽，仅创建 RX，不驱动 DOUT。
// 调用方负责外部音频模块供电和时钟；成功后仍是停止状态。
// 同一实例的全部接口由调用方串行执行；不允许 read 与 stop/destroy 并发。
esp_err_t i2s_microphone_create(const i2s_microphone_config_t *config,
                               i2s_microphone_handle_t *out_handle);

// 开始一次采集，丢弃上次停止时的残留；已经运行时返回 INVALID_STATE。
esp_err_t i2s_microphone_start(i2s_microphone_handle_t handle);

// 以 stereo frame 为单位读取：每帧两个 int32_t，依次为 L、R，未缩放/混音。
// timeout_ms 范围 1..1000。ESP_ERR_TIMEOUT 可能带有效短读，须先消费 frames_read。
// 半帧字节留待下次读取，不将右声道误当成下一帧左声道。
// INVALID_SIZE 表示 DMA 溢出或积压进入可能跳帧区，frames_read=0，错误锁存
// 到下一次 start；调用方必须停止本次录音，不得将丢帧当作正常连续音频。
esp_err_t i2s_microphone_read(i2s_microphone_handle_t handle, int32_t *interleaved_lr,
                             size_t capacity_frames, size_t *frames_read,
                             uint32_t timeout_ms);

// 停止后可再次 start；重复停止成功。残留不足一帧的数据被丢弃。
esp_err_t i2s_microphone_stop(i2s_microphone_handle_t handle);
// 自动停止后释放；仅返回 ESP_OK 时句柄失效。板级借用句柄不得由应用销毁。
esp_err_t i2s_microphone_destroy(i2s_microphone_handle_t handle);

#ifdef __cplusplus
}
#endif
