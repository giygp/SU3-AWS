/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh 
 * @LastEditTime: 2026-07-14 18:19:35
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "use_adc.h"
#include "use_wifi.h"
#include "ble_server.h"
#include "use_pwm.h"
#include "use_ota.h"
#include "common.h"
#include "app_control.h"
#include "use_uart.h"
#include "esp_log.h"
static const char *TAG = "main";



void app_main(void)
{
  //初始化 NVS
  vTaskDelay(500 / portTICK_PERIOD_MS);
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
  device_init();

  // config_store();               //默认配置 需单独配置
  // read_config();                //读取用户配置
  /* 震动马达相关
    //Key_Gpio_init();
    //pwm_init();                   //PWM初始化
  */
  // initialize_adc();             //气压采集系统

  initialize_ble_server();      //蓝牙初始化
  ESP_LOGI(TAG, "ble_server init Successfully");
  initialize_wifi();            //wifi初始化
  ESP_LOGI(TAG, "WiFi init Successfully");
  mfp_gpio_config();
  app_control_server();           //app通信控制系统
  ota_launcher_init();            //常驻OTA启动器（开机建好，避免指令到达时无内存）

}
