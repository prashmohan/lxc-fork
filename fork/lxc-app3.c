/*
 * lxc test app 3:
 * open a file and write some information into it continuously
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <unistd.h>

int main()
{
        unsigned int time = 0;
        int len = 0;
        FILE *f = fopen("./pid.me", "w+");

        if (!f) {
                printf("open pid.me file failed, exit.\n");
                return -1;
        }

        while (1) {
                len = fprintf(f, "I'm process %d from process %d, i'm %d seconds old\n",
                              getpid(), getppid(), time * 10);
                if (!len) {
                        printf("Something wrong, write to file 0 bytes.\n");
                        return -2;
                } else {
                        printf("I'm process %d from process %d, i'm %d seconds old\n",
                               getpid(), getppid(), time * 10);
                        //fclose(f);
                        fflush(f);
                }
                time++;
                sleep(10);
        }
}
