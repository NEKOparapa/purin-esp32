// 板载 USB UVC 摄像头启停与抓帧，供电/MUX/USB Host 时序参考 Metalio-E-INK4（见 drivers/ssd1677/NOTICE.md）。
#include "board.h"
#include "board_config.h"
#include "board_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "board_camera";

enum {
    USB_HOST_PRIORITY = 15,
    FRAME_QUEUE_LEN = 3,
    WARMUP_FRAMES = 2,
    STILL_NUM_URBS = 2,
    MUX_SETTLE_MS = 100,
    ENUM_WAIT_MS = 6500,
    OPEN_TIMEOUT_MS = 8000,
    DISCONNECT_WAIT_MS = 2000,
    ALL_FREE_WAIT_MS = 2000,
    HUB_SETTLE_MS = 120,
    OTG_ENTRY_DELAY_MS = 60,
    OTG_OFF_SETTLE_MS = 50,
};

// CX25601N registers used to initialize charger/boost and supply 5V/1A OTG VBUS to the camera.
enum {
    REG_IOTG_LO = 0x0A,
    REG_IOTG_HI = 0x0B,
    REG_VOTG_LO = 0x0C,
    REG_VOTG_HI = 0x0D,
    REG_CHG_CTRL0 = 0x14,
    REG_CHG_TMR = 0x15,
    REG_CHG_CTRL1 = 0x16,
    REG_CHG_CTRL3 = 0x18,
    REG_STATUS1 = 0x1E,
    REG_PART_INFO = 0x38,
    REG_UNLOCK = 0x70,
    VBUS_STAT_MASK = 0x07,
    VBUS_STAT_OTG = 7,
    OTG_TARGET_MV = 5040,
    OTG_TARGET_MA = 1000,
    CHG_I2C_TIMEOUT_MS = 100,
};

static SemaphoreHandle_t camera_lock;
static SemaphoreHandle_t all_free_sem;
static QueueHandle_t frame_queue;
static usb_phy_handle_t host_phy;
static TaskHandle_t usb_lib_task;
static uvc_host_stream_hdl_t active_stream;
static i2c_master_dev_handle_t charger_dev;

static bool saved_chg_enabled;
static bool otg_held;
static bool mux_held;
static bool host_installed;
static bool root_port_powered;
static bool uvc_installed;
static bool session_open;
static atomic_bool uvc_connected;
static volatile uint8_t conn_dev_addr;
static volatile uint8_t conn_stream_idx;
static unsigned warmup_remaining;

static esp_err_t chg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(charger_dev, &reg, 1, val, 1, CHG_I2C_TIMEOUT_MS);
}

static esp_err_t chg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(charger_dev, buf, sizeof(buf), CHG_I2C_TIMEOUT_MS);
}

static esp_err_t chg_update_bits(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t field)
{
    uint8_t cur = 0;
    esp_err_t err = chg_read(reg, &cur);
    if (err != ESP_OK) return err;
    uint8_t m = (uint8_t)(mask << shift);
    cur = (uint8_t)((cur & (uint8_t)~m) | ((field << shift) & m));
    return chg_write(reg, cur);
}

static void chg_unlock_private(bool enable)
{
    if (!enable) {
        (void)chg_write(REG_UNLOCK, 0x00);
        return;
    }
    for (int i = 0; i < 10; ++i) {
        (void)chg_write(REG_UNLOCK, 0x00);
        (void)chg_write(REG_UNLOCK, 0x50);
        (void)chg_write(REG_UNLOCK, 0x57);
        (void)chg_write(REG_UNLOCK, 0x44);
        uint8_t v = 0;
        if (chg_read(REG_UNLOCK, &v) == ESP_OK && v == 0x03) return;
    }
    ESP_LOGW(TAG, "CX25601N private register unlock failed");
}

// Align with Metalio-E-INK4 cx25601n.cc hw_init_defaults(): disable D+/D- auto-detect and
// 40s I2C watchdog (otherwise WDT resets EN_OTG mid-scan), and apply vendor boost trim.
static esp_err_t charger_hw_init(void)
{
    esp_err_t err = chg_update_bits(REG_CHG_TMR, 0x01, 6, 0); // EN_AUTO_INDET = 0 (dis_dpdm)
    if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL1, 0x01, 4, 0); // EN_HIZ = 0
    if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL1, 0x03, 0, 0); // WDT = disabled
    if (err != ESP_OK) return err;

    chg_unlock_private(true);
    (void)chg_write(0x86, 0x06);
    (void)chg_write(0x3A, 0x10);
    (void)chg_write(0x46, 0x20);
    chg_unlock_private(false);

    (void)chg_update_bits(REG_CHG_CTRL0, 0x01, 0, 1);
    (void)chg_update_bits(REG_CHG_CTRL3, 0x01, 2, 0); // BATFET_DLY = 0
    (void)chg_update_bits(0x1A, 0x01, 7, 1);
    (void)chg_update_bits(0x23, 0x07, 2, 0x07);
    (void)chg_update_bits(0x24, 0x01, 3, 1);
    return ESP_OK;
}

static esp_err_t ensure_charger(void)
{
    if (charger_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_master_probe(bus, BOARD_CHARGER_I2C_ADDRESS, CHG_I2C_TIMEOUT_MS);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "CX25601N not detected at 0x%02x; trying camera without OTG boost",
                 BOARD_CHARGER_I2C_ADDRESS);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CHARGER_I2C_ADDRESS,
        .scl_speed_hz = BOARD_CHARGER_I2C_HZ,
    };
    uint8_t part = 0;
    err = i2c_master_bus_add_device(bus, &cfg, &charger_dev);
    if (err != ESP_OK) return err;
    err = chg_read(REG_PART_INFO, &part);
    if (err == ESP_OK) err = charger_hw_init();
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(charger_dev);
        charger_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "CX25601N ready at 0x%02x (part=0x%02x)", BOARD_CHARGER_I2C_ADDRESS, part);
    return ESP_OK;
}

static esp_err_t charger_set_otg(bool enable)
{
    if (enable) {
        ESP_RETURN_ON_ERROR(ensure_charger(), TAG, "probe camera power supply");
        if (!charger_dev) return ESP_OK;
        uint8_t ctrl1 = 0;
        if (!otg_held) {
            ESP_RETURN_ON_ERROR(chg_read(REG_CHG_CTRL1, &ctrl1), TAG, "read charging state");
            saved_chg_enabled = ((ctrl1 >> 5) & 0x01) != 0;
            otg_held = true; // Restore EN_CHG even if a later register write fails.
        }
        const uint32_t votg_code = OTG_TARGET_MV / 80; // 0x3F -> 5.04 V
        const uint32_t iotg_code = OTG_TARGET_MA / 20; // 0x32 -> 1.00 A
        esp_err_t err = chg_update_bits(REG_CHG_CTRL1, 0x03, 0, 0); // WDT = disabled
        if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL1, 0x01, 5, 0); // EN_CHG = 0
        if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL1, 0x01, 4, 0); // EN_HIZ = 0
        if (err == ESP_OK) err = chg_update_bits(REG_VOTG_LO, 0x03, 6, (uint8_t)(votg_code & 0x03));
        if (err == ESP_OK) err = chg_update_bits(REG_VOTG_HI, 0x1F, 0, (uint8_t)((votg_code >> 2) & 0x1F));
        if (err == ESP_OK) err = chg_update_bits(REG_IOTG_LO, 0x0F, 4, (uint8_t)(iotg_code & 0x0F));
        if (err == ESP_OK) err = chg_update_bits(REG_IOTG_HI, 0x0F, 0, (uint8_t)((iotg_code >> 4) & 0x0F));
        if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL3, 0x01, 6, 1); // EN_OTG = 1
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(OTG_ENTRY_DELAY_MS));
            uint8_t st1 = 0;
            if (chg_read(REG_STATUS1, &st1) == ESP_OK) {
                ESP_LOGI(TAG, "CX25601N boost requested (STATUS1=0x%02x, VBUS_STAT=%u)", st1, st1 & VBUS_STAT_MASK);
            }
        }
        return err;
    }
    esp_err_t err = chg_update_bits(REG_CHG_CTRL3, 0x01, 6, 0); // EN_OTG = 0
    if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL1, 0x01, 4, 0);
    if (err == ESP_OK) err = chg_update_bits(REG_CHG_CTRL1, 0x01, 5, saved_chg_enabled ? 1 : 0);
    if (err == ESP_OK) otg_held = false;
    return err;
}

static void route_phy_to_usj(void)
{
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
}

static esp_err_t init_host_phy(void)
{
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_phy_enable_external(true);
    const usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };
    esp_err_t err = usb_new_phy(&phy_cfg, &host_phy);
    if (err != ESP_OK) {
        host_phy = NULL;
        route_phy_to_usj();
    }
    return err;
}

typedef struct {
    const usb_host_config_t *cfg;
    esp_err_t err;
    SemaphoreHandle_t done;
} host_install_arg_t;

static void host_install_task(void *arg)
{
    host_install_arg_t *a = arg;
    a->err = usb_host_install(a->cfg);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

static esp_err_t install_usb_host_on_core1(const usb_host_config_t *cfg)
{
    host_install_arg_t arg = {.cfg = cfg, .err = ESP_FAIL, .done = xSemaphoreCreateBinary()};
    if (!arg.done) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(host_install_task, "usb_h_inst", 4096, &arg,
                                USB_HOST_PRIORITY, NULL, 1) != pdPASS) {
        esp_err_t err = usb_host_install(cfg);
        vSemaphoreDelete(arg.done);
        return err;
    }
    xSemaphoreTake(arg.done, portMAX_DELAY);
    vSemaphoreDelete(arg.done);
    return arg.err;
}

static void usb_lib_daemon(void *unused)
{
    (void)unused;
    for (;;) {
        uint32_t flags = 0;
        if (usb_host_lib_handle_events(pdMS_TO_TICKS(200), &flags) == ESP_OK) {
            if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                esp_err_t free_err = usb_host_device_free_all();
                if (free_err == ESP_OK && all_free_sem) {
                    xSemaphoreGive(all_free_sem);
                }
            }
            if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) && all_free_sem) {
                xSemaphoreGive(all_free_sem);
            }
        }
    }
}

static bool uvc_frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    QueueHandle_t q = user_ctx;
    if (!q || !frame) return true;
    return xQueueSendToBack(q, &frame, 0) != pdPASS;
}

static void uvc_stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event) return;
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        uvc_connected = false;
    }
}

static void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event && event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        conn_dev_addr = event->device_connected.dev_addr;
        conn_stream_idx = event->device_connected.uvc_stream_index;
        uvc_connected = true;
    }
}

static void drain_frame_queue(uvc_host_stream_hdl_t stream)
{
    if (!frame_queue) return;
    uvc_host_frame_t *frame = NULL;
    while (xQueueReceive(frame_queue, &frame, 0) == pdPASS) {
        if (stream && frame) (void)uvc_host_frame_return(stream, frame);
    }
}

static esp_err_t close_stream_if_any(void)
{
    if (!active_stream) return ESP_OK;
    drain_frame_queue(active_stream);
    (void)uvc_host_stream_stop(active_stream);
    drain_frame_queue(active_stream);
    ESP_RETURN_ON_ERROR(uvc_host_stream_close(active_stream), TAG, "close UVC stream");
    active_stream = NULL;
    return ESP_OK;
}

// Strict hardware teardown sequence from Metalio-E-INK4:
// Close the stream and disconnect the physical port before freeing the UVC client.
static esp_err_t release_camera_usb(void)
{
    esp_err_t err = close_stream_if_any();
    if (err != ESP_OK) return err;

    if (mux_held) {
        ESP_RETURN_ON_ERROR(board_set_usb_camera_mux(false), TAG, "restore flash mux");
        vTaskDelay(pdMS_TO_TICKS(MUX_SETTLE_MS));
        mux_held = false;
    }

    if (!uvc_installed) return ESP_OK;
    // A closed stream no longer receives UVC disconnect callbacks.
    usb_host_lib_info_t info;
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        ESP_RETURN_ON_ERROR(usb_host_lib_info(&info), TAG, "USB device count");
        if (!info.num_devices) break;
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(DISCONNECT_WAIT_MS)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(HUB_SETTLE_MS));

    while (xSemaphoreTake(all_free_sem, 0) == pdTRUE) {}
    ESP_RETURN_ON_ERROR(uvc_host_uninstall(), TAG, "uninstall UVC");
    uvc_installed = false;
    (void)xSemaphoreTake(all_free_sem, pdMS_TO_TICKS(ALL_FREE_WAIT_MS));
    return ESP_OK;
}

static esp_err_t close_camera_unlocked(void)
{
    esp_err_t err = release_camera_usb();
    if (otg_held) {
        vTaskDelay(pdMS_TO_TICKS(OTG_OFF_SETTLE_MS));
        esp_err_t power_err = charger_set_otg(false);
        if (err == ESP_OK) err = power_err;
    }

    uvc_connected = false;
    session_open = false;
    if (err != ESP_OK) ESP_LOGW(TAG, "Camera teardown: %s", esp_err_to_name(err));
    return err;
}

static size_t urb_size_for_res(unsigned w, unsigned h)
{
    uint32_t pixels = (uint32_t)w * h;
    if (pixels >= 1280U * 720U) return 8 * 1024;
    if (pixels >= 800U * 600U) return 6 * 1024;
    if (pixels >= 640U * 480U) return 4 * 1024;
    return 2 * 1024;
}

static size_t frame_buf_for_res(unsigned w, unsigned h)
{
    uint32_t pixels = (uint32_t)w * h;
    if (pixels >= 1280U * 720U) return 280 * 1024;
    if (pixels >= 800U * 600U) return 160 * 1024;
    if (pixels >= 640U * 480U) return 100 * 1024;
    return 48 * 1024;
}

// Prefer ~640x480 / 640x360 MJPEG for reliable QR recognition over 12 Mbps Full-Speed USB.
static uint32_t resolution_rank(unsigned w, unsigned h)
{
    int32_t pixels = (int32_t)w * (int32_t)h;
    int32_t diff = pixels >= 640 * 480 ? pixels - 640 * 480 : 640 * 480 - pixels;
    if (w >= 480 && w <= 800) return (uint32_t)diff;
    if (w >= 320 && w < 480) return 1000000U + (uint32_t)diff;
    if (w > 800 && w <= 1280) return 2000000U + (uint32_t)diff;
    return 3000000U + (uint32_t)diff;
}

static esp_err_t try_open_stream(uint8_t dev_addr, uint8_t stream_idx, unsigned w, unsigned h, float fps)
{
    uvc_host_stream_config_t cfg = {
        .event_cb = uvc_stream_event_cb,
        .frame_cb = uvc_frame_cb,
        .user_ctx = frame_queue,
        .usb = {
            .dev_addr = dev_addr,
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = stream_idx,
        },
        .vs_format = {
            .h_res = w,
            .v_res = h,
            .fps = fps,
            .format = UVC_VS_FORMAT_MJPEG,
        },
        .advanced = {
            .number_of_frame_buffers = FRAME_QUEUE_LEN,
            .frame_size = frame_buf_for_res(w, h),
            .frame_heap_caps = MALLOC_CAP_SPIRAM,
            .number_of_urbs = STILL_NUM_URBS,
            .urb_size = urb_size_for_res(w, h),
        },
    };
    esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(OPEN_TIMEOUT_MS), &active_stream);
    if (err != ESP_OK) active_stream = NULL;
    return err;
}

static esp_err_t open_best_mjpeg_stream(void)
{
    const uint8_t dev_addr = conn_dev_addr;
    const uint8_t stream_idx = conn_stream_idx;

    if (dev_addr != 0) {
        uvc_host_frame_info_t list[32];
        size_t list_size = sizeof(list) / sizeof(list[0]);
        if (uvc_host_get_frame_list(dev_addr, stream_idx, &list, &list_size) == ESP_OK && list_size > 0) {
            size_t order[32];
            size_t count = 0;
            for (size_t i = 0; i < list_size && count < 32; ++i) {
                if (list[i].format == UVC_VS_FORMAT_MJPEG) order[count++] = i;
            }
            for (size_t a = 0; a + 1 < count; ++a) {
                for (size_t b = a + 1; b < count; ++b) {
                    if (resolution_rank(list[order[b]].h_res, list[order[b]].v_res) <
                        resolution_rank(list[order[a]].h_res, list[order[a]].v_res)) {
                        size_t tmp = order[a];
                        order[a] = order[b];
                        order[b] = tmp;
                    }
                }
            }
            for (size_t i = 0; i < count; ++i) {
                const uvc_host_frame_info_t *info = &list[order[i]];
                if (try_open_stream(dev_addr, stream_idx, info->h_res, info->v_res, 0.0f) == ESP_OK ||
                    try_open_stream(dev_addr, stream_idx, info->h_res, info->v_res, 15.0f) == ESP_OK) {
                    ESP_LOGI(TAG, "Opened UVC MJPEG stream %ux%u", info->h_res, info->v_res);
                    return ESP_OK;
                }
            }
        }
    }

    static const struct { unsigned w, h; float fps; } fallbacks[] = {
        {640, 480, 0.0f}, {640, 360, 0.0f}, {320, 240, 0.0f},
        {640, 480, 15.0f}, {320, 240, 15.0f}, {800, 600, 0.0f}, {1280, 720, 0.0f},
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
        if (try_open_stream(dev_addr, stream_idx, fallbacks[i].w, fallbacks[i].h, fallbacks[i].fps) == ESP_OK) {
            ESP_LOGI(TAG, "Opened fallback UVC MJPEG stream %ux%u", fallbacks[i].w, fallbacks[i].h);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t install_uvc_driver(void)
{
    if (uvc_installed) return ESP_OK;
    const uvc_host_driver_config_t uvc_cfg = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority = USB_HOST_PRIORITY + 1,
        .xCoreID = 1,
        .create_background_task = true,
        .event_cb = uvc_driver_event_cb,
    };
    esp_err_t err = uvc_host_install(&uvc_cfg);
    if (err == ESP_OK) uvc_installed = true;
    return err;
}

static esp_err_t ensure_usb_host(void)
{
    if (!host_installed) {
        ESP_RETURN_ON_ERROR(init_host_phy(), TAG, "USB host PHY");
        const usb_host_config_t host_cfg = {
            .skip_phy_setup = true,
            .root_port_unpowered = true,
            .intr_flags = ESP_INTR_FLAG_LOWMED,
        };
        esp_err_t err = install_usb_host_on_core1(&host_cfg);
        if (err != ESP_OK) {
            usb_del_phy(host_phy);
            host_phy = NULL;
            route_phy_to_usj();
            return err;
        }
        host_installed = true;
    }
    if (!usb_lib_task && xTaskCreatePinnedToCore(usb_lib_daemon, "uvc_usb_lib", 4096, NULL,
                                                 USB_HOST_PRIORITY, &usb_lib_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return install_uvc_driver();
}

static esp_err_t wait_for_camera(board_camera_stage_t *stage)
{
    *stage = BOARD_CAMERA_DETECT;
    TickType_t started = xTaskGetTickCount();
    while (!uvc_connected && xTaskGetTickCount() - started < pdMS_TO_TICKS(ENUM_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (uvc_connected) return ESP_OK;

    usb_host_lib_info_t info;
    esp_err_t err = usb_host_lib_info(&info);
    if (err != ESP_OK) {
        *stage = BOARD_CAMERA_USB_HOST;
        return err;
    }
    if (info.num_devices > 0) return ESP_ERR_NOT_SUPPORTED;
    if (!charger_dev) {
        *stage = BOARD_CAMERA_POWER;
        return ESP_ERR_NOT_FOUND;
    }
    // An external supply can also power a working camera. Only diagnose lack of
    // boost after the enumeration window expires with an empty USB device list.
    uint8_t status;
    if (chg_read(REG_STATUS1, &status) == ESP_OK && (status & VBUS_STAT_MASK) != VBUS_STAT_OTG) {
        ESP_LOGW(TAG, "No USB device, CX25601N VBUS_STAT=%u", status & VBUS_STAT_MASK);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t board_camera_open_with_status(board_camera_stage_t *stage)
{
    if (!stage) return ESP_ERR_INVALID_ARG;
    *stage = BOARD_CAMERA_INIT;
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board init");
    if (!camera_lock) camera_lock = xSemaphoreCreateMutex();
    if (!camera_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(camera_lock, portMAX_DELAY);
    if (session_open && active_stream) {
        xSemaphoreGive(camera_lock);
        return ESP_OK;
    }

    if (!all_free_sem) all_free_sem = xSemaphoreCreateBinary();
    if (!frame_queue) frame_queue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(uvc_host_frame_t *));
    if (!all_free_sem || !frame_queue) {
        xSemaphoreGive(camera_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (mux_held || uvc_installed || otg_held) {
        // Finish a previous failed disconnect before presenting a new attach.
        err = close_camera_unlocked();
        if (err != ESP_OK) {
            if (active_stream) *stage = BOARD_CAMERA_STREAM;
            else if (mux_held) *stage = BOARD_CAMERA_USB_MUX;
            else if (uvc_installed) *stage = BOARD_CAMERA_USB_HOST;
            else *stage = BOARD_CAMERA_POWER;
            xSemaphoreGive(camera_lock);
            return err;
        }
    }
    uvc_connected = false;
    *stage = BOARD_CAMERA_POWER;
    err = charger_set_otg(true);
    if (err != ESP_OK) goto failed;

    // IDF sends NEW_DEV only to clients registered when enumeration completes.
    // Register UVC before connecting the mux, on both first use and retry.
    *stage = BOARD_CAMERA_USB_HOST;
    err = ensure_usb_host();
    if (err != ESP_OK) goto failed;
    *stage = BOARD_CAMERA_USB_MUX;
    mux_held = true;
    err = board_set_usb_camera_mux(true);
    if (err != ESP_OK) goto failed;
    vTaskDelay(pdMS_TO_TICKS(MUX_SETTLE_MS));

    if (!root_port_powered) {
        *stage = BOARD_CAMERA_USB_HOST;
        err = usb_host_lib_set_root_port_power(true);
        if (err != ESP_OK) goto failed;
        root_port_powered = true; // Keep the root port running across scan sessions.
    }

    err = wait_for_camera(stage);
    if (err != ESP_OK) goto failed;

    *stage = BOARD_CAMERA_STREAM;
    err = open_best_mjpeg_stream();
    if (err == ESP_OK) err = uvc_host_stream_start(active_stream);
    if (err != ESP_OK) goto failed;
    warmup_remaining = WARMUP_FRAMES;
    session_open = true;
    xSemaphoreGive(camera_lock);
    return ESP_OK;

failed:
    ESP_LOGW(TAG, "Camera startup stage %d: %s", (int)*stage, esp_err_to_name(err));
    close_camera_unlocked();
    xSemaphoreGive(camera_lock);
    return err;
}

esp_err_t board_camera_open(void)
{
    board_camera_stage_t stage;
    return board_camera_open_with_status(&stage);
}

static bool valid_jpeg_header(const uvc_host_frame_t *frame)
{
    return frame && frame->data && frame->data_len >= 4 &&
           frame->data[0] == 0xFF && frame->data[1] == 0xD8 && frame->data[2] == 0xFF;
}

esp_err_t board_camera_capture_jpeg(uint8_t **jpeg_out, size_t *len_out, uint32_t timeout_ms)
{
    if (!jpeg_out || !len_out) return ESP_ERR_INVALID_ARG;
    *jpeg_out = NULL;
    *len_out = 0;
    if (!camera_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(camera_lock, portMAX_DELAY);
    if (!session_open || !active_stream || !frame_queue) {
        xSemaphoreGive(camera_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!uvc_connected) {
        xSemaphoreGive(camera_lock);
        return ESP_ERR_NOT_FOUND;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t err = ESP_ERR_TIMEOUT;

    while (xTaskGetTickCount() < deadline) {
        if (!uvc_connected) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = deadline > now ? deadline - now : 0;
        uvc_host_frame_t *frame = NULL;
        if (xQueueReceive(frame_queue, &frame, wait) != pdPASS || !frame) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (!valid_jpeg_header(frame)) {
            (void)uvc_host_frame_return(active_stream, frame);
            continue;
        }
        if (warmup_remaining > 0) {
            --warmup_remaining;
            (void)uvc_host_frame_return(active_stream, frame);
            continue;
        }
        // Drain any queued older frames so the caller always inspects the freshest image.
        uvc_host_frame_t *newer = NULL;
        while (xQueueReceive(frame_queue, &newer, 0) == pdPASS) {
            if (valid_jpeg_header(newer)) {
                (void)uvc_host_frame_return(active_stream, frame);
                frame = newer;
            } else if (newer) {
                (void)uvc_host_frame_return(active_stream, newer);
            }
        }
        uint8_t *copy = heap_caps_malloc(frame->data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) copy = malloc(frame->data_len);
        const size_t len = frame->data_len;
        if (copy) memcpy(copy, frame->data, len);
        (void)uvc_host_frame_return(active_stream, frame);
        if (!copy) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        *jpeg_out = copy;
        *len_out = len;
        err = ESP_OK;
        break;
    }

    xSemaphoreGive(camera_lock);
    return err;
}

void board_camera_close(void)
{
    if (!camera_lock) return;
    xSemaphoreTake(camera_lock, portMAX_DELAY);
    close_camera_unlocked();
    xSemaphoreGive(camera_lock);
}
