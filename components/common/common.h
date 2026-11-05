/*
 * @Author: fzh
 * @Date: 2026-07-14 18:19:35
 * @LastEditors: fzh 
 * @LastEditTime: 2026-07-14 18:19:35
 */
#ifndef COMMON_H_
#define COMMON_H_
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/*-----------传感器类型------------------------*/
#define BLE_TEST                                   0
/* 新产品联调：用下方宏三元组；量产烧录 storage 后再改回 1 */
#define ALIYUN_BURN                                1
/* ALIYUN_BURN=1：三元组来自 flash「storage」分区（产测烧录），与下方 PRODUCT_KEY 宏无关，易导致 MQTT「bad username or password」。
 * 迁移涂鸦 iot.smartbed.ink / 更正 ProductKey 期间改为 0，使用下方宏；量产烧录正确三元组到 storage 后再改回 1。 */

#define DATA_UP                                     1
#define OTA_ON_OFF                                  1

/* 1=使用 components/SU3 新栈（旧 UART 解析已移除，勿改回 0） */
#ifndef SU3_USE_NEW_STACK
#define SU3_USE_NEW_STACK                           1
#endif
#define SU3_SENSOR_SIDE_MAX                         2

#define MY_WIFI_SSID                                "keeson-office"//"keeson-office"//"DS" // xmhdesktop
#define MY_WIFI_PASSWD                              "Smartbed2025@"//"ksn88888"//"ds654321"  // 87632154
/* 涂鸦新产品 DeviceName（基座 00）；左右业务 id 改第 8～9 位为 03/06 → KSSLBED03000001 / KSSLBED06000001 */
#define DEVEICE_ID                                  "KSSLBED00000001"
/* 涂鸦 IoT ProductKey（控制台 ProductID） */
#define PRODUCT_KEY                                 "QuTVRApZSgFHJ3Vm"
#define DEVEICE_SECRET                              "f44a9751040c5a4de9c89a6c7dfd00b0"
/* OTA inform / 版本比较格式：SL_ESP_YYYY_A_B_C（A/B/C 为单位数字） */
#define INIT_VERSION                                "SL_ESP_2026_1_1_3"
// #define CINFIG_VERSION                              "settingConfig_001" 

#define UART1_TXD                                   (22)
#define UART1_RXD                                   (26)
#define RX_BUF_SIZE                                 (1024*10)   //缩减1024*8
#define LED_BLUE                                    (19)
#define LED_GREEN                                   (18) 
#define LED_RED                                     (5)   

#define ESP_TX0                                     (19)
#define ESP_RX0                                     (25)
#define UART_CTR                                    (33)
/** SU3 传感器模块电源使能：高电平上电（原理图 GPIO12，复位默认关） */
#define SU3_PWR_EN_GPIO                             (12)
/** APP/板端命令：GPIO12 先关再开的断电时长（ms） */
#define SU3_PWR_CYCLE_OFF_MS                        (500)

#define BUF_SIZE                                    (1024)  // uart2 接收缓存

#define UART_TX_BUF_SIZE                            1024                                         /**< UART TX buffer size. */
#define UART_RX_BUF_SIZE                            1024 

#define BOARD_MC232  1
#define BOARD_MC242  2
/*-----------板子型号------------------------*/
#define BOARD BOARD_MC242

#if BOARD == BOARD_MC232
    #define MOTOR_NUM      3
    #define REPORT_DATA_FMT "MC232_DATA"    // MC232 的数据格式
#elif BOARD == BOARD_MC242
    #define MOTOR_NUM      4
    #define REPORT_DATA_FMT "MC242_DATA"     // MC242 的数据格式
#endif


// 普通按键键值
#define		KEY_MOTOR_STOP					0x00000000	
#define		KEY_M1_OUT						0x00000001	//头部驱动器伸出；
#define		KEY_M1_IN						0x00000002	//头部驱动器缩进；
#define		KEY_M2_OUT						0x00000004	//脚部驱动器伸出；
#define		KEY_M2_IN						0x00000008	//脚部驱动器缩进；
#define		KEY_M3_OUT						0x00000010	//腰部顶出； 
#define		KEY_M3_IN						0x00000020	//腰部缩进； 
#define		KEY_M4_OUT						0x00000040	
#define		KEY_M4_IN						0x00000080	
#define		KEY_MASSAGE_All					0x00000100	
#define		KEY_MASSAGE_TIMER				0x00000200	
#define		KEY_MASSAGE_FEET				0x00000400		
#define		KEY_MASSAGE_HEAD				0x00000800	
#define		KEY_ZEROG					  	0x00001000	
#define		KEY_MEMORY2						0x00002000	//阅读位置
#define		KEY_MEMORY3						0x00004000	//TV	
#define		KEY_MEMORY4						0x00008000	//打鼾位置
#define		KEY_MEMORY5						0x00010000	//音乐位置
#define		KEY_UBB							0x00020000	
#define		KEY_STRECHMOVE					0x00040000	
#define		KEY_INTENSITY1					0x00080000	
#define		KEY_INTENSITY2					0x00100000	
#define		KEY_INTENSITY3					0x00200000	
#define		KEY_MASSAGE_WAIST				0x00400000	
#define		KEY_MASSAGE_HEAD_MINUS      	0x00800000	
#define		KEY_MASSAGE_FEET_MIUNS	        0x01000000	
#define		KEY_MASSAGE_STOP_ALL	    	0x02000000	
#define		KEY_MASSAGE_MODE				0x04000000	
#define		KEY_ALLFATE						0x08000000	// 所有电机放平
#define		KEY_MEMORY8						0x10000000	
#define		KEY_ANGLEADJUST					0x20000000	
#define		KEY_EXTMEMORY1				    0x40000000	
#define		KEY_EXTMEMORY2					0x80000000	

// 缓启动控制按键键值
#define		KEY_HEAD_LIFT					0x00008000	// 头部抬升
#define		KEY_HEAD_LOWER					0x08000000	// 头部放下
#define		KEY_ALL_MOTORS_LEVEL			0x08000000	// 所有电机放平


//组合键值
#define   KEY_FLAT_ZEROG      		(KEY_ZEROG|KEY_ALLFATE)
#define 	KEY_MASSAGE_LED			(KEY_UBB|KEY_MASSAGE_HEAD_MINUS|KEY_MASSAGE_FEET_MIUNS|KEY_MASSAGE_STOP_ALL|KEY_MASSAGE_MODE|KEY_MASSAGE_All|KEY_MASSAGE_TIMER|KEY_MASSAGE_FEET|KEY_MASSAGE_HEAD)
#define   KEY_SET_AB                (KEY_M1_OUT|KEY_M1_IN|KEY_M2_OUT|KEY_M2_IN)
#define   KEY_BED_UP                (KEY_M4_OUT|KEY_M3_OUT)
#define   KEY_BED_IN                (KEY_M4_IN|KEY_M3_IN)

// 按键类型
#define   KEYS_TYPE_NORMAL          0    // 普通键值
#define   KEYS_TYPE_SOFT_START      1    // 缓启动
#define   KEYS_TYPE_MASSAGE         2    // 按摩枚举

//一键助眠
#define KEY_ZEROG_MASSAGEALL      (KEY_ZEROG|KEY_MASSAGE_All)
#define KEY_ZEROG_MASSAGESTOP     (KEY_ZEROG|KEY_MASSAGE_STOP_ALL)
#define KEY_FLAT_MASSAGESTOP      (KEY_ALLFATE|KEY_MASSAGE_STOP_ALL)

// 打鼾干预
#define BLOCK_BUFFER_SIZE           24      // 2分钟内，共24次更新（每5秒一次）
#define SNORING_THRESHOLD           15      // 2分钟内打鼾阈值
#define SNORING_THRESHOLD_5S        1       // 5秒内打鼾阈值
#define SNORE_COOLDOWN_SECONDS      1800    // 打鼾干预持续时间 30分钟=1800秒

#define  UP_HOLD_TIME_S             1800    // 打鼾干预持续时间 30分钟=1800秒
#define  UP_HOLD_TIME_S_DEMO        120      // 打鼾演示流程冷却时间（秒）
#define  THRESHOLD_5S               1       // 5秒有一个打鼾包就判断为该包打鼾
#define  THRESHOLD                  15      // 2分钟内打鼾包阈值 2分钟15个则判断为打鼾 进行打鼾干预
#define  SNORING_PWM                80      // 打鼾干预PWM值 （速度）
#define  SNORING_TMR                20      // 打鼾演示流程抬起时间（时间 s） 

enum 
{
    wifi_ok = 0,
    wifi_fail,
    mqtt_ok,
    mqtt_fail,
}one_key_config_wifi_event_id_t;

typedef enum {
    WIFI_STATUS_WAITING = 0,     // 等待连接
    WIFI_STATUS_CONNECTED = 1,   // 连接成功
    WIFI_STATUS_RECONNECTING = 2,// 重连中
    WIFI_STATUS_FAILED = 3       // 重连失败，超过次数
} wifi_status_t;

//一键配网参数定义
typedef struct
{
    uint8_t flag;
    QueueHandle_t xQueue;
    char ssid[32];              //wifi名                                              
    char passwd[64];            //wifi密码
}one_key_config_wifi_info_t;

typedef struct
{
    uint8_t value[512];
    uint16_t len;
} data_rec_t;


typedef struct
{
    uint8_t value[64];
    uint16_t len;
} mqtt_key_rec_t;

typedef struct
{
    char topic[512];
    uint32_t topic_len;
    uint8_t data[1024];   /* OTA 等下发含长 URL，512 易截断 */
    uint32_t data_len;
} mmqtt_msg_t;

//wifi信息定义
typedef struct
{
    one_key_config_wifi_info_t one_key_config;         
    char ip_addr[16];                                               //ip地址
    int rssi;                                                       //wifi信号强度
    uint8_t flag;                                                     
}wifi_link_info_t;

typedef struct
{
    char running_version[32];
    char upgrade_version[32];
    bool flag;
    char url[1024];
}ota_info_t;

typedef struct
{
    char product_key[20];
    char device_id[20];
    char device_secret[50];
    bool flag;
    QueueHandle_t xQueue;
    mmqtt_msg_t msg;
}aliyun_link_info_t;

typedef struct
{
    char product_key[20];
    char device_name[20];
    char device_secret[50];
} qs_settings_aliyun_t;

typedef struct
{
    bool flag;
    uint32_t time_stamp;
    uint8_t time[6];
}utc_info_t;

/** 同时支持的手机 BLE 连接数（与业务“双用户同连”一致） */
#define BLE_CONN_MAX                                2

typedef struct
{
    bool in_use;
    uint16_t conn_id;
    bool notify_en;   /* CCCD Notify 是否开启 */
} ble_conn_t;

typedef struct
{
    bool flag;                /* 至少一路已连接 */
    uint16_t gatts_if;
    uint16_t handle;          /* Char A value handle，供 Notify */
    ble_conn_t conns[BLE_CONN_MAX];
    uint8_t conn_count;
    QueueHandle_t xQueue;
    data_rec_t data_rec;
}ble_link_info_t;

/* 485数据结构 */
#pragma pack(1)

struct asyncCtrlMode_t
{
    uint8_t addr  			: 2;
    uint8_t mode  			: 2;
    uint8_t strechMove	: 2;
    uint8_t music 			: 1;
    uint8_t reserved 		:1;
} __attribute__ ((packed));

union SyncCommunicationData_t
{
    struct
    {
        uint8_t length;
        uint8_t type;
        uint8_t data[UART_RX_BUF_SIZE];
    } __attribute__ ((packed)) Syncdata;
    
    struct
    {
        unsigned char length;
        unsigned char type;
        unsigned long keys;       // 键值
        unsigned char ctrlMode;   // 保留，默认0
        uint8_t checksum;
    } __attribute__ ((packed)) PlugInPacket; // 插针数据
    
    
    struct
    {
        unsigned char length;     // 数据长度（7）
        unsigned char type;       // 数据类型，固定为0x01
        unsigned long keys;       // 键值
        unsigned char ctrlMode;   // 保留，默认0
        unsigned char drivePwm;   // 驱动PWM，范围1-255，1表示最小，255表示最大
        unsigned char driveTmr;   // 驱动定时器，范围0-255，单位待定
        uint8_t checksum;
    } __attribute__ ((packed)) btPacket;
    
    struct
    {
        uint8_t length;     // 0x05
        uint8_t type;       // 0x01
        uint32_t keys;       // 键值
        uint8_t ctrlMode;   // 保留，默认0
        uint8_t checksum;
    } __attribute__ ((packed)) PlugInPacket_Normal; // 博创插针数据 普通键值

    struct
    {
        uint8_t length;     // 0x07
        uint8_t type;       // 0x01
        uint32_t keys;       // 键值
        uint8_t cmd;        // 左右床命令
        uint8_t pwm;        // 速度 0-255
        uint8_t tmr;        // 时间 单位1s
        uint8_t checksum;
    } __attribute__ ((packed)) PlugInPacket_SlowStart; // 博创插针数据 缓启动控制

    struct
    {
        uint8_t length;         // 0x0a
        uint8_t type;           // 0x01
        uint32_t keys;       // 键值，固定 0x00000000
        uint16_t reserve1;   // 保留，默认0x0000
        uint8_t cmd;        // 固定 0xAA 
        uint8_t ctrcmd;     // 控制指令 
        uint16_t reserve2;   // 保留，默认0x0000
        uint8_t checksum;
    } __attribute__ ((packed)) PlugInPacket_MASSAGE; // 博创插针数据 按摩枚举

#if BOARD == BOARD_MC242
    struct
    {
        uint8_t length;                           // 数据长度：length + 3（包含length + type + checkSum）
        uint8_t type;                             // 数据类型，固定为0x07 
        uint32_t keys;                            // 键值
        uint8_t ledData[5];                       // LED数据
        uint8_t UBB           : 1;                // UBB状态（床底灯状态）
        uint8_t stopAll       : 1;                // 停止所有标志位，1表示停止，0表示运行
        uint8_t automaticMovementIsActive : 1;    // 自动运动激活标志，1表示自动运动
        uint8_t sync          : 1;                // 同步标志
        uint8_t lock          : 1;                // 锁定标志
        uint8_t angleAdj      : 1;                // 角度调节标志位，1表示左侧调节，0表示右侧调节
        uint8_t factoryMode   : 1;                // 工厂模式，1表示工厂模式，0表示正常模式
        uint8_t addr          : 1;                // 设备地址，用于区分多个设备           12

        uint8_t massage_status[2];                // 按摩状态（按摩模式和力度）
        uint32_t massageTimer;                    // 按摩计时器，单位10ms               18
        uint16_t pulseCounter[MOTOR_NUM];         // 脉冲计数，根据电机数量（当前支持2/3/4个） 24
        int16_t  current[MOTOR_NUM];              // 电流值，根据电机数量（当前支持2/3/4个）30
        uint16_t U_div_2;                         // 电压分压值32
        uint16_t massageCurrent;                  // 按摩电流34
        uint16_t mfpCurrent;                      // mfp电流36

        uint8_t  dummy[2];                        // 保留字节38  (uint16_t selfcheck;)
        uint8_t  slow_pwm;                        // 慢速PWM
        uint8_t  slow_timer;                      // 慢速定时器 40
        uint8_t  bedtype;                         // 按摩床类型
        struct asyncCtrlMode_t asyncCtrlFrame;    // 异步控制帧 42
        uint8_t heating;                          // 加热标志位
        uint8_t aromaswitch;                      // 香薰开关标志位/感应灯开关标志位 
        uint8_t rgb;                              // RGB灯状态
        uint8_t reddata;                          // 红色数据，范围0~255
        uint8_t greendata;                        // 绿色数据，范围0~255
        uint8_t bluedata;                         // 蓝色数据，范围0~255
        uint8_t massge_mode;                      // 按摩模式     [49]
        uint8_t brightness;                       // 普通模式下的亮度，范围0~255  50（床底灯亮度）
  
        uint8_t checkSum;                          // 53 57  61
    } __attribute__ ((packed)) syncPacket;

#elif BOARD == BOARD_MC232 
    struct
    {
        uint8_t length;                           // 数据长度：length + 3（包含length + type + checkSum）
        uint8_t type;                             // 数据类型，固定为0x07 
        uint32_t keys;                            // 键值
        uint8_t ledData[5];                       // LED数据
        uint8_t UBB           : 1;                // UBB状态（床底灯状态）
        uint8_t stopAll       : 1;                // 停止所有标志位，1表示停止，0表示运行
        uint8_t automaticMovementIsActive : 1;    // 自动运动激活标志，1表示自动运动
        uint8_t sync          : 1;                // 同步标志
        uint8_t lock          : 1;                // 锁定标志
        uint8_t angleAdj      : 1;                // 角度调节标志位，1表示左侧调节，0表示右侧调节
        uint8_t factoryMode   : 1;                // 工厂模式，1表示工厂模式，0表示正常模式
        uint8_t addr          : 1;                // 设备地址，用于区分多个设备           12

        uint8_t massage_status[2];                // 按摩状态（按摩模式和力度）
        uint32_t massageTimer;                    // 按摩计时器，单位10ms               18
        uint16_t pulseCounter[MOTOR_NUM];         // 脉冲计数，根据电机数量（当前支持2/3/4个） 24
        int16_t  current[MOTOR_NUM];              // 电流值，根据电机数量（当前支持2/3/4个）30
        uint16_t U_div_2;                         // 电压分压值32
        uint16_t massageCurrent;                  // 按摩电流34
        uint16_t mfpCurrent;                      // mfp电流36

        uint8_t  dummy[2];                        // 保留字节38  (uint16_t selfcheck;)
        uint8_t  slow_pwm;                        // 慢速PWM
        uint8_t  slow_timer;                      // 慢速定时器 40
        uint8_t  bedtype;                         // 按摩床类型
        struct asyncCtrlMode_t asyncCtrlFrame;    // 异步控制帧 42
        uint8_t heating;                          // 加热标志位
        uint8_t aromaswitch;                      // 香薰开关标志位/感应灯开关标志位 
        uint8_t rgb;                              // RGB灯状态
        uint8_t reddata;                          // 红色数据，范围0~255
        uint8_t greendata;                        // 绿色数据，范围0~255
        uint8_t bluedata;                         // 蓝色数据，范围0~255
        uint8_t massge_mode;                      // 按摩模式     [49]
        uint8_t brightness;                       // 普通模式下的亮度，范围0~255  50（床底灯亮度）

        uint8_t reserve_1;                        //预留,0x00
        uint8_t Gemini_flag;                      //双子星标志,0x00
        uint8_t Bedtype;                          //床型在前,0x05
        uint8_t Customertype;                     //客户编号在后，鸿蒙为 0x02
        uint8_t ReleaseNo;                        //协议版本，0x02
        uint8_t M1_position :2;                   //头
        uint8_t M4_position :2;                   //预留
        uint8_t M2_position :2;                   //脚，兼容老版本
        uint8_t M3_position :2;                   //腰，预留
                                            //电机位置，0：底部；1：顶部；2：中部     
        uint8_t checkSum;                          // 53 57  61
    } __attribute__ ((packed)) syncPacket;
#endif

    uint8_t rawData[UART_RX_BUF_SIZE];
};


typedef struct
{
    uint32_t check_err;        // 校验错误计数
    uint32_t check_timerout;   // 校验超时计数
} MFPDatacheck_t;

union keys_t
{
	struct
	{
		unsigned char m1up:1;			  //	Bit 0	0x00 00 00 01 - identisch zu Elegance-N
		unsigned char m1down:1;			//	Bit 1	0x00 00 00 02 - identisch zu Elegance-N
		unsigned char m2up:1;			  //	Bit 2	0x00 00 00 04 - identisch zu Elegance-N
		unsigned char m2down:1;			//	Bit 3	0x00 00 00 08 - identisch zu Elegance-N
		unsigned char m3up:1;			  //	Bit 4	0x00 00 00 10 - identisch zu Elegance-N
		unsigned char m3down:1;			//	Bit 5	0x00 00 00 20 - identisch zu Elegance-N
		unsigned char m4up:1;			  //	Bit 6	0x00 00 00 40 - identisch zu Elegance-N
		unsigned char m4down:1;			//	Bit 7	0x00 00 00 80 - identisch zu Elegance-N

		unsigned char massageAll:1;		//	Bit 8	0x00 00 01 00
		unsigned char massageTimer:1;	//	Bit 9	0x00 00 02 00
		unsigned char massageFeet:1;	//	Bit 10	0x00 00 04 00
		unsigned char massageHead:1;	//	Bit 11	0x00 00 08 00
		unsigned char zeroG:1;			  //	Bit 12	0x00 00 10 00 - identisch zu Elegance-N memory 1
		unsigned char memory2:1;		  //	Bit 13	0x00 00 20 00 - identisch zu Elegance-N
		unsigned char memory3:1;		  //	Bit 14	0x00 00 40 00 - identisch zu Elegance-N
		unsigned char memory4:1;		  //	Bit 15	0x00 00 80 00 - identisch zu Elegance-N

		unsigned char memory5:1;	    //	Bit 16	0x00 01 00 00 - identisch zu Elegance-N
		unsigned char ubb:1;			    //	Bit 17	0x00 02 00 00 - identisch zu Elegance-N
		unsigned char StrechMove:1;		//	Bit 18	0x00 04 00 00 - identisch zu Elegance-N
		unsigned char intensity1:1;		//	Bit 19	0x00 08 00 00
		unsigned char intensity2:1;		//	Bit 20	0x00 10 00 00
		unsigned char intensity3:1;		//	Bit 21	0x00 20 00 00
		unsigned char massageWaist:1;			//	Bit 22	0x00 40 00 00
		unsigned char massageHeadMinus:1; //	Bit 23	0x00 80 00 00

		unsigned char massageFeetMinus:1; //	Bit 24	0x01 00 00 00
		unsigned char massagestop:1;	    //	Bit 25	0x02 00 00 00
		unsigned char massageMode:1;	    //	Bit 26	0x04 00 00 00
		unsigned char allFlat:1;		      //	Bit 27	0x08 00 00 00
		unsigned char massageWave:1;	    //	Bit	28	0x10 00 00 00
		unsigned char angleAdjust:1;	    //	Bit	29	0x20 00 00 00
		unsigned char extMem1:1;		      //	Bit 30	0x40 00 00 00
		unsigned char extMem2:1;		      //	Bit 31	0x80 00 00 00
	}allkey;
	uint32_t data;
};

typedef struct
{
	union   keys_t   key;   									// 遥控器键值
  	uint32_t keyTimeroOut;									    // 无线键值超时时间
    uint8_t mfp_tx_ready;                                       // mfp发送准备标志 1可发送
    uint8_t mqtt_data_flag;                                     // mqtt数据标志 1有数据
    uint8_t snore_event_triggered;                              // 打鼾事件触发标志 1触发
    uint8_t snore_event_triggered_demo;                         // 打鼾事件演示流程触发标志 1触发
	uint8_t keys_type;                                          // 键值类型 0普通 1缓启动 2按摩枚举


    uint8_t snore_trigger_flag;                              // 打鼾事件触发标志 1触发
    uint8_t snore_trigger_flagdemo_flag;                     // 打鼾事件演示流程触发标志 1触发
    uint8_t Key_send_flag;                                   // mqtt/BL 按键发送标志 1发送

}g_system_flag_t; 


typedef struct
{
    uint32_t  key;
    uint8_t pwm;
    uint8_t tmr;
}g_keys_t;


typedef struct
{
	uint32_t usartCheck_error;  			 //串口校验错误
	uint32_t usartTimer_error;     		  // 串口接收数据超时次数
} errorCode_t;

typedef struct
{
	uint8_t  syncModetimer; 					            //同步模式时间 
	uint8_t  syncModeSendInterval; 		                    //同步模式发送间隔
	uint8_t  sync_stopall; 						            //停止
	uint8_t  syncsendflag;  					        //串口发送标志位
	uint8_t  keySame;  					 			    //两边同时有键值的时候打断
    errorCode_t sync_error;
} MFPData_t;

/*
// 按键数据结构 包含：按键、PWM、时间
typedef struct {
    uint32_t snore_last_event_time;
    bool     snore_in_progress;
    bool     snore_event_triggered;
    bool     snore_event_triggered_demo;                        // 打鼾事件触发标志 演示流程
    
    uint32_t snore_cooldown_period;                             // 打鼾冷却时间（秒）
    uint32_t  snore_cooldown_period_demo;                        // 打鼾演示流程冷却时间（秒）s

    uint8_t  snoring_block[BLOCK_BUFFER_SIZE];                  // 2 分钟打鼾检测数组
    uint8_t  block_index;                                       // 打鼾检测数组索引

    uint8_t  block_size;                                        // 2分钟的检测窗口（默认24） 120/5=24
    uint8_t  snoring_threshold;                                 // 2分钟打鼾包阈值（默认15） 2分钟内有15包打鼾包则判断为打鼾   
    uint8_t  snoring_threshold_5s;                              // 5秒内打鼾包判断阈值（默认4）5个里面4个是打鼾状态则这包判断为打鼾包

    g_keys_t snore_keys;
} snore_intervention_t;
*/

typedef struct {
    uint32_t up_hold_time_s;            // 打鼾冷却时间（秒）
    uint8_t threshold;                  // 2分钟打鼾包阈值
    uint8_t threshold_5s;               // 5秒内打鼾包判断阈值
    uint8_t pwm;                        // 缓启动速度
    uint8_t tmr;                        // 缓启动时间                      
} snore_parameters_t;

typedef struct {
    uint32_t last_triggered_time_s;                 // 上次触发时间
    bool     is_intervening;                        // 打鼾干预中
    bool     triggered_flag;                        // 触发
    bool     triggered_flag_demo;                   // 演示触发

    uint8_t  snoring_block[BLOCK_BUFFER_SIZE];      // 2分钟打鼾检测数组
    uint8_t  block_index;                    
    uint8_t  block_size;                            // 检测窗口大小
} snore_state_t;

typedef struct {
    snore_parameters_t snore_parameters;
    snore_state_t snore_state;
} snore_intervention_t;


#pragma pack()


//mqtt接收键值参数定义
typedef struct
{
    bool flag;
    QueueHandle_t xQueue;
    mqtt_key_rec_t data_rec;
    g_keys_t mqtt_keys;
}mqtt_key_info_t;

typedef struct
{
    wifi_link_info_t wifi;
    ble_link_info_t *ble;
    ota_info_t ota;
    aliyun_link_info_t aliyun;
    utc_info_t utc;
    mqtt_key_info_t *mqtt_key;
    snore_intervention_t *snore;
    uint8_t data_up_switch;
    char report[128];
    char id[20];
    /** 传感器固件版本缓存（NVS key=sensor_ver；左右同源硬件，只存一份） */
    char sensor_ver[48];
} device_info_t;

#if SU3_USE_NEW_STACK
#include "qs_protobuf.h"
/** 双路传感器本地缓存（左=0x33，右=0x36）
 * hello/mode/rtc/devid 为整机状态（两侧 setup_done 同步）；
 * 实时与报告缓存仍按左右分路。
 */
typedef struct {
    bool hello_seen;
    bool need_setup;      /* 整机待 set mode/devid；两侧同步置位 */
    bool setup_done;      /* 整机 mode+rtc 完成；两侧同步置位 */
    bool data_seen;       /* 该侧已收到 1s/60s；整机 setup 可用任一侧 */
    bool fresh_1s;
    bool fresh_1min;
    char device_id[24];
    qs_pb_msg_sensor_1sec_info info_1s;
    qs_pb_msg_sensor_1min_info info_1min;
    qs_pb_msg_sleep_apnea_info info_sa;
} su3_sensor_side_t;

extern su3_sensor_side_t g_su3_sensor[SU3_SENSOR_SIDE_MAX];
#endif

void device_init(void);

/**
 * SU3 供电脉冲使能：GPIO12 先关 SU3_PWR_CYCLE_OFF_MS 再开（冷启动）。
 * 供 APP deviceCli `su3Enable` / 板端自用；不含上电 10s 错峰等待。
 */
void su3_pwr_cycle(void);

/** GPIO12 仅拉低 / 仅拉高（deviceCli 诊断用，不含 re-setup） */
void su3_pwr_off(void);
void su3_pwr_on(void);

/** 查询 GPIO12 输出电平，写入 out：`high` 或 `low` */
void su3_pwr_gpio_query(char *out, size_t out_len);

uint8_t get_wifi_status(void);
void set_wifi_status(uint8_t value);
uint8_t get_ble_status(void);
void set_ble_status(uint8_t value);
uint8_t get_mqtt_status(void);
void set_mqtt_status(uint8_t value);
uint8_t get_one_key_config_wifi_status(void);
void set_one_key_config_wifi_status(uint8_t value);

/** 打印堆内存：内部 RAM / DMA / PSRAM，where 为场景标签 */
void log_heap_info(const char *where);

#endif