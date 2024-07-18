#ifndef MIN_ID_H
#define MIN_ID_H

/*
    Put applation id here
    #define MIN_ID_GET_GPIO                     0x00
    #define MIN_ID_SET_GPIO                     0x01
    #define MIN_ID_UPDATE_ALL                   0x02
    #define MIN_ID_PING                         0x03
    #define MIN_ID_BUTTON_ISR                   0x04
    #define MIN_ID_SLAVE_RESET                  0x05
    #define MIN_ID_RESET						0x06
    #define MIN_ID_OTA_UPDATE_START             0x07
    #define MIN_ID_OTA_UPDATE_TRANSFER          0x08
    #define MIN_ID_OTA_UPDATE_END               0x09
    #define MIN_ID_OTA_ACK                      0x0A
    #define MIN_ID_OTA_FAILED                   0x0B
    #define MIN_ID_BUFFER_FULL                  0x0C
    #define MIN_ID_MASTER_OTA_BEGIN             0x0D
    #define MIN_ID_MASTER_OTA_END               0x0E
    #define MIN_ID_RS485_FORWARD                0x0F
    #define MIN_ID_RS232_FORWARD                0x10
*/
#define MIN_ID_PING                         0x00
#define MIN_ID_STREAM_REPORT                0x01
#define MIN_ID_START_STREAMMING             0x02
#define MIN_ID_STOP_STREAMING               0x03

#endif /* MIN_ID_H */
