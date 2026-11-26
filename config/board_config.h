#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/i2s_types.h"
#include "driver/uart.h"

/*
 * Metalio E-Ink 4 板级配置。
 * 引脚依据原项目 main/boards/metalio-e-ink-4/config.h 和 IOExpander.hpp。
 * 来源及许可见 drivers/ssd1677/NOTICE.md。
 *
 * GPIO_NUM_x 表示 ESP32-S3 的直接 GPIO；BOARD_IO_*_MASK 是 TCA9555
 * 的 16 位端口掩码，不能传给 gpio_set_level() 等 ESP32 GPIO 接口。
 * 本文件仅声明配置，音频、SD、蜂窝模块等外设不会因此被自动初始化。
 */

/* 墨水屏：GDEM0397T81 / SSD1677，以下为控制器原生画布。
 * 主页的 480×800 竖屏由 LVGL 适配层转换，不改变控制器的 800×480 寻址。
 */
#define BOARD_DISPLAY_WIDTH          800          // 控制器原生宽度
#define BOARD_DISPLAY_HEIGHT         480          // 控制器原生高度
#define BOARD_EPD_SPI_HOST           SPI3_HOST    // 屏幕使用 SPI3
#define BOARD_EPD_SPI_HZ             10000000     // SPI 时钟：10 MHz
#define BOARD_EPD_SPI_MODE           0            // SPI 模式 0
#define BOARD_EPD_MOSI_GPIO          GPIO_NUM_8   // SPI 数据输出
#define BOARD_EPD_SCLK_GPIO          GPIO_NUM_14  // SPI 时钟输出
#define BOARD_EPD_CS_GPIO            GPIO_NUM_45  // 屏幕片选，低有效
#define BOARD_EPD_DC_GPIO            GPIO_NUM_13  // 命令/数据选择
#define BOARD_EPD_RST_GPIO           GPIO_NUM_18  // 屏幕复位，低有效
#define BOARD_EPD_BUSY_GPIO          GPIO_NUM_9   // 屏幕忙信号，高表示正在刷新

/* 板上共享 I2C 总线：扩展器与触摸控制器使用同一个主机。 */
#define BOARD_I2C_PORT               I2C_NUM_0
#define BOARD_I2C_SDA_GPIO           GPIO_NUM_41  // I2C 数据线
#define BOARD_I2C_SCL_GPIO           GPIO_NUM_42  // I2C 时钟线
#define BOARD_IO_EXPANDER_ADDRESS    0x20         // TCA9555 的 7 位 I2C 地址
#define BOARD_IO_EXPANDER_I2C_HZ     100000       // 扩展器速率：100 kHz
#define BOARD_IO_EXPANDER_INT_GPIO   GPIO_NUM_2   // TCA9555 中断输出
#define BOARD_CHARGER_I2C_ADDRESS    0x6B         // CX25601N 充电/OTG 芯片地址（摄像头 5V OTG 供电）
#define BOARD_CHARGER_I2C_HZ         400000       // CX25601N I2C 速率：400 kHz

/* CST816S 触摸：原生坐标为竖向 480×800，显示坐标变换由触摸适配层完成。 */
#define BOARD_TOUCH_I2C_ADDRESS      0x15         // CST816S 的 7 位 I2C 地址
#define BOARD_TOUCH_I2C_HZ           400000       // 触摸速率：400 kHz
#define BOARD_TOUCH_INT_GPIO         GPIO_NUM_1  // 触摸中断，低有效
#define BOARD_TOUCH_RST_GPIO         GPIO_NUM_NC // 不接 ESP32 GPIO；实际接 TCA9555 P1.1
#define BOARD_TOUCH_NATIVE_WIDTH     480          // 触摸有效屏区的原生 X 范围：0～479
#define BOARD_TOUCH_NATIVE_HEIGHT    800          // 触摸有效屏区的原生 Y 范围：0～799
#define BOARD_TOUCH_SWAP_XY          0            // 竖屏主页直接使用原生 X/Y
#define BOARD_TOUCH_MIRROR_X         0            // 竖屏 X 方向不镜像
#define BOARD_TOUCH_MIRROR_Y         0            // 竖屏 Y 方向不镜像

/* TCA9555 扩展口：位 0～7 对应 P0.0～P0.7，位 8～15 对应 P1.0～P1.7。 */
#define BOARD_IO_USB_MUX_MASK        (1U << 0)   // P0.0：高=USB烧录/调试，低=USB摄像头
#define BOARD_IO_PA_SWITCH_MASK      (1U << 1)   // P0.1：低=ESP32音源，高=蜂窝通话音源
#define BOARD_IO_PA_ENABLE_MASK      (1U << 4)   // P0.4：音频功放使能，高有效
#define BOARD_IO_SCREEN_POWER_MASK   (1U << 5)   // P0.5：屏幕卡座电源，高有效
#define BOARD_IO_MAIN_POWER_MASK     (1U << 6)   // P0.6：外设总电源，高有效
#define BOARD_IO_VOLUME_DOWN_MASK    (1U << 7)   // P0.7：音量减按键输入
#define BOARD_IO_VOLUME_UP_MASK      (1U << 8)   // P1.0：音量加按键输入
#define BOARD_IO_TOUCH_RESET_MASK    (1U << 9)   // P1.1（原理图P11）：触摸复位，低有效
#define BOARD_IO_POWER_KEY_MASK      (1U << 11)  // P1.3（原理图P13）：关机脉冲，空闲必须保持高
#define BOARD_IO_ACCEL_INT_MASK      (1U << 12)  // P1.4：加速度计中断输入

/* 直接连接 ESP32 的按键与震动马达。 */
#define BOARD_BOOT_GPIO              GPIO_NUM_0  // BOOT 按键，低有效
#define BOARD_POWER_BUTTON_GPIO     GPIO_NUM_3  // 电源按键/开关机芯片信号
#define BOARD_VIBRATION_GPIO         GPIO_NUM_44 // 震动马达，高有效
#define BOARD_VIBRATION_ACTIVE_HIGH  1           // 板上马达开关高电平导通
#define BOARD_VIBRATION_PULSE_MS     35          // 默认触感脉冲：35 ms，沿用原项目
#define BOARD_VIBRATION_MAX_MS       1000        // 单次脉冲上限：1 秒，限制持续输出时间

/* SDMMC 使用 1-bit 模式；GPIO46 仅可输入，不要将 DAT3 配置成输出。 */
#define BOARD_SDMMC_CLK_GPIO         GPIO_NUM_38 // SD 时钟
#define BOARD_SDMMC_CMD_GPIO         GPIO_NUM_40 // SD 命令
#define BOARD_SDMMC_D0_GPIO          GPIO_NUM_39 // SD 数据 0
#define BOARD_SDMMC_DAT3_GPIO        GPIO_NUM_46 // DAT3/CD，上电需保持高以避免卡进入 SPI 模式
#define BOARD_SDMMC_MAX_FREQ_KHZ     20000       // SDMMC 时钟上限：20 MHz（单位 kHz）
#define BOARD_SD_MOUNT_POINT         "/sdcard"   // FAT 文件系统挂载目录

/* USB D-/D+ 与板上复用开关配合；Serial/JTAG 和 USB Host/MSC 不能同时使用。 */
#define BOARD_USB_DM_GPIO            GPIO_NUM_19 // USB D-
#define BOARD_USB_DP_GPIO            GPIO_NUM_20 // USB D+

/* I2S 麦克风/扬声器，以及外置蓝牙音频模块的 UART。 */
#define BOARD_AUDIO_I2S_PORT         I2S_NUM_0   // ESP32 为从机，时钟由外置音频模块提供
#define BOARD_AUDIO_SAMPLE_RATE_HZ   16000       // 左右两路各 16 kHz，32-bit Philips 槽
#define BOARD_AUDIO_PCM_SHIFT        12          // 沿用原板缩放：双麦平均后右移，再限幅至 PCM16
#define BOARD_AUDIO_BCLK_GPIO        GPIO_NUM_6  // 麦克风与扬声器共用的 I2S 位时钟
#define BOARD_AUDIO_MIC_WS_GPIO      GPIO_NUM_43 // 麦克风 I2S 字选择
#define BOARD_AUDIO_MIC_DIN_GPIO     GPIO_NUM_17 // 麦克风 I2S 数据输入
#define BOARD_AUDIO_SPK_DOUT_GPIO    GPIO_NUM_7  // 扬声器 I2S 数据输出
#define BOARD_AUDIO_SPK_BCLK_GPIO    BOARD_AUDIO_BCLK_GPIO // 兼容原引脚名称，实际为共用时钟
#define BOARD_BT_AUDIO_TX_GPIO       GPIO_NUM_48 // ESP32 -> 蓝牙音频模块
#define BOARD_BT_AUDIO_RX_GPIO       GPIO_NUM_47 // 蓝牙音频模块 -> ESP32
#define BOARD_BT_AUDIO_UART_PORT     UART_NUM_2  // 外置音频模块模式设置串口
#define BOARD_BT_AUDIO_UART_BAUD     115200      // 8 数据位、无校验、1 停止位、无流控
#define BOARD_BT_AUDIO_POWER_WAIT_MS 400         // 外设上电稳定等待
#define BOARD_BT_AUDIO_MODE_WAIT_MS  700         // AT+RX=2 与 AT+MODE=1 之间的等待

/* NT26 串口网卡及 MRDY/SRDY 握手。 */
#define BOARD_NT26_UART_PORT         UART_NUM_1
#define BOARD_NT26_UART_BAUD         2000000
#define BOARD_NT26_RX_GPIO           GPIO_NUM_11 // ESP32 UART 接收
#define BOARD_NT26_TX_GPIO           GPIO_NUM_12 // ESP32 UART 发送
#define BOARD_NT26_MRDY_GPIO         GPIO_NUM_21 // 主机就绪握手
#define BOARD_NT26_SRDY_GPIO         GPIO_NUM_5  // 从机就绪握手

/* 盖板虚拟键的原生触摸中心，Y=900 在屏幕有效区之外，不能直接送给 LVGL。 */
#define BOARD_TOUCH_VK_HOME_X        80
#define BOARD_TOUCH_VK_PREV_X        400
#define BOARD_TOUCH_VK_NEXT_X        240
#define BOARD_TOUCH_VK_Y             900
