/*
 * lxc test app4:
 * test parent-child/brother relationship between processes.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SLEEPTIME 20

/*
 *        root
 *    /         \
 * child1        child2
 *          /     \          \
 *      grandson1 grandson2 grandson3
 *                    |
 *             grand - grand son
 */

void multiprocesses() {
        int i;
        pid_t child;
        char space[16] = { '\0' };
        unsigned int slot = 0;

        if ((child = fork()) == 0) { //child
                space[0] = '-';
                goto loop;
        }

        if ((child = fork()) == 0) { //second child
                space[0] = '-';
                for (i = 0; i < 3; i++) {
                        if ((child = fork()) == 0) {
                                space[1] = '-';
                                if (i == 1)
                                        if (fork() == 0)
                                                space[2] = '-';
                                goto loop;
                        }
                }
        }

 loop:
        while (1) {
                printf("%sI am process %d from process %d, Now it's %d.\n",
                       space, getpid(), getppid(), slot);
                slot++;
                sleep(SLEEPTIME);
        }
}

/*
 * lxc fork mmap test function
 */
int lxc_mmap()
{
        int fd = -1;
        void *map = NULL;
        char *ptr;
        size_t length = 8192 * 5;
        off_t off = 0;
        size_t wsize;
        unsigned long slot = 0;
        mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
        //S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH | S_IWGRP | S_IWOTH;

        fd = open("./mmap.file", O_RDWR | O_CREAT, mode);
        if (fd == -1) {
                printf("open mmap file failed.\n");
                return -1;
        }

        //Make the file big enough
        lseek (fd, length + 1, SEEK_SET);
        write (fd, "*\n", 2);
        lseek (fd, 0, SEEK_SET);

        map = mmap(NULL, length, PROT_WRITE | PROT_READ, MAP_SHARED, fd, off);

        if (!map) {
                close(fd);
                printf("map file failed.\n");
                return -2;
        }
        ptr = (char *)map;
        while (1) {
                wsize = sprintf(ptr, "I am process %d from process %d, Now it's %ld\n",
                                getpid(), getppid(), slot);
                printf("---I am process %d from process %d, Now it's %ld\n",
                       getpid(), getppid(), slot);
                slot++;
                sleep(SLEEPTIME);
                ptr += wsize;
        }
        sync();
        munmap(map, length);
        return 0;
}

int lxc_mpmap()
{
        int i;
        pid_t child;
        char space[16] = { '\0' };
        unsigned int slot = 0;

        if ((child = fork()) == 0) { //child
                space[0] = '-';
                goto loop;
        }

        if ((child = fork()) == 0) { //second child
                space[0] = '-';
                for (i = 0; i < 3; i++) {
                        if ((child = fork()) == 0) {
                                space[1] = '-';
                                if (i == 1)
                                        if (fork() == 0) {
                                                //space[2] = '-';
                                                lxc_mmap();
                                                return 0;
                                        }
                                goto loop;
                        }
                }
        }

 loop:
        while (1) {
                printf("%sI am process %d from process %d, Now it's %d.\n",
                       space, getpid(), getppid(), slot);
                slot++;
                sleep(SLEEPTIME);
        }
}

int main(int argc, char *argv[]) {
        int type = 0;

        if (argc > 1) {
                type = atol(argv[1]);
        }

        switch (type) {
        case 0: //print
                break;
        case 1: //write file
                break;
        case 2: //multiple process
                multiprocesses();
                break;
        case 3: //mmap
                lxc_mmap();
                break;
        case 4: //mp + mmap
                lxc_mpmap();
                break;
        default:
                break;
        }
        return 0;
}
