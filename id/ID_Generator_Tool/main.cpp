#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
    // uint8_t auto_start;
    // uint8_t connect_result;
    
    // uint16_t port;
    // char host[65];
    char product_key[20];
    char device_name[20];
    char device_secret[50];
} qs_settings_aliyun_t;
static qs_settings_aliyun_t m_aliyun;

//多个
#if 1
static char product_host[] = "%s.iot-060a3upv.mqtt.iothub.aliyuncs.com";  //w801： iot-as-mqtt.cn-shanghai.aliyuncs.com
static char csv_file_name[] = "./ziyong.csv";
static char aliyun_bin_name[] = "./ziyong/%s.bin";
static char bin_file_name[60];
static char product_key[20];
static char device_name[20];
static char device_secret[50];
int main(int argc, char* argv[])
{
    char open_csv_file_name[80] = { 0 };
    printf("%s %s\r\n", argv[0], argv[1]);
    if (argc == 2)
    {
        memcpy(open_csv_file_name, argv[1], strlen(argv[1]));
    }
    else
    {
        memcpy(open_csv_file_name, csv_file_name, strlen(csv_file_name));
    }
    char buffer[1024];
    static char tmp[3][50];
    int i;
    char* record;
    char delims[] = ",";
    FILE* fp = fopen(open_csv_file_name, "r");  
    if (!fp)
    {
        printf("read csv err,can't open this CSV\r\n");
        return -1;
    }
    printf(">>%d\r\n",fscanf(fp, "%s", buffer));
    while (fscanf(fp, "%s", buffer) > 0)
    {
        record = strtok(buffer, ",");
        i = 0;
        while (record)
        {
            memcpy(tmp[i], record, strlen(record));
            i++;
            record = strtok(NULL, ",");
        }
        snprintf(bin_file_name, 60, aliyun_bin_name, tmp[0]);
        FILE* fp_bin = fopen(bin_file_name, "wb");
        if (!fp_bin)
        {
            printf("can't open the aliyun_bin file\r\n");
            return -2;
        }
        // m_aliyun.auto_start = 0;
        // m_aliyun.connect_result = 0;

        // m_aliyun.port = 1883;
        // snprintf(m_aliyun.host, 65, product_host, tmp[2]);
        printf(">>>%s\r\n", tmp[0]);
        memcpy(m_aliyun.device_name,    tmp[0], strlen(tmp[0]));
        memcpy(m_aliyun.device_secret,  tmp[1], strlen(tmp[1]));
        memcpy(m_aliyun.product_key,    tmp[2], strlen(tmp[2]));
        fwrite(&m_aliyun, 1, sizeof(m_aliyun), fp_bin);
        fclose(fp_bin);
    }
    fclose(fp);
}

#endif

//单个
#if 0

#define EXAMPLE_BIN_FILE            "BSLC0000001.aliyun"

#define EXAMPLE_HOST                "a1trPIy9rnm.iot-as-mqtt.cn-shanghai.aliyuncs.com"
#define EXAMPLE_PRODUCKEY           "a1trPIy9rnm"
#define EXAMPLE_DEVICENAME          "BSLC0000001"
#define EXAMPLE_DEVICESECRET        "bf4c27b1c1940243585c6ec5db6ca82a"
int main()
{

    FILE* fp_bin = fopen(EXAMPLE_BIN_FILE, "wb");
    if (!fp_bin)
    {
        printf("can't open the aliyun_bin file\r\n");
        return -2;
    }
    m_aliyun.auto_start = 0;
    m_aliyun.connect_result = 0;
    m_aliyun.port = 1883;
    memcpy(m_aliyun.host, EXAMPLE_HOST, sizeof(EXAMPLE_HOST));
    memcpy(m_aliyun.device_name, EXAMPLE_DEVICENAME, sizeof(EXAMPLE_DEVICENAME));
    memcpy(m_aliyun.device_secret, EXAMPLE_DEVICESECRET, sizeof(EXAMPLE_DEVICESECRET));
    memcpy(m_aliyun.product_key, EXAMPLE_PRODUCKEY, sizeof(EXAMPLE_PRODUCKEY));

    fwrite(&m_aliyun, 1, sizeof(m_aliyun), fp_bin);

    fclose(fp_bin);
    return 0;
}

#endif