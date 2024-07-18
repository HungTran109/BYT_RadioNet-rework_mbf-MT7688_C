#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include <stddef.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>

void do_reboot()
{
    FILE *sub;
    pid_t subpid;
    int status = -1;
    sub = popen("reboot", "r");
    usleep(100000000);
    fclose(sub);
}

int main()
{    
    usleep(1000000);
    while (1)
    {
        int status;
        pid_t pid = fork();
        if (-1 == pid) 
        { 
            perror("fork failed!"); 
            do_reboot();
        }

        if (0 == pid) 
        {
            /* Child */
            execlp("/usr/bin/vs_speaker", NULL);
            perror("execlp failed!");
            do_reboot();
        }

        waitpid(pid, &status, 0);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) 
        {
            usleep(10000000);       // Delay 10s de co the nap chuong trinh
            do_reboot();
        }
        usleep(10000000);
    }
}