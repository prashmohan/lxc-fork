/*
 *  linux/fs/file.c
 *
 *  Copyright (C) 1998-1999, Stephen Tweedie and Bill Hawes
 *
 *  Manage the dynamic fd arrays in the process files_struct.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>

struct fdtable_defer {
	spinlock_t lock;
	struct work_struct wq;
	struct fdtable *next;
};

int sysctl_nr_open __read_mostly = 1024*1024;
int sysctl_nr_open_min = BITS_PER_LONG;
int sysctl_nr_open_max = 1024 * 1024; /* raised later */

/*
 * We use this list to defer free fdtables that have vmalloced
 * sets/arrays. By keeping a per-cpu list, we avoid having to embed
 * the work_struct in fdtable itself which avoids a 64 byte (i386) increase in
 * this per-task structure.
 */
static DEFINE_PER_CPU(struct fdtable_defer, fdtable_defer_list);

static void *alloc_fdmem(unsigned int size)
{
	/*
	 * Very large allocations can stress page reclaim, so fall back to
	 * vmalloc() if the allocation size will be considered "large" by the VM.
	 */
	if (size <= (PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER)) {
		void *data = kmalloc(size, GFP_KERNEL|__GFP_NOWARN);
		if (data != NULL)
			return data;
	}
	return vmalloc(size);
}

static void free_fdmem(void *ptr)
{
	is_vmalloc_addr(ptr) ? vfree(ptr) : kfree(ptr);
}

static void __free_fdtable(struct fdtable *fdt)
{
	free_fdmem(fdt->fd);
	free_fdmem(fdt->open_fds);
	kfree(fdt);
}

static void free_fdtable_work(struct work_struct *work)
{
	struct fdtable_defer *f =
		container_of(work, struct fdtable_defer, wq);
	struct fdtable *fdt;

	spin_lock_bh(&f->lock);
	fdt = f->next;
	f->next = NULL;
	spin_unlock_bh(&f->lock);
	while(fdt) {
		struct fdtable *next = fdt->next;

		__free_fdtable(fdt);
		fdt = next;
	}
}

void free_fdtable_rcu(struct rcu_head *rcu)
{
	struct fdtable *fdt = container_of(rcu, struct fdtable, rcu);
	struct fdtable_defer *fddef;

	BUG_ON(!fdt);

	if (fdt->max_fds <= NR_OPEN_DEFAULT) {
		/*
		 * This fdtable is embedded in the files structure and that
		 * structure itself is getting destroyed.
		 */
		kmem_cache_free(files_cachep,
				container_of(fdt, struct files_struct, fdtab));
		return;
	}
	if (!is_vmalloc_addr(fdt->fd) && !is_vmalloc_addr(fdt->open_fds)) {
		kfree(fdt->fd);
		kfree(fdt->open_fds);
		kfree(fdt);
	} else {
		fddef = &get_cpu_var(fdtable_defer_list);
		spin_lock(&fddef->lock);
		fdt->next = fddef->next;
		fddef->next = fdt;
		/* vmallocs are handled from the workqueue context */
		schedule_work(&fddef->wq);
		spin_unlock(&fddef->lock);
		put_cpu_var(fdtable_defer_list);
	}
}

/*
 * Expand the fdset in the files_struct.  Called with the files spinlock
 * held for write.
 */
static void copy_fdtable(struct fdtable *nfdt, struct fdtable *ofdt)
{
	unsigned int cpy, set;

	BUG_ON(nfdt->max_fds < ofdt->max_fds);

	cpy = ofdt->max_fds * sizeof(struct file *);
	set = (nfdt->max_fds - ofdt->max_fds) * sizeof(struct file *);
	memcpy(nfdt->fd, ofdt->fd, cpy);
	memset((char *)(nfdt->fd) + cpy, 0, set);

	cpy = ofdt->max_fds / BITS_PER_BYTE;
	set = (nfdt->max_fds - ofdt->max_fds) / BITS_PER_BYTE;
	memcpy(nfdt->open_fds, ofdt->open_fds, cpy);
	memset((char *)(nfdt->open_fds) + cpy, 0, set);
	memcpy(nfdt->close_on_exec, ofdt->close_on_exec, cpy);
	memset((char *)(nfdt->close_on_exec) + cpy, 0, set);
}

static struct fdtable * alloc_fdtable(unsigned int nr)
{
	struct fdtable *fdt;
	char *data;

	/*
	 * Figure out how many fds we actually want to support in this fdtable.
	 * Allocation steps are keyed to the size of the fdarray, since it
	 * grows far faster than any of the other dynamic data. We try to fit
	 * the fdarray into comfortable page-tuned chunks: starting at 1024B
	 * and growing in powers of two from there on.
	 */
	nr /= (1024 / sizeof(struct file *));
	nr = roundup_pow_of_two(nr + 1);
	nr *= (1024 / sizeof(struct file *));
	/*
	 * Note that this can drive nr *below* what we had passed if sysctl_nr_open
	 * had been set lower between the check in expand_files() and here.  Deal
	 * with that in caller, it's cheaper that way.
	 *
	 * We make sure that nr remains a multiple of BITS_PER_LONG - otherwise
	 * bitmaps handling below becomes unpleasant, to put it mildly...
	 */
	if (unlikely(nr > sysctl_nr_open))
		nr = ((sysctl_nr_open - 1) | (BITS_PER_LONG - 1)) + 1;

	fdt = kmalloc(sizeof(struct fdtable), GFP_KERNEL);
	if (!fdt)
		goto out;
	fdt->max_fds = nr;
	data = alloc_fdmem(nr * sizeof(struct file *));
	if (!data)
		goto out_fdt;
	fdt->fd = (struct file **)data;
	data = alloc_fdmem(max_t(unsigned int,
				 2 * nr / BITS_PER_BYTE, L1_CACHE_BYTES));
	if (!data)
		goto out_arr;
	fdt->open_fds = (fd_set *)data;
	data += nr / BITS_PER_BYTE;
	fdt->close_on_exec = (fd_set *)data;
	fdt->next = NULL;

	return fdt;

out_arr:
	free_fdmem(fdt->fd);
out_fdt:
	kfree(fdt);
out:
	return NULL;
}

/*
 * Expand the file descriptor table.
 * This function will allocate a new fdtable and both fd array and fdset, of
 * the given size.
 * Return <0 error code on error; 1 on successful completion.
 * The files->file_lock should be held on entry, and will be held on exit.
 */
static int expand_fdtable(struct files_struct *files, int nr)
	__releases(files->file_lock)
	__acquires(files->file_lock)
{
	struct fdtable *new_fdt, *cur_fdt;

	spin_unlock(&files->file_lock);
	new_fdt = alloc_fdtable(nr);
	spin_lock(&files->file_lock);
	if (!new_fdt)
		return -ENOMEM;
	/*
	 * extremely unlikely race - sysctl_nr_open decreased between the check in
	 * caller and alloc_fdtable().  Cheaper to catch it here...
	 */
	if (unlikely(new_fdt->max_fds <= nr)) {
		__free_fdtable(new_fdt);
		return -EMFILE;
	}
	/*
	 * Check again since another task may have expanded the fd table while
	 * we dropped the lock
	 */
	cur_fdt = files_fdtable(files);
	if (nr >= cur_fdt->max_fds) {
		/* Continue as planned */
		copy_fdtable(new_fdt, cur_fdt);
		rcu_assign_pointer(files->fdt, new_fdt);
		if (cur_fdt->max_fds > NR_OPEN_DEFAULT)
			free_fdtable(cur_fdt);
	} else {
		/* Somebody else expanded, so undo our attempt */
		__free_fdtable(new_fdt);
	}
	return 1;
}

/*
 * Expand files.
 * This function will expand the file structures, if the requested size exceeds
 * the current capacity and there is room for expansion.
 * Return <0 error code on error; 0 when nothing done; 1 when files were
 * expanded and execution may have blocked.
 * The files->file_lock should be held on entry, and will be held on exit.
 */
int expand_files(struct files_struct *files, int nr)
{
	struct fdtable *fdt;

	fdt = files_fdtable(files);

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	if (nr >= rlimit(RLIMIT_NOFILE))
		return -EMFILE;

	/* Do we need to expand? */
	if (nr < fdt->max_fds)
		return 0;

	/* Can we expand? */
	if (nr >= sysctl_nr_open)
		return -EMFILE;

	/* All good, so we try */
	return expand_fdtable(files, nr);
}

static int count_open_files(struct fdtable *fdt)
{
	int size = fdt->max_fds;
	int i;

	/* Find the last open fd */
	for (i = size/(8*sizeof(long)); i > 0; ) {
		if (fdt->open_fds->fds_bits[--i])
			break;
	}
	i = (i+1) * 8 * sizeof(long);
	return i;
}

/*
 * cj-hack @ 20120423
 * flush regular file dirty data of task for lxc-fork
 */
void lxc_flush_taskfile(struct task_struct *tsk)
{
        //flush data and metadata
        int datasync = 0;
        int open_files, i;
        struct fdtable *fdt;
        struct file **fds;
        struct file *f;
        struct inode *inode = NULL;
        umode_t i_mode;

	fdt = files_fdtable(tsk->files);
	open_files = count_open_files(fdt);
        fds = fdt->fd;
        for (i = open_files; i != 0; i--) {
                f = *fds++;
                if (f) {
                        inode = f->f_path.dentry->d_inode;
                        i_mode = inode->i_mode;
                        if ((i_mode & S_IFMT) == S_IFREG) {
                                //sync this regular file
                                vfs_fsync(f, datasync);
                        }
                }
        }
}

/*
 * cj-hack @ April-23-2012
 * find the corresponding files in new task
 */
struct file *lxc_find_new_file(struct file *old_file, struct task_struct *tsk,
                               struct task_struct *old_tsk)
{
        int open_files, i;
        struct fdtable *ofdt, *nfdt;
        struct file **ofds, **nfds;
        struct file *of, *nf;

        ofdt = files_fdtable(old_tsk->files);
        nfdt = files_fdtable(tsk->files);
        ofds = ofdt->fd;
        nfds = nfdt->fd;
        open_files = count_open_files(ofdt);

        if (unlikely(open_files != count_open_files(nfdt))) {
                printk("file struct unmatch!\n");
                return NULL;
        }

        for (i = open_files; i != 0; i--) {
                of = *ofds++;
                nf = *nfds++;
                if (of == old_file) {
                        printk("find corresponding file fd=%d, nf=%p.\n", open_files - i, nf);
                        return nf;
                }
        }

        return NULL;
}

/*
 * cj-hack @ 20120328
 * Allocate a new files structure and copy contents from the
 * passed in files structure and deep copy all files contents
 * at the same time
 */
#include <linux/fs_struct.h>
extern int aa_get_name(struct path *path, int flags, char **buffer, const char **name);
extern void show_sock_info(void *);

struct files_struct *deep_dup_fd(struct files_struct *oldf, struct files_struct *parentf,
                                 int *errorp, struct task_struct *tsk, struct task_struct *otsk,
                                 struct lxc_fork_pipepair **ppair_head)
{
	struct files_struct *newf;
	struct file **old_fds, **new_fds, **parent_fds;
	int open_files, size, i;
	struct fdtable *old_fdt, *new_fdt, *parent_fdt;
        struct file *of, *nf = NULL, *pf;
        //int lookup;
        struct inode *inode = NULL;
        umode_t i_mode;
        struct path root;

	*errorp = -ENOMEM;
	newf = kmem_cache_alloc(files_cachep, GFP_KERNEL);
	if (!newf)
		goto out;

	atomic_set(&newf->count, 1);

	spin_lock_init(&newf->file_lock);
	newf->next_fd = 0;
	new_fdt = &newf->fdtab;
	new_fdt->max_fds = NR_OPEN_DEFAULT;
	new_fdt->close_on_exec = (fd_set *)&newf->close_on_exec_init;
	new_fdt->open_fds = (fd_set *)&newf->open_fds_init;
	new_fdt->fd = &newf->fd_array[0];
	new_fdt->next = NULL;

	spin_lock(&oldf->file_lock);
	old_fdt = files_fdtable(oldf);
        parent_fdt = files_fdtable(parentf);
	open_files = count_open_files(old_fdt);

	/*
	 * Check whether we need to allocate a larger fd array and fd set.
	 */
	while (unlikely(open_files > new_fdt->max_fds)) {
		spin_unlock(&oldf->file_lock);

		if (new_fdt != &newf->fdtab)
			__free_fdtable(new_fdt);

		new_fdt = alloc_fdtable(open_files - 1);
		if (!new_fdt) {
			*errorp = -ENOMEM;
			goto out_release;
		}

		/* beyond sysctl_nr_open; nothing to do */
		if (unlikely(new_fdt->max_fds < open_files)) {
			__free_fdtable(new_fdt);
			*errorp = -EMFILE;
			goto out_release;
		}

		/*
		 * Reacquire the oldf lock and a pointer to its fd table
		 * who knows it may have a new bigger fd table. We need
		 * the latest pointer.
		 */
		spin_lock(&oldf->file_lock);
		old_fdt = files_fdtable(oldf);
		open_files = count_open_files(old_fdt);
	}

	old_fds = old_fdt->fd;
	new_fds = new_fdt->fd;
        parent_fds = parent_fdt->fd;

	memcpy(new_fdt->open_fds->fds_bits,
               old_fdt->open_fds->fds_bits, open_files / 8);
	memcpy(new_fdt->close_on_exec->fds_bits,
               old_fdt->close_on_exec->fds_bits, open_files / 8);

        get_fs_root(otsk->fs, &root);
        //spin_lock(&(current->fs->lock));
        //root = current->fs->root;
        //path_get(&root);
        //spin_unlock(&(current->fs->lock));

        printk("Begin to copy open_files...\n");
        //open a new file and copy old content into it
        for (i = open_files; i != 0; i--) {
                printk("file[%d]  ", open_files - i);
                of = *old_fds++;
                pf = *parent_fds++;
                //clear new file pointer
                nf = NULL;
		if (of) {
                        if (open_files - i < 3 && pf) { //fd 0,1,2 are std in,out and error
                                get_file(pf);
                                rcu_assign_pointer(*new_fds++, pf);
                                continue;
                        }
                        //else : fd {0,1,2} are beed redirected
                        /*
                         * notice here:
                         * we reopen a file in a new contianer with a new fs
                         * which has a different vfsmount infor.
                         * Need to test it
                         */
                        /*
                         * read the inode->i_mode to find the file type
                         *      pipe, file, socket...
                         */
                        inode = of->f_path.dentry->d_inode;
                        //just get the i_mode
                        //no need the inode->i_op->getattr(**, **, stat)
                        i_mode = inode->i_mode;
                        switch (i_mode & S_IFMT) {
                        case S_IFREG: {//regular file
                                int buflen = 256;
                                char *name = NULL;
                                char buffer[256] = { '\0' };
                                char *test = NULL;
                                const char *aa_buf = NULL;
                                int aa_error;
                                test = d_absolute_path(&of->f_path, buffer, buflen);
                                if (!test || IS_ERR(test)) {
                                        printk("d_absolute_path get error: %p.\n", test);
                                } else
                                        printk("d_absolute_path get %s.\n", test);

                                aa_error = aa_get_name(&of->f_path, 0x0, &test, &aa_buf);
                                printk("aa_get_name error:%d, name=%s\n", aa_error, aa_buf);
                                //struct dentry *d_parent = of->f_path.dentry->d_parent;
                                name = __d_path(&of->f_path, &root,
                                                buffer, buflen);
                                if (!name || IS_ERR(name)) {
                                        printk("{D}find file error: %p.\n", name);
                                        break;
                                }
                                printk("[%d]filename:%s, flags:0%o, mode:0%o\n",
                                       open_files - i, name, of->f_flags, of->f_mode);
                                nf = filp_open(name, of->f_flags, of->f_mode);
                                if (IS_ERR(nf)) {
                                        printk("Open file failed: %p.\n", nf);
                                        break;
                                }
                                //copy file status into new file structure, handle vma later
                                nf->f_pos = of->f_pos;
                                break;
                        }
                        case S_IFIFO: {//pipe: we don't handle file vma here
                                struct lxc_fork_pipepair *ppair_cur = *ppair_head;
                                struct lxc_fork_pipepair *ppair_prev = NULL;
                                //first check if there's a corresponding pipe file
                                printk("[%d]pipefile : %d\n", open_files - i, of->f_mode);
                                while (ppair_cur) {
                                        if (of->f_mapping ==
                                            ppair_cur->ori_file->f_mapping) {
                                                nf = lxc_create_pipe(ppair_cur->lxc_forked_file, of->f_flags, of->f_mode);
                                                if (IS_ERR(nf)) {
                                                        printk("lxc create second pipe file"
                                                               "failed: %p\n", nf);
                                                        //nf = NULL;
                                                }
                                                if (ppair_prev) {
                                                        ppair_prev->next =
                                                                ppair_cur->next;
                                                } else {
                                                        *ppair_head = ppair_cur->next;
                                                }
                                                kfree(ppair_cur);
                                                break;
                                        } else {
                                                ppair_prev = ppair_cur;
                                                ppair_cur = ppair_cur->next;
                                        }
                                }
                                //it seems we need create the first file for this pipe
                                if (!nf) {
                                        struct lxc_fork_pipepair *new_ppair;
                                        nf = lxc_create_pipe(NULL, of->f_flags, of->f_mode);
                                        //nf = lxc_create_pipe();
                                        if (IS_ERR(nf)) {
                                                printk("{D} alloc first pipe file failed:%p\n",
                                                       nf);
                                                break;
                                        }
                                        new_ppair = kmalloc(sizeof(struct lxc_fork_pipepair),
                                                            GFP_KERNEL);
                                        if (!new_ppair) {
                                                printk("{D} malloc pipe pair failed: %p\n",
                                                       new_ppair);
                                                break;
                                        }
                                        new_ppair->ori_file = of;
                                        new_ppair->lxc_forked_file = nf;
                                        new_ppair->next = *ppair_head;
                                        *ppair_head = new_ppair;
                                }
                                break;
                        }
                        case S_IFSOCK: {//socket
                                printk("[%d]socket: ", open_files - i);
                                show_sock_info(of->private_data);
                                break;
                        }
                        default: //unknown type
                                printk("[%d] unknown file i_mode: 0%o[0x%x].\n",
                                       open_files - i, i_mode, i_mode);
                                break;
                        }
		} else {
			/*
			 * The fd may be claimed in the fd bitmap but not yet
			 * instantiated in the files array if a sibling thread
			 * is partway through open().  So make sure that this
			 * fd is available to the new process.
			 */
			FD_CLR(open_files - i, new_fdt->open_fds);
		}
		rcu_assign_pointer(*new_fds++, nf);
	}
	spin_unlock(&oldf->file_lock);

	/* compute the remainder to be cleared */
	size = (new_fdt->max_fds - open_files) * sizeof(struct file *);

	/* This is long word aligned thus could use a optimized version */
	memset(new_fds, 0, size);

	if (new_fdt->max_fds > open_files) {
		int left = (new_fdt->max_fds-open_files)/8;
		int start = open_files / (8 * sizeof(unsigned long));

		memset(&new_fdt->open_fds->fds_bits[start], 0, left);
		memset(&new_fdt->close_on_exec->fds_bits[start], 0, left);
	}

	rcu_assign_pointer(newf->fdt, new_fdt);

	return newf;

out_release:
	kmem_cache_free(files_cachep, newf);
out:
	return NULL;
}

/*
 * Allocate a new files structure and copy contents from the
 * passed in files structure.
 * errorp will be valid only when the returned files_struct is NULL.
 */
struct files_struct *dup_fd(struct files_struct *oldf, int *errorp)
{
	struct files_struct *newf;
	struct file **old_fds, **new_fds;
	int open_files, size, i;
	struct fdtable *old_fdt, *new_fdt;

	*errorp = -ENOMEM;
	newf = kmem_cache_alloc(files_cachep, GFP_KERNEL);
	if (!newf)
		goto out;

	atomic_set(&newf->count, 1);

	spin_lock_init(&newf->file_lock);
	newf->next_fd = 0;
	new_fdt = &newf->fdtab;
	new_fdt->max_fds = NR_OPEN_DEFAULT;
	new_fdt->close_on_exec = (fd_set *)&newf->close_on_exec_init;
	new_fdt->open_fds = (fd_set *)&newf->open_fds_init;
	new_fdt->fd = &newf->fd_array[0];
	new_fdt->next = NULL;

	spin_lock(&oldf->file_lock);
	old_fdt = files_fdtable(oldf);
	open_files = count_open_files(old_fdt);

	/*
	 * Check whether we need to allocate a larger fd array and fd set.
	 */
	while (unlikely(open_files > new_fdt->max_fds)) {
		spin_unlock(&oldf->file_lock);

		if (new_fdt != &newf->fdtab)
			__free_fdtable(new_fdt);

		new_fdt = alloc_fdtable(open_files - 1);
		if (!new_fdt) {
			*errorp = -ENOMEM;
			goto out_release;
		}

		/* beyond sysctl_nr_open; nothing to do */
		if (unlikely(new_fdt->max_fds < open_files)) {
			__free_fdtable(new_fdt);
			*errorp = -EMFILE;
			goto out_release;
		}

		/*
		 * Reacquire the oldf lock and a pointer to its fd table
		 * who knows it may have a new bigger fd table. We need
		 * the latest pointer.
		 */
		spin_lock(&oldf->file_lock);
		old_fdt = files_fdtable(oldf);
		open_files = count_open_files(old_fdt);
	}

	old_fds = old_fdt->fd;
	new_fds = new_fdt->fd;

	memcpy(new_fdt->open_fds->fds_bits,
		old_fdt->open_fds->fds_bits, open_files/8);
	memcpy(new_fdt->close_on_exec->fds_bits,
		old_fdt->close_on_exec->fds_bits, open_files/8);

	for (i = open_files; i != 0; i--) {
		struct file *f = *old_fds++;
		if (f) {
			get_file(f);
		} else {
			/*
			 * The fd may be claimed in the fd bitmap but not yet
			 * instantiated in the files array if a sibling thread
			 * is partway through open().  So make sure that this
			 * fd is available to the new process.
			 */
			FD_CLR(open_files - i, new_fdt->open_fds);
		}
		rcu_assign_pointer(*new_fds++, f);
	}
	spin_unlock(&oldf->file_lock);

	/* compute the remainder to be cleared */
	size = (new_fdt->max_fds - open_files) * sizeof(struct file *);

	/* This is long word aligned thus could use a optimized version */
	memset(new_fds, 0, size);

	if (new_fdt->max_fds > open_files) {
		int left = (new_fdt->max_fds-open_files)/8;
		int start = open_files / (8 * sizeof(unsigned long));

		memset(&new_fdt->open_fds->fds_bits[start], 0, left);
		memset(&new_fdt->close_on_exec->fds_bits[start], 0, left);
	}

	rcu_assign_pointer(newf->fdt, new_fdt);

	return newf;

out_release:
	kmem_cache_free(files_cachep, newf);
out:
	return NULL;
}

static void __devinit fdtable_defer_list_init(int cpu)
{
	struct fdtable_defer *fddef = &per_cpu(fdtable_defer_list, cpu);
	spin_lock_init(&fddef->lock);
	INIT_WORK(&fddef->wq, free_fdtable_work);
	fddef->next = NULL;
}

void __init files_defer_init(void)
{
	int i;
	for_each_possible_cpu(i)
		fdtable_defer_list_init(i);
	sysctl_nr_open_max = min((size_t)INT_MAX, ~(size_t)0/sizeof(void *)) &
			     -BITS_PER_LONG;
}

struct files_struct init_files = {
	.count		= ATOMIC_INIT(1),
	.fdt		= &init_files.fdtab,
	.fdtab		= {
		.max_fds	= NR_OPEN_DEFAULT,
		.fd		= &init_files.fd_array[0],
		.close_on_exec	= (fd_set *)&init_files.close_on_exec_init,
		.open_fds	= (fd_set *)&init_files.open_fds_init,
	},
	.file_lock	= __SPIN_LOCK_UNLOCKED(init_task.file_lock),
};

/*
 * allocate a file descriptor, mark it busy.
 */
int alloc_fd(unsigned start, unsigned flags)
{
	struct files_struct *files = current->files;
	unsigned int fd;
	int error;
	struct fdtable *fdt;

	spin_lock(&files->file_lock);
repeat:
	fdt = files_fdtable(files);
	fd = start;
	if (fd < files->next_fd)
		fd = files->next_fd;

	if (fd < fdt->max_fds)
		fd = find_next_zero_bit(fdt->open_fds->fds_bits,
					   fdt->max_fds, fd);

	error = expand_files(files, fd);
	if (error < 0)
		goto out;

	/*
	 * If we needed to expand the fs array we
	 * might have blocked - try again.
	 */
	if (error)
		goto repeat;

	if (start <= files->next_fd)
		files->next_fd = fd + 1;

	FD_SET(fd, fdt->open_fds);
	if (flags & O_CLOEXEC)
		FD_SET(fd, fdt->close_on_exec);
	else
		FD_CLR(fd, fdt->close_on_exec);
	error = fd;
#if 1
	/* Sanity check */
	if (rcu_dereference_raw(fdt->fd[fd]) != NULL) {
		printk(KERN_WARNING "alloc_fd: slot %d not NULL!\n", fd);
		rcu_assign_pointer(fdt->fd[fd], NULL);
	}
#endif

out:
	spin_unlock(&files->file_lock);
	return error;
}

int get_unused_fd(void)
{
	return alloc_fd(0, 0);
}
EXPORT_SYMBOL(get_unused_fd);
