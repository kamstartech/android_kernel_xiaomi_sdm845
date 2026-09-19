/*
 *  linux/fs/proc/root.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  proc root directory handling functions
 */

#include <asm/uaccess.h>

#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/user_namespace.h>
#include <linux/mount.h>
#include <linux/pid_namespace.h>
#include <linux/parser.h>
#include <linux/slab.h>

#include "internal.h"

enum {
	Opt_gid, Opt_hidepid, Opt_err,
};

static const match_table_t tokens = {
	{Opt_hidepid, "hidepid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_err, NULL},
};

int proc_parse_options(char *options, struct pid_namespace *pid)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;

	if (!options)
		return 1;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		args[0].to = args[0].from = NULL;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_gid:
			if (match_int(&args[0], &option))
				return 0;
			pid->pid_gid = make_kgid(current_user_ns(), option);
			break;
		case Opt_hidepid:
			if (match_int(&args[0], &option))
				return 0;
			if (option < 0 || option > 2) {
				pr_err("proc: hidepid value must be between 0 and 2.\n");
				return 0;
			}
			pid->hide_pid = option;
			break;
		default:
			pr_err("proc: unrecognized mount option \"%s\" "
			       "or missing value\n", p);
			return 0;
		}
	}

	return 1;
}

int proc_remount(struct super_block *sb, int *flags, char *data)
{
	struct pid_namespace *pid = sb->s_fs_info;

	sync_filesystem(sb);
	return !proc_parse_options(data, pid);
}

static struct dentry *proc_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	struct pid_namespace *ns;
	struct dentry *root;
	char *reparse_opts = NULL;

	if (flags & MS_KERNMOUNT) {
		ns = (struct pid_namespace *)data;
		data = NULL;
	} else {
		ns = task_active_pid_ns(current);

		if (data) {
			/*
			 * Kaos/HybridOS fix, 2026-09-15 (see the big comment
			 * below for why sget()-by-pointer was replaced by
			 * mount_ns()/sget_userns()): sget_userns() may hand
			 * back an *existing* procfs superblock for this pid
			 * namespace without invoking proc_fill_super() again,
			 * which is where hidepid=/gid= actually get parsed --
			 * so this mount's own options would otherwise be
			 * silently dropped whenever that happens. Keep an
			 * independent copy to re-apply them explicitly after
			 * mount_ns() returns, since proc_parse_options()
			 * destructively strsep()s its input in place and we
			 * don't know up front whether fill_super() will also
			 * consume the original "data" buffer.
			 */
			reparse_opts = kstrdup(data, GFP_KERNEL);
			if (!reparse_opts)
				return ERR_PTR(-ENOMEM);
		}
	}

	/*
	 * mount_ns() (fs/super.c) instead of a bare sget() keyed only by
	 * "sb->s_fs_info == this pid_namespace pointer": that bare-pointer
	 * match (still in kernel/pid_namespace.c's history as the pre-2016
	 * behaviour, and again as of the "Revert 'proc: Convert proc_mount
	 * to use mount_ns.'" Android common-kernel commit this replaces --
	 * Bug: 79705088) ignores *which user namespace* is doing the
	 * mounting. sget_userns(), which mount_ns() uses, additionally
	 * requires the match's owning user_ns to agree, and EBUSYs
	 * otherwise -- fatally, for anyone mounting via ns->user_ns instead
	 * of current_user_ns() at the plain sget() call site, since
	 * task_active_pid_ns(current)->user_ns is fixed at pid_ns creation
	 * while current_user_ns() tracks whatever the caller's *current*
	 * mount is running under.
	 *
	 * That fires every time a fresh PID namespace's first mount("proc")
	 * call comes from a *different* user namespace than the one already
	 * on file for it -- which is exactly what happens the moment a
	 * sandboxer (bubblewrap, and anything using the same
	 * unshare(CLONE_NEWUSER|CLONE_NEWPID) pattern -- Flatpak, Firejail,
	 * systemd's PrivateUsers=+ProtectProc=) creates a new PID namespace:
	 * the kernel's own bookkeeping mount from
	 * pid_ns_prepare_proc() (fs/proc/root.c, called by
	 * kernel/pid.c:alloc_pid() for every namespace's child-reaper pid,
	 * automatically, before any userspace mount() call happens) already
	 * registered a procfs superblock for that pid_namespace; the
	 * sandboxed process's own later, explicit mount("proc", ...) call --
	 * now running inside the brand-new user namespace it just
	 * unshared -- collides with it under the bare-sget() scheme and
	 * gets EBUSY, unconditionally, regardless of target path.
	 * mount_ns()/sget_userns() use ns->user_ns (fixed, and identical for
	 * both the kernel-internal and every later userspace mount of the
	 * same pid_namespace) instead, so the mismatch this depends on can't
	 * happen.
	 *
	 * Confirmed live on a Kaos/HybridOS Xiaomi Mi Mix 3 (perseus)
	 * device, 2026-09-15: `bwrap --unshare-pid --unshare-user --proc
	 * /proc ...` reliably hit this EBUSY (reproduced too with a minimal
	 * unshare(CLONE_NEWUSER|CLONE_NEWPID) + mount("proc",...) program,
	 * no bwrap involved) before this fix, and stopped after it.
	 */
	root = mount_ns(fs_type, flags, data, ns, ns->user_ns, proc_fill_super);

	if (!IS_ERR(root) && reparse_opts) {
		if (!proc_parse_options(reparse_opts, ns)) {
			/*
			 * mount_ns() returned with s_umount still held (its
			 * contract: the caller -- mount_fs(), fs/super.c --
			 * releases it, but only on the *success* path; once
			 * type->mount() returns an error, as we're about to,
			 * mount_fs() assumes we already released it
			 * ourselves). dput(root) alone (as this used to do)
			 * drops our dentry reference but leaves s_umount
			 * permanently held -- same pattern mount_fs()'s own
			 * out_sb: label uses for exactly this situation.
			 */
			struct super_block *sb = root->d_sb;
			dput(root);
			deactivate_locked_super(sb);
			root = ERR_PTR(-EINVAL);
		}
	}
	kfree(reparse_opts);

	return root;
}

static void proc_kill_sb(struct super_block *sb)
{
	struct pid_namespace *ns;

	ns = (struct pid_namespace *)sb->s_fs_info;
	if (ns->proc_self)
		dput(ns->proc_self);
	if (ns->proc_thread_self)
		dput(ns->proc_thread_self);
	kill_anon_super(sb);
	put_pid_ns(ns);
}

static struct file_system_type proc_fs_type = {
	.name		= "proc",
	.mount		= proc_mount,
	.kill_sb	= proc_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

void __init proc_root_init(void)
{
	int err;

	proc_init_inodecache();
	err = register_filesystem(&proc_fs_type);
	if (err)
		return;

	proc_self_init();
	proc_thread_self_init();
	proc_symlink("mounts", NULL, "self/mounts");

	proc_net_init();
	proc_uid_init();
#ifdef CONFIG_SYSVIPC
	proc_mkdir("sysvipc", NULL);
#endif
	proc_mkdir("fs", NULL);
	proc_mkdir("driver", NULL);
	proc_create_mount_point("fs/nfsd"); /* somewhere for the nfsd filesystem to be mounted */
#if defined(CONFIG_SUN_OPENPROMFS) || defined(CONFIG_SUN_OPENPROMFS_MODULE)
	/* just give it a mountpoint */
	proc_create_mount_point("openprom");
#endif
	proc_tty_init();
	proc_mkdir("bus", NULL);
	proc_sys_init();
}

static int proc_root_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat
)
{
	generic_fillattr(d_inode(dentry), stat);
	stat->nlink = proc_root.nlink + nr_processes();
	return 0;
}

static struct dentry *proc_root_lookup(struct inode * dir, struct dentry * dentry, unsigned int flags)
{
	if (!proc_pid_lookup(dir, dentry, flags))
		return NULL;
	
	return proc_lookup(dir, dentry, flags);
}

static int proc_root_readdir(struct file *file, struct dir_context *ctx)
{
	if (ctx->pos < FIRST_PROCESS_ENTRY) {
		int error = proc_readdir(file, ctx);
		if (unlikely(error <= 0))
			return error;
		ctx->pos = FIRST_PROCESS_ENTRY;
	}

	return proc_pid_readdir(file, ctx);
}

/*
 * The root /proc directory is special, as it has the
 * <pid> directories. Thus we don't use the generic
 * directory handling functions for that..
 */
static const struct file_operations proc_root_operations = {
	.read		 = generic_read_dir,
	.iterate_shared	 = proc_root_readdir,
	.llseek		= generic_file_llseek,
};

/*
 * proc root can do almost nothing..
 */
static const struct inode_operations proc_root_inode_operations = {
	.lookup		= proc_root_lookup,
	.getattr	= proc_root_getattr,
};

/*
 * This is the root "inode" in the /proc tree..
 */
struct proc_dir_entry proc_root = {
	.low_ino	= PROC_ROOT_INO, 
	.namelen	= 5, 
	.mode		= S_IFDIR | S_IRUGO | S_IXUGO, 
	.nlink		= 2, 
	.count		= ATOMIC_INIT(1),
	.proc_iops	= &proc_root_inode_operations, 
	.proc_fops	= &proc_root_operations,
	.parent		= &proc_root,
	.subdir		= RB_ROOT,
	.name		= "/proc",
};

int pid_ns_prepare_proc(struct pid_namespace *ns)
{
	struct vfsmount *mnt;

	mnt = kern_mount_data(&proc_fs_type, ns);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	ns->proc_mnt = mnt;
	return 0;
}

void pid_ns_release_proc(struct pid_namespace *ns)
{
	kern_unmount(ns->proc_mnt);
}
