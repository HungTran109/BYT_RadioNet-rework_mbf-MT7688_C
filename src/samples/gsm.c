#include <app_serial.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include "app_debug.h"
#include "gsm_utilities.h"
#include "gsm.h"

#define GSM_BAUD_RATE 115200
#define AT_CMD_BUFFER_SIZE 1024
#define AT_RX_BUFFER_SIZE 2048

static char m_gsm_imei[17] = "";
static char m_sim_imei[17];
static char m_gsm_port[64];
static char m_gsm_module_name[64];
static char m_ccid[21];
static char m_imsi[21];

uint64_t gsm_utilities_get_ms() 
{
  struct timespec spec;
  if (clock_gettime(1, &spec) == -1) 
  { /* 1 is CLOCK_MONOTONIC */
    abort();
  }
  return spec.tv_sec * 1000 + spec.tv_nsec / 1e6;
}

bool gsm_send_at_cmd(int timeout_ms, 
                    int retires, 
                    char *expected_response, 
                    char *payload_response, 
                    const char *format, ...)
{
    bool result = false;
    int len = 0;
    char *cmd = NULL;
    char *rx_buf = NULL;
    int rx_index = 0;
    int rc;
    int fd = -1;

    if (!expected_response)
    {
        goto exit;
    }
    cmd = malloc(AT_CMD_BUFFER_SIZE+1);
    rx_buf = malloc(AT_RX_BUFFER_SIZE+1);

    if (!cmd)
    {
        DEBUG_WARN("Can't allocate mem!\r\n");
        goto exit;
    }

    if (!rx_buf)
    {
        DEBUG_WARN("Can't allocate mem!\r\n");
        goto exit;
    }

    va_list arg_ptr;
    va_start(arg_ptr, format);
    len = vsnprintf(cmd, AT_CMD_BUFFER_SIZE, format, arg_ptr);
    va_end(arg_ptr);
    DEBUG_VERBOSE("ATC->\r\n%s", cmd);

    fd = app_serial_open(m_gsm_port, GSM_BAUD_RATE);
    if (-1 == fd)
    {
        DEBUG_WARN("Open serial port %s failed\r\n", m_gsm_port);
        goto exit;
    }

    while (retires >= 0 && result == false)
    {
        uint64_t start = gsm_utilities_get_ms();
        retires--;
        memset(rx_buf, 0, AT_RX_BUFFER_SIZE+1);
        rx_index = 0;

        int size = app_serial_write(fd, cmd, strlen(cmd));
        if (size <= 0)
        {
            DEBUG_WARN("Write to serial port failed\r\n");
            break;
        }

        while (1)
        {
            int max_read_size = AT_RX_BUFFER_SIZE - rx_index;
            size = app_serial_read(fd, rx_buf + rx_index, max_read_size);
            if (size > 0)
            {
                // DEBUG_RAW("%.*s", size, rx_buf + rx_index);
                rx_index += size;
                if (strstr(rx_buf, expected_response))
                {
                    result = true;
                    if (payload_response)
                    {
                        memcpy(payload_response, rx_buf, rx_index);
                        payload_response[rx_index] = 0;
                    }
                    uint64_t now = gsm_utilities_get_ms();
                    DEBUG_VERBOSE("AT in %ums\r\n", now - start);
                    break;
                }
                else if (strstr(rx_buf, "CME ERROR:"))
                {
                    DEBUG_WARN("%s", rx_buf);
                    break;
                }
            }
            else
            {
                usleep(10000);  // Sleep 10ms
            }

            uint64_t now = gsm_utilities_get_ms();
            if (now - start >= timeout_ms)
            {
                DEBUG_WARN("GSM AT timeout, retries %d\r\n", retires);
                break;
            }
        }
    }

exit:
    if (cmd)
        free(cmd);
    if (rx_buf)
        free(rx_buf);
    if (fd != -1)
        app_serial_close(fd);

    return result;
}

char *gsm_get_module_imei(void)
{
    char *payload = NULL;
    if (strlen(m_gsm_imei) > 10)
    {
        goto exit;
    }
    payload = malloc(AT_RX_BUFFER_SIZE+1);
    if (payload)
    {
        memset(payload, 0, AT_RX_BUFFER_SIZE+1);
        if (gsm_send_at_cmd(1000, 2, "86", payload, "AT+GSN\r\n"))
        {
            char *p = strstr(payload, "86");
            gsm_utilities_get_imei(p, m_gsm_imei, 17);
        }
    }
exit:
    if (payload)
        free(payload);
    return m_gsm_imei;
}

uint8_t gsm_get_csq(void)
{
    uint8_t csq = 0;
    char *payload = NULL;
    payload = calloc(AT_RX_BUFFER_SIZE + 1, 1);
    if (payload)
    {
        memset(payload, 0, AT_RX_BUFFER_SIZE+1);
        if (gsm_send_at_cmd(1000, 2, "OK\r\n", payload, "AT+CSQ\r\n"))
        {
            char *p = strstr(payload, "+CSQ");
            gsm_utilities_get_signal_strength_from_buffer(payload, &csq);
        }
    }
exit:
    if (payload)
    {
        free(payload);
    }
    return csq;
}

void gsm_set_serial_port(char *port)
{
    snprintf(m_gsm_port, sizeof(m_gsm_port), "%s", port);
}

void gsm_disable_echo()
{
    gsm_send_at_cmd(1000, 3, "OK\r\n", NULL, "ATE0\r\n");
}

char *gsm_get_module_name()
{
    char *payload = NULL;
    payload = malloc(AT_RX_BUFFER_SIZE+1);
    if (payload)
    {
        memset(payload, 0, (AT_RX_BUFFER_SIZE+1));
        if (gsm_send_at_cmd(1000, 3, "OK\r\n", payload, "AT+CGMM\r\n"))
        {
            char *p = strstr(payload, "OK");
            if (p)
            {
                memset(m_gsm_module_name, 0, sizeof(m_gsm_module_name));
                memcpy(m_gsm_module_name, payload+2, p-payload-4);    // 2 = strlen("\r\n")
                p = strstr(m_gsm_module_name, "\r\n");
                if (p) *p = 0;
            }
        }
        DEBUG_VERBOSE("GSM module %s\r\n", payload);
    }
exit:
    if (payload)
        free(payload);
    return m_gsm_module_name;
}


char *gsm_get_sim_imei_yoga()
{
    char *payload = NULL;
    if (strlen(m_ccid) < 10)
    {
        payload = malloc(AT_RX_BUFFER_SIZE+1);
        memset(payload, 0, (AT_RX_BUFFER_SIZE+1));
        if (payload)
        {
            if (gsm_send_at_cmd(1000, 1, "ICCID: ", payload, "AT+ICCID\r\n"))
            {
                char *p = strstr(payload, "ICCID: ");
                p += 7;
                char *q = strstr(p, "\r\n");
                if (q)
                {
                    memcpy(m_ccid, p, q-p);
                    m_ccid[19] = 0;
                }
            }
        }
    }
    if (payload)
        free(payload);
    return m_ccid;
}

char *gsm_get_sim_imei_ec2x()
{
    char *payload = NULL;
    if (strlen(m_ccid) < 10)
    {
        payload = malloc(AT_RX_BUFFER_SIZE+1);
        memset(payload, 0, (AT_RX_BUFFER_SIZE+1));
        if (payload)
        {
            if (gsm_send_at_cmd(2000, 1, "QCCID: ", payload, "AT+QCCID\r\n"))
            {
                char *p = strstr(payload, "QCCID: ");
                p += 7;
                char *q = strstr(p, "\r\n");
                if (q)
                {
                    memcpy(m_ccid, p, q-p);
                    m_ccid[20] = 0;
                }
            }
        }
    }
    if (payload)
        free(payload);
    return m_ccid;
}

char *gsm_get_sim_cimi()
{
    char *payload = NULL;
    if (strlen(m_imsi) < 10)
    {
        payload = malloc(AT_RX_BUFFER_SIZE+1);
        memset(payload, 0, (AT_RX_BUFFER_SIZE+1));
        if (payload)
        {
            if (gsm_send_at_cmd(2000, 2, "OK\r\n", payload, "AT+CIMI\r\n"))
            {
                char *q = strstr(payload, "OK");
                if (q)
                {
                    memcpy(m_imsi, payload+2, q - payload - 4);     // Remove \r\n at the end
                    q = strstr(m_imsi, "\r");
                    if (q) *q = 0;
                }
            }
        }
    }
    if (payload)
        free(payload);
    return m_imsi;
}

void gsm_get_network_operator(char *nw_operator, char *technology)
{
    char *payload = NULL;
    payload = malloc(AT_RX_BUFFER_SIZE+1);
    if (payload && gsm_send_at_cmd(5000, 2, "+COPS: ", payload, "AT+COPS?\r\n"))
    {
        char tmp[33];
        memset(tmp, 0, sizeof(tmp));
        DEBUG_INFO("%s", payload);
        gsm_utilities_get_network_operator(payload, tmp, 32);

        if (strstr("45201", tmp))
        {                
            sprintf(nw_operator, "%s", "MOBIFONE");
        }
        else if (strstr("45202" , tmp))
        {
            sprintf(nw_operator, "%s", "VINAPHONE");
        }
        else if (strstr("45204" , tmp))
        {
            sprintf(nw_operator, "%s", "VIETTEL");
        }
        else if (strstr( "45205" , tmp))
        {
            sprintf(nw_operator, "%s", "VNMOBILE");
        }
        else
        {
            sprintf(nw_operator, "%s", tmp);
        }
        char *p =  strstr(payload, tmp);
        if (p)
        {
            p += strlen(tmp);
            p = strstr(p, ",");
            if (p)
            {
                // 0 GSM
                // 2 UTRAN
                // 3 GSM W/EGPRS
                // 4 UTRAN W/HSDPA
                // 5 UTRAN W/HSUPA
                // 6 UTRAN W/HSDPA and HSUPA
                // 7 E-UTRAN
                // 8 UTRAN HSPA+
                p++;
                int access = *p - '0';
                switch (access)
                {
                case 0:
                case 1:
                    sprintf(technology, "%s", "GSM");
                    break;
                case 2:
                    sprintf(technology, "%s", "UTRAN");
                    break;
                case 3:
                    sprintf(technology, "%s", "GSM W/EGPRS");
                    break;
                case 4:
                case 5:
                    sprintf(technology, "%s", "3G");
                    break;
                case 6:
                case 7:
                case 8:
                    sprintf(technology, "%s", "4G");
                    break;
                default:
                    break;
                }
            }
        }
    }

    // (network_info, _) = gsm_send_at_cmd('AT+COPS?\r\n', '+COPS: ', 5)
    // sprintf(nw_operator, "%s", '
    // access_tech = ''

    // if '+COPS: ' in network_info and len(network_info) > 13:        # +COPS: 0\r\n
    //     # logger.info(network_info)
    //     network_info = network_info[(network_info.index('+COPS: ') + 7):len(network_info)]
    //     network_info = network_info[network_info.index('\"'):len(network_info)]
    //     # logger.info('NetworkInfo = ' + network_info)
    //     arr = network_info.split(',')
    //     # logger.info(arr)

    //     if len(arr) >= 2:
    //         sprintf(nw_operator, "%s", '
    //         arr[0] = arr[0].replace('\'', '').upper()
    //         arr[1] = arr[1].replace('\r\n', '').upper()
    //         if '45201' , tmp)
    //             sprintf(nw_operator, "%s", MOBIFONE'
    //         else if (strstr("45202" , tmp)
    //             sprintf(nw_operator, "%s", VINAPHONE'
    //         else if (strstr("45204" , tmp)
    //             sprintf(nw_operator, "%s", VIETTEL'
    //         else if (strstr( "45205" , tmp)
    //             sprintf(nw_operator, "%s", VNMOBILE'
    //         if 'MOBIFONE' , tmp)
    //             sprintf(nw_operator, "%s", MOBIFONE'
    //         else if (strstr("VINAPHONE" , tmp)
    //             sprintf(nw_operator, "%s", VINAPHONE'
    //         else if (strstr("VIETTEL" , tmp)
    //             sprintf(nw_operator, "%s", VIETTEL'
    //         else if (strstr( "VNMOBILE" , tmp)
    //             sprintf(nw_operator, "%s", VNMOBILE'

    //         tech = eval(arr[1])            
    //         if (tech == 0):
    //             access_tech = 'GSM'
    //         else if (strstr((tech == 1):
    //             access_tech = 'GSM compact'
    //         else if (strstr((tech == 2):
    //             access_tech = 'UTRAN'
    //         else if (strstr((tech == 3):
    //             access_tech = 'GSM w/EGPRS'
    //         else if (strstr((tech == 4):
    //             access_tech = 'UTRAN w/HSDPA'
    //         else if (strstr((tech == 5):
    //             access_tech = 'UTRAN w/HSUPA'
    //         else if (strstr((tech == 6):
    //             access_tech = 'UTRAN w/HSDPA and HSUPA'
    //         else if (strstr((tech == 7):
    //             access_tech = 'E-UTRAN'
    //         else if (strstr((tech == 8):
    //             access_tech = 'LTE-M'
    //         else if (strstr((tech == 9):
    //             access_tech = 'NB-IoT'
    //     else:
    //         logger.warning('Get COPS failed')
    //     logger.info('COPS = ' + cops)

    if (payload)
        free(payload);
}

void gsm_get_network_band_yoga_clm920(char *access_tech, char *band, char *channel)
{
    char *payload = NULL;
    payload = malloc(AT_RX_BUFFER_SIZE+1);
    if (payload && gsm_send_at_cmd(5000, 3, "BANDIND:", payload, "AT*BANDIND?\r\n"))
    {
        int access_int = 0, band_int = 0, channel_int = 0;
        if (3 == sscanf(payload,
                        "%*[^0123456789]%d%*[^0123456789]%d%*[^0123456789]%d",
                        &channel_int,
                        &band_int,
                        &access_int))
        {
            sprintf(band, "Band %d", band_int);
            sprintf(channel, "%d", channel_int);
            if (access_int == 0 || access_int == 1)
                sprintf(access_tech, "%s", "GSM");
            else if (access_int < 6)
                sprintf(access_tech, "%s", "3G");
            else
                sprintf(access_tech, "%s", "4G");
        }
    }

    if (payload)
        free(payload);
}

void gsm_get_network_band_ec2x(char *access_tech, char *band, char *channel)
{
    char *payload = NULL;
    payload = calloc(AT_RX_BUFFER_SIZE + 1, 1);
    if (payload && gsm_send_at_cmd(5000, 3, "QNWINFO: ", payload, "AT+QNWINFO\r\n"))
    {
        char *info = strstr(payload, "QNWINFO: ");
        if (info != NULL)
        {
            info += 10;
            char *q = strstr(info, ",");
            if (q)
            {
                snprintf(access_tech, q-info, "%s", info);
                info = q+1;

                // # Skip operator
                q = strstr(info, ",");
                info = q+2;


                // Band
                q = strstr(info, ",");
                snprintf(band, q-info, "%s", info);
                info = q+1;

                // Channel
                q = strstr(info, "\r\n");
                snprintf(channel, q-info + 1, "%s", info);
            }
        }
    }

    if (payload)
        free(payload);
}

bool gsm_is_sim_inserted(void)
{
    char *payload = NULL;
    bool sim_inserted = true;
    payload = malloc(AT_RX_BUFFER_SIZE+1);
    if (payload && gsm_send_at_cmd(2000, 0, "SIM not inserted", payload, "AT+CPIN?\r\n"))
    {
        sim_inserted = false;
    }

    if (payload)
        free(payload);
    
    return sim_inserted;
}
