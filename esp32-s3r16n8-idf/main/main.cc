#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// --- 【新增】引入 ESP-IDF 硬件 UART 驱动头文件 ---
#include <driver/uart.h>

#include "application.h"

#define TAG "main"

// --- 【新增】定义跨板异步通信的串口参数 ---
#define S3_UART_NUM     UART_NUM_1       // 使用硬件串口 1
#define S3_TXD_PIN      GPIO_NUM_17      // S3 发送脚，接 WROOM 的 RX (16)
#define S3_RXD_PIN      GPIO_NUM_18      // S3 接收脚，接 WROOM 的 TX (17)
#define UART_BUF_SIZE   (1024)           // 缓存大小

/**
 * @brief 【新增】初始化跨板异步通信串口 UART1
 */
static void init_cross_board_uart()
{
    uart_config_t uart_config = {
        .baud_rate = 115200,                     // 115200 波特率，与 WROOM-32 保持一致
        .data_bits = UART_DATA_8_BITS,                // 8 位数据位
        .parity = UART_PARITY_DISABLE,           // 无校验位
        .stop_bits = UART_STOP_BITS_1,           // 1 位停止位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,   // 关闭硬件流控
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,         // 默认时钟源
    };

    // 安装驱动：设置 RX 缓冲区，设置 TX 缓冲区（启用环形缓冲区模式，避免阻塞发送线程）
    ESP_ERROR_CHECK(uart_driver_install(S3_UART_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0));
    // 配置串口基础参数
    ESP_ERROR_CHECK(uart_param_config(S3_UART_NUM, &uart_config));
    // 将物理引脚分配给 UART1 模块
    ESP_ERROR_CHECK(uart_set_pin(S3_UART_NUM, S3_TXD_PIN, S3_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "跨板异步通信串口 UART1 (GPIO 17/18) 初始化完成");
}

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- 【新增】在应用程序启动前，率先打通硬件发送管道 ---
    init_cross_board_uart();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}