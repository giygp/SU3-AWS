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
#include "nvs.h"
#include "nvs_flash.h"
#include "use_ota.h"
#include "common.h"
#include "mqtt_client.h"
#include "app_control.h"

static const char *TAG = "ota";
extern device_info_t *device_info;
extern esp_mqtt_client_handle_t client;
extern char ota_infor_publish_topic[64];

//http操作函数
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
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
    ESP_LOGI(TAG, "Starting Advanced OTA example");
    printf("url: %s\n", device_info->ota.url);
    esp_err_t ota_finish_err = ESP_OK;
    esp_http_client_config_t config = {
        .url = device_info->ota.url,
        .event_handler = _http_event_handler,
        .cert_pem = (char *)ca_root_cert,
        .timeout_ms = CONFIG_EXAMPLE_OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed");
        set_ota_now_flag(0);
        vTaskDelete(NULL);
    }

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
            vTaskDelete(NULL);
        }
    }
ota_end:
    set_ota_now_flag(0);
    esp_https_ota_abort(https_ota_handle);
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

