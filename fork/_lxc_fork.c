#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <lxc/lxc.h>
#include <lxc/log.h>
#include "lxc_fork.h"

/*
 * _lxc_fork: fork the process set of lxc container "orig"
 * orig: forked container name
 */
int main(int argc, char *argv[])
{
        char *orig;
        char buf[64] = { '\0' };
        pid_t init_pid;
        int fd, ret, len;
        int test = 0;
        char c;
        int i = 0;
        FILE *f;
        struct timeval time;
        unsigned long start, end; //milliseconds

        lf_debug("This is for debug lxc_start new container: %s.\n", argv[1]);

        //get initial pid of original contianer by get_init_pid
        orig = argv[1];
        //init_pid = get_init_pid(orig);
        /*
         * Since we can't use get_init_pid to read the initial process pid of
         * original contianer, read from file first :(
         */
        printf("Please input the initial pid of original container %s.\n", orig);
        if (test)
                f = fopen("./pid.txt", "r+");
        else
                f = fopen("/root/pid.txt", "r+");
        if (f == NULL) {
                lf_debug("Can't find pid file.\n");
                return -1;
        }
        /*
        while ((c = fgetc(f)) != ' ') {
                buf[i++] = c;
        }
        */
        if (fgets(buf, 64, f) == NULL) {
                lf_debug("Can't get initial pid, exit.\n");
                return 1;
        }
        fclose(f);

        init_pid = atol(buf);
        if (argc > 2) {
                init_pid = atol(argv[2]);
        }

        lf_debug("Get inital pid of original container %s : %d\n", orig, init_pid);

        if (init_pid < 0) {
                lf_debug("failed to get the initial pid of original container.\n");
                return -1;
        }
        //len = sprintf(buf, "echo \"fork %d\" > /proc/cgroups", init_pid);
        len = sprintf(buf, "fork %d", init_pid);
        lf_debug("echo cmd: %s\n", buf);
        //system(buf);
        //write "fork orig-container" to /proc/cgroups for call __lxc_fork in kernel
        ///*
        fd = open("/proc/cgroups", O_WRONLY);
        if (fd < 0) {
                lf_debug("failed to open cgroup for lxc-fork.\n");
                return -1;
        }
        //before we call lxc fork in kernel
        gettimeofday(&time, NULL);
        start = time.tv_sec * 1000 + time.tv_usec / 1000;

        ret = write(fd, buf, len + 1);

        //after lxc fork done
        gettimeofday(&time, NULL);
        end = time.tv_sec * 1000 + time.tv_usec / 1000;
        printf("The time of lxc fork without fs copy: %ld ms\n", end - start);

        close(fd);
        //*/
        return 0;
}
