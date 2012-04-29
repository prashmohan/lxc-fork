#ifndef __LXC_FORK_H__
#define __LXC_FORK_H__

#define LF_DEBUG

#ifdef LF_DEBUG
#define lf_debug(fmt, args...) printf("%s%s:%d "fmt, "[D]", __FILE__, __LINE__, ##args)
#else
#define lf_debug(fmt, args...)
#endif

#define LXC_PATH "/var/lib/lxc"

#endif /* __LXC_FORK_H__ */
