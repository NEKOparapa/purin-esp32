#include "sdkconfig.h"
#include "test_apps.h"

// 唯一固件入口：选择应用，不在这里配置引脚或拼装硬件驱动。
void app_main(void)
{
#ifdef CONFIG_WINTERMUTE_DISPLAY_DIAGNOSTIC
    display_diagnostic_run();
#else
    hardware_test_run();
#endif
}
