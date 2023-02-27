/* C++ Program to demonstrate use of left shift
 * operator */
#include<stdio.h>
#include <stdint.h>
#define _XOPEN_SOURCE_EXTENDED 1
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h> 
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
char SCOPE_FILE_FIFO[]="test";
int main(void)
{
        pid_t pid, sid;
            int fd;
                int ret = 0;

                    char buff[16] = {0};
                        int errno;


                            openlog("ScopeServer", LOG_PID | LOG_LOCAL0,  LOG_LOCAL0);

                                pid = fork();
                                    if(pid < 0) {
                                                syslog(LOG_ERR, "Fork failed, daemon couldn't start! (errno = %d)\n", -errno);
                                                        return EXIT_FAILURE;
                                                            } else if(pid > 0){
                                                                        return EXIT_SUCCESS;
                                                                            }

                                                                                openlog("ScopeServer", LOG_PID | LOG_LOCAL0,  LOG_LOCAL0);

                                                                                    umask(0);

                                                                                        syslog(LOG_INFO, "[ScopeServer]: Daemon started!\n");

                                                                                            sid = setsid();
                                                                                                if(sid < 0) {
                                                                                                            syslog(LOG_ERR, "Setsid failed, couldn't create new sid! (errno = %d)\n", -errno);
                                                                                                                    return EXIT_FAILURE;
                                                                                                                        }

                                                                                                                            sprintf(buff, "%ld", (long)getpid());
                                                                                                                                fd = open("pid", O_CREAT | O_WRONLY);
                                                                                                                                    if(fd <0) {
                                                                                                                                                syslog(LOG_ERR, "Couldn't create pid file! (errno = %d)\n", -errno);
                                                                                                                                                        ret = EXIT_FAILURE;
                                                                                                                                                                goto init_fail;
                                                                                                                                                                    }

                                                                                                                                                                        if(write(fd, buff, strlen(buff)+1) < 0) {
                                                                                                                                                                                    syslog(LOG_ERR, "Couldn't save pid file for ScopeServer! (errno = %d)\n", -errno);
                                                                                                                                                                                            close(fd);
                                                                                                                                                                                                    ret = EXIT_FAILURE;
                                                                                                                                                                                                            goto init_fail;
                                                                                                                                                                                                                }
                                                                                                                                                                                                                    close(fd);

                                                                                                                                                                                                                        if(mkfifo("fifo", O_RDWR) < 0) {
                                                                                                                                                                                                                                    syslog(LOG_ERR, "Couldn't create fifo file: %s! (errno = %d)\n", SCOPE_FILE_FIFO, -errno);
                                                                                                                                                                                                                                            ret = EXIT_FAILURE;
                                                                                                                                                                                                                                                    goto init_fail;
                                                                                                                                                                                                                                                        }

                                                                                                                                                                                                                                                            if(chdir("/") < 0) {
                                                                                                                                                                                                                                                                        syslog(LOG_ERR, "Chdir failed! (errno = %d)\n", -errno);
                                                                                                                                                                                                                                                                                ret = EXIT_FAILURE;
                                                                                                                                                                                                                                                                                        goto init_fail;
                                                                                                                                                                                                                                                                                            }

                                                                                                                                                                                                                                                                                                close(STDIN_FILENO);
                                                                                                                                                                                                                                                                                                    close(STDOUT_FILENO);
                                                                                                                                                                                                                                                                                                        close(STDERR_FILENO);



                                                                                                                                                                                                                                                                                                        init_fail:
                                                                                                                                                                                                                                                                                                            unlink("fifo");
                                                                                                                                                                                                                                                                                                                unlink("pid");
                                                                                                                                                                                                                                                                                                                    closelog();

                                                                                                                                                                                                                                                                                                                        return ret;
}

