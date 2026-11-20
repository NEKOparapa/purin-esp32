#include "board.h"
#include "board_config.h"
#include "uart_eth_modem.h"
#include "esp_log.h"

#include <atomic>
#include <memory>
#include <new>

namespace {
constexpr char TAG[] = "board_nt26";
constexpr EventBits_t CONNECTED = BIT0;
constexpr EventBits_t FAILED = BIT1;
constexpr EventBits_t NO_SIM = BIT2;
constexpr EventBits_t RESULT_BITS = CONNECTED | FAILED | NO_SIM;
std::atomic<EventGroupHandle_t> events{nullptr};
std::unique_ptr<UartEthModem> modem;

void on_modem_event(UartEthModem::UartEthModemEvent event, const std::string &detail)
{
    using Event = UartEthModem::UartEthModemEvent;
    EventGroupHandle_t group = events.load();
    switch (event) {
    case Event::Connected:
        xEventGroupClearBits(group, FAILED | NO_SIM);
        xEventGroupSetBits(group, CONNECTED);
        break;
    case Event::Disconnected:
    case Event::Connecting:
        xEventGroupClearBits(group, CONNECTED);
        break;
    case Event::ErrorNoSim:
        xEventGroupClearBits(group, CONNECTED | FAILED);
        xEventGroupSetBits(group, NO_SIM);
        break;
    case Event::ErrorInitFailed:
    case Event::ErrorNoCarrier:
    case Event::ErrorRegistrationDenied:
    case Event::InFlightMode:
        xEventGroupClearBits(group, CONNECTED);
        xEventGroupSetBits(group, FAILED);
        ESP_LOGW(TAG, "%s %s", UartEthModem::GetNetworkEventName(event), detail.c_str());
        break;
    default:
        break;
    }
}
} // namespace

bool board_cellular_connected(void)
{
    EventGroupHandle_t group = events.load();
    return group && (xEventGroupGetBits(group) & CONNECTED);
}

void board_cellular_stop(void)
{
    if (modem && modem->IsInitialized()) {
        std::string response;
        esp_err_t error = modem->SendAt("AT+CFUN=0", response, 3000);
        if (error != ESP_OK) ESP_LOGW(TAG, "Disable radio: %s", esp_err_to_name(error));
    }
    modem.reset();
    EventGroupHandle_t group = events.load();
    if (group) xEventGroupClearBits(group, RESULT_BITS);
}

esp_err_t board_cellular_connect(void)
{
    if (board_cellular_connected()) return ESP_OK;
    board_cellular_stop();

    EventGroupHandle_t group = events.load();
    if (!group) {
        esp_err_t error = gpio_install_isr_service(0);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
        group = xEventGroupCreate();
        if (!group) return ESP_ERR_NO_MEM;
        events.store(group);
    }

    const UartEthModem::Config config{
        .uart_num = BOARD_NT26_UART_PORT,
        .baud_rate = BOARD_NT26_UART_BAUD,
        .tx_pin = BOARD_NT26_TX_GPIO,
        .rx_pin = BOARD_NT26_RX_GPIO,
        .mrdy_pin = BOARD_NT26_MRDY_GPIO,
        .srdy_pin = BOARD_NT26_SRDY_GPIO,
    };
    modem.reset(new (std::nothrow) UartEthModem(config));
    if (!modem) return ESP_ERR_NO_MEM;
    modem->SetNetworkEventCallback(on_modem_event);
    esp_err_t error = modem->Start();
    if (error == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(group, RESULT_BITS, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
        if (bits & CONNECTED) return ESP_OK;
        error = (bits & NO_SIM) ? ESP_ERR_NOT_FOUND : ((bits & FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT);
    }
    board_cellular_stop();
    return error;
}
