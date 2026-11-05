/*
 * @Author: zhong chenjian zhongcj@softide.cn
 * @Date: 2021-10-07 23:03:40
 * @LastEditors: zhong chenjian zhongcj@softide.cn
 * @LastEditTime: 2022-06-22 00:01:41
 * @FilePath: /smart-air-bed-board-program/components/use_wifi/use_ota.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef USE_OTA_H_
#define USE_OTA_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

void advanced_ota_example_task(void *pvParameter);
void ota_start(void);

/** 云端 version 是否为传感器固件（SU2/SU3），走下载落盘而非板端分区 OTA */
bool ota_upgrade_is_sensor_fw(const char *version);
/** 启动传感器固件：版本比对 → HTTPS 下载 → port4 串口刷写 → 写 sensor_ver */
void sensor_ota_start(void);

/**
 * MQTT 连上时上报传感器当前版本（module=sensor）。
 * 云端需在产品下创建同名 OTA 模块，传感器包挂到该模块。
 */
void sensor_ota_report_version_on_mqtt(void);

bool sensor_ota_image_ready(void);
const uint8_t *sensor_ota_image_data(void);
size_t sensor_ota_image_len(void);
const char *sensor_ota_image_version(void);
void sensor_ota_image_free(void);

/** 切换 boot 分区：0=ota_0，1=ota_1，-1=当前非运行槽（OTA 回退推荐） */
#define OTA_ROLLBACK_SLOT_OTHER  (-1)

/** otaRollback 应答：回退前运行槽 / 回退目标槽及镜像版本 */
typedef struct {
    char running_part[16];
    char running_ver[32];
    char target_part[16];
    char target_ver[32];
} ota_rollback_result_t;

esp_err_t ota_rollback_to_partition(int slot, ota_rollback_result_t *result,
                                    char *out_msg, size_t out_len);
void ota_rollback_restart(void);
/** 上电打印当前运行分区、下次启动分区及镜像版本 */
void ota_log_boot_partition_info(void);
/** HTTPS 下载期间 MQTT 已 pause，GOT_IP 勿再 start（避免双 TLS） */
bool ota_mqtt_is_paused(void);

#define ca_root_cert "\
-----BEGIN CERTIFICATE-----\n\
MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG\n\
A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv\n\
b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw\n\
MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i\n\
YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT\n\
aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ\n\
jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp\n\
xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp\n\
1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG\n\
snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ\n\
U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8\n\
9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E\n\
BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B\n\
AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz\n\
yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE\n\
38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP\n\
AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad\n\
DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME\n\
HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==\n\
-----END CERTIFICATE-----\n\
-----BEGIN CERTIFICATE-----\n\
MIIEBTCCAu2gAwIBAgIUOiMDlRFu6/Snmcl3ajKz0WcXM5swDQYJKoZIhvcNAQEL\n\
BQAwgZAxCzAJBgNVBAYTAkNOMREwDwYDVQQIDAhaaGVKaWFuZzEQMA4GA1UEBwwH\n\
SmlhWGluZzEPMA0GA1UECgwGS2Vlc29uMQwwCgYDVQQLDANEJlIxGTAXBgNVBAMM\n\
EGlvdC5zbWFydGJlZC5pbmsxIjAgBgkqhkiG9w0BCQEWE3NxbC5zb25nQGtlZXNv\n\
bi5jb20wIBcNMjYwNjA4MDAyMzE3WhgPMjEyNjA1MTUwMDIzMTdaMIGQMQswCQYD\n\
VQQGEwJDTjERMA8GA1UECAwIWmhlSmlhbmcxEDAOBgNVBAcMB0ppYVhpbmcxDzAN\n\
BgNVBAoMBktlZXNvbjEMMAoGA1UECwwDRCZSMRkwFwYDVQQDDBBpb3Quc21hcnRi\n\
ZWQuaW5rMSIwIAYJKoZIhvcNAQkBFhNzcWwuc29uZ0BrZWVzb24uY29tMIIBIjAN\n\
BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAicRN0yZQuG+m6DgivDWDa3O02xJf\n\
oqE+ho0V1O8lrWahS42lrRKbLWQRyKaXfNseNQklJg+J2AueDt4oAfzAhQg3Pn1/\n\
SufwwtujTLPpfXNqgBbm6tF9DlVL+ML66FdOFXcUCT3uRD599pPIP99u4dS02OsV\n\
7O5drbn6GQTFWm26gjwLN12fOf0BgsjnQ1ugQ44YJR7kzIRny47kl3zzoLj2WMm2\n\
0Mf3R0OuKXZ/WuUcxZSMwNMLMeToFIjoKFbB24w5vBv2YkduB/F6w2QzPFK9zAf4\n\
wf4zn+UDbIHkD4WVbLwUG/d4xEECjpAYCP+qCB1DL8tbiZxjDF9svGr7dQIDAQAB\n\
o1MwUTAdBgNVHQ4EFgQU8wf5AZDvjUrJIHsK9eZTJpu79bIwHwYDVR0jBBgwFoAU\n\
8wf5AZDvjUrJIHsK9eZTJpu79bIwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0B\n\
AQsFAAOCAQEAh33JCkUzAyiUMLhOKWQmhkso5T/n1WsShTL5saNI5E2+k5VvOUqU\n\
o3s+IWc5Jg7biH05GyvXPk+laAkgP1ImsnTijNznzJRxJqGVOYSNViyfB1VxcdwS\n\
0WH/qSk/ZZxPNcS1rlRloLVmtEwoQo/EHN0hndHE3qNnmQanokHSfuRRsTltjObf\n\
+kaU/ewFoAaRMe80hfNSqSBylR+euetT31YQ7dZZSf5qecOFEOPHauNOpsHGa7d4\n\
Wv+4RO+c5jrfCXFMUdzPqUJpTuLCFWL9wH6xnBr4Aletklik+AJz56cTzB4jWDHa\n\
CxXCO4hhMTdGsPU3m5KzWO+bq0mo1Gr+BQ==\n\
-----END CERTIFICATE-----\n\
" 
#endif