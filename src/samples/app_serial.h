#ifndef APP_SERIAL_H
#define APP_SERIAL_H

#include <stdint.h>
int app_serial_open(char *name, uint32_t baudrate);
int app_serial_read(int fd, uint8_t *buffer, uint32_t size);
int app_serial_write(int fd, uint8_t *buffer, uint32_t size);
int app_serial_close(int fd);

#endif // APP_SERIAL_H