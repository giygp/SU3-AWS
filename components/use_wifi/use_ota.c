/*
 * @Author: zhong chenjian zhongcj@softide.cn
 * @Date: 2021-10-07 23:03:40
 * @LastEditors: zhong chenjian zhongcj@softide.cn
 * @LastEditTime: 2022-06-22 00:03:29
 * @FilePath: /smart-air-bed-board-program/components/use_wifi/use_ota.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "use_ota.h"
#include "common.h"
#include "mqtt_client.h"
#include "app_control.h"
#include "su3_client.h"

static const char *TAG = "ota";
extern device_info_t *device_info;
extern esp_mqtt_client_handle_t client;
extern char ota_infor_publish_topic[64];
extern char ota_progress_publish_topic[64];

static bool s_mqtt_paused_for_ota = false;

/* OTA HTTPS 下载前暂停 MQTT TLS，释放内部 RAM 给第二路 mbedTLS（对齐 ESP_BC） */
static void ota_mqtt_pause_for_download(void)
{
    if (client == NULL) {
        return;
    }
    ESP_LOGI(TAG, "OTA pause MQTT for HTTPS download");
    esp_mqtt_client_stop(client);
    set_mqtt_status(0);
    s_mqtt_paused_for_ota = true;
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void ota_mqtt_resume_after_download(void)
{
    if (!s_mqtt_paused_for_ota || client == NULL) {
        return;
    }
    s_mqtt_paused_for_ota = false;
    if (!get_wifi_status()) {
        return;
    }
    ESP_LOGI(TAG, "OTA resume MQTT after HTTPS download");
    {
        esp_err_t err = esp_mqtt_client_start(client);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "resume MQTT failed: %s", esp_err_to_name(err));
        }
    }
}

bool ota_mqtt_is_paused(void)
{
    return s_mqtt_paused_for_ota;
}

/** 板端 OTA 模块名（阿里云 default） */
#define OTA_MODULE_DEFAULT  "default"
/** 传感器 OTA 模块名（云端须创建同名模块） */
#define OTA_MODULE_SENSOR   "sensor"

/**
 * 阿里云 OTA 进度：/ota/device/progress/...
 * step: 1~100 进度；-1 失败；-2 下载失败；-4 烧写失败
 */
static void ota_report_progress(const char *module, const char *step, const char *desc)
{
    char buf[280];

    if (step == NULL || ota_progress_publish_topic[0] == '\0') {
        return;
    }
    if (module == NULL || module[0] == '\0') {
        module = OTA_MODULE_DEFAULT;
    }
    snprintf(buf, sizeof(buf),
             "{\"id\":\"1\",\"params\":{\"step\":\"%s\",\"desc\":\"%s\",\"module\":\"%s\"}}",
             step, (desc && desc[0]) ? desc : "", module);
    ESP_LOGD(TAG, "ota progress module=%s step=%s", module, step);
    if (client != NULL && get_mqtt_status()) {
        esp_mqtt_client_publish(client, ota_progress_publish_topic, buf,
                                (int)strlen(buf), 1, 0);
    }
}

/** 上报版本 inform（云端以版本匹配判定 OTA 成功） */
static void ota_report_inform(const char *module, const char *version)
{
    char buf[192];

    if (version == NULL || version[0] == '\0' || ota_infor_publish_topic[0] == '\0') {
        return;
    }
    if (module == NULL || module[0] == '\0') {
        module = OTA_MODULE_DEFAULT;
    }
    snprintf(buf, sizeof(buf),
             "{\"id\":\"1\",\"params\":{\"version\":\"%s\",\"module\":\"%s\"}}",
             version, module);
    ESP_LOGI(TAG, "ota inform module=%s ver=%s", module, version);
    if (client != NULL && get_mqtt_status()) {
        esp_mqtt_client_publish(client, ota_infor_publish_topic, buf,
                                (int)strlen(buf), 1, 0);
    }
}

/**
 * CLI 版本 → 云端包名风格，便于控制台与升级包对齐。
 * SU2-2.0.3,Board:2.0 → SU2_2_0_3_Board_2
 * 已是下划线格式则原样拷贝。
 */
static bool sensor_ver_to_cloud_format(const char *in, char *out, size_t out_len)
{
    int a = 0, b = 0, c = 0, board = 0;
    const char *prefix = "SU2";

    if (in == NULL || out == NULL || out_len < 16U) {
        return false;
    }
    if (strncmp(in, "SU3", 3) == 0) {
        prefix = "SU3";
    } else if (strncmp(in, "SU2", 3) != 0) {
        return false;
    }
    /* 已是云端包名 */
    if (in[3] == '_') {
        strncpy(out, in, out_len - 1U);
        out[out_len - 1U] = '\0';
        return true;
    }
    /* SU2-2.0.3,Board:2.0 */
    if ((in[3] == '-') &&
        sscanf(in + 4, "%d.%d.%d,Board:%d", &a, &b, &c, &board) == 4) {
        snprintf(out, out_len, "%s_%d_%d_%d_Board_%d", prefix, a, b, c, board);
        return true;
    }
    return false;
}

/** 传感器 OTA 成功：进度 100 + inform 目标版本 */
static void sensor_ota_report_ok(const char *version)
{
    ota_report_progress(OTA_MODULE_SENSOR, "100", "sensor ota success");
    ota_report_inform(OTA_MODULE_SENSOR,
                      version && version[0] ? version : device_info->ota.upgrade_version);
}

/** 传感器 OTA 失败 */
static void sensor_ota_report_fail(const char *step, const char *desc)
{
    ota_report_progress(OTA_MODULE_SENSOR, step, desc);
}

void sensor_ota_report_version_on_mqtt(void)
{
    const char *cli_ver = su3_get_sensor_fw_version();
    char cloud_ver[48];

    if (cli_ver == NULL || cli_ver[0] == '\0') {
        return;
    }
    if (sensor_ver_to_cloud_format(cli_ver, cloud_ver, sizeof(cloud_ver))) {
        ota_report_inform(OTA_MODULE_SENSOR, cloud_ver);
    } else {
        ota_report_inform(OTA_MODULE_SENSOR, cli_ver);
    }
}
//http操作函数
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
    case HTTP_EVENT_HEADER_SENT:
    case HTTP_EVENT_ON_HEADER:
    case HTTP_EVENT_ON_DATA:
    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
    return ESP_OK;
}

/*
 * 与 ESP_BC 相同：cert_pem = GlobalSign + iot.smartbed.ink 自签 CA。
 * 云端 URL 为 https://oss-iot.smartbed.ink/...（私有域名），公网 crt_bundle 验不过。
 * IDF 4.4 下 bundle 与 cert_pem 互斥，此处必须用 cert_pem。
 */
static void ota_fill_https_cfg(esp_http_client_config_t *cfg, const char *url)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->event_handler = _http_event_handler;
    cfg->cert_pem = (char *)ca_root_cert;
    cfg->timeout_ms = CONFIG_EXAMPLE_OTA_RECV_TIMEOUT;
    cfg->keep_alive_enable = true;
}

//校验是否需要升级(版本比较)
/* 版本格式：SL_ESP_YYYY_A_B_C / BC_ESP_YYYY_A_B_C，取末三段数字拼接比较 */
static int ota_version_code(const char *ver)
{
    int a = 0, b = 0, c = 0;

    if (ver == NULL || ver[0] == '\0') {
        return -1;
    }
    /* SL_ESP_2026_1_0_0 → 忽略前两段与年份，取 A_B_C */
    if (sscanf(ver, "%*[^_]_%*[^_]_%*d_%d_%d_%d", &a, &b, &c) != 3) {
        /* 兼容旧硬编码下标：??_ESP_YYYY_A_B_C 且 A/B/C 为单位数字 */
        if (strlen(ver) > 16U) {
            a = ver[12] - '0';
            b = ver[14] - '0';
            c = ver[16] - '0';
        } else {
            return -1;
        }
    }
    return a * 100 + b * 10 + c;
}

esp_err_t device_firmware_version_check(void)
{
    int run_code;
    int up_code;

    ESP_LOGI(TAG, "Running version: %s\nupgrade version: %s\n", device_info->ota.running_version,
             device_info->ota.upgrade_version);
    run_code = ota_version_code(device_info->ota.running_version);
    up_code = ota_version_code(device_info->ota.upgrade_version);
    ESP_LOGI(TAG, "version code run=%d upgrade=%d", run_code, up_code);
    if (run_code >= 0 && up_code >= 0 && run_code == up_code)
    {
        ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
        //return ESP_FAIL;
    }
    return ESP_OK;
}
void upgrade_nvs_infor(void)
{
    char ota_version[128] = {0};
    sprintf(ota_version,"{\"id\": \"1\",\"params\": {\"version\": \"%s\",\"module\":\"default\"}}",device_info->ota.upgrade_version);
    printf("%s\n",ota_version);
    esp_mqtt_client_publish(client, ota_infor_publish_topic, (char *)ota_version, strlen((char *)ota_version), 1, 0);

    nvs_handle ota_handlel;
    esp_err_t err;
    err = nvs_open("config_cfg", NVS_READWRITE, &ota_handlel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle, error: %s", esp_err_to_name(err));
        return; 
    }
    err = nvs_set_u8(ota_handlel, "otaFlag", 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set otaFlag, error: %s", esp_err_to_name(err));
        return; 
    }
    err = nvs_set_str(ota_handlel, "version", device_info->ota.upgrade_version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set version, error: %s", esp_err_to_name(err));
        return; 
    }
    err = nvs_commit(ota_handlel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes, error: %s", esp_err_to_name(err));
        return; 
    }
    nvs_close(ota_handlel);

    /* xiugai
        ESP_ERROR_CHECK(nvs_open("config_cfg", NVS_READWRITE, &ota_handlel));
        ESP_ERROR_CHECK(nvs_set_u8(ota_handlel, "otaFlag", 1));
        ESP_ERROR_CHECK(nvs_set_str(ota_handlel, "version", device_info->ota.upgrade_version));
        ESP_ERROR_CHECK(nvs_commit(ota_handlel));
        nvs_close(ota_handlel);
    */
}

void advanced_ota_example_task(void *pvParameter)
{
    ESP_LOGI(TAG, "ESP board OTA start version=%s", device_info->ota.upgrade_version);
    ESP_LOGI(TAG, "url: %s", device_info->ota.url);
    ESP_LOGI(TAG, "OTA https verify=cert_pem (GlobalSign+tuya CA)");
    log_heap_info("ota_before_https");
    ota_mqtt_pause_for_download();
    esp_err_t ota_finish_err = ESP_OK;
    esp_http_client_config_t config;
    ota_fill_https_cfg(&config, device_info->ota.url);

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed: %s", esp_err_to_name(err));
        log_heap_info("ota_begin_fail");
        set_ota_now_flag(0);
        ota_mqtt_resume_after_download();
        vTaskDelete(NULL);
        return;
    }
    log_heap_info("ota_begin_ok");

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_https_ota_read_img_desc failed");
        goto ota_end;
    }
    err = device_firmware_version_check();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "image header verification failed");
        goto ota_end;
    }

    while (1)
    {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            ESP_LOGD(TAG, "https_ota_perform err");
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        ESP_LOGD(TAG, "Image bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true)
    {
        // the OTA image was not completely received and user can customise the response to this situation.
        ESP_LOGE(TAG, "Complete data was not received.");   // 检查ota完整性
    }else {
        ota_finish_err = esp_https_ota_finish(https_ota_handle);
        set_ota_now_flag(0);
        if ((err == ESP_OK) && (ota_finish_err == ESP_OK)) {
            /* 版本号以云端 OTA 下发的 upgrade_version 为准，写入 NVS（upgrade_nvs_infor） */
            upgrade_nvs_infor();
            ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }else {
            if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            }
            ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed 0x%d", ota_finish_err);
            ota_mqtt_resume_after_download();
            vTaskDelete(NULL);
        }
    }
ota_end:
    set_ota_now_flag(0);
    if (https_ota_handle != NULL) {
        esp_https_ota_abort(https_ota_handle);
    }
    ota_mqtt_resume_after_download();
    ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed");
    vTaskDelete(NULL);
/*之前版本
ota_end:
    ota_finish_err = esp_https_ota_finish(https_ota_handle);
    set_ota_now_flag(0);
    if ((err == ESP_OK) && (ota_finish_err == ESP_OK))
    {
        upgrade_nvs_infor();
        ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    }
    else
    {
        if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed %d", ota_finish_err);
        vTaskDelete(NULL);
    }
*/
}

static esp_err_t ota_get_partition_version(const esp_partition_t *part, char *ver, size_t ver_len)
{
    esp_app_desc_t desc;

    if (part == NULL || ver == NULL || ver_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ver[0] = '\0';
    esp_err_t err = esp_ota_get_partition_description(part, &desc);
    if (err != ESP_OK) {
        return err;
    }
    strncpy(ver, desc.version, ver_len - 1);
    ver[ver_len - 1] = '\0';
    return ESP_OK;
}

static esp_err_t ota_nvs_write_version(const char *version)
{
    nvs_handle nvs_hdl;
    esp_err_t err;

    if (version == NULL || version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open("config_cfg", NVS_READWRITE, &nvs_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_hdl, "version", version);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_hdl, "otaFlag", 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_hdl);
    }
    nvs_close(nvs_hdl);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write version failed: %s", esp_err_to_name(err));
        return err;
    }

    if (device_info != NULL) {
        strncpy(device_info->ota.running_version, version,
                sizeof(device_info->ota.running_version) - 1);
        device_info->ota.running_version[sizeof(device_info->ota.running_version) - 1] = '\0';
        device_info->ota.flag = 1;
    }

    ESP_LOGI(TAG, "rollback nvs version=%s, otaFlag=1 (mqtt will report)", version);
    return ESP_OK;
}

void ota_log_boot_partition_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_app_desc_t running_desc = {0};
    esp_app_desc_t boot_desc = {0};
    esp_err_t running_err = ESP_FAIL;
    esp_err_t boot_err = ESP_FAIL;

    if (running != NULL) {
        running_err = esp_ota_get_partition_description(running, &running_desc);
        ESP_LOGI(TAG, "running partition: %s @ 0x%x size 0x%x",
                 running->label, (unsigned)running->address, (unsigned)running->size);
        if (running_err == ESP_OK) {
            ESP_LOGI(TAG, "running image ver: %s", running_desc.version);
        } else {
            ESP_LOGW(TAG, "running image desc read failed: %s", esp_err_to_name(running_err));
        }
    } else {
        ESP_LOGW(TAG, "running partition unknown");
    }

    if (boot != NULL) {
        boot_err = esp_ota_get_partition_description(boot, &boot_desc);
        if (running != NULL && boot->address == running->address) {
            ESP_LOGI(TAG, "next boot partition: %s (same as running)", boot->label);
        } else {
            ESP_LOGI(TAG, "next boot partition: %s @ 0x%x size 0x%x",
                     boot->label, (unsigned)boot->address, (unsigned)boot->size);
        }
        if (boot_err == ESP_OK) {
            ESP_LOGI(TAG, "next boot image ver: %s", boot_desc.version);
        } else {
            ESP_LOGW(TAG, "next boot image desc read failed: %s", esp_err_to_name(boot_err));
        }
    } else {
        ESP_LOGW(TAG, "boot partition unknown");
    }

    /* 两 OTA 槽镜像 app_desc.version（回退写 NVS 即读此值，与 NVS 业务 version 可能不同） */
    {
        const esp_partition_t *slots[2];
        char slot_ver[32];

        slots[0] = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                            ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        slots[1] = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                            ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
        for (int i = 0; i < 2; i++) {
            if (slots[i] == NULL) {
                ESP_LOGW(TAG, "slot ota_%d: partition not found", i);
                continue;
            }
            if (ota_get_partition_version(slots[i], slot_ver, sizeof(slot_ver)) == ESP_OK) {
                ESP_LOGI(TAG, "slot %s app_desc.version: %s", slots[i]->label, slot_ver);
            } else {
                ESP_LOGW(TAG, "slot %s: empty or invalid image", slots[i]->label);
            }
        }
    }
}

void ota_start(void)
{
    if (get_ota_now_flag()) {
        ESP_LOGW(TAG, "OTA already in progress, ignore duplicate upgrade");
        return;
    }
    set_ota_now_flag(1);
    xTaskCreate(advanced_ota_example_task, "advanced_ota_example_task", 1024 * 4, NULL, 1, NULL);
}

/* ---------- 传感器固件：云端 → ESP 下载落盘（串口 port4 刷写后续再接） ---------- */

#define SENSOR_FW_MAX_BYTES   (1024U * 1024U)

typedef struct {
    uint8_t *data;
    size_t   len;
    char     version[32];
    bool     ready;
} sensor_fw_image_t;

static sensor_fw_image_t s_sensor_fw;

bool ota_upgrade_is_sensor_fw(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return false;
    }
    /* 云端：SU2_2_0_3_Board_2；传感器应答：SU2-2.0.2,Board:2.0 */
    if (strncmp(version, "SU2", 3) == 0 || strncmp(version, "SU3", 3) == 0) {
        return true;
    }
    return false;
}

/**
 * 归一化版本码，便于 2.0.2 与 2_0_3 比较。
 * 支持：SU2-2.0.2,Board:2.0 / SU2_2_0_3_Board_2 / 2.0.3
 */
static int sensor_fw_version_code(const char *ver)
{
    const char *p = ver;
    int a = 0, b = 0, c = 0;

    if (ver == NULL || ver[0] == '\0') {
        return -1;
    }
    if (strncmp(p, "SU2", 3) == 0 || strncmp(p, "SU3", 3) == 0) {
        p += 3;
        while (*p == '-' || *p == '_') {
            p++;
        }
    }
    if (sscanf(p, "%d.%d.%d", &a, &b, &c) == 3) {
        return a * 10000 + b * 100 + c;
    }
    if (sscanf(p, "%d_%d_%d", &a, &b, &c) == 3) {
        return a * 10000 + b * 100 + c;
    }
    return -1;
}

void sensor_ota_image_free(void)
{
    if (s_sensor_fw.data != NULL) {
        free(s_sensor_fw.data);
        s_sensor_fw.data = NULL;
    }
    s_sensor_fw.len = 0;
    s_sensor_fw.ready = false;
    s_sensor_fw.version[0] = '\0';
}

bool sensor_ota_image_ready(void)
{
    return s_sensor_fw.ready && s_sensor_fw.data != NULL && s_sensor_fw.len > 0U;
}

const uint8_t *sensor_ota_image_data(void)
{
    return sensor_ota_image_ready() ? s_sensor_fw.data : NULL;
}

size_t sensor_ota_image_len(void)
{
    return sensor_ota_image_ready() ? s_sensor_fw.len : 0U;
}

const char *sensor_ota_image_version(void)
{
    return s_sensor_fw.version;
}

static esp_err_t sensor_fw_http_download(const char *url, uint8_t **out_buf, size_t *out_len)
{
    esp_http_client_config_t cfg;
    ota_fill_https_cfg(&cfg, url);
    ESP_LOGI(TAG, "sensor fw https verify=cert_pem (GlobalSign+tuya CA)");
    esp_http_client_handle_t client_http;
    int content_len;
    int status;
    int total = 0;
    int n;
    uint8_t *buf = NULL;
    esp_err_t err;

    *out_buf = NULL;
    *out_len = 0;

    client_http = esp_http_client_init(&cfg);
    if (client_http == NULL) {
        return ESP_FAIL;
    }
    err = esp_http_client_open(client_http, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor fw http open fail: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client_http);
        return err;
    }
    content_len = esp_http_client_fetch_headers(client_http);
    status = esp_http_client_get_status_code(client_http);
    printf("ota: sensor fw HTTP status=%d len=%d\n", status, content_len);
    if (status != 200 || content_len <= 0 || (size_t)content_len > SENSOR_FW_MAX_BYTES) {
        ESP_LOGE(TAG, "sensor fw bad response status=%d len=%d", status, content_len);
        esp_http_client_close(client_http);
        esp_http_client_cleanup(client_http);
        return ESP_FAIL;
    }

    buf = (uint8_t *)heap_caps_malloc((size_t)content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)malloc((size_t)content_len);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "sensor fw malloc %d fail", content_len);
        esp_http_client_close(client_http);
        esp_http_client_cleanup(client_http);
        return ESP_ERR_NO_MEM;
    }

    {
        int last_pct = -1;
        while (total < content_len) {
            int pct;
            n = esp_http_client_read(client_http, (char *)buf + total, content_len - total);
            if (n < 0) {
                ESP_LOGE(TAG, "sensor fw read err");
                free(buf);
                esp_http_client_close(client_http);
                esp_http_client_cleanup(client_http);
                return ESP_FAIL;
            }
            if (n == 0) {
                break;
            }
            total += n;
            pct = (int)(((int64_t)total * 100) / content_len);
            if (pct >= last_pct + 25 || total >= content_len) {
                last_pct = pct;
                printf("ota: download %d%%\n", pct);
            }
        }
    }
    esp_http_client_close(client_http);
    esp_http_client_cleanup(client_http);

    if (total != content_len) {
        ESP_LOGE(TAG, "sensor fw short read %d/%d", total, content_len);
        free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    *out_len = (size_t)total;
    return ESP_OK;
}

static void sensor_ota_download_task(void *pv)
{
    const char *cur_ver;
    int run_code;
    int up_code;
    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t err;
    esp_err_t flash_err = ESP_FAIL;
    char rsp[96];
    int wait_i;
    (void)pv;

    /* 等双侧 setup 完成后再动串口，避免与初始化/首轮补传抢总线 */
    ota_report_progress(OTA_MODULE_SENSOR, "5", "wait setup");
    for (wait_i = 0; wait_i < 120; wait_i++) {
        if (su3_sensors_setup_ready() && su3_is_ready()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!su3_sensors_setup_ready() || !su3_is_ready()) {
        ESP_LOGE(TAG, "sensor OTA abort: setup not ready");
        sensor_ota_report_fail("-1", "setup not ready");
        set_ota_now_flag(0);
        vTaskDelete(NULL);
        return;
    }

    cur_ver = su3_get_sensor_fw_version();
    run_code = sensor_fw_version_code(cur_ver);
    up_code = sensor_fw_version_code(device_info->ota.upgrade_version);
    printf("ota: sensor OTA %s -> %s\n",
           (cur_ver && cur_ver[0]) ? cur_ver : "?",
           device_info->ota.upgrade_version);

    if (run_code >= 0 && up_code >= 0 && run_code >= up_code) {
        ESP_LOGW(TAG, "sensor already >= target");
        sensor_ota_report_ok(device_info->ota.upgrade_version);
        set_ota_now_flag(0);
        vTaskDelete(NULL);
        return;
    }

    ota_report_progress(OTA_MODULE_SENSOR, "10", "downloading");
    sensor_ota_image_free();
    ota_mqtt_pause_for_download();
    err = sensor_fw_http_download(device_info->ota.url, &buf, &len);
    ota_mqtt_resume_after_download();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor fw download failed");
        sensor_ota_report_fail("-2", "download failed");
        set_ota_now_flag(0);
        vTaskDelete(NULL);
        return;
    }

    s_sensor_fw.data = buf;
    s_sensor_fw.len = len;
    s_sensor_fw.ready = true;
    strncpy(s_sensor_fw.version, device_info->ota.upgrade_version,
            sizeof(s_sensor_fw.version) - 1U);
    s_sensor_fw.version[sizeof(s_sensor_fw.version) - 1U] = '\0';
    printf("ota: sensor fw downloaded len=%u\n", (unsigned)len);
    for (wait_i = 0; wait_i < 20; wait_i++) {
        if (get_mqtt_status()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ota_report_progress(OTA_MODULE_SENSOR, "50", "download ok");

    for (wait_i = 0; wait_i < 120; wait_i++) {
        if (!su3_cli_worker_busy()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (su3_cli_worker_busy()) {
        ESP_LOGE(TAG, "sensor OTA abort: cli busy");
        sensor_ota_image_free();
        sensor_ota_report_fail("-1", "cli busy");
        set_ota_now_flag(0);
        vTaskDelete(NULL);
        return;
    }

    ota_report_progress(OTA_MODULE_SENSOR, "60", "uart flashing");
    flash_err = su3_sensor_ota(su3_side_to_addr(SU3_SIDE_LEFT), buf, len);

    if (flash_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        memset(rsp, 0, sizeof(rsp));
        err = su3_cli_exec(su3_side_to_addr(SU3_SIDE_LEFT), "version",
                           rsp, sizeof(rsp), 2000);
        if (err == ESP_OK && rsp[0] != '\0' && strncmp(rsp, "ok", 2) != 0) {
            printf("ota: sensor OTA ok ver=%s\n", rsp);
            su3_sensor_ver_save(rsp);
        } else {
            ESP_LOGW(TAG, "sensor OTA ok, version confirm fail");
            su3_sensor_ver_save(s_sensor_fw.version);
        }
        sensor_reboot_config();
        sensor_ota_report_ok(device_info->ota.upgrade_version);
    } else {
        ESP_LOGE(TAG, "sensor uart flash failed");
        sensor_ota_report_fail("-4", "uart flash failed");
    }

    sensor_ota_image_free();
    set_ota_now_flag(0);
    vTaskDelete(NULL);
}

void sensor_ota_start(void)
{
    if (get_ota_now_flag()) {
        ESP_LOGW(TAG, "OTA already in progress, ignore sensor upgrade");
        return;
    }
    if (device_info->ota.url[0] == '\0' || device_info->ota.upgrade_version[0] == '\0') {
        ESP_LOGE(TAG, "sensor OTA missing url/version");
        sensor_ota_report_fail("-1", "missing url or version");
        return;
    }
    set_ota_now_flag(1);
    xTaskCreate(sensor_ota_download_task, "sensor_ota_dl", 10240, NULL, 5, NULL);
}

esp_err_t ota_rollback_to_partition(int slot, ota_rollback_result_t *result,
                                    char *out_msg, size_t out_len)
{
    const esp_partition_t *target = NULL;
    const esp_partition_t *running = esp_ota_get_running_partition();
    char running_ver[32] = {0};
    char target_ver[32] = {0};

    if (out_msg == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_msg[0] = '\0';

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    if (running != NULL) {
        ota_get_partition_version(running, running_ver, sizeof(running_ver));
        if (result != NULL) {
            strncpy(result->running_part, running->label, sizeof(result->running_part) - 1);
            strncpy(result->running_ver, running_ver, sizeof(result->running_ver) - 1);
        }
    }

    if (get_ota_now_flag()) {
        snprintf(out_msg, out_len, "ota in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (slot == OTA_ROLLBACK_SLOT_OTHER) {
        target = esp_ota_get_next_update_partition(running);
    } else if (slot == 0) {
        target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                          ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    } else if (slot == 1) {
        target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                          ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    } else {
        snprintf(out_msg, out_len, "invalid slot");
        return ESP_ERR_INVALID_ARG;
    }

    if (target == NULL) {
        snprintf(out_msg, out_len, "target partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (running != NULL && target->address == running->address) {
        snprintf(out_msg, out_len, "already on %s", target->label);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ota_get_partition_version(target, target_ver, sizeof(target_ver));
    if (err != ESP_OK) {
        snprintf(out_msg, out_len, "%s empty or invalid", target->label);
        ESP_LOGE(TAG, "partition %s invalid: %s", target->label, esp_err_to_name(err));
        return err;
    }

    if (result != NULL) {
        strncpy(result->target_part, target->label, sizeof(result->target_part) - 1);
        strncpy(result->target_ver, target_ver, sizeof(result->target_ver) - 1);
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        snprintf(out_msg, out_len, "set boot failed:%s", esp_err_to_name(err));
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ota_nvs_write_version(target_ver);

    ESP_LOGI(TAG, "rollback %s(%s) -> %s(%s)",
             running != NULL ? running->label : "?",
             running_ver[0] ? running_ver : "?",
             target->label, target_ver);
    snprintf(out_msg, out_len, "ok rebooting");
    return ESP_OK;
}

void ota_rollback_restart(void)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

