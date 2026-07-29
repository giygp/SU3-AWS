#ifndef AIOT_MQTT_AIGN_H_
#define AIOT_MQTT_AIGN_H_








 /* use_tls_sign：与 MQTT 链路一致。明文 mqtt:// 用 securemode=3；MQTTS 用 securemode=2。
* 签名中的 timestamp 与 clientId 扩展字段使用同一毫秒时间戳（与控制台工具生成规则一致）。
 */
int aiotMqttSign(const char *productKey, const char *deviceName, const char *deviceSecret,
                 char clientId[150], char username[64], char password[65],
                 int use_tls_sign);




/*
int aiotMqttSign(const char *productKey, const char *deviceName, const char *deviceSecret,
                    char clientId[150], char username[64], char password[65]);
*/
#endif 