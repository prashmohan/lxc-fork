/*
 * Simple application running in the container.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
        unsigned int slot = 0;
        //say something to prove i am live
        while (1) {
                printf("I am process %d from process %d, Now it's %d.\n",
                       getpid(), getppid(), slot);
                slot++;
                sleep(10);
        }
}
