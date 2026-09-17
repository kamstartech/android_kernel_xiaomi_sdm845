/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FS_CONTEXT_H
#define _LINUX_FS_CONTEXT_H

#include <linux/fs.h>
#include <linux/mount.h>
#include <uapi/linux/mount.h>

struct fs_context;

enum fs_context_state {
        FS_CONTEXT_CREATE_PARAMS = 0,   /* Still collecting parameters */
        FS_CONTEXT_AWAITING_CREATE,     /* Ready to create superblock */
        FS_CONTEXT_CREATING,            /* In the process of creating sb */
        FS_CONTEXT_FAILED,              /* Failed, requires reconfiguration */
        FS_CONTEXT_AWAITING_RECONF,     /* Ready to reconfigure */
        FS_CONTEXT_RECONFIRMING,        /* Reconfiguring */
        FS_CONTEXT_IS_ALIVE,            /* Superblock created and usable */
};

/*
 * Minimal fs_context used to backport the new mount API into older VFS trees.
 * This intentionally avoids the full upstream fs_context abstraction and builds
 * a legacy-style mount option string that existing filesystems already parse.
 */
struct fs_context {
        struct file_system_type *fs_type;
        enum fs_context_state    state;
        unsigned int             sb_flags;      /* MS_* flags */
        unsigned int             mnt_flags;     /* MNT_* flags */
        char                    *source;        /* source/device string */
        char                    *data;          /* fs-specific option string */
        struct vfsmount         *root_mnt;      /* mount created by CMD_CREATE */
};

extern struct fs_context *fs_context_new(struct file_system_type *fs_type,
                                         unsigned int flags);
extern void fs_context_free(struct fs_context *fc);

extern int fs_context_set_source(struct fs_context *fc, const char *source);
extern int fs_context_set_string(struct fs_context *fc, const char *key,
                                 const char *value);
extern int fs_context_set_flag(struct fs_context *fc, const char *key);

extern int fs_context_create(struct fs_context *fc);
extern int fs_context_reconfigure(struct fs_context *fc);

extern int fs_context_apply_mount_attrs(struct fs_context *fc,
                                        unsigned int attr_flags);

#endif /* _LINUX_FS_CONTEXT_H */
