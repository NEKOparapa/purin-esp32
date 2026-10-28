// 板卡引脚来自 Metalio-E-INK4-main，来源见 drivers/ssd1677/NOTICE.md。
#include "board.h"

#include "board_config.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"

static const char *TAG = "board_display";
static esp_lcd_panel_io_handle_t panel_io;
static esp_lcd_panel_handle_t board_panel;
static bool spi_initialized;

static esp_err_t display_cleanup(void)
{
    if (board_panel) {
        // 等待已排队的 DMA 完成，再释放 panel 持有的传输缓冲。
        // 负命令号不会发送命令，只同步队列（ESP-IDF panel IO 约定）。
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0), TAG, "drain SPI DMA");
        ESP_RETURN_ON_ERROR(epaper_panel_wait_ready(board_panel), TAG, "wait panel idle");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_del(board_panel), TAG, "delete panel");
        board_panel = NULL;
    }
    if (panel_io) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(panel_io), TAG, "delete LCD IO");
        panel_io = NULL;
    }
    if (spi_initialized) {
        ESP_RETURN_ON_ERROR(spi_bus_free(BOARD_EPD_SPI_HOST), TAG, "delete EPD SPI");
        spi_initialized = false;
    }
    return ESP_OK;
}

esp_err_t board_display_init(esp_lcd_panel_handle_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel output is NULL");
    *panel = board_panel;
    if (board_panel) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board power init");
    // 上次创建失败时的残留 IO 也必须释放，避免重新初始化同一 SPI 主机。
    ESP_RETURN_ON_ERROR(display_cleanup(), TAG, "cleanup previous initialization");
    const spi_bus_config_t spi_config = {
        .mosi_io_num = BOARD_EPD_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = BOARD_EPD_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SSD1677_PANEL_BUFFER_SIZE + 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_EPD_SPI_HOST, &spi_config, SPI_DMA_CH_AUTO), TAG, "EPD SPI");
    spi_initialized = true;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_EPD_CS_GPIO,
        .dc_gpio_num = BOARD_EPD_DC_GPIO,
        .spi_mode = BOARD_EPD_SPI_MODE,
        .pclk_hz = BOARD_EPD_SPI_HZ,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_EPD_SPI_HOST, &io_config, &panel_io);
    if (err == ESP_OK) {
        esp_lcd_ssd1677_config_t vendor = {
            .busy_gpio_num = BOARD_EPD_BUSY_GPIO,
            .non_copy_mode = true,
            .use_fast_full_update = true,
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = BOARD_EPD_RST_GPIO,
            .vendor_config = &vendor,
        };
        err = esp_lcd_new_panel_ssd1677(panel_io, &panel_config, &board_panel);
    }
    if (err != ESP_OK) {
        esp_err_t cleanup_err = display_cleanup();
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG, "Display cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
        return err;
    }
    *panel = board_panel;
    ESP_LOGI(TAG, "SSD1677 ready %dx%d; SPI %dHz, BUSY=GPIO%d (active high)",
             BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT, BOARD_EPD_SPI_HZ, BOARD_EPD_BUSY_GPIO);
    return ESP_OK;
}
