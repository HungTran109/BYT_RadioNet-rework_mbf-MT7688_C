#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdint.h>

int app_serial_open(char *name, uint32_t baudrate)
{
    struct termios tio;       // struct for serial port
    int tty_fd;
    memset(&tio, 0, sizeof(tio));

    struct termios tty; 
    tty.c_cflag &= ~PARENB; 
    tty.c_cflag &= ~CSTOPB; 
    tty.c_cflag &= ~CSIZE; 
    tty.c_cflag |= CS8; 
    tio.c_cc[VMIN] = 1;  // timeout period it should be set to zero if you want to print 0 if nothing is received before timeout,
    tio.c_cc[VTIME] = 5; // time out period in units of 1/10th of a second, so here the period is 500ms

    tty_fd = open(name, O_RDWR | O_NONBLOCK | O_NOCTTY); // open the serial port as opening a file with Read and write attributes
    if (tty_fd != -1)
    {
        // printf("Serial port %s opened successfully\r\n", name);
    }
    else
    {
        printf("Serial Port %s is not available\r\nPossible reasons:-\r\n"
                "1.Port is not present\r\n2.Port is in use by some other app\r\n"
                "3.You may not have rights to access it\r\n", 
                name);
        close(tty_fd);
        return -1;
    }
    usleep(10000);
    tcflush(tty_fd, TCIOFLUSH);

    if (tcgetattr(tty_fd, &tty) != 0)  // apply settings to serial port 
    { 
        perror("tcgetattr"); 
        close(tty_fd);
        return -1; 
    } 

    uint32_t baud = B57600;
    switch (baudrate)
    {
    case 9600:
        baud = B9600;
        break;
    case 115200:
        baud = B115200;
        break;
    case 57600:
        baud = B57600;
        break;
    default:
        break;
    }
    cfsetospeed(&tio, baud); 
    cfsetispeed(&tio, baud); 

    tcsetattr(tty_fd, TCSANOW, &tio);
    return tty_fd;
}

int app_serial_read(int fd, uint8_t *buffer, uint32_t size)
{
    if (fd == -1)
    {
        printf("[%s] Invalid FD\r\n", __FUNCTION__);
        return -1;
    }

    return read(fd, buffer, size);
}

int app_serial_write(int fd, uint8_t *buffer, uint32_t size)
{
    if (fd == -1)
    {
        return -1;
    }

    return write(fd, buffer, size);
}

int app_serial_close(int fd)
{
    if (fd == -1)
    {
        return -1;
    }

    close(fd);                                 // close the serial port
    return 0;
}
