#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "cst816s_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 板级入口：只在这里组合具体引脚、电源、总线和驱动。
 * 应用无需访问 board_config.h 或操作 IO 扩展器。
 * 初始化接口由调用方串行执行，已成功的初始化可重复调用；
 * 板级总线、面板、触摸资源持续到重启，不由上层删除借用句柄。
 */
esp_err_t board_init(void);

// 创建屏幕 SPI 和面板；面板 reset/init 由 LVGL 适配或原始诊断执行。
esp_err_t board_display_init(esp_lcd_panel_handle_t *panel);

// BOOT 为低有效；board_init 成功后读取，消抖和按键动作由应用处理。
bool board_boot_pressed(void);

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

#ifdef __cplusplus
}
#endif
