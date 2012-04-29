/*
 * lxc: linux Container library
 * lxc_fork: fork a exsiting container by COW
 * First line of code added at 20120320 by Jin Chen
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "lxc_fork.h"
#include "log.h"

int main(int argc, char *argv[])
{
        int opt, ret;
        char *orig, *new, *pidbuf;
        char clonecmd[256] = { 0 };
        char startcmd[256] = { 0 };
        char cpcmd[256] = { 0 };
        char freezecmd[256] = { 0 };
        char *rcfile = NULL;
        struct lxc_conf *conf = NULL;

        //parse the arguments
        while ((opt = getopt(argc, argv, "o:n:p:")) != -1) {
                switch (opt) {
                case 'o':
                        //FIXME: we need to do string boundary check to avoid buffer overflow !
                        orig = optarg;
                        break;
                case 'n':
                        new = optarg;
                        break;
                case 'p':
                        pidbuf = optarg;
                        break;
                default: /* ? */
                        printf("unrecognized argu: %s.\n", optarg);
                        break;
                }
        }
        lf_debug("Parse argument, old container name: %s, new name: %s, pid: %s.\n",
                 orig, new, pidbuf);

        //freeze the original container first,
        // maybe we need to flush in-memory dirty data to disk before lxc-clone
        sprintf(freezecmd, "lxc-freeze -n %s", orig);
        lf_debug("freeze original container: %s.\n", freezecmd);
        ret = system(freezecmd);

        //copy lxc container config by lxc-clone
        sprintf(clonecmd, "lxc-clone -o %s -n %s", orig, new);
        lf_debug("clone command: %s.\n", clonecmd);

        ret = system(clonecmd);
        if (ret == -1) {
                printf("lxc clone error: %d, exit now.\n", ret);
                return ret;
        }

        //check if the new lxc files exist, config, fstab, file system
        ret = asprintf(&rcfile, LXC_PATH "/%s/config", new);
        if (ret == -1) {
                lf_debug("failed to allocate memory");
                return ret;
        }

        if (access(rcfile, F_OK)) {
                free(rcfile);
                rcfile = NULL;
                lf_debug("failed to find the config file %s", rcfile);
                return -1;
        }

        sprintf(cpcmd, "cp ./_lxc_fork ./pid.txt /var/lib/lxc/%s/rootfs/root/", new);
        //copy binary to root directory at new container
        system(cpcmd);

        //initial new lxc contianer
        sprintf(startcmd, "lxc-start -n %s /root/_lxc_fork %s %s", new, orig, pidbuf);
        ret = system(startcmd);

        //unfreeze original contianer
        sprintf(freezecmd, "lxc-unfreeze -n %s", orig);
        ret = system(freezecmd);
        return 0;
}
