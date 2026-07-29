/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh 
 * @LastEditTime: 2026-07-14 18:19:35
 */
#include "common.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "pb_decode.h"
#include "keesoncloud.pb.h"
#include "qs_protobuf.h"
#include "esp_rom_gpio.h"

static const char *TAG = "common";

device_info_t *device_info;
qs_pb_msg_sensor_5sec_info *user_5s_sensor_info;
qs_pb_msg_sensor_1min_info *user_60s_sensor_info;
qs_pb_msg_sleep_apnea_info *user_sa_sensor_info;

#if SU3_USE_NEW_STACK
su3_sensor_side_t g_su3_sensor[SU3_SENSOR_SIDE_MAX];
#endif
char last_saved_ssid[32] = {0};
char last_saved_passwd[64] = {0};
MFPData_t 		g_MFPData_t; 	// MFP数据结构体（间隔时间之类的）

void print_nvs_usage()
{
    nvs_stats_t stats;
    esp_err_t err = nvs_get_stats(NULL, &stats); // NULL 表示默认分区 16KB/32byte = 512 总共512个键值对
    if (err == ESP_OK) {
        printf("NVS usage:\n");
        printf("  Used entries: %d\n", stats.used_entries);		// 已使用键值对
        printf("  Free entries: %d\n", stats.free_entries);
        printf("  Total entries: %d\n", stats.total_entries);
    } else {
        printf("Failed to get NVS stats: %s\n", esp_err_to_name(err));
    }
}

uint8_t get_wifi_status(void)
{
	return device_info->wifi.flag;
}

//0：wifi等待连接；1：wifi连接成功；2：wifi重连；3：wifi重连失败，超过重连次数；
void set_wifi_status(uint8_t value)
{
	device_info->wifi.flag = value;
}

uint8_t get_one_key_config_wifi_status(void)
{
	return device_info->wifi.one_key_config.flag;
}
//0：wifi连接参数未修改；1：wifi连接参数修改
void set_one_key_config_wifi_status(uint8_t value)
{
	device_info->wifi.one_key_config.flag = value;
}

uint8_t get_ble_status(void)
{
	return device_info->ble->flag;
}

void set_ble_status(uint8_t value)
{
	device_info->ble->flag = value;
}

uint8_t get_mqtt_status(void)
{
	return device_info->aliyun.flag;
}

void set_mqtt_status(uint8_t value)
{
	device_info->aliyun.flag = value;
}

static qs_settings_aliyun_t m_aliyun;

static void read_aliyun_config(void)
{
	esp_partition_t *partition;
	
	partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");

	if(partition != NULL)
	{
		esp_partition_read(partition, 0, &m_aliyun, sizeof(qs_settings_aliyun_t));
		memcpy(device_info->id, m_aliyun.device_name, strlen(m_aliyun.device_name));
		//aliyun 参数配置
		memcpy(device_info->aliyun.device_id, m_aliyun.device_name, strlen(m_aliyun.device_name));
		memcpy(device_info->aliyun.product_key, m_aliyun.product_key, strlen(m_aliyun.product_key));
		memcpy(device_info->aliyun.device_secret, m_aliyun.device_secret, strlen(m_aliyun.device_secret));

		printf("Device Name: %s\n", m_aliyun.device_name);
		printf("Product Key: %s\n", m_aliyun.product_key);
		printf("Device Secret: %s\n", m_aliyun.device_secret);
		printf("s11\n");
	}else
	{
		//设备id设置
		memcpy(device_info->id, DEVEICE_ID, strlen(DEVEICE_ID));
		
		//aliyun 参数配置
		memcpy(device_info->aliyun.product_key, PRODUCT_KEY, strlen(PRODUCT_KEY));
		memcpy(device_info->aliyun.device_secret, DEVEICE_SECRET, strlen(DEVEICE_SECRET));
		memcpy(device_info->aliyun.device_id, DEVEICE_ID, strlen(DEVEICE_ID));	
		printf("s22\n");	
	}
	printf("id:%s,key:%s,secret:%s \n",device_info->aliyun.device_id,device_info->aliyun.product_key,device_info->aliyun.device_secret);
}

//设置默认参数
void param_config_init(void)
{
	//参数初始化
	device_info = (device_info_t *)malloc(sizeof(device_info_t));
	memset(device_info, 0, sizeof(device_info_t));

	device_info->ble = (ble_link_info_t *)malloc(sizeof(ble_link_info_t));
	memset(device_info->ble, 0, sizeof(ble_link_info_t));
	device_info->ble->xQueue = xQueueCreate(4, 512);   //缩减10 512
	if (device_info->ble->xQueue == NULL)
		ESP_LOGW(TAG,"ble.xQueue create error!");

	device_info->mqtt_key = (mqtt_key_info_t *)malloc(sizeof(mqtt_key_info_t));
	memset(device_info->mqtt_key, 0, sizeof(mqtt_key_info_t));
	device_info->mqtt_key->xQueue = xQueueCreate(10, 64);   //缩减10 512
	if (device_info->mqtt_key->xQueue == NULL)
		ESP_LOGW(TAG,"mqtt_key.xQueue create error!");


	device_info->aliyun.xQueue = xQueueCreate(10, sizeof(mmqtt_msg_t));
	if (device_info->aliyun.xQueue == NULL)
		ESP_LOGW(TAG,"aliyun.xQueue create error!");

	device_info->wifi.one_key_config.xQueue = xQueueCreate(10, 10);
	if (device_info->wifi.one_key_config.xQueue == NULL)
		ESP_LOGW(TAG,"one_key_config.xQueue create error!");

	// 打鼾干预初始化为
	device_info->snore = (snore_intervention_t *) malloc(sizeof(snore_intervention_t));
	memset(device_info->snore, 0, sizeof(snore_intervention_t));		
	device_info->snore->snore_parameters.up_hold_time_s = UP_HOLD_TIME_S;
	device_info->snore->snore_parameters.threshold_5s = THRESHOLD_5S;
	device_info->snore->snore_parameters.threshold = THRESHOLD;
	device_info->snore->snore_parameters.pwm = SNORING_PWM;
	device_info->snore->snore_parameters.tmr = SNORING_TMR;
	device_info->snore->snore_state.block_size = 24;
	//固件版本设置
	memcpy(device_info->ota.running_version, INIT_VERSION, strlen(INIT_VERSION));

#if ALIYUN_BURN
	read_aliyun_config();
#else
	//设备id设置
	memcpy(device_info->id, DEVEICE_ID, strlen(DEVEICE_ID));
	
	//aliyun 参数配置
	memcpy(device_info->aliyun.product_key, PRODUCT_KEY, strlen(PRODUCT_KEY));
	memcpy(device_info->aliyun.device_secret, DEVEICE_SECRET, strlen(DEVEICE_SECRET));
	memcpy(device_info->aliyun.device_id, DEVEICE_ID, strlen(DEVEICE_ID));
#endif

	//数据上传开关设置
	device_info->data_up_switch = DATA_UP;
	device_info->ota.flag = OTA_ON_OFF;


	//默认wifi设置
	memcpy(device_info->wifi.one_key_config.ssid, MY_WIFI_SSID, strlen(MY_WIFI_SSID));
	memcpy(device_info->wifi.one_key_config.passwd, MY_WIFI_PASSWD, strlen(MY_WIFI_PASSWD));

	//5s数据
	user_5s_sensor_info = (qs_pb_msg_sensor_5sec_info *)malloc(sizeof(qs_pb_msg_sensor_5sec_info));
    memset(user_5s_sensor_info, 0, sizeof(qs_pb_msg_sensor_5sec_info));

	//60s数据
	user_60s_sensor_info = (qs_pb_msg_sensor_1min_info *)malloc(sizeof(qs_pb_msg_sensor_1min_info));
    memset(user_60s_sensor_info, 0, sizeof(qs_pb_msg_sensor_1min_info));

	//SA数据
	user_sa_sensor_info = (qs_pb_msg_sleep_apnea_info *)malloc(sizeof(qs_pb_msg_sleep_apnea_info));
	memset(user_sa_sensor_info, 0, sizeof(qs_pb_msg_sleep_apnea_info));

#if SU3_USE_NEW_STACK
	memset(g_su3_sensor, 0, sizeof(g_su3_sensor));
#endif
}

void config_store_to_flash(void)
{
	char out_value[300] = {0};
	size_t len;
	
	/*=======================================配置出厂设备信息============================*/
	nvs_handle nvs_config_handler;
	
	ESP_ERROR_CHECK(nvs_open("config_cfg", NVS_READWRITE, &nvs_config_handler));
	//nvs_erase_all(nvs_config_handler);
	
	nvs_get_str(nvs_config_handler, "configFlag", out_value, &len);
	
	// nvs_get_str(nvs_config_handler, "id", out_value, &len);
	// printf("id = %s \n",out_value);
	// if(strstr(out_value, DEVEICE_ID))
	if(strstr(out_value, "config_1bc"))    //esp32 flash中已经存在相应参数，读取参数
	{
		ESP_LOGI(TAG, "read nvs config now... \n");
		len = sizeof(out_value);
		ESP_ERROR_CHECK(nvs_get_str(nvs_config_handler, "wifiSsid", out_value, &len));
		memcpy(device_info->wifi.one_key_config.ssid, out_value, len);
		len = sizeof(out_value);
		ESP_ERROR_CHECK(nvs_get_str(nvs_config_handler, "wifiPasswd", out_value, &len));
		memcpy(device_info->wifi.one_key_config.passwd, out_value, len);

		size_t snore_param_size = sizeof(snore_parameters_t);
        esp_err_t err = nvs_get_blob(nvs_config_handler, "snore_param",  &device_info->snore->snore_parameters, &snore_param_size);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "no snore_param in flash, using default values");
        }
        else
        {
            ESP_LOGI(TAG, "snore param loaded from flash");
        }
		// memcpy(device_info->wifi.one_key_config.ssid, "kakakaka", 9);
		// memcpy(device_info->wifi.one_key_config.passwd, "77885522", 9);

		// 读取成功后同步到last_saved变量
		strncpy(last_saved_ssid, device_info->wifi.one_key_config.ssid, sizeof(last_saved_ssid));
		strncpy(last_saved_passwd, device_info->wifi.one_key_config.passwd, sizeof(last_saved_passwd));
        printf("In common last_saved_ssid = %s\r\n",last_saved_ssid);
		ESP_LOGI(TAG, "ssid= %s, passwd = %s", device_info->wifi.one_key_config.ssid, device_info->wifi.one_key_config.passwd); 
//阿里云三元素每次从.bin文件读取，不再从flash读取
// #if ALIYUN_BURN		
// 		len = sizeof(out_value);
// 		ESP_ERROR_CHECK(nvs_get_str(nvs_config_handler, "id", out_value, &len));
// 		memcpy(device_info->id, out_value, len);
// 		memcpy(device_info->aliyun.device_id, device_info->id, strlen(device_info->id));

// 		len = sizeof(out_value);
// 		ESP_ERROR_CHECK(nvs_get_str(nvs_config_handler, "iotKey", out_value, &len));
// 		memcpy(device_info->aliyun.product_key, out_value, len);
// 		len = sizeof(out_value);
// 		ESP_ERROR_CHECK(nvs_get_str(nvs_config_handler, "iotSecret", out_value, &len));
// 		memcpy(device_info->aliyun.device_secret, out_value, len);
// #endif

		ESP_ERROR_CHECK(nvs_get_u8(nvs_config_handler, "dataUpSwitch", &device_info->data_up_switch));

		len = sizeof(out_value);
		ESP_ERROR_CHECK(nvs_get_u8(nvs_config_handler, "otaFlag", &device_info->ota.flag));
		ESP_ERROR_CHECK(nvs_get_str(nvs_config_handler, "version", out_value, &len));
		/* 新产品线版本前缀 SL_ESP_；NVS 若仍是 BC_ESP_* 则覆盖为 INIT_VERSION */
		if (strncmp(out_value, "SL_ESP_", 7) == 0) {
			memset(device_info->ota.running_version, 0, sizeof(device_info->ota.running_version));
			memcpy(device_info->ota.running_version, out_value,
			       len < sizeof(device_info->ota.running_version) ? len : sizeof(device_info->ota.running_version) - 1U);
		} else {
			ESP_LOGW(TAG, "nvs version \"%s\" 非 SL_ESP_，改用 INIT_VERSION=%s",
			         out_value, INIT_VERSION);
			memset(device_info->ota.running_version, 0, sizeof(device_info->ota.running_version));
			strncpy(device_info->ota.running_version, INIT_VERSION,
			        sizeof(device_info->ota.running_version) - 1U);
			ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "version", device_info->ota.running_version));
			ESP_ERROR_CHECK(nvs_commit(nvs_config_handler));
		}
		len = sizeof(out_value);

		/* 左右各一份最新报告进度；device_info->report 镜像左路供 deviceCli */
		len = sizeof(out_value);
		memset(out_value, 0, sizeof(out_value));
		err = nvs_get_str(nvs_config_handler, "report_L", out_value, &len);
		if (err == ESP_OK && out_value[0] != '\0') {
			memcpy(device_info->report, out_value, len < sizeof(device_info->report) ? len : sizeof(device_info->report) - 1);
		} else {
			memcpy(device_info->report, "NONE", 5);
		}
		ESP_LOGI(TAG, "report_L= %s", device_info->report); 
		printf("snore: up_hold_time_s = %d, threshold_5s = %d, threshold = %d, pwm = %d, tmr = %d\n",
																									device_info->snore->snore_parameters.up_hold_time_s,
																									device_info->snore->snore_parameters.threshold_5s,
																									device_info->snore->snore_parameters.threshold,
																									device_info->snore->snore_parameters.pwm,
																									device_info->snore->snore_parameters.tmr);

		ESP_LOGI(TAG, "read nvs config ok... \n");
	}
	else
	{
		ESP_LOGI(TAG, "nvs config init now... \n");    //esp32 flash中不存在相应参数，将默认参数存入
		ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "wifiSsid", device_info->wifi.one_key_config.ssid));
		ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "wifiPasswd", device_info->wifi.one_key_config.passwd));


		ESP_LOGI(TAG, "write default snore param to flash...");
        ESP_ERROR_CHECK(nvs_set_blob(nvs_config_handler, "snore_param",  &device_info->snore->snore_parameters, sizeof(snore_parameters_t)));
		printf("snore: up_hold_time_s = %d, threshold_5s = %d, threshold = %d, pwm = %d, tmr = %d\n",
																									device_info->snore->snore_parameters.up_hold_time_s,
																									device_info->snore->snore_parameters.threshold_5s,
																									device_info->snore->snore_parameters.threshold,
																									device_info->snore->snore_parameters.pwm,
																									device_info->snore->snore_parameters.tmr);
//阿里云三元素不再转存到flash
		// ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "id", device_info->id));
		// ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "iotKey", device_info->aliyun.product_key));
		// ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "iotSecret", device_info->aliyun.device_secret));

		ESP_ERROR_CHECK(nvs_set_u8(nvs_config_handler, "dataUpSwitch", device_info->data_up_switch));

		ESP_ERROR_CHECK(nvs_set_u8(nvs_config_handler, "otaFlag", device_info->ota.flag));
    	ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "version", device_info->ota.running_version));

		ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "report_L", "NONE"));
		ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "report_R", "NONE"));
		memcpy(device_info->report, "NONE", 5);
		ESP_ERROR_CHECK(nvs_set_str(nvs_config_handler, "configFlag", "config_1bc"));
		printf("nvs config update ok. \n");
	}
	ESP_ERROR_CHECK(nvs_commit(nvs_config_handler));
	print_nvs_usage();
	nvs_close(nvs_config_handler);

	strncpy(last_saved_ssid, device_info->wifi.one_key_config.ssid, sizeof(last_saved_ssid));
	strncpy(last_saved_passwd, device_info->wifi.one_key_config.passwd, sizeof(last_saved_passwd));
	ESP_LOGI(TAG, "ssid= %s, passwd = %s", device_info->wifi.one_key_config.ssid, device_info->wifi.one_key_config.passwd);
	ESP_LOGI(TAG, "otaFlag= %d, version = %s", device_info->ota.flag, device_info->ota.running_version);
}

static void led_init(void)
{
	esp_rom_gpio_pad_select_gpio(LED_BLUE);    
	gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);
	gpio_set_level(LED_BLUE, 1);

	esp_rom_gpio_pad_select_gpio(LED_GREEN);    
	gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
	gpio_set_level(LED_GREEN, 1);

	esp_rom_gpio_pad_select_gpio(LED_RED);    
	gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
	gpio_set_level(LED_RED, 0);
}

/** 上电默认打开 SU3 传感器供电（GPIO12 高电平） */
static void su3_pwr_en_init(void)
{
	gpio_config_t io = {
		.pin_bit_mask = (1ULL << SU3_PWR_EN_GPIO),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io);
	gpio_set_level(SU3_PWR_EN_GPIO, 1);
	ESP_LOGI(TAG, "SU3 power EN GPIO%d = 1", SU3_PWR_EN_GPIO);
	/* 错峰：等传感器上电电流回落后再启 BLE/WiFi，减轻 brownout */
	vTaskDelay(pdMS_TO_TICKS(10000)); /// 感觉硬件问题，20s太长了，后面可以改短点，2s会一直重启
	ESP_LOGI(TAG, "SU3 power settle 2000ms done");
}

void device_init(void)
{
	param_config_init();
	config_store_to_flash();
	led_init();
	su3_pwr_en_init();  // 上电默认打开 SU3 传感器供电（GPIO12 高电平）
	print_nvs_usage();
	//set_one_key_config_wifi_status(1);
}
