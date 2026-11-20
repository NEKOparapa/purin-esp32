#pragma once

// 复制为 upload_config.local.h 后填写；local 文件已被 Git 忽略。
// 改动后重新编译。凭据会进入设备固件，不要提交或分发含私密凭据的固件。
// Wi-Fi 在设备的「设置 → 网络」中配网。以下两项仅作为老固件升级时的备用配置。
#define WINTERMUTE_WIFI_SSID ""
#define WINTERMUTE_WIFI_PASSWORD ""
// 填服务根地址，不带末尾 / 或 /v1，例如 http://192.168.1.10:8080；公网使用 HTTPS。
#define WINTERMUTE_SERVER_URL ""
// 留空时使用 esp32- 加本板出厂 MAC；串口启动日志可查看，登记到后端后使用。
#define WINTERMUTE_DEVICE_ID ""
#define WINTERMUTE_DEVICE_NAME "我的录音板"
#define WINTERMUTE_DEVICE_TOKEN ""
// 设为 1 后向主 Agent 提供只读的 device_status 工具。
#define WINTERMUTE_DEVICE_STATUS_ENABLED 0
