#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MQTTAsync.h"
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "sys/un.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <min.h>
#include "min_id.h"
#include <stddef.h>
#include <stdarg.h>
#include "base64.h"
#if defined(_WRS_KERNEL)
#include <OsWrapper.h>
#endif
#include <signal.h>
#include "app_debug.h"
#include "app_aes.h"
#include "cJSON.h"
#include "app_serial.h"
#include "gsm.h"
#include "gsm_utilities.h"
#include "lwrb.h"
#include "app_md5.h"

#define FIRMWARE_VERSION "SLMB_0.0.1"
#define QOS         1
#define MQTT_THREAD_CYCLE_MS 1000L
#define DEFAULT_MQTT_TIMEOUT_DO_RESET (1000000*MQTT_THREAD_CYCLE_MS)  // 1000s
#define MQTT_AUTO_SUB_INTERVAL (61000/MQTT_THREAD_CYCLE_MS)           // 61s
#define MQTT_AUTO_PUB_INTERVAL (60000/MQTT_THREAD_CYCLE_MS)           // 60s
#define AUTO_REPORT_LOAD_AVG (900000/MQTT_THREAD_CYCLE_MS)           // 900s
#define MQTT_AUTO_STOP_STREAM (120000/MQTT_THREAD_CYCLE_MS)           // 60s
#define MAX_OTA_FILE_SIZE (2000000)
#define MAX_IPC_PAYLOAD_SIZE 512
#define IPC_KEY 1234569
#define LOG_TO_FILE_INTERVAL_MS (5000)
#define AUTO_REMOVE_LOG_INTERVAL    (6000000/LOG_TO_FILE_INTERVAL_MS)
#define SOCKET_PATH "/tmp/socketname"
#define BUFFER_SIZE 128
#define MSLEEP(ms)              usleep(ms*1000)
#define GPIO_LED_RED_PATH "/sys/class/gpio/gpio41/value"
#define GPIO_LED_GREEN_PATH "/sys/class/gpio/gpio44/value"
#define GPIO_CLASS_D_MUTE_PATH "/sys/class/gpio/gpio42/value"
#define GPIO_FM_RELAY_PATH "/sys/class/gpio/gpio46/value"
#define GPIO_CODEC_MUTE_PATH "/sys/class/gpio/gpio45/value"
#define SYS_CONFIG_RAW_DATA "/usr/bin/raw_config.txt"
#define SYS_CONFIG_BINARY_DATA "/usr/bin/app_config.bin"
#define FFMPEG_BUFFER_SIZE  (16384)
#define FIRMWARE_SIGNATURE "LPM"

/*
 * Slave sub topic config: vs/sub/<Slave IMEI>
 * Slave sub topic master: vs/pub/<Master IMEI>
 * Slave pub topic: vs/pub/<Slave IMEI>
 */
#define SLAVE_PUB_TOPIC_HEADER "vs/pub/"
#define SLAVE_SUB_TOPIC_CONF_HEADER "vs/sub/" /* sub config from user */
#define SLAVE_SUB_TOPIC_CMD_HEADER "vs/pub/"  /* sub command from master */

#define WDT_THREAD_MQTT 0
#define WDT_THREAD_TCP 1
#define WDT_THREAD_GSM 2
#define WDT_THREAD_ALL ((1 << WDT_THREAD_MQTT) | (1 << WDT_THREAD_GSM))

typedef enum
{
    TCP_STATE_INIT,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_DISCONNECTED,
    TCP_STATE_NEED_DESTROY
} tcp_state_t;

typedef enum
{
    MQTT_STATE_INIT,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_NEED_DESTROY
} mqtt_state_t;

typedef struct
{
    char url[128];
    int port;
    char username[64];
    char password[64];
} broker_info_t;


typedef struct
{
    char imei[17];
    char ccid[33];
    char imsi[33];
    char master[9][32];
    char stream_url[164];
    uint8_t volume;
    uint8_t mode;
    broker_info_t mqtt;
    char last_stream_master[24];
    int reset_counter;
    int reset_reason;
    int debug_level;
    int log_to_tty;
    int log_level;
    int log_to_file;
} device_config_t;

typedef struct
{
    long msg_type;
    uint32_t size;
    uint8_t payload[MAX_IPC_PAYLOAD_SIZE];
} ipc_data_t;

typedef struct
{   
    uint8_t stream_state;
    uint32_t total_streaming_in_second;
    uint32_t total_streaming_in_bytes;
} __attribute__((packed)) rx_ping_msg_t;

typedef enum
{
    GSM_STATE_INIT = 0,
    GSM_STATE_OK = 1,
    GSM_RESET = 2
} gsm_state_t;

typedef enum
{
    STREAM_IDLE = 0,
    STREAM_RUNNING = 1,
    STREAM_STOP = 2
} stream_state_t;

typedef struct
{
    size_t downloaded;
    size_t stream_time; // in second
    stream_state_t state;
    int mute;
} stream_monitor_info_t;

typedef union
{
    struct
    {
        char header[3];                 // Image header
        char fw_version[3];       // Firmware version
        char hw_version[3];       // Hardware version
        uint32_t size;         // fw size =  image_size + 16 byte md5 or 4 byte CRC32, excluded header
        uint8_t release_year;           // From 2000
        uint8_t release_month;          // 1 to 12
        uint8_t release_date;           // 0 to 31
    } __attribute__((packed)) name;
    uint8_t raw[16];
}  ota_image_header_t;

typedef struct
{
    uint32_t crc;
} crc_calculate_crc32_ctx_t;

pthread_mutex_t m_mutex_dbg, m_mutex_mqtt, m_mutex_python, m_mutex_wdt, m_mutex_log_file;
pthread_t p_mqtt_tid, p_socket_tid, p_gsm_tid, p_wdt_tid, p_audio_tid, p_log_tid;
static gsm_state_t m_gsm_state = GSM_STATE_INIT;
static uint32_t m_gsm_fsm_counter = 0;
static char *gsm_module_name = "NA";
static char m_gsm_operator[33];
static char m_gsm_technology[33];
static char m_gsm_channel[33];
static char m_gsm_band[33];
static uint8_t m_gsm_csq = 0;
static uint8_t m_mac_addr[6];
void gsm_change_state(int new_state);
void sys_save_volume(int volume);
void sys_save_mode(int mode);
void sys_save_master();
void sys_save_stream_url();
void sys_save_mqtt_info();
void sys_set_last_streaming_master();
void sys_save_tty_flag(int enable);
void sys_save_log_level(int level);
void *log_to_file_thread(void* arg);
void audio_codec_mute(int is_mute);
void on_mqtt_connected_cb(void* context, MQTTAsync_successData* response);
void on_mqtt_connect_failure_cb(void* context, MQTTAsync_failureData* response);
static MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
MQTTAsync client;
static lwrb_t m_ringbuffer_log_to_file;
static char *m_log_to_file_buffer = NULL;
// MQTT state machine
static mqtt_state_t m_mqtt_state = MQTT_STATE_INIT;
static int mqtt_fsm_cycle = 0;
// TCP state machine
static tcp_state_t m_ipc_tcp_state = TCP_STATE_INIT;
static int m_tcp_cli_fd = -1; 
static rx_ping_msg_t m_last_python_ping_msg;
static int m_auto_disconnect_mqtt = 0;
void alsa_set_volume(int vol);
char *cpu_get_load_avg();
char *sys_get_free_ram();
int run_shell_cmd(char *buffer, int buffer_size, bool wait_for_complete, const char *fmt, ...);
void mqtt_publish_message(char *header, const char *format, ...);
static bool m_mqtt_do_resub = false;
void *audio_ffmpeg_thread(void* arg);
char *sys_get_last_streaming_master();
uint32_t debug_to_tty(const void *buffer, uint32_t len);
void sys_save_log_to_file_flag(int enable);
static char m_application_path[256];
static int m_auto_stop_stream_s = 0;
uint32_t debug_cb_log_to_file(const void *buffer, uint32_t len);
static char m_last_stream_url[512];

static stream_monitor_info_t m_stream_monitor = 
{
    .mute = -1,
};
static device_config_t m_device_config = 
{
    .imei = "000000000000000",
    .master[0] = "000",
    .master[1] = "000",
    .master[2] = "000",
    .master[3] = "000",
    .master[4] = "000",
    .master[5] = "000",
    .master[6] = "000",
    .master[7] = "000",
    .master[8] = "000",
    .log_to_file = 0,
};
static unsigned long m_mqtt_disconnect_timeout = DEFAULT_MQTT_TIMEOUT_DO_RESET;      // 1000s

static int do_reboot(void);
static int m_ipc_msqid;
static int m_delay_reboot = 0;
static int m_gsm_max_timeout_wait_for_imei = 35;
// Min protocol in socket
static uint32_t m_min_host_rx_buffer[(MIN_MAX_PAYLOAD+3)/4];
static uint8_t m_dma_payload[1024];
static min_context_t m_min_host_context;
static min_frame_cfg_t m_min_host_setting = MIN_DEFAULT_CONFIG();
static bool uart_tx_byte_to_host(void *ctx, uint8_t data);
static void on_host_frame_callback(void *ctx, min_msg_t *frame);
static void on_host_timeout_callback(void *ctx);
static uint32_t sys_get_ms(void);
static uint8_t m_raw_sys_configuration[1024];
static uint8_t m_raw_aes_buf[1024];
static uint8_t m_out_aes_buf[2048];
static char m_sys_mac_str[24];
static int m_delay_wait_for_sim_imei = 20000/MQTT_THREAD_CYCLE_MS;
static bool m_send_config_to_svr = true;
static bool m_send_reset_to_svr = true;
static void send_msg_to_watchdog_mcu(char *msg);
static int m_wdt_value = WDT_THREAD_ALL;
static bool m_allow_audio_thread_run = true;
static int m_last_streaming_master_level = -1;
static int m_mqtt_error_counter = 0;
void uqmi_setting_prefer_lte();

void reset_stream_monitor_data(void)
{
    m_stream_monitor.state = STREAM_IDLE;
    m_stream_monitor.stream_time = 0;
    m_stream_monitor.downloaded = 0;
}

static bool is_audio_thread_alive(void)
{
    int is_alive = false;
    int ret;
    if (p_audio_tid == 0)
    {
       goto end;
    } 
    
    ret = pthread_kill(p_audio_tid, 0);
    if (ret == 0)
    {
        is_alive = true;
    }
end:
    return is_alive;
}

static void kill_audio_thread(void)
{
    m_allow_audio_thread_run = false;
    while (is_audio_thread_alive())
    {
        MSLEEP(50);
    }
    MSLEEP(1000);
    run_shell_cmd(NULL, 0, true, "timeout 3 pkill -9 -f ffmpeg");
    run_shell_cmd(NULL, 0, true, "timeout 3 pkill -9 -f aplay");
    p_audio_tid = 0;
}

static bool create_audio_thread(char *url)
{
    if (is_audio_thread_alive())
    {
        DEBUG_WARN("Audio thread already running\r\n");
        return false;
    }
    int err = pthread_create(&p_audio_tid, 
                            NULL, 
                            &audio_ffmpeg_thread, 
                            url);
    if (err)
    {
        DEBUG_WARN("Create audio thread err %d\r\n", err);
    }

    if (m_device_config.volume)
    {
        audio_codec_mute(0);
    }
    return true;
}

static void send_msg_to_watchdog_mcu(char *msg)
{
    if (!msg)
    {
        return;
    }
    // DEBUG_INFO("timeout 1 echo %s > /dev/ttyS0", msg);
    run_shell_cmd(NULL, 0, true, "timeout 1 echo %s > /dev/ttyS0", msg);
}

static void thread_wdt_feed(int thread_id)
{
    pthread_mutex_lock(&m_mutex_wdt);
    m_wdt_value |= (1 << thread_id);
    pthread_mutex_unlock(&m_mutex_wdt);
}

void set_mqtt_monitor_timeout(unsigned long timeout)
{
    pthread_mutex_lock(&m_mutex_dbg);
    m_mqtt_disconnect_timeout = timeout;
    pthread_mutex_unlock(&m_mutex_dbg);
}

unsigned long get_mqtt_monitor_timeout()
{                            
    unsigned long tmp;
    pthread_mutex_lock(&m_mutex_mqtt);
    tmp = m_mqtt_disconnect_timeout;
    pthread_mutex_unlock(&m_mutex_mqtt);
    return tmp;
}

void mqtt_set_fsm_state(mqtt_state_t state)
{
    pthread_mutex_lock(&m_mutex_mqtt);
    if (state != m_mqtt_state)
    {
        DEBUG_INFO("Change mqtt state to %d\r\n", state);
        mqtt_fsm_cycle = 0;
        m_mqtt_state = state;
    }
    pthread_mutex_unlock(&m_mutex_mqtt);
}

void on_mqtt_conn_lost(void *context, char *cause)
{
	MQTTAsync client = (MQTTAsync)context;
	int rc;

	DEBUG_WARN("\nMQTT : Connection lost\n");
	if (cause)
		DEBUG_INFO("     cause: %s\n", cause);
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;

    mqtt_set_fsm_state(MQTT_STATE_DISCONNECTED);
}

bool is_valid_digit_string(char* str)
{
    int leng = strnlen(str, 200);
    if (leng == 0) 
        return false;

    for (int i = 0; i < leng; i++) 
    {
        if (str[i] < '0' || str[i] > '9') 
        {
            return false;
        }
    }
    return true;
}

static void mqtt_process_set_config_msg(char *payload, int *cfg_change, int *do_resub)
{
    int cfg_count = 0;
    int resub_count = 0;

    *cfg_change = 0;
    *do_resub = 0;
    // Volume
    char *p, *q, *m;
    char *mqqt_port_num = NULL;
    p = strstr(payload, "VOLUME(");
    if (p)
    {
        p += 7;
        int new_vol = atoi(p);
        if (new_vol != m_device_config.volume)
        {
            DEBUG_INFO("Volume change from %d to %d\r\n", 
                        m_device_config.volume, 
                        new_vol);
            alsa_set_volume(new_vol);
            cfg_count++;
            if (new_vol == 0)
            {
                audio_codec_mute(1);
            }
            else
            {
                audio_codec_mute(0);
            }
            sys_save_volume(new_vol);
        }
    }

    // Mode
    p = strstr(payload, "MODE(");
    if (p)
    {
        p += 5;
        int new_mode = atoi(p);
        if ((new_mode != m_device_config.mode) && (new_mode == 4 || new_mode == 1))
        {
            DEBUG_INFO("Mode change from %d to %d\r\n", m_device_config.mode, new_mode);
            m_device_config.mode = new_mode;
            if (new_mode == 4)       // STOP mode
            {
                audio_codec_mute(1);
                if (is_audio_thread_alive())
                {
                    kill_audio_thread();
                }
            }
            else if (new_mode == 1 && m_device_config.volume)
            {
                audio_codec_mute(0);
            }
            sys_save_mode(new_mode);
            cfg_count++;
        }
    }


    // Master
    p = strstr(payload, "MASTER1(");
    q = strstr(payload, "MASTER2(");
    m = strstr(payload, "MASTER3(");
    if (p && q && m)
    {
        char tmp_str[96];
        char list_imei_buf[4][25];
        char *m_token;
        uint8_t list_imei_idex = 0;
        int master_size = 0;

        // Reset buffer
        memset(tmp_str, 0, sizeof(tmp_str));
        gsm_utilities_copy_parameters(p, tmp_str, '(', ')');
        list_imei_idex = 0;

        // Master tinh
        m_token = strtok(tmp_str, "|");
        while (m_token != NULL)
        {
            snprintf(list_imei_buf[list_imei_idex++], 25, "%s", m_token);
            m_token = strtok(NULL, "|");

            if (list_imei_idex >= 3)
                break;
        }

        for (uint8_t i = 0; i < 3; i++)
        {
            if (is_valid_digit_string(list_imei_buf[i]) 
                && strlen(list_imei_buf[i]) >= 3)
            { 
                DEBUG_INFO("%s\r\n", list_imei_buf[i]);
                sprintf(m_device_config.master[i], "%s", list_imei_buf[i]);
            }
        }

        // Reset buffer
        memset(tmp_str, 0, sizeof(tmp_str));
        gsm_utilities_copy_parameters(q, tmp_str, '(', ')');
        list_imei_idex = 0;
        
        // Master huyen
        m_token = strtok(tmp_str, "|");
        while (m_token != NULL)
        {
            snprintf(list_imei_buf[list_imei_idex++], 25, "%s", m_token);
            m_token = strtok(NULL, "|");

            if (list_imei_idex >= 3)
                break;
        }

        for (uint8_t i = 0; i < 3; i++)
        {
            if (is_valid_digit_string(list_imei_buf[i]) 
                && strlen(list_imei_buf[i]) >= 3)
            { 
                DEBUG_INFO("%s\r\n", list_imei_buf[i]);
                sprintf(m_device_config.master[3+i], "%s", list_imei_buf[i]);
            }
        }

        // Reset buffer
        memset(tmp_str, 0, sizeof(tmp_str));
        gsm_utilities_copy_parameters(m, tmp_str, '(', ')');
        list_imei_idex = 0;

        // Master xa
        m_token = strtok(tmp_str, "|");
        while (m_token != NULL)
        {
            snprintf(list_imei_buf[list_imei_idex++], 25, "%s", m_token);
            m_token = strtok(NULL, "|");

            if (list_imei_idex >= 3)
                break;
        }

        for (uint8_t i = 0; i < 3; i++)
        {
            if (is_valid_digit_string(list_imei_buf[i]) 
                && strlen(list_imei_buf[i]) >= 3)
            { 
                DEBUG_INFO("%s\r\n", list_imei_buf[i]);
                sprintf(m_device_config.master[6+i], "%s", list_imei_buf[i]);
            }
        }
    
        sys_save_master();
        cfg_count++;
        resub_count++;
    }

    // # HTTP stream url
    p = strstr(payload, "STREAM_URL(");
    if (p)
    {
        char url[164];
        memset(url, 0, sizeof(url));
        gsm_utilities_copy_parameters(p, url, '(', ')');
        if (strlen(url) 
            && strstr(url, "http") 
            && strcmp(url, m_device_config.stream_url))
        {
            strcpy(m_device_config.stream_url, url);
            sys_save_stream_url(m_raw_sys_configuration);
            cfg_count++;
        }
        else
        {
            DEBUG_WARN("Invalid stream URL -> %s\r\n", url);
        }
    }

    // MQTT info
    // MQTT_URL(village-speaker.bytechjsc.vn:2591),MQTT_USER(village-speaker),MQTT_PASS(vs.bytech@2019)
    p = strstr(payload, "MQTT_URL(");
    q = strstr(payload, "MQTT_USER(");
    m = strstr(payload, "MQTT_PASS(");
    
    if (p) mqqt_port_num = strstr(p, ":");
    int port_num = 0;

    if (mqqt_port_num)
    {
        port_num = atoi(mqqt_port_num+1);
    }
    if (p && q && m && port_num > 0 && port_num < 65536)
    {
        char tmp_url[128];
        memset(tmp_url, 0, sizeof(tmp_url));

        // Broker
        gsm_utilities_copy_parameters(p, tmp_url, '(', ':');
        sprintf(m_device_config.mqtt.url, "%s", tmp_url);
        DEBUG_INFO("Broker %s\r\n", m_device_config.mqtt.url);
        *mqqt_port_num = 0;     // Remove ':'

        // Port
        m_device_config.mqtt.port = port_num;
        DEBUG_INFO("Port %d\r\n", m_device_config.mqtt.port);

        // Username
        gsm_utilities_copy_parameters(q, tmp_url, '(', ')');
        sprintf(m_device_config.mqtt.username, "%s", tmp_url);
        DEBUG_INFO("Username %s\r\n", m_device_config.mqtt.username);

        // Username
        gsm_utilities_copy_parameters(m, tmp_url, '(', ')');
        sprintf(m_device_config.mqtt.password, "%s", tmp_url);
        DEBUG_INFO("Password %s\r\n", m_device_config.mqtt.password);

        sys_save_mqtt_info(m_raw_sys_configuration);

        m_auto_disconnect_mqtt = 10;
        cfg_count++;
    }

    // TTY console
    p = strstr(payload, "tty_enable(1)");
    q = strstr(payload, "tty_enable(0)");

    if (p)
    {
        if (m_device_config.log_to_tty == 0)
        {
            m_device_config.log_to_tty = 1; 
            sys_save_tty_flag(1);
            app_debug_register_callback_print(debug_to_tty);
        }
        cfg_count++;
    }
    else if (q)
    {
        if (m_device_config.log_to_tty)
        {
            m_device_config.log_to_tty = 0; 
            sys_save_tty_flag(0);
            app_debug_unregister_callback_print(debug_to_tty);
        }
        cfg_count++;
    }

    // Log level
    p = strstr(payload, "log_level(");
    if (p)
    {
        p += strlen("log_level(");
        int level = atoi(p);
        sys_save_log_level(level);
        app_debug_set_level(level);
        cfg_count++;
    }

    // Log to file
    p = strstr(payload, "log_to_file(1)");
    q = strstr(payload, "log_to_file(0)");
    if (p)
    {
        if (m_device_config.log_to_file == 0)
        {
            DEBUG_INFO("Enable log to file\r\n");
            m_device_config.log_to_file = 1;
            sys_save_log_to_file_flag(1);
            pthread_create(&p_log_tid, NULL, &log_to_file_thread, NULL);
            MSLEEP(1000);
            cfg_count++;
        }
    }
    else if (q)
    {
        if (m_device_config.log_to_file == 1)
        {
            DEBUG_INFO("Disable log to file\r\n");
            app_debug_unregister_callback_print(debug_cb_log_to_file);
            cfg_count++;
            sys_save_log_to_file_flag(0);
        }
    }
    

    *cfg_change = cfg_count;
    *do_resub = resub_count;
}

void mqtt_audio_default_auto_reset_stream(int timeout)
{
    pthread_mutex_lock(&m_mutex_mqtt);
    m_auto_stop_stream_s = timeout;
    pthread_mutex_unlock(&m_mutex_mqtt);
}

int mqtt_get_auto_stop_stream_timeout(void)
{
    int remain = 0;
    pthread_mutex_lock(&m_mutex_mqtt);
    remain = m_auto_stop_stream_s;
    pthread_mutex_unlock(&m_mutex_mqtt);
    return remain;
}

void crc32_init_context(crc_calculate_crc32_ctx_t *context)
{
    context->crc = 0xFFFFFFFFU;
}

void crc32_step(crc_calculate_crc32_ctx_t *context, uint8_t byte)
{
    context->crc ^= byte;
    for (uint32_t j = 0; j < 8; j++)
    {
        uint32_t mask = (uint32_t) - (context->crc & 1U);
        context->crc = (context->crc >> 1) ^ (0xEDB88320U & mask);
    }
}

uint32_t crc32_finalize(crc_calculate_crc32_ctx_t *context)
{
    return ~context->crc;
}

static int process_firmware_update_on_file(char *filename)
{
    /**
     * Download firmware region detail
     * -------------------------------
     *         16 bytes header ota_image_header_t
     * -------------------------------
     *         Raw firmware
     * -------------------------------
     *         4 bytes CRC32-SUM or 16 bytes MD5, depend on checksum method
     * -------------------------------
    */
    int retval = -1;
    int file_size = -1;
    FILE *f_in = NULL, *f_out = NULL;
    char *out_file_name = NULL;

    if (!filename)
    {
        goto end;
    }

    out_file_name = calloc(strlen(m_application_path)+16, 1);
    if (!out_file_name)
    {
        DEBUG_WARN("Malloc file name failed\r\n");
        goto end;
    }
    sprintf(out_file_name, "%s.ota", m_application_path);

    // Get file size
    f_in = fopen(filename, "r");
    f_out = fopen(out_file_name, "w");
    if (!f_in || !f_out)
    {
        DEBUG_WARN("Open file failed\r\n");
        goto end;
    }

    // Get file size
    fseek(f_in, 0L, SEEK_END);
    file_size = ftell(f_in);
    fseek(f_in, 0L, SEEK_SET);
    DEBUG_INFO("OTA firmware size %d\r\n", file_size);

    if (!(file_size > 32 && file_size < MAX_OTA_FILE_SIZE))     // 16 bytes header
    {
        DEBUG_WARN("Invalid file size\r\n");
        goto end;
    }

    // Check header
    ota_image_header_t header;
    uint8_t exptect_md5[16];
    memset(&header, 0, sizeof(header));
    
    // Read hdr
    fread(header.raw, 1, sizeof(ota_image_header_t), f_in);

    // Read 16 bytes md5
    fseek(f_in, file_size-16, SEEK_SET);
    fread(exptect_md5, 1, 16, f_in);
    
    if (memcmp(header.name.header, FIRMWARE_SIGNATURE, 3))
    {
        DEBUG_WARN("Invalid header %.*s\r\n", 3, header);
        goto end;
    }

    DEBUG_INFO("%.*s, fw %.*s, hw %.*s, build on %d/%02d/%02d\r\n",
                3, header.name.header,
                3, header.name.fw_version,
                3, header.name.hw_version,
                header.name.release_year+2000,
                header.name.release_month,
                header.name.release_date);

    // Seek to firmware raw data
    fseek(f_in, 16L, SEEK_SET);

    // Calculate CRC
    app_md5_ctx	md5_ctx;
    uint8_t calculate_md5[16];
    app_md5_init(&md5_ctx);

    for (int i = 0; i < file_size - 32; i++)
    {
        uint8_t tmp[1];
        fread(tmp, 1, 1, f_in);
        fwrite(tmp, 1, 1, f_out);
        app_md5_update(&md5_ctx, tmp, 1);
        // Verify file again???
    }
    app_md5_final(calculate_md5, &md5_ctx);
            
    if (memcmp(calculate_md5, exptect_md5, 16) == 0)
    {
        DEBUG_WARN("Valid CRC\r\n");
        retval = 0;
    }
    else
    {
        DEBUG_WARN("Invalid firmware CRC\r\n");
        DEBUG_DUMP(exptect_md5, 16, "Expected");
        DEBUG_DUMP(calculate_md5, 16, "Calculated");
        retval = -1;
    }

end:
    if (f_in)
    {
        fclose(f_in);
        remove(filename);
    }

    if (f_out)
    {
        fclose(f_out);
        // remove(out_file_name);
    }

    if (retval == 0)
    {
        // Delete old binaray
        remove(m_application_path);
        // Update new binaray
        rename(out_file_name, m_application_path);

        DEBUG_INFO("Rename file %s to %s\r\n", out_file_name, m_application_path);
    }

    if (out_file_name)
        free(out_file_name);
    return retval;
}

static void process_mqtt_rx_msg(char *topic_name, char *payload)
{
    char *q, *p;
    bool valid_topic = false;
    char *incomming_imei;
    int incomming_master_priority = -1;
    if (!payload)
    {
        return;
    }

    // Master topic
    q = strstr(topic_name, "vs/pub/");
    if (q)
    {
        // # Get master IMEI
        q += 7;
        for (int i = 0; i < 9; i++)
        {
            if (strstr(m_device_config.master[i], q))
            {
                valid_topic = true;
                incomming_imei = q;
                incomming_master_priority = 9-i;
                DEBUG_VERBOSE("Master %s, level %d\r\n", 
                            incomming_imei, 
                            incomming_master_priority);
                break;
            }
        }

        if (incomming_master_priority == -1)
        {
            DEBUG_WARN("You [%s] are not my master\r\n", q);
            MQTTAsync_unsubscribe(client, topic_name, NULL);
            return;
        }
    }
    if (valid_topic == false)
    {
        // My topic
        q = strstr(topic_name, "vs/sub/");
        if (q)
        {
            q += 7;
            if (strstr(m_device_config.imei, q))
            {
                valid_topic = true;
            }
        }
    }

    if (valid_topic == false)
    {
        DEBUG_WARN("Invalid topic\r\n");
        return;
    }

    if (strstr(payload, "REBOOT#"))
    {
        m_delay_reboot = 5;
        mqtt_publish_message("DBG", "Reset by reboot cmd");
        return;
    }

    if (strstr(payload, "RESET_GSM#"))
    {
        gsm_change_state(GSM_RESET);
        return;
    }

    if (strstr(payload, "SET,"))
    {
        DEBUG_INFO("\t--- Set Parameter ---\r\n");
        int config_changed = 0;
        int do_resub = 0;
        mqtt_process_set_config_msg(payload, &config_changed, &do_resub);
        if (config_changed)
        {
            m_send_config_to_svr = true;
        }

        if (do_resub)
        {
            m_mqtt_do_resub = true;
        }

        return;
    }

    q = strstr(payload, "SHELL(");
    if (q)
    {
        char *shell_stdout = calloc(1024, 1);
        if (shell_stdout)
        {
            run_shell_cmd(shell_stdout, 1024, true, "%s", q+strlen("SHELL("));
            mqtt_publish_message("DBG", "SHELL\r\n%s", shell_stdout);
            free(shell_stdout);
        }
        return;
    }
            
    if (strstr(topic_name, "GET,"))
    {
        m_send_config_to_svr = true;
        return;
    }

    // # Ko in topic RAC
    if (strstr(payload, "DBG,") 
        || strstr(payload, "SELF_TEST") 
        || strstr(payload, "STREAM_RUNNING_SCHEDULE") 
        || strstr(payload, "STREAM_PREPAIR")
        || strstr(payload, "SELF_PUB"))
        return;

    bool do_stream = false;
    char new_stream_url[256];
    bool delay_stream = false;

    if (m_device_config.mode == 1)      // mode internet
    {
        // Process stream running command
        q = strstr(payload, "STREAM_RUNNING(");
        if (q)
        {
            q += strlen("STREAM_RUNNING(");
            p = strstr(q, ")");
            *p = 0;

            do_stream = true;
            sprintf(new_stream_url, "%s", q);
        }
        else
        {
            q = strstr(payload, "STREAM_RUNNING");
            if (q)
            {
                q += strlen("STREAM_RUNNING");
                do_stream = true;
                sprintf(new_stream_url, "%s%s", m_device_config.stream_url, incomming_imei);
            }
        }

        // Stream start cmd : STREAM_START(URL)
        q = strstr(payload, "STREAM_START(");
        if (q)
        {
            q += strlen("STREAM_START(");
            p = strstr(q, ")");
            *p = 0;
            do_stream = true;
            sprintf(new_stream_url, "%s", q);
        }
        else
        {
            q = strstr(payload, "STREAM_START");
            if (q)
            {
                q += strlen("STREAM_START");
                do_stream = true;
                sprintf(new_stream_url, "%s%s", m_device_config.stream_url, incomming_imei);
            }
        }

        q = strstr(payload, "STREAM_HIGH_PRIORITY(");
        if (q)
        {
            q += strlen("STREAM_HIGH_PRIORITY(");
            p = strstr(q, ")");
            *p = 0;
            do_stream = true;
            sprintf(new_stream_url, "%s", q);
        }
    }

    if (do_stream)
    {
        mqtt_audio_default_auto_reset_stream(MQTT_AUTO_STOP_STREAM);
        if (incomming_master_priority > m_last_streaming_master_level
            || (incomming_master_priority == m_last_streaming_master_level && strcmp(new_stream_url, m_last_stream_url)))
        {
            DEBUG_INFO("Start streaming by url %s\r\n", new_stream_url);
            if (m_last_streaming_master_level != -1)
            {
                // Higher master
                mqtt_publish_message("DBG", "Start stream by higher master %s, url %s", 
                                incomming_imei, new_stream_url);
                if (is_audio_thread_alive())
                {
                    kill_audio_thread();
                }
            }
            else
            {
                mqtt_publish_message("DBG", "Start stream on master %s, url %s", 
                                incomming_imei, new_stream_url);
            }
            m_last_streaming_master_level = incomming_master_priority;


            sprintf(m_device_config.last_stream_master, "%s", incomming_imei);
            sys_set_last_streaming_master();
            snprintf(m_last_stream_url, 512, "%s", new_stream_url);
            create_audio_thread(m_last_stream_url);
            if (delay_stream)
            {
                MSLEEP(4500);   // Delay de icecase du buffer, neu khong se link 404
            }
            else
            {
                MSLEEP(100);    // phong truong hop mqtt ban nhieu ban tin ma thread stream chua kip start
            }
        }
        return;
    }

    p = strstr(payload, "STREAM_STOP");
    if (p 
        && is_audio_thread_alive() 
        && (m_last_streaming_master_level == incomming_master_priority))
    {
        DEBUG_WARN("Stop stream\r\n");
        mqtt_publish_message("DBG", "Stop current stream on master %s", incomming_imei);
        MSLEEP(5000); // Sleep 5s de flush het data
        kill_audio_thread();
        m_last_streaming_master_level = -1;
        mqtt_audio_default_auto_reset_stream(0);
        audio_codec_mute(1);
        memset(m_last_stream_url, 0, sizeof(m_last_stream_url));
    }

    q = strstr(payload, "UDFW_MBF(");
    p = NULL;
    if (q)
    {
        q += strlen("UDFW_MBF(");
        p = strstr(q, ")");
    }
    if (p && q && strstr(q, "http"))
    {
        q = strstr(q, "http");
        *p = 0;
        DEBUG_INFO("Update firmware on %s\r\n", q);
        mqtt_publish_message("DBG", "Update firmware on %s\r\n", q);
        char *download_info = calloc(2048, 1);
        if (download_info)
        {
            mqtt_publish_message("DBG", "Downloading firmware");
            MSLEEP(500);
            int status = run_shell_cmd(download_info, 2048, true, "timeout 30 wget -O /tmp/firmware.bin %s", q);
            if (status == 0)
            {
                DEBUG_INFO("Download firmware success\r\n");
                mqtt_publish_message("DBG", "Download firmware completed");
                status = process_firmware_update_on_file("/tmp/firmware.bin");
                mqtt_publish_message("DBG", "Update firmware %s", status ? "failed" : "succes" );
                m_delay_reboot = 5;
            }
            else
            {
                DEBUG_WARN("Download firmware failed\r\n");
                if (strlen(download_info))
                {
                    mqtt_publish_message("DBG", "Download failed %d", status);
                }
            }
            free(download_info);
        }
    }
}

int on_mqtt_data_callback(void *context, char *topicName, int topicLen, MQTTAsync_message *message)
{
    DEBUG_VERBOSE("MQTT : Message arrived\n");
    DEBUG_VERBOSE("Topic: %s\n", topicName);
    DEBUG_VERBOSE("Message: %.*s\n", message->payloadlen, (char*)message->payload);

    set_mqtt_monitor_timeout(DEFAULT_MQTT_TIMEOUT_DO_RESET);
    char *payload = malloc(message->payloadlen+1);
    if (!payload)
    {
        DEBUG_WARN("No memory\r\n");
        do_reboot();
    }

    memcpy(payload, (char*)message->payload, message->payloadlen);
    payload[message->payloadlen] = 0;
    process_mqtt_rx_msg(topicName, payload);
    free(payload);

    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);
    return 1;
}

void on_mqtt_disconnect_failure_cb(void* context, MQTTAsync_failureData* response)
{
    DEBUG_INFO("MQTT : Disconnect failed, rc %d\n", response->code);
}

void on_mqtt_disconnect_success_cb(void* context, MQTTAsync_successData* response)
{
    DEBUG_INFO("MQTT : Successful disconnection\n");
}

void on_mqtt_subscribed_cb(void* context, MQTTAsync_successData* response)
{
    DEBUG_INFO("MQTT : Subscribe succeeded\n");
    set_mqtt_monitor_timeout(DEFAULT_MQTT_TIMEOUT_DO_RESET);
}

void on_subscribe_failure_cb(void* context, MQTTAsync_failureData* response)
{
    DEBUG_WARN("MQTT : Subscribe failed, rc %d\n", response->code);
}


void on_mqtt_connect_failure_cb(void* context, MQTTAsync_failureData* response)
{
    DEBUG_WARN("[%s] : Connect failed, rc %d\n", __FUNCTION__, response->code);
}


void on_mqtt_connected_cb(void* context, MQTTAsync_successData* response)
{
    MQTTAsync client = (MQTTAsync)context;
    DEBUG_INFO("MQTT : successful connection\n");

    mqtt_set_fsm_state(MQTT_STATE_CONNECTED);
    set_mqtt_monitor_timeout(DEFAULT_MQTT_TIMEOUT_DO_RESET);
}

void mqtt_delivery_complete(void* context, MQTTAsync_token token)
{
    DEBUG_VERBOSE("MQTT: delivery_complete\n");
}


void mqtt_publish_failure_cb(void* context, MQTTAsync_failureData* response)
{
    DEBUG_WARN("MQTT : Message send failed token %d error code %d\n", 
                response->token, response->code);
    mqtt_set_fsm_state(MQTT_STATE_DISCONNECTED);
}

void mqtt_publish_complete_cb(void* context, MQTTAsync_successData* response)
{
    DEBUG_VERBOSE("MQTT : Message with token value %d delivery confirmed\n", 
                response->token);
    set_mqtt_monitor_timeout(DEFAULT_MQTT_TIMEOUT_DO_RESET);
}


uint32_t sys_get_ms(void)
{
    return time(NULL);
}

uint32_t debug_to_tty(const void *buffer, uint32_t len)
{
    // printf("%.*s", len, buffer);
    if (buffer)
    {
        int ttyfd;
        ttyfd = open("/dev/ttyS2", O_WRONLY);
        if (ttyfd!= -1)
        {
            dprintf(ttyfd, "%.*s", len, buffer);
            close(ttyfd);
        }
        // else
        // {
        //     perror("Open tty failed : ");
        // }
        return len;
    }
    return 0;
}

uint32_t debug_cb_log_to_file(const void *buffer, uint32_t len)
{
    if (buffer && m_log_to_file_buffer)
    {
        size_t written = lwrb_write(&m_ringbuffer_log_to_file, buffer, len);
        if (written != len)
        {
            printf("Log to ringbuffer file failed %d/%d\r\n", written, len);
        }
        return len;
    }
    return 0;
}


uint32_t app_debug_output_cb(const void *buffer, uint32_t len)
{
    if (buffer)
    {
        // const uint8_t *ptr = buffer;
        // for (uint32_t i = 0; i < len; i++)
        // {
        //     putchar(ptr[i]);
        // }
        // fflush(stdout);
        return len;
    }
    return 0;
}

bool app_debug_lock(bool lock, uint32_t timeout_ms)
{
    if (lock)
    {
        pthread_mutex_lock(&m_mutex_dbg);
    }
    else
    {
        pthread_mutex_unlock(&m_mutex_dbg);
    }
    return true;
}

static const char *app_mqtt_get_username()
{
    return m_device_config.mqtt.username;
}

static const char *app_mqtt_get_password()
{
    return m_device_config.mqtt.password;
}

static const char *app_mqtt_get_broker_url(void)
{
    static char m_url[128];
    sprintf(m_url, "%s:%d", m_device_config.mqtt.url, m_device_config.mqtt.port);
    return m_url;
}

static int do_reboot(void)
{
    send_msg_to_watchdog_mcu("reset me, please");
    app_debug_unregister_callback_print(debug_cb_log_to_file);
    MSLEEP(1000);
    return run_shell_cmd(NULL, 0, false, "reboot");
}

void audio_classD_mute(int is_mute)
{
    // # Unmute external ClassD, inverted logic
    // #   mraa-gpio set 34 1
    // int f;
    // f = open(GPIO_CLASS_D_MUTE_PATH, O_RDWR);
    // if (f == -1) 
    // {
    //     perror("Unable to open "GPIO_CLASS_D_MUTE_PATH);
    //     return;
    // }
    // if (write(f, is_mute ? "0" : "1", 1) != 1) 
    // {
    //     perror("Error writing to "GPIO_CLASS_D_MUTE_PATH);
    // }

    // close(f);
    if (is_mute)
    {
        run_shell_cmd(NULL, 0, true, "timeout 2 mraa-gpio set 34 0");
    }
    else
    {
        run_shell_cmd(NULL, 0, true, "timeout 2 mraa-gpio set 34 1");
    }
}

void fm_relay_control(int disable_audio_from_headphone)
{
    // mraa-gpio set 16 0 -> enable audio
    // int f;
    // f = open(GPIO_FM_RELAY_PATH, O_RDWR);
    // if (f == -1) 
    // {
    //     perror("Unable to open "GPIO_FM_RELAY_PATH);
    //     return;
    // }
    // if (write(f, disable_audio_from_headphone ? "1" : "0", 1) != 1) 
    // {
    //     perror("Error writing to "GPIO_FM_RELAY_PATH);
    // }
    // close(f);

    if (disable_audio_from_headphone)
    {
        run_shell_cmd(NULL, 0, true, "timeout 2 mraa-gpio set 16 1");
    }
    else
    {
        run_shell_cmd(NULL, 0, true, "timeout 2 mraa-gpio set 16 0");
    }
}

void audio_codec_mute(int is_mute)
{
    if (m_stream_monitor.mute == is_mute)
        return;
    m_stream_monitor.mute = is_mute;
    
    // timeout', '2', 'mraa-gpio', 'set', '17', str(mute)
    fm_relay_control(is_mute);
    audio_classD_mute(is_mute);

    // int f;
    // f = open(GPIO_CODEC_MUTE_PATH, O_RDWR);
    // if (f == -1) 
    // {
    //     perror("Unable to open "GPIO_CODEC_MUTE_PATH);
    //     return;
    // }

    // if (write(f, is_mute ? "1" : "0", 1) != 1) 
    // {
    //     perror("Error writing to "GPIO_CODEC_MUTE_PATH);
    // }

    // close(f);

    if (is_mute)
    {
        run_shell_cmd(NULL, 0, true, "timeout 2 mraa-gpio set 17 1");
    }
    else
    {
        run_shell_cmd(NULL, 0, true, "timeout 2 mraa-gpio set 17 0");
    }
}

void led_indicator()
{
    // If server is not connected -> led red blink
    // If server is connected -> led green blink
    static int led_green_value = 0;
    if (m_mqtt_state != MQTT_STATE_CONNECTED)
    {
        int f;

        f = open(GPIO_LED_GREEN_PATH, O_RDWR);
        if (f == -1) 
        {
            perror("Unable to open "GPIO_LED_GREEN_PATH);
            return;
        }
        if (led_green_value == 0)
        {
            led_green_value = 1;
            if (write(f, "1", 1) != 1) 
            {
                perror("Error writing to "GPIO_LED_GREEN_PATH);
            }
        }
        else
        {
            led_green_value = 0;
            if (write(f, "0", 1) != 1) 
            {
                perror("Error writing to "GPIO_LED_GREEN_PATH);
            }
        }

        close(f);
    }
    else
    {
        int f;

        f = open(GPIO_LED_GREEN_PATH, O_RDWR);
        if (f == -1) 
        {
            perror("Unable to open "GPIO_LED_GREEN_PATH);
            return;
        }

        // Toggle LED 50 ms on, 50ms off, 100 times (10 seconds)
        if (write(f, "1", 1) != 1) 
        {
            perror("Error writing to "GPIO_LED_GREEN_PATH);
        }
        close(f);
    }
}

void ext_mcu_please_reset_me()
{
    // #warning "Implement later"
}

void mqtt_publish_message(char *header, const char *format, ...)
{
    int len = 0;
    char *payload = calloc(1024, 1);
    char topic[32];
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
    int index = 0;
    int rc;

    if (!payload)
    {
        DEBUG_WARN("Can't allocate mem!\r\n");
        do_reboot();
        goto end;
    }

    index = sprintf(payload, "%s,", header);
    va_list arg_ptr;

    if (strlen(m_device_config.imei) >= 15)
    {
        sprintf(topic, "%s%s", SLAVE_PUB_TOPIC_HEADER, m_device_config.imei);
    }
    else
    {
        sprintf(topic, "%s%s", SLAVE_PUB_TOPIC_HEADER, "NA");
    }

    va_start(arg_ptr, format);
    len = vsnprintf(payload+index, 1024-index, format, arg_ptr);
    va_end(arg_ptr);


    opts.onSuccess = mqtt_publish_complete_cb;
    opts.onFailure = mqtt_publish_failure_cb;
    opts.context = client;
    pubmsg.payload = payload;
    pubmsg.payloadlen = (int)(index+len);
    pubmsg.qos = QOS;
    pubmsg.retained = 0;
    if ((rc = MQTTAsync_sendMessage(client, topic, &pubmsg, &opts)) 
        != MQTTASYNC_SUCCESS)
    {
        DEBUG_WARN("Failed to start sendMessage, return code %d\n", rc);
    }
    else
    {
        DEBUG_VERBOSE("slave publish OK\r\n");
        DEBUG_INFO("Pub - > %s\r\n", payload);
    }

end:
    if (payload)
        free(payload);
}

char *get_stream_state(int state)
{
    if (m_device_config.mode == 4)      // No operation
    {
        return "NONE";
    }
    if (state == STREAM_IDLE)
    {
        return "INIT";
    }
    else if (state == STREAM_STOP)
    {
        return "STOP";
    }
    else if (state == STREAM_RUNNING)
    {
        return "RUNNING";
    }
    return "STOP";
}

void mqtt_publish_heartbeat_message(void)
{
    char *heartbeat_msg = NULL;

    heartbeat_msg = calloc(1024, 1);
    if (!heartbeat_msg)
    {
        DEBUG_INFO("Cannot malloc memory\r\n");
        // do_reboot();
        goto exit;
    }

    const char *network_interface = "4G";
    const char *gps_lat = "0.000";
    const char * gps_long = "0.000";

    sprintf(heartbeat_msg, "%s,%s,%s,%s,%d,%d,%s,%d,%d,MIC,OFF,OFF,10,0,0,%s,%s,CSQ:%d,%s,%s,%s,%s,255,0,0,0,4200",
            m_device_config.imei, FIRMWARE_VERSION, get_stream_state(m_stream_monitor.state), network_interface,   \
            m_stream_monitor.stream_time, m_stream_monitor.downloaded,  \
            sys_get_last_streaming_master(), 0, m_device_config.volume,   \
            gps_lat, gps_long, m_gsm_csq, m_gsm_operator, m_gsm_technology, m_gsm_band, m_gsm_channel);

    mqtt_publish_message("INF", heartbeat_msg);
exit:
    if (heartbeat_msg)
        free(heartbeat_msg);
}

void mqtt_send_reset_msg()
{
    char *msg = NULL;
    int index = 0;
    msg = calloc(1024, 1);
    if (!msg)
    {
        DEBUG_INFO("Cannot malloc memory\r\n");
        // do_reboot();
        goto exit;
    }
    // msg = 'CFG,'
    
    // # update master
    index += sprintf(msg+index, "RESET|MAC=%s,", m_sys_mac_str);
    index += sprintf(msg+index, "GSM=%s-%s,", m_device_config.imei, gsm_module_name);
    index += sprintf(msg+index, "SIM=%s-%s,", m_device_config.imsi, m_device_config.ccid);
    index += sprintf(msg+index, "FW:=%s,", FIRMWARE_VERSION);
    index += sprintf(msg+index, "Reason:%d-0-0,", m_device_config.reset_reason);
    index += sprintf(msg+index, "Count:=%u,IO:00,", m_device_config.reset_counter);
    index += sprintf(msg+index, "VOL:%d,", m_device_config.volume);
    index += sprintf(msg+index, "Build:%s-%s", __DATE__, __TIME__);

    mqtt_publish_message("DBG", msg);

exit:
    if (msg)
        free(msg);
}


void mqtt_send_configuration_to_server()
{
    char *msg = NULL;
    int index = 0;
    msg = calloc(1024, 1);
    if (!msg)
    {
        DEBUG_INFO("Cannot malloc memory\r\n");
        // do_reboot();
        goto exit;
    }

    // # update master
    index += sprintf(msg+index, "MASTER1=%s,", m_device_config.master[0]);
    index += sprintf(msg+index, "MASTER1_1=%s,", m_device_config.master[1]);
    index += sprintf(msg+index, "MASTER1_2=%s,", m_device_config.master[2]);
    
    index += sprintf(msg+index, "MASTER2=%s,", m_device_config.master[3]);
    index += sprintf(msg+index, "MASTER2_1=%s,", m_device_config.master[4]);
    index += sprintf(msg+index, "MASTER2_2=%s,", m_device_config.master[5]);

    index += sprintf(msg+index, "MASTER3=%s,", m_device_config.master[6]);
    index += sprintf(msg+index, "MASTER3_1=%s,", m_device_config.master[7]);
    index += sprintf(msg+index, "MASTER3_2=%s,", m_device_config.master[8]);

    // # Volume and working mode
    index += sprintf(msg+index, "VOLUME=%d,", m_device_config.volume);
    index += sprintf(msg+index, "MODE=%d,", m_device_config.mode);

    // # Stream url
    index += sprintf(msg+index, "STREAM=%s,", m_device_config.stream_url);
    // Log
    index += sprintf(msg+index, "TTY:%d,", m_device_config.log_to_tty);
    index += sprintf(msg+index, "LOG_FILE:%d,", m_device_config.log_to_file);
    // # MQTT
    // msg += ("MQTT_SERVER=%s," % sys_configuration_json['broker'])
    // msg += ("MQTT_USERNAME=%s," % sys_configuration_json['username'])
    // msg += ("MQTT_PASSWORD=%s," % sys_configuration_json['password'])

    mqtt_publish_message("CFG", msg);
exit:
    if (msg)
        free(msg);
}

void* mqtt_thread(void* arg)
{
    int rc;
	MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;
    static int sub_seq = 0;
    // ipc_data_t tx_ipc;
    // // Create IPC
    // if (-1 == (m_ipc_msqid = msgget((key_t)IPC_KEY, IPC_CREAT | 0666)))
	// {
	// 	perror("msgget() failed");
    //     // do_reboot();
	// 	exit( 1);
	// }
    // DEBUG_INFO("IPC id %d\r\n", m_ipc_msqid);
    // tx_ipc.msg_type = 1;
    // sprintf((char*)tx_ipc.payload, "%s", "Hello");
    // tx_ipc.size = strlen((char*)tx_ipc.payload);

    while (1)
    {
        // tx_ipc.msg_type++;
        // if (-1 == msgsnd(m_ipc_msqid, &tx_ipc, sizeof(tx_ipc), IPC_NOWAIT))
        // {
        //     perror("msgsnd() failed");
        //     // exit( 1);
        // }
        bool do_auto_stop_stream = false;
        bool do_restart_interface = false;
        led_indicator();
        if (m_delay_wait_for_sim_imei > 0)
        {
            m_delay_wait_for_sim_imei--;
        }
        // Ensure thread safe
        pthread_mutex_lock(&m_mutex_mqtt);

        if (m_mqtt_disconnect_timeout == (MQTT_THREAD_CYCLE_MS/2))
        { 
            do_restart_interface = true;
        }
        if (m_mqtt_disconnect_timeout-- <= 10)
        {
            do_reboot();

            if (m_mqtt_disconnect_timeout < 5)
            {
                ext_mcu_please_reset_me();
            }
        }
        
        if (m_auto_stop_stream_s > 0)
        {
            m_auto_stop_stream_s--;
            if (m_auto_stop_stream_s == 0)
            {
                do_auto_stop_stream = true;
            }
        }
        pthread_mutex_unlock(&m_mutex_mqtt);

        if (do_restart_interface)
        {
            DEBUG_WARN("Auto reset interface\r\n");
            // run_shell_cmd(NULL, 0, true, "timeout 2 service network restart");
            
        }
        if (do_auto_stop_stream)
        {
            mqtt_publish_message("DBG", "Auto stop stream");
            if (is_audio_thread_alive())
            {
                kill_audio_thread();
                audio_codec_mute(1);
            }
            m_last_streaming_master_level = -1;
        }

        if (m_delay_reboot > 0)
        {
            m_delay_reboot--;
            audio_codec_mute(1);
            if (m_delay_reboot <= 2)
            {
                do_reboot();
            }
        }

        switch (m_mqtt_state)
        {
        case MQTT_STATE_INIT:
        {
            static char m_cli_id[48];
            sprintf(m_cli_id, "mbf_%s", m_sys_mac_str);
            sub_seq = 0;
            conn_opts.username = app_mqtt_get_username();
            conn_opts.password = app_mqtt_get_password();
            DEBUG_VERBOSE("Broker %s\r\n", app_mqtt_get_broker_url());
            if ((rc = MQTTAsync_create(&client, 
                                        app_mqtt_get_broker_url(), 
                                        m_cli_id, 
                                        MQTTCLIENT_PERSISTENCE_NONE, NULL))
                != MQTTASYNC_SUCCESS)
            {
                DEBUG_WARN("Failed to create client, return code %d\n", rc);
                break;
            }

            if ((rc = MQTTAsync_setCallbacks(client, 
                                            client, 
                                            on_mqtt_conn_lost, 
                                            on_mqtt_data_callback, 
                                            mqtt_delivery_complete)) 
                != MQTTASYNC_SUCCESS)
            {
                DEBUG_WARN("Failed to set callbacks, return code %d\n", rc);
                m_mqtt_state = MQTT_STATE_NEED_DESTROY;
                break;
            }

            conn_opts.keepAliveInterval = 20;
            conn_opts.cleansession = 1;
            conn_opts.onSuccess = on_mqtt_connected_cb;
            conn_opts.onFailure = on_mqtt_connect_failure_cb;
            conn_opts.context = client;

            mqtt_fsm_cycle = 0;
            m_mqtt_state = MQTT_STATE_CONNECTING;
        }
            break;
        
        case MQTT_STATE_CONNECTING:
            // if (m_mqtt_error_counter++ > MQTT_TIMEOUT_DO_RESET_INTERFACE)
            // {
            //     m_mqtt_error_counter = 0;
            //     DEBUG_INFO("Restart interface\r\n");
            //     run_shell_cmd(NULL, 0, true, "timeout 3 service network restart");
            // }
            m_send_config_to_svr = true;
            if (mqtt_fsm_cycle % 40 == 0)
            {
                if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
                {
                    DEBUG_WARN("Failed to start connect, return code %d\n", rc);
                    m_mqtt_state = MQTT_STATE_NEED_DESTROY;
                }
            }
            mqtt_fsm_cycle++;
            break;
        
        case MQTT_STATE_CONNECTED:
            m_mqtt_error_counter = 0;
            if (m_delay_wait_for_sim_imei > 0)
            {
                break;
            }
            /* code */
            if (mqtt_fsm_cycle % MQTT_AUTO_SUB_INTERVAL == 0 || m_mqtt_do_resub)
            {
                m_mqtt_do_resub = false;
                sub_seq = 0;
                static char topic[10][32];
                int qos_list[10];
                int j = 1;
                char* multi_topics[10];
       
                memset(topic, 0, sizeof(topic));
   
                sprintf(topic[0], "vs/sub/%s", m_device_config.imei);
                for (int i = 0; i < 9; i++)
                {
                    if (strlen(m_device_config.master[i]) > 10)
                    {
                        sprintf(topic[j++], "vs/pub/%s", m_device_config.master[i]);
                    }
                }
                DEBUG_VERBOSE("Subscribing %d topic, using QoS%d\n", 
                            j,
                            QOS);

                for (int i = 0 ; i < 10; i++)
                {
                    multi_topics[i] = topic[i];
                    qos_list[i] = 1;
                }

                for (int k = 0; k < 10; k++)
                {
                    if (strlen(topic[k]))
                    {
                        DEBUG_VERBOSE("Topic[%d] = %s\r\n", k, topic[k]);
                    }
                }

                MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
                opts.onSuccess = on_mqtt_subscribed_cb;
                opts.onFailure = on_subscribe_failure_cb;
                opts.context = client;
                if ((rc = MQTTAsync_subscribeMany(client, j, multi_topics, qos_list, &opts)) 
                    != MQTTASYNC_SUCCESS)
                {
                    DEBUG_WARN("Failed to start subscribe, return code %d\n", rc);
                    MSLEEP(1000);
                }
            }

            if (mqtt_fsm_cycle % MQTT_AUTO_PUB_INTERVAL == 0)
            {
                mqtt_publish_heartbeat_message();
            }
            else if (m_send_reset_to_svr)
            {
                m_send_reset_to_svr = false;
                mqtt_send_reset_msg();
                MSLEEP(500);
            }
            else if (m_send_config_to_svr)
            {
                m_send_config_to_svr = false;
                mqtt_send_configuration_to_server();
                MSLEEP(500);
            }

            if (mqtt_fsm_cycle % AUTO_REPORT_LOAD_AVG == 0)
            {
                char *cpu_load = cpu_get_load_avg();
                char *free_ram = sys_get_free_ram();
                if (cpu_load && free_ram)
                {
                    char *endline = strstr(cpu_load, "\n");
                    if (endline) *endline = 0;
                    time_t rawtime;
                    struct tm * timeinfo;

                    time (&rawtime);
                    timeinfo = localtime(&rawtime);

                    mqtt_publish_message("DGB", "[%02d:%02d] CPU usage %s\r\n%s", 
                                                timeinfo->tm_hour, timeinfo->tm_min, 
                                                cpu_load, 
                                                free_ram);
                }
            }

            mqtt_fsm_cycle++;
            if (m_auto_disconnect_mqtt > 0)
            {
                m_auto_disconnect_mqtt--;
                if (m_auto_disconnect_mqtt == 0)
                {
                    m_mqtt_state = MQTT_STATE_DISCONNECTED;
                }
            }
            break;
        case MQTT_STATE_DISCONNECTED:
        {
            DEBUG_INFO("MQTT disconnected\r\n");
            MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
            int rc;
            opts.onSuccess = on_mqtt_disconnect_success_cb;
            opts.onFailure = on_mqtt_disconnect_failure_cb;
            if ((rc = MQTTAsync_disconnect(client, &opts)) != MQTTASYNC_SUCCESS)
            {
                DEBUG_WARN("Failed to start disconnect, return code %d\n", rc);
            }
            MSLEEP(2000);
            mqtt_set_fsm_state(MQTT_STATE_INIT);
        }
            break;

        case MQTT_STATE_NEED_DESTROY:
            MQTTAsync_destroy(&client);
            m_mqtt_state = MQTT_STATE_INIT;
            break;

        default:
            break;
        }

        thread_wdt_feed(WDT_THREAD_MQTT);
        MSLEEP(MQTT_THREAD_CYCLE_MS);
    }
    DEBUG_WARN("Exit mqtt thread\r\n");
    // exit the current thread
    pthread_exit(NULL);
}

static void do_ping(void)
{
    static uint32_t seq = 0;
    min_msg_t msg;
    msg.id = MIN_ID_PING;
    msg.payload = &seq;
    msg.len = sizeof(seq);
    min_send_frame(&m_min_host_context, &msg);
}


void* socket_thread(void* arg)
{
    DEBUG_INFO("Start socket thread\r\n");
    static int monitor_err_counter = 0;
    struct sockaddr_un addr;
    int auto_disconnect = 0;
    while (1)   
    {
        thread_wdt_feed(WDT_THREAD_TCP);
        switch (m_ipc_tcp_state)
        {
        case TCP_STATE_INIT:
        {
            // Create data socket
            auto_disconnect = 0;
            m_tcp_cli_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_tcp_cli_fd != -1)
            {
                // Construct server socket address
                memset(&addr, 0, sizeof(struct sockaddr_un));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
                struct timeval timeout;      
                timeout.tv_sec = 0;
                timeout.tv_usec = 500000;

                if (setsockopt (m_tcp_cli_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    sizeof timeout) < 0)
                {
                    DEBUG_WARN("setsockopt failed\r\n");
                    monitor_err_counter = 0;
                    m_ipc_tcp_state = TCP_STATE_NEED_DESTROY;
                }
                else
                {
                    monitor_err_counter = 0;
                    m_ipc_tcp_state = TCP_STATE_CONNECTING;
                }
            }
            else
            {
                monitor_err_counter++;
                DEBUG_WARN("Cannot create client socket [%d] times\r\n", monitor_err_counter);
                if (monitor_err_counter > 10)
                {
                    do_reboot();
                }
            }
        }
            break;
        case TCP_STATE_CONNECTING:
            if (monitor_err_counter % 10 == 0)
            {
                DEBUG_WARN("Connecting to %s\r\n", SOCKET_PATH);
                // Connect client socket to socket address
                if (connect(m_tcp_cli_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) 
                    == -1)
                {
                    DEBUG_WARN("Cannot connect to server (is the server running?)\r\n");
                    MSLEEP(1000);
                }
                else
                {
                    monitor_err_counter = 0;
                    m_ipc_tcp_state = TCP_STATE_CONNECTED;
                    DEBUG_INFO("Connected\r\n");
                }
            }
            monitor_err_counter++;
            break;
        case TCP_STATE_CONNECTED:
        {
            struct pollfd fd;
            int ret;
            uint8_t rx_buffer[128];

            fd.fd = m_tcp_cli_fd; // your socket handler 
            fd.events = POLLIN;
            ret = poll(&fd, 1, 500); // ms second for timeout
            switch (ret) 
            {
                case -1:
                    // Error
                    DEBUG_WARN("Poll TCP socket error\r\n");
                    m_ipc_tcp_state = TCP_STATE_NEED_DESTROY;
                    break;
                case 0:
                    do_ping();
                    break;
                default:
                {
                    int size = recv(m_tcp_cli_fd, rx_buffer, sizeof(rx_buffer)-1, 0);
                    if (size > 0)
                    {
                        // DEBUG_INFO("RX [%u] : %.*s\r\n", size, size, rx_buffer);
                        min_rx_feed(&m_min_host_context, rx_buffer, size);\
                    }
                    else
                    {
                        DEBUG_WARN("No socket data\r\n");
                        auto_disconnect++;
                        if (auto_disconnect > 5)
                        {
                            DEBUG_WARN("Socket lost\r\n");
                            m_ipc_tcp_state = TCP_STATE_NEED_DESTROY;
                        }
                    }
                }
                    break;
            }
        }
            break;

        case TCP_STATE_DISCONNECTED:
        {
            
        }
            break;

        case TCP_STATE_NEED_DESTROY:
        {
            DEBUG_WARN("Destroy TCP sock\r\n");
            close(m_tcp_cli_fd);
            m_tcp_cli_fd = -1;
            monitor_err_counter = 0;
            m_ipc_tcp_state = TCP_STATE_INIT;
        }
            break;
        default:
            break;
        }
        MSLEEP(1000);
    }
    pthread_exit(NULL);
}

bool sys_get_mac_addr(char *mac)
{
    char str_mac2[24];
    bool retval = false;
    FILE *f = fopen("/sys/class/net/eth0/address", "r");
    if (!f)
    {
        DEBUG_WARN("Unable to open /sys/class/net/eth0/address\r\n");
        goto exit;
    }

    if (fgets(m_sys_mac_str, 23, f))
    {
        // Remove \r\n
        char *crlf = strstr(m_sys_mac_str, "\r");
        if (crlf)
        {
            *crlf = 0;
        }
        crlf = strstr(m_sys_mac_str, "\n");
        if (crlf)
        {
            *crlf = 0;
        }

        int j = 0;
        for (int i = 0; i < strlen(m_sys_mac_str); i++)
        {
            if (m_sys_mac_str[i] != ':')      // remove ":"
            {
                str_mac2[j++] = m_sys_mac_str[i];
            }
        }

        char *pos = str_mac2;
        for (size_t count = 0; count < 6; count++) 
        {
            sscanf(pos, "%2hhx", &mac[count]);
            pos += 2;
        }
        retval = true;
    }

    fclose(f);
exit:
    return retval;
}

static uint8_t *gen_aes_key(uint8_t *mac)
{
    static uint8_t aes_key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 22, 11, 12, 13, 14, 15};
    memcpy(aes_key, mac, 6);
    
    for (int i = 0; i < 10; i++)
    {
        int j = i % 6;
        aes_key[i+6] = (aes_key[6-j-1]*aes_key[j] + 1)%(255); 
    }
    // DEBUG_DUMP(aes_key, 16, "KEY=");
    return aes_key;
}

void sys_load_configuration(uint8_t *mac)
{
    int file_size = 0;
    FILE *f = fopen(SYS_CONFIG_RAW_DATA, "r");
    uint8_t *key;
    cJSON *root = NULL;
    cJSON *key_reset_counter, *key_reset_reason, *key_broker, *key_username, *key_password, *key_port;
    cJSON *key_last_streaming_master, *key_vol, *key_mode;
    cJSON *key_gsm_imei, *key_sim_imei, *key_stream_url;
    cJSON *key_debug_level, *key_master, *key_log_tty;

    m_device_config.debug_level = DEBUG_LEVEL_INFO;
    if (!f)
    {
        DEBUG_VERBOSE("Unable to open"SYS_CONFIG_RAW_DATA"\r\n");
        goto open_encrypted_file;
    }

    key = gen_aes_key(mac);
    if (fgets((char*)m_raw_sys_configuration, sizeof(m_raw_sys_configuration)-1, f))
    {
        fclose(f);
        f = NULL;
        DEBUG_INFO("%s\r\n", m_raw_sys_configuration);
        int aes_buffer_length = AES_ECB_encrypt((uint8_t *)m_raw_sys_configuration, 
                                                (uint8_t *)key, 
                                                (char*)m_raw_aes_buf, 
                                                strlen((char *)m_raw_sys_configuration));
        memset(m_raw_sys_configuration, 0, sizeof(m_raw_sys_configuration));

        // Save to encrypted file
        f = fopen(SYS_CONFIG_BINARY_DATA,"w");
        if (!f)
        {
            DEBUG_WARN("Create encrypted configuration file err\r\n");
            goto exit;
        }
        
        file_size = fwrite(m_raw_aes_buf, 1, aes_buffer_length, f);
        if (file_size != aes_buffer_length)
        {
            DEBUG_WARN("Write binary data to enc file failed %d != %d\r\n", 
                        file_size, 
                        aes_buffer_length);
            goto exit;
        }

        DEBUG_INFO("Write to file success\r\n");
        fclose(f);
        f = NULL;
        remove(SYS_CONFIG_RAW_DATA);
        goto open_encrypted_file;
    }
    else
    {
        fclose(f);
        f = NULL;
        goto exit;
    }

open_encrypted_file:
    f = fopen(SYS_CONFIG_BINARY_DATA, "r");
    if (!f)
    {
        DEBUG_WARN("Open encrypted file failed\r\n");
        goto exit;
    }
    // Get file size
    file_size = fseek(f, 0L, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0L, SEEK_SET);
    
    // Read raw data
    fread(m_raw_aes_buf, file_size, 1, f);

    if (file_size > 0)
    {
        memset(m_raw_sys_configuration, 0, sizeof(m_raw_sys_configuration));
        key = gen_aes_key(mac);
        AES_ECB_decrypt((uint8_t*)m_raw_aes_buf, 
                        key, 
                        (uint8_t*)m_raw_sys_configuration, 
                        file_size);
        DEBUG_VERBOSE("Decrypted conf : %s\r\n", 
                        (char*)m_raw_sys_configuration);
    }

    // Load configuration
    root = cJSON_Parse(m_raw_sys_configuration);

    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    // Broker
    key_broker = cJSON_GetObjectItem(root, "broker");
    if (!key_broker)
    {
        DEBUG_WARN("JSON key broker invalid\r\n");
        goto exit;
    }
    sprintf(m_device_config.mqtt.url, "%s", key_broker->valuestring);

    // Username
    key_username = cJSON_GetObjectItem(root, "username");
    if (!key_username)
    {
        DEBUG_WARN("JSON key username invalid\r\n");
        goto exit;
    }
    sprintf(m_device_config.mqtt.username, "%s", key_username->valuestring);

    // Password
    key_password = cJSON_GetObjectItem(root, "password");
    if (!key_password)
    {
        DEBUG_WARN("JSON key password invalid\r\n");
        goto exit;
    }
    sprintf(m_device_config.mqtt.password, "%s", key_password->valuestring);

    // Port
    key_port = cJSON_GetObjectItem(root, "port");
    if (!key_port)
    {
        DEBUG_WARN("JSON key port invalid\r\n");
        goto exit;
    }
    m_device_config.mqtt.port = key_port->valueint;

    // Vol
    key_vol = cJSON_GetObjectItem(root, "volume");
    if (key_vol)
    {
        m_device_config.volume = key_vol->valueint;
    }

    // Last streaming master
    key_last_streaming_master = cJSON_GetObjectItem(root, "last_streaming_master");
    if (key_last_streaming_master)
    {
        sprintf(m_device_config.last_stream_master, 
                "%s", 
                key_last_streaming_master->valuestring);
    }

    // IMEI & SIM
    key_gsm_imei = cJSON_GetObjectItem(root, "imei");
    if (key_gsm_imei)
    {
        sprintf(m_device_config.imei, "%s", key_gsm_imei->valuestring);
    }

    key_sim_imei = cJSON_GetObjectItem(root, "sim_imei");
    if (key_sim_imei)
    {
        sprintf(m_device_config.ccid, "%s", key_sim_imei->valuestring);
    }

    // Stream URL
    key_stream_url = cJSON_GetObjectItem(root, "stream_url");
    if (key_stream_url)
    {
        sprintf(m_device_config.stream_url, "%s", key_stream_url->valuestring);
    }

    // Mode
    key_mode = cJSON_GetObjectItem(root, "working_mode");
    if (key_mode)
    {
        m_device_config.mode = key_mode->valueint;
    }

    // Reset counter & reset reason
    key_reset_counter = cJSON_GetObjectItem(root, "reset_counter");
    if (key_reset_counter)
    {
        m_device_config.reset_counter = key_reset_counter->valueint;
    }

    key_reset_reason = cJSON_GetObjectItem(root, "reset_reason");
    if (key_reset_reason)
    {
        m_device_config.reset_reason = key_reset_reason->valueint;
    }

    // Load debug level
    key_debug_level = cJSON_GetObjectItem(root, "debug_level");
    if (key_debug_level)
    {
        m_device_config.debug_level = key_debug_level->valueint;
    }

    // Load master T
    key_master = cJSON_GetObjectItem(root, "master_t");
    if (key_master)
    {
        cJSON *tmp;
        int i = 0;
        cJSON_ArrayForEach(tmp, key_master)
        {
            sprintf(m_device_config.master[i], "%s", tmp->valuestring);
            i++;
        }
    }

    key_master = cJSON_GetObjectItem(root, "master_h");
    if (key_master)
    {
        cJSON *tmp;
        int i = 0;
        cJSON_ArrayForEach(tmp, key_master)
        {
            sprintf(m_device_config.master[3+i], "%s", tmp->valuestring);
            i++;
        }
    }

    key_master = cJSON_GetObjectItem(root, "master_x");
    if (key_master)
    {
        cJSON *tmp;
        int i = 0;
        cJSON_ArrayForEach(tmp, key_master)
        {
            sprintf(m_device_config.master[6+i], "%s", tmp->valuestring);
            i++;
        }
    }

    // Log to file
    key_log_tty = cJSON_GetObjectItem(root, "tty");
    if (key_log_tty)
    {
        m_device_config.log_to_tty = key_log_tty->valueint;
    }

    key_log_tty = cJSON_GetObjectItem(root, "log_level");
    if (key_log_tty)
    {
        m_device_config.log_level = key_log_tty->valueint;
    }

    key_log_tty = cJSON_GetObjectItem(root, "log_file");
    if (key_log_tty)
    {
        m_device_config.log_to_file = key_log_tty->valueint;
    }
    m_device_config.log_to_file = 1;

    // app_debug_set_level(m_device_config.log_level);
exit:
    if (root)
        cJSON_Delete(root);
    if (f)
        fclose(f);
}

static char *gsm_get_sim_ccid()
{
    if (strstr("EC", gsm_module_name))
        return gsm_get_sim_imei_ec2x();
    return gsm_get_sim_imei_yoga();
}

void gsm_change_state(int new_state)
{
    if (m_gsm_state != new_state)
    {
        DEBUG_INFO("GSM state change from %d to %d\r\n", 
                    m_gsm_state, new_state);
        m_gsm_fsm_counter = 0;
        m_gsm_state = new_state;
    }
}

void save_current_config(void)
{
    FILE *f;
    int aes_buffer_length;
    uint8_t *key;
    int size_written;
    DEBUG_INFO("New cfg \r\n%s\r\n", m_raw_sys_configuration);
    // Save to encrypted file
    f = fopen(SYS_CONFIG_BINARY_DATA,"w");
    if (!f)
    {
        DEBUG_WARN("Create encrypted configuration file err\r\n");
        goto exit;
    }
    key = gen_aes_key(m_mac_addr);
    aes_buffer_length = AES_ECB_encrypt((uint8_t *)m_raw_sys_configuration, 
                                        (uint8_t *)key, 
                                        (char*)m_raw_aes_buf, 
                                        strlen((char *)m_raw_sys_configuration));

    size_written = fwrite(m_raw_aes_buf, 1, aes_buffer_length, f);
    memset(m_raw_aes_buf, 0, sizeof(m_raw_aes_buf));
    if (size_written != aes_buffer_length)
    {
        DEBUG_WARN("Write binary data to enc file failed %d != %d\r\n", 
                    size_written, aes_buffer_length);
        goto exit;
    }

    DEBUG_VERBOSE("Write to file success\r\n");
exit:
    if (f)
        fclose(f);
}

void sys_save_gsm_imei(char *imei)
{
    if (!imei)
    {
        return;
    }
    cJSON *key_gsm_imei;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_gsm_imei = cJSON_GetObjectItem(root, "imei");
    if (!key_gsm_imei)
    {
        DEBUG_WARN("JSON key imei invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_gsm_imei, imei);
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_master()
{
    cJSON *key_master, *tmp;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);
    int i = 0;
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }
    
    // Master T
    key_master = cJSON_GetObjectItem(root, "master_t");
    if (!key_master)
    {
        DEBUG_WARN("JSON key master_t invalid\r\n");
        goto exit;
    }
    cJSON_ArrayForEach(tmp, key_master)
    {
        cJSON_SetValuestring(tmp, m_device_config.master[i]);
        i++;
    }
    i = 3;

    // Master H
    key_master = cJSON_GetObjectItem(root, "master_h");
    if (!key_master)
    {
        DEBUG_WARN("JSON key master_h invalid\r\n");
        goto exit;
    }
    cJSON_ArrayForEach(tmp, key_master)
    {
        cJSON_SetValuestring(tmp, m_device_config.master[i]);
        i++;
    }
    i = 6;
    
    key_master = cJSON_GetObjectItem(root, "master_x");
    if (!key_master)
    {
        DEBUG_WARN("JSON key master_x invalid\r\n");
        goto exit;
    }
    cJSON_ArrayForEach(tmp, key_master)
    {
        cJSON_SetValuestring(tmp, m_device_config.master[i]);
        i++;
    }
    
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_stream_url()
{
    cJSON *key_stream_url;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);

    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }
    
    // Stream URL
    key_stream_url = cJSON_GetObjectItem(root, "stream_url");
    if (!key_stream_url)
    {
        DEBUG_WARN("JSON key stream_url invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_stream_url, m_device_config.stream_url);
    
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_mqtt_info()
{
    cJSON *key_broker, *key_port, *key_username, *key_password;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);

    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON, \r\n%s\r\n", 
                    __FUNCTION__, m_raw_sys_configuration);
        goto exit;
    }
    
    // Broker
    key_broker = cJSON_GetObjectItem(root, "broker");
    if (!key_broker)
    {
        DEBUG_WARN("JSON key broker invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_broker, m_device_config.mqtt.url);

    // Port
    key_port = cJSON_GetObjectItem(root, "port");
    if (!key_port)
    {
        DEBUG_WARN("JSON key port invalid\r\n");
        goto exit;
    }
    cJSON_SetIntValue(key_port, m_device_config.mqtt.port);

    // Username
    key_username = cJSON_GetObjectItem(root, "username");
    if (!key_username)
    {
        DEBUG_WARN("JSON key username invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_username, m_device_config.mqtt.username);

    // Password
    key_password = cJSON_GetObjectItem(root, "password");
    if (!key_username)
    {
        DEBUG_WARN("JSON key password invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_password, m_device_config.mqtt.password);

    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_volume(int volume)
{
    cJSON *key_vol;
    char *json_str = NULL;
    cJSON *root = NULL;
    
    m_device_config.volume = volume;
    root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_vol = cJSON_GetObjectItem(root, "volume");
    if (!key_vol)
    {
        DEBUG_WARN("JSON key volume invalid\r\n");
        cJSON_AddNumberToObject(root, "volume", volume);
        goto save_config;
    }
    cJSON_SetIntValue(key_vol, volume);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_tty_flag(int tty_enable)
{
    cJSON *key_tty;
    char *json_str = NULL;
    cJSON *root = NULL;

    root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_tty = cJSON_GetObjectItem(root, "tty");
    if (!key_tty)
    {
        DEBUG_WARN("JSON key tty invalid\r\n");
        cJSON_AddNumberToObject(root, "tty", tty_enable);
        goto save_config;
    }
    cJSON_SetIntValue(key_tty, tty_enable);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_log_level(int level)
{
    cJSON *key_level;
    char *json_str = NULL;
    cJSON *root = NULL;

    root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_level = cJSON_GetObjectItem(root, "log_level");
    if (!key_level)
    {
        DEBUG_WARN("JSON key log_level invalid\r\n");
        cJSON_AddNumberToObject(root, "log_level", level);
        goto save_config;
    }
    cJSON_SetIntValue(key_level, level);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_log_to_file_flag(int enable)
{
    cJSON *key_log_to_file;
    char *json_str = NULL;
    cJSON *root = NULL;

    root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_log_to_file = cJSON_GetObjectItem(root, "log_file");
    if (!key_log_to_file)
    {
        DEBUG_WARN("JSON key log_level invalid\r\n");
        cJSON_AddNumberToObject(root, "log_file", enable);
        goto save_config;
    }
    cJSON_SetIntValue(key_log_to_file, enable);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_mode(int mode)
{
    cJSON *key_mode;
    char *json_str = NULL;
    cJSON *root = NULL;
    
    m_device_config.mode = mode;
    root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_mode = cJSON_GetObjectItem(root, "working_mode");
    if (!key_mode)
    {
        DEBUG_WARN("JSON key mode invalid\r\n");
        cJSON_AddNumberToObject(root, "working_mode", mode);
        goto save_config;
    }
    cJSON_SetIntValue(key_mode, mode);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_reset_reason(int reason)
{
    cJSON *key_reset_json;
    char *json_str = NULL;
    cJSON *root = NULL;
    
    m_device_config.reset_reason = reason;
    root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_reset_json = cJSON_GetObjectItem(root, "reset_reason");
    if (!key_reset_json)
    {
        DEBUG_WARN("JSON key reset reason invalid\r\n");
        cJSON_AddNumberToObject(root, "reset_reason", reason);
        goto save_config;
    }
    cJSON_SetIntValue(key_reset_json, reason);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

int sys_get_reset_counter()
{
    return m_device_config.reset_counter;
}

int sys_get_volume()
{
    return m_device_config.volume;
}

char *sys_get_last_streaming_master()
{
    return m_device_config.last_stream_master;
}

void sys_set_last_streaming_master()
{
    cJSON *key_last_master;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);

    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON, \r\n%s\r\n", 
                    __FUNCTION__, m_raw_sys_configuration);
        goto exit;
    }
    
    // Broker
    key_last_master = cJSON_GetObjectItem(root, "last_streaming_master");
    if (!key_last_master)
    {
        DEBUG_WARN("JSON key last_streaming_master invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_last_master, m_device_config.last_stream_master);

    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

int sys_get_working_mode()
{
    return m_device_config.mode;
}

void sys_save_reset_counter(int counter)
{
    cJSON *key_reset_counter;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_reset_counter = cJSON_GetObjectItem(root, "reset_counter");
    if (!key_reset_counter)
    {
        DEBUG_WARN("JSON key reset counter invalid\r\n");
        cJSON_AddNumberToObject(root, "reset_counter", counter);
        goto save_config;
    }
    cJSON_SetIntValue(key_reset_counter, counter);

save_config:
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void sys_save_sim_ccid(char *ccid)
{
    if (!ccid)
    {
        return;
    }
    cJSON *key_ccid;
    char *json_str = NULL;
    cJSON *root = cJSON_Parse(m_raw_sys_configuration);
    if (!root)
    {
        DEBUG_WARN("[%s] : Not a JSON\r\n", __FUNCTION__);
        goto exit;
    }

    key_ccid = cJSON_GetObjectItem(root, "sim_imei");
    if (!key_ccid)
    {
        DEBUG_WARN("JSON key sim_imei invalid\r\n");
        goto exit;
    }
    cJSON_SetValuestring(key_ccid, ccid);
    json_str = cJSON_Print(root);
    strcpy(m_raw_sys_configuration, json_str);
    
    save_current_config();
exit:
    if (json_str)
        cJSON_free(json_str);

    if (root)
        cJSON_Delete(root);
}

void gsm_state_machine_poll()
{
    static uint32_t gsm_last_poll;
    uint32_t now = time(NULL);
    static uint32_t get_sim_err_counter = 0;
    if (now - gsm_last_poll < 1)
    {
        return;
    }
    gsm_last_poll = now;

    switch (m_gsm_state)
    {
        case GSM_STATE_INIT:
        {
            m_gsm_technology[0] = 0;
            // Disable echo
            if (m_gsm_fsm_counter == 0)
            {
                get_sim_err_counter = 0;
                gsm_disable_echo();
            }
            else if (m_gsm_fsm_counter == 1)    // Get module name
            {
                get_sim_err_counter = 0;
                gsm_module_name = gsm_get_module_name();
                DEBUG_INFO("Found module %s\r\n", gsm_module_name);
                if (strstr(gsm_module_name, "CLM"))
                {
                    DEBUG_INFO("Setting LTE prefer mode for YOGA CLM920\r\n");
                    // # Set network mode 4g
                    // # 2  --- AUTO
                    // # 14 --- WCDMA only
                    // # 38 --- LTE only
                    // # 55 --- UMTS_LTE, UMTS preferred
                    // # 56 --- UMTS_LTE, LTE preferred
                    gsm_send_at_cmd(1000, 2, "OK", NULL, "AT^MODECONFIG=56\r\n");
                }
                else
                {
                    // TODO EC2X LTE
                    uqmi_setting_prefer_lte();
                    MSLEEP(2000);
                }
            }
            else if (m_gsm_fsm_counter == 2) // Get GSM imei
            {
                char *imei = gsm_get_module_imei();
                if (strlen(imei) < 10)
                    return;
                DEBUG_ERROR("IMEI %s\r\n", imei);
                // Check imei
                if (!strstr(m_device_config.imei, imei))
                {
                    DEBUG_WARN("Found new imei\r\n");
                    sprintf(m_device_config.imei, "%s", imei);
                    sys_save_gsm_imei(imei);
                }
            }
            else if (m_gsm_fsm_counter == 3)
            {
                char *ccid = gsm_get_sim_ccid();
                char *gsm_imsi = gsm_get_sim_cimi();
                sprintf(m_device_config.imsi, "%s", gsm_imsi);
                DEBUG_ERROR("CCID %s, IMEI %s\r\n", ccid, gsm_imsi);
                if (strlen(ccid) < 10)
                {
                    get_sim_err_counter += 1;
                    if (get_sim_err_counter > 10)
                    {
                        sys_save_reset_reason('8');
                        get_sim_err_counter = 0;
                        DEBUG_INFO("Shutdown gsm module\r\n");
                        // subprocess.Popen(['sh', APPLICATION_PATH + 'restart-interface.sh'])
                        gsm_change_state(GSM_RESET);
                    }
                    return;
                }
                else
                {
                    if (!strstr(m_device_config.ccid, ccid))
                    {
                        sprintf(m_device_config.ccid, "%s", ccid);
                        DEBUG_WARN("Save sim ccid\r\n");
                        sys_save_sim_ccid(ccid);
                    }
                    // TODO save imei sim
                    // if (ccid not in sys_configuration_json['sim_imei']):
                    //     sys_configuration_json['sim_imei'] = ccid
                    //     sys_save_current_configuration()
                }
            }
            else if (m_gsm_fsm_counter == 4) //Get network operator
            {
                memset(m_gsm_operator, 0, sizeof(m_gsm_operator));
                memset(m_gsm_technology, 0, sizeof(m_gsm_technology));

                gsm_get_network_operator(m_gsm_operator, m_gsm_technology);
                // gsm_get_network_technology(technology);

                if (strlen(m_gsm_operator) < 4)
                {
                    get_sim_err_counter += 1;
                    if (get_sim_err_counter > 5)
                    {
                        gsm_change_state(GSM_RESET);
                    }
                    return;
                }
                // gsm_access_tech = technology
                // gsm_operator = operator
                get_sim_err_counter = 0;
                DEBUG_ERROR("Network operator %s, technology %s\r\n", 
                            m_gsm_operator, m_gsm_technology);
            }
            else if (m_gsm_fsm_counter == 5)
            {
                if (strstr(gsm_module_name, "CLM"))
                {
                    gsm_get_network_band_yoga_clm920(m_gsm_technology, m_gsm_band, m_gsm_channel);
                }
                else    // EC2x
                {
                    DEBUG_INFO("Get band EC2X\r\n");
                    gsm_get_network_band_ec2x(m_gsm_technology, m_gsm_band, m_gsm_channel);
                }
                DEBUG_INFO("Network info %s, %s, channel %s\r\n", 
                            m_gsm_band, m_gsm_technology, m_gsm_channel);
                m_delay_wait_for_sim_imei = 0;
                gsm_change_state(GSM_STATE_OK);
                return;
            }   
            m_gsm_fsm_counter += 1;
        }
            break;
        
        case GSM_STATE_OK:
        {
            // # Auto get csq
            if ((m_gsm_fsm_counter % 45) == 0)
            {
                m_gsm_csq = gsm_get_csq();
            }
            // Auto get network band
            else if ((m_gsm_fsm_counter % 251) == 0)
            {
                if (strstr(gsm_module_name, "CLM"))
                {
                    gsm_get_network_band_yoga_clm920(m_gsm_technology, m_gsm_band, m_gsm_channel);
                }
                else    // EC2x
                {
                    gsm_get_network_band_ec2x(m_gsm_technology, m_gsm_band, m_gsm_channel);
                }
            }
            
            if ((m_gsm_fsm_counter % 121) == 0)
            {
                if (!gsm_is_sim_inserted())
                {
                    DEBUG_ERROR("Sim not inserted\r\n");
                    gsm_change_state(GSM_RESET);
                    return;
                }
            }
            m_gsm_fsm_counter += 1;
            m_gsm_max_timeout_wait_for_imei = 0;
            if (m_gsm_csq == 0 || m_gsm_csq == 99)
            {
                m_gsm_fsm_counter = 45;
                get_sim_err_counter++;
                if (get_sim_err_counter > 20)
                {
                    get_sim_err_counter = 0;
                    m_gsm_fsm_counter = 0;
                    gsm_change_state(GSM_RESET);
                }
            }
        }
            break;

        case GSM_RESET: 
            if (m_gsm_fsm_counter == 0)
            {
                gsm_send_at_cmd(3000, 2, "OK", NULL, "AT+CFUN=0\r\n");
            }
            else if (m_gsm_fsm_counter == 5)
            {
                gsm_send_at_cmd(3000, 2, "OK", NULL, "AT+CFUN=1,1\r\n");
            }
            else if (m_gsm_fsm_counter > 20)
            {
                gsm_change_state(GSM_STATE_INIT);
                return;
            }
            m_gsm_fsm_counter++;
            break;
        default:
            break;
    }
}

void* gsm_thread(void* arg)
{
    // Auto detect serial port by send AT command
    char port[32] = "/dev/ttyUSB2";     // default port

    for (int i = 0; i < 3; i++)
    {
        thread_wdt_feed(WDT_THREAD_GSM);
        char tmp[32];
        sprintf(tmp, "/dev/ttyUSB%d", i);
        gsm_set_serial_port(tmp);

        // check command
        if (gsm_send_at_cmd(1000, 1, "OK\r\n", NULL, "AT\r\n"))
        {
            sprintf(port, "%s", tmp);
            DEBUG_VERBOSE("Found port %s\r\n", port);
            break;
        }
        else
        {
            MSLEEP(1000);
        }
    }
    DEBUG_INFO("Set serial port %s\r\n", port);
    gsm_set_serial_port(port);
    static int monitor_current_time = 0;

    while (1)
    {
        thread_wdt_feed(WDT_THREAD_GSM);
        gsm_state_machine_poll();
        if (monitor_current_time++ == 20)
        {
            monitor_current_time = 0;
            time_t rawtime;
            struct tm * timeinfo;

            time (&rawtime);
            timeinfo = localtime (&rawtime);
            if (timeinfo->tm_hour == 23 && timeinfo->tm_min == 59)
            {
                mqtt_publish_message("DBG", "Reset after 24h");
                do_reboot();
                MSLEEP(30000);
            }
        }
        MSLEEP(500);
    }
    pthread_exit(NULL);
}

void *wdt_thread(void* arg)
{
    MSLEEP(5000);
    while (1)
    {
        int value = 0;
        pthread_mutex_lock(&m_mutex_wdt);

        value = m_wdt_value;
        m_wdt_value = 0;

        pthread_mutex_unlock(&m_mutex_wdt);

        if (0 == (value & WDT_THREAD_ALL))
        {
            if (!(value & (1 << WDT_THREAD_GSM)))
            {
                DEBUG_WARN("GSM thread dead\r\n");
            }

            if (!(value & (1 << WDT_THREAD_MQTT)))
            {
                DEBUG_WARN("MQTT thread dead\r\n");
            }

            if (!(value & (1 << WDT_THREAD_TCP)))
            {
                DEBUG_WARN("TCP thread dead\r\n");
            }
            MSLEEP(1000);
            do_reboot();
        }
        else
        {
            DEBUG_VERBOSE("All task alive\r\n");
        }
        send_msg_to_watchdog_mcu("alive");
        MSLEEP(20000);
    }
    pthread_exit(NULL);
}

void *log_to_file_thread(void* arg)
{
    // Auto remove log file
    int auto_remove = 0;
    int sd_err = 0;

    // Create ringbuffer for log file
    if (m_log_to_file_buffer == NULL)
    {
        m_log_to_file_buffer = malloc(32768);
        lwrb_init(&m_ringbuffer_log_to_file, m_log_to_file_buffer, 32768);
        app_debug_register_callback_print(debug_cb_log_to_file);
    }

    while (m_log_to_file_buffer && m_device_config.log_to_file)
    {
        size_t rx_size;
        char tmp[512];
        
        // Read ringbuffer data
        rx_size = lwrb_read(&m_ringbuffer_log_to_file, tmp, 512);

        if (rx_size && sd_err == 0) 
        {
            time_t rawtime;
            struct tm * timeinfo;
            static char file_name[128];
            
            // format 2023_12_27.txt
            time(&rawtime);
            timeinfo = localtime(&rawtime);

            sprintf(file_name, "/mnt/mmcblk0p1/%04d_%02d_%02d_log.txt", 
                    timeinfo->tm_year+1900, timeinfo->tm_mon, timeinfo->tm_mday);

            FILE *f = fopen(file_name, "r");
            if (!f)
            {
                // File not exist, create log file
                printf("File %s not existed\r\n", file_name);

                f = fopen(file_name, "w");
                if (!f)
                {
                    perror("Create file failed :");
                    sd_err = 1;
                    // MSLEEP(100);
                }
            }
            else    // File existed
            {
                fclose(f);
                f = fopen(file_name, "a");
            }

            // Write log to file
            if (f)
            {
                if (fwrite(tmp, 1, rx_size, f) != rx_size)
                {
                    perror("Write to file failed\r\n");
                    sd_err = 1;
                }
                fclose(f);
            }
            else
            {
                perror("Log to file failed ");
                sd_err = 1;
            }
        }

        // Auto clean trash
        if (auto_remove++ % AUTO_REMOVE_LOG_INTERVAL == 0)
        {
            time_t rawtime;
            struct tm * timeinfo;
            char remove_file_name[128];
            memset(remove_file_name, 0, sizeof(remove_file_name));

            time (&rawtime);
            timeinfo = localtime(&rawtime);
            if (timeinfo->tm_mon == 0)
            {
                sprintf(remove_file_name, "/mnt/mmcblk0p1/%04d*_log.txt", 
                        (timeinfo->tm_year+1900-1));
            }
            else if (timeinfo->tm_mon < 12)
            {
                sprintf(remove_file_name, "/mnt/mmcblk0p1/%04d_%02d_*_log.txt", 
                                        timeinfo->tm_year+1900, (timeinfo->tm_mon-1));
            }

            run_shell_cmd(NULL, 0, true, "timeout 2 rm -rf %s", remove_file_name);
        }
        MSLEEP(LOG_TO_FILE_INTERVAL_MS);       // Tranh viec ghi lien tuc vao the nho
    }

    if (m_device_config.log_to_file == 0)
    {
        DEBUG_WARN("Delete all log file in sdcard\r\n");
        run_shell_cmd(NULL, 0, true, "timeout 20 rm -rf /mnt/mmcblk0p1/*_log.txt");
    }

    if (m_log_to_file_buffer)
    {
        free(m_log_to_file_buffer);
        m_log_to_file_buffer = NULL;
    }

    pthread_exit(NULL);
}

void *audio_ffmpeg_thread(void* arg)
{
    // Neu khong ngu 5s thi khi load icecast se thay load roi dung
    // Do con server icecast no the
    MSLEEP(5000);
    FILE *ffmpeg_sub = NULL, *aplay_sub = NULL;
    int timeout_close_process = 5;
    pid_t subpid;
    int status = -1;
    int len;
    static char buffer_ffmpeg[FFMPEG_BUFFER_SIZE];
    char *url = (char*)arg;
    static char cmd_buffer[1024];
    // struct pollfd pfd[1];
    fd_set rfds;
    struct timeval tv;
    int fd;

    int poll_timeout_counter = 0;

    sprintf(cmd_buffer, "ffmpeg -y -hide_banner -loglevel error -i %s -vn -f wav -", url);

    DEBUG_INFO("audio cmd : %s\r\n", cmd_buffer);
    reset_stream_monitor_data();
    // DEBUG_VERBOSE("Shell '%s'\r\n", body);

    ffmpeg_sub = popen(cmd_buffer, 
                "r");
    if (!ffmpeg_sub) 
    {
        /* popen() failed. */
        // DEBUG_WARN("Run shell cmd %s failed ", body);
        perror("Open ffmpeg failed :");
        goto end;
    }

    // pfd[0].fd = fileno(ffmpeg_sub);
    // pfd[0].events = POLLIN;
    fd = fileno(ffmpeg_sub);

    aplay_sub = popen("aplay -", 
                "wb");
    if (!aplay_sub) 
    {
        /* popen() failed. */
        // DEBUG_WARN("Run shell cmd %s failed ", body);
        perror("Open aplay failed :");
        goto end;
    }
    DEBUG_VERBOSE("Subprocess = %d\r\n", ffmpeg_sub);
    
    size_t stream_on = time(NULL);
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    
    size_t monitor_downloaded_kb = time(NULL);
    while (m_allow_audio_thread_run)
    {
        if (feof(ffmpeg_sub) || ferror(ffmpeg_sub))
        {
            DEBUG_INFO("Get PID for the command failed\r\n");
            break;
        }
        else
        {
            // int max_size = buffer_size-size-1;
            int poll_ret;
            // poll_ret = poll(pfd, 1, 200);
            poll_ret = select(fd+1, &rfds, NULL, NULL, &tv);
            if (poll_ret == 0)
            {
                DEBUG_WARN("poll timed out %d\r\n", poll_timeout_counter);
                poll_timeout_counter++;
            }
            else if (poll_ret < 0)
            {
                perror("poll");
            }
            else if (/*pfd[0].revents & POLLIN */FD_ISSET(fd, &rfds))
            {
                int nbytes = 0;
                ioctl(fd, FIONREAD, &nbytes);
                if (nbytes >= FFMPEG_BUFFER_SIZE)
                {
                    nbytes = FFMPEG_BUFFER_SIZE;
                }
                
                size_t rx = fread(buffer_ffmpeg, 1, nbytes, ffmpeg_sub);
                if (rx > 0)
                {
                    static int count = 0;
                    if (count++ > 150)
                    {
                        count = 0;
                        DEBUG_VERBOSE("Rx %u KB\r\n", m_stream_monitor.downloaded/1024);
                    }

                    monitor_downloaded_kb = time(NULL);

                    poll_timeout_counter = 0;
                    // Get streaming time
                    size_t now = time(NULL);
                    m_stream_monitor.stream_time = now - stream_on;
                    m_stream_monitor.state = STREAM_RUNNING;
                    m_stream_monitor.downloaded += rx;

                    fwrite(buffer_ffmpeg, 1, rx, aplay_sub);
                }

                if (time(NULL) - monitor_downloaded_kb >= 20)
                {
                   poll_timeout_counter = 21;
                }
            }

            if (poll_timeout_counter > 20)      //10s
            {
                poll_timeout_counter = 0;
                m_allow_audio_thread_run = false;
                mqtt_publish_message("DBG", "Stream bad");
            }
        }
    }
    fclose(ffmpeg_sub);
    fclose(aplay_sub);
    ffmpeg_sub = NULL;
    aplay_sub = NULL;

close:
    timeout_close_process = 5;
    errno = 0;
    DEBUG_INFO("Close ffmpeg process\r\n");
    if (ffmpeg_sub)
    {
        do 
        {
            status = pclose(ffmpeg_sub);
            DEBUG_INFO("Close ffmpeg return %d\r\n", status);
            MSLEEP(100);
            timeout_close_process--;
        } while (status == -1 && errno == EINTR && timeout_close_process > 0);
    }

    DEBUG_INFO("Close aplay process\r\n");
    timeout_close_process = 5;
    if (aplay_sub)
    {
        do 
        {
            status = pclose(aplay_sub);
            DEBUG_INFO("Close aplay_sub return %d\r\n", status);
            MSLEEP(100);
            timeout_close_process--;
        } while (status == -1 && errno == EINTR && timeout_close_process > 0);
    }


    if (status) 
    {
        /* Problem: sub exited with nonzero exit status 'status',
         * or if status == -1, some other error occurred. */
    } else 
    {
        /* Sub exited with success (zero exit status). */
    }
end:
    // if (body)
    // {
    //     free(body);
    // }
    DEBUG_WARN("Exit audio thread\r\n");
    reset_stream_monitor_data();

    p_audio_tid = 0;
    m_allow_audio_thread_run = true;
    m_last_streaming_master_level = -1;
    pthread_exit(NULL);
}

static void send_socket_data(void *ctx, uint8_t *data, uint8_t len)
{
    if (m_tcp_cli_fd == -1)
    {
        return;
    }
    ssize_t num_written = write(m_tcp_cli_fd, data, len);
    if (num_written == -1) 
    {
        DEBUG_WARN("Error in sending data to server\r\n");
        if (errno == EPIPE) 
        {
            DEBUG_WARN("Server has disconnected. Quitting\r\n");
            m_ipc_tcp_state = TCP_STATE_NEED_DESTROY;
        }
        DEBUG_INFO("Sock err %d\r\n", errno);
    }
}

void min_signal_cb(void *ctx, min_tx_signal_t signal)
{
    min_context_t *min = ctx;
    if (m_tcp_cli_fd == -1)
    {
        return;
    }

    switch (signal)
    {
    case MIN_TX_BEGIN:
        break;
    case MIN_TX_FULL:
    {
        send_socket_data(ctx, 
                        min->tx_frame_payload_buf, 
                        min->tx_frame_bytes_count);
    }
        break;
    case MIN_TX_END:
    {
        send_socket_data(ctx, 
                        min->tx_frame_payload_buf,
                        min->tx_frame_bytes_count);
    }
        break;
    default:
        break;
    }
}

static void on_host_frame_callback(void *ctx, min_msg_t *frame)
{
    // printf("Received min frame id %d, total packet received %d\r\n", frame->id, m_total_frame_received);
    DEBUG_INFO("Min rx [%u] bytes, id %d\r\n", 
                frame->len, frame->id);
    switch (frame->id)
    {
    case MIN_ID_PING:
        {
            rx_ping_msg_t *msg = (rx_ping_msg_t*)frame->payload;
            DEBUG_INFO("Stream : %d, in %u sec, download %u bytes\r\n", 
                    msg->stream_state,
                    msg->total_streaming_in_second,
                    msg->total_streaming_in_bytes);

            pthread_mutex_lock(&m_mutex_python);
            memcpy(&m_last_python_ping_msg, msg, sizeof(rx_ping_msg_t));
            pthread_mutex_unlock(&m_mutex_python);
        }
        break;
    
    default:
        break;
    }
}

int run_shell_cmd(char *buffer, int buffer_size, bool wait_to_complete, const char *fmt, ...)
{
    FILE *sub;
    pid_t subpid;
    int status = -1;
    int len;

    char *body = calloc(512, 1);
    if (!body)
    {
        DEBUG_INFO("[%s] No mem\r\n", __FUNCTION__);
        goto end;
    }

    va_list arg_ptr;

    va_start(arg_ptr, fmt);
    len = vsnprintf(body, 512, fmt, arg_ptr);
    va_end(arg_ptr);


    DEBUG_VERBOSE("Shell '%s'\r\n", body);

    sub = popen(body, "r");
    if (!sub) 
    {
        /* popen() failed. */
        DEBUG_WARN("Run shell cmd %s failed ", body);
        perror(":");
        goto end;
    }
    DEBUG_VERBOSE("Subprocess = %d\r\n", sub);

    /* Read the first line from sub. It contains the PID for the command. */
    int size = 0;
    if (buffer)
    {
        while (1)
        {
            if (feof(sub) || ferror(sub))
            {
                DEBUG_INFO("Get PID for the command failed\r\n");
                break;
            }
            else
            {
                int max_size = buffer_size-size-1;
                char *p = fgets(buffer+size, max_size, sub);
                if (p)
                {
                    DEBUG_INFO("%u bytes\r\n", strlen(p));
                    DEBUG_RAW(p);
                    size += strlen(p);
                }
                else
                {
                    break;
                }
            }
        }
    }

close:
    if (wait_to_complete)
    {
        char tmp[256];
        // DEBUG_RAW("------Subprocess output--------\r\n");
        while (fgets(tmp, 255, sub) != NULL)
        {
            // DEBUG_RAW("%s", tmp);
        }
        // DEBUG_RAW("-------End--------\r\n");
    }
    errno = 0;
    do 
    {
        status = pclose(sub);
        MSLEEP(100L);
    } while (status == -1 && errno == EINTR);

    if (status) 
    {
        /* Problem: sub exited with nonzero exit status 'status',
         * or if status == -1, some other error occurred. */
        DEBUG_WARN("Subprocess close failed %d\r\n", status);
    } 
end:
    if (body)
    {
        free(body);
    }
    return status;
}

void uqmi_setting_prefer_lte()
{
    run_shell_cmd(NULL, 0, true, "/usr/bin/timeout 5 uqmi --set-network-modes lte --device=/dev/cdc-wdm0");
}

void alsa_set_volume(int vol)
{
    if (vol > 100)
        vol = 100;
    if (vol < 0)
        vol = 0;
    run_shell_cmd(NULL, 0, true, "amixer set Headphone -M %d%%", vol);
}

char *cpu_get_load_avg()
{
    static char m_load_avg[128];
    memset(m_load_avg, 0, sizeof(m_load_avg));
    run_shell_cmd(m_load_avg, 128, false, "/usr/bin/timeout 2 cat /proc/loadavg");
    return m_load_avg;
}

char *sys_get_free_ram()
{
    static char m_free_ram[256];
    memset(m_free_ram, 0, sizeof(m_free_ram));
    run_shell_cmd(m_free_ram, sizeof(m_free_ram), NULL, "/usr/bin/timeout 2 free");
    char *p = strstr(m_free_ram, "\nSwap:");
    if (p)
    {
        *p = 0;
    }

    return m_free_ram;
}

void export_gpio()
{
    // Export GPIO
    // run_shell_cmd(NULL, 0, true, "echo 41 > /sys/class/gpio/export");
    // run_shell_cmd(NULL, 0, true, "echo 42 > /sys/class/gpio/export");
    // run_shell_cmd(NULL, 0, true, "echo 45 > /sys/class/gpio/export");
    // run_shell_cmd(NULL, 0, true, "echo 46 > /sys/class/gpio/export");

    // run_shell_cmd(NULL, 0, true, "echo out > /sys/class/gpio/gpio41/direction");
    // run_shell_cmd(NULL, 0, true, "echo out > /sys/class/gpio/gpio42/direction");
    // run_shell_cmd(NULL, 0, true, "echo out > /sys/class/gpio/gpio45/direction");
    // run_shell_cmd(NULL, 0, true, "echo out > /sys/class/gpio/gpio46/direction");
}

int main(int argc, char* argv[])
{
    pthread_mutex_init(&m_mutex_dbg, NULL);
    pthread_mutex_init(&m_mutex_mqtt, NULL);
    pthread_mutex_init(&m_mutex_python, NULL);
    pthread_mutex_init(&m_mutex_wdt, NULL);
    pthread_mutex_init(&m_mutex_log_file, NULL);
    
    // Init debug module
    app_debug_init(sys_get_ms, app_debug_lock);
    app_debug_register_callback_print(app_debug_output_cb);

    m_min_host_setting.get_ms = sys_get_ms;
    m_min_host_setting.last_rx_time = 0x00;
    m_min_host_setting.rx_callback = on_host_frame_callback;
    m_min_host_setting.invalid_crc_callback = NULL;
    m_min_host_setting.timeout_callback = NULL;
    m_min_host_setting.tx_byte = NULL;
    m_min_host_setting.use_timeout_method = 0;
    m_min_host_setting.use_dma_frame = true;
    m_min_host_setting.signal = min_signal_cb;
    m_min_host_setting.tx_frame = send_socket_data;

    m_min_host_context.callback = &m_min_host_setting;
    m_min_host_context.rx_frame_payload_buf = (uint8_t*)m_min_host_rx_buffer;
    m_min_host_context.tx_frame_payload_buf = (uint8_t*)m_dma_payload;
    m_min_host_context.tx_frame_payload_size = sizeof(m_dma_payload);
    min_init_context(&m_min_host_context);

    // Get mac address
    sys_get_mac_addr(m_mac_addr);

    m_device_config.log_level = DEBUG_LEVEL_ERROR;
    app_debug_set_level(m_device_config.log_level);
    // Load all configuration
    sys_load_configuration(m_mac_addr);
    
    // Get binary path
    readlink("/proc/self/exe", m_application_path, sizeof(m_application_path));

    if (argc == 1)
    {
        MSLEEP(15000);      // Cho OS on dinh
    }

    if (m_device_config.log_to_tty)
    {
        app_debug_register_callback_print(debug_to_tty);
    }

    app_debug_set_level(m_device_config.log_level);

    export_gpio();

    // LTE prefer
    uqmi_setting_prefer_lte();

    // Save new reset counter
    m_device_config.reset_counter++;
    DEBUG_INFO("Reset counter %d\r\n", m_device_config.reset_counter);
    sys_save_reset_counter(m_device_config.reset_counter);

    DEBUG_VERBOSE("\r\n%s\r\n", m_raw_sys_configuration);

    // Set default vol
    if (m_device_config.volume == 0)
    {
        audio_codec_mute(1);
    }
    alsa_set_volume(m_device_config.volume);

    // Create application thread
    pthread_create(&p_mqtt_tid, NULL, &mqtt_thread, NULL);
    // pthread_create(&p_socket_tid, NULL, &socket_thread, NULL);
    pthread_create(&p_gsm_tid, NULL, &gsm_thread, NULL);

    // Create wdt thread
    pthread_create(&p_wdt_tid, NULL, &wdt_thread, NULL);
    if (m_device_config.log_to_file)
    {
        pthread_create(&p_log_tid, NULL, &log_to_file_thread, NULL);
    }
    // pthread_create(&p_audio_tid, NULL, &audio_ffmpeg_thread, NULL);

    pthread_join(p_mqtt_tid, NULL);
    // pthread_join(p_socket_tid, NULL);
    pthread_join(p_gsm_tid, NULL);
    pthread_join(p_wdt_tid, NULL);

    while (1)
    {
        // gsm_get_imei();
        MSLEEP(3000);
    }
}
