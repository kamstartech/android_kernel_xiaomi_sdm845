/*
 * Minimal fs_context backport for the new mount API on pre-fs_context VFS trees.
 *
 * This is intentionally not a full upstream fs_context implementation.  It builds
 * a legacy-style mount option string from fsconfig() calls and drives the
 * existing vfs_kern_mount()/do_remount_sb2() paths.  That is enough to satisfy
 * systemd's credential-setup path, which is the only known load-bearing user of
 * fsopen/fsconfig/fsmount/move_mount on this kernel.
 */

#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/capability.h>
#include <linux/mnt_namespace.h>
/*
 * <linux/mnt_namespace.h> only forward-declares struct mnt_namespace --
 * the real definition (needed below for current->nsproxy->mnt_ns->user_ns)
 * is kept private to fs/, in fs/mount.h. fs/namespace.c already pulls this
 * in transitively via "pnode.h"; do the same here rather than reaching for
 * fs/mount.h directly, to match the rest of this directory's convention.
 */
#include "pnode.h"
#include <linux/user_namespace.h>
#include <linux/seqlock.h>
#include <uapi/linux/mount.h>

/* Defined in fs/super.c but not exported/declared in public headers. */
extern int do_remount_sb2(struct vfsmount *mnt, struct super_block *sb,
                          int flags, void *data, int force);

static inline bool fc_may_mount(void)
{
        return ns_capable(current->nsproxy->mnt_ns->user_ns, CAP_SYS_ADMIN);
}

static int fc_append_option(struct fs_context *fc, const char *key,
                            const char *value)
{
        size_t oldlen = fc->data ? strlen(fc->data) : 0;
        size_t keylen = strlen(key);
        size_t vallen = value ? strlen(value) : 0;
        size_t add = keylen + (value ? 1 + vallen : 0) + (oldlen ? 1 : 0) + 1;
        char *new;

        new = krealloc(fc->data, oldlen + add, GFP_KERNEL);
        if (!new)
                return -ENOMEM;
        fc->data = new;

        if (oldlen)
                new[oldlen++] = ',';
        memcpy(new + oldlen, key, keylen);
        if (value) {
                new[oldlen + keylen] = '=';
                memcpy(new + oldlen + keylen + 1, value, vallen + 1);
        } else {
                new[oldlen + keylen] = '\0';
        }
        return 0;
}

/*
 * Recognise the fs-independent mount flags.  Anything unknown is rejected with
 * -EINVAL so that callers (systemd) can fall back to older mechanisms.
 */
static int fc_parse_flag(struct fs_context *fc, const char *key)
{
        if (!strcmp(key, "ro")) {
                fc->sb_flags |= MS_RDONLY;
                fc->mnt_flags |= MNT_READONLY;
        } else if (!strcmp(key, "rw")) {
                fc->sb_flags &= ~MS_RDONLY;
                fc->mnt_flags &= ~MNT_READONLY;
        } else if (!strcmp(key, "nosuid")) {
                fc->sb_flags |= MS_NOSUID;
                fc->mnt_flags |= MNT_NOSUID;
        } else if (!strcmp(key, "suid")) {
                fc->sb_flags &= ~MS_NOSUID;
                fc->mnt_flags &= ~MNT_NOSUID;
        } else if (!strcmp(key, "nodev")) {
                fc->sb_flags |= MS_NODEV;
                fc->mnt_flags |= MNT_NODEV;
        } else if (!strcmp(key, "dev")) {
                fc->sb_flags &= ~MS_NODEV;
                fc->mnt_flags &= ~MNT_NODEV;
        } else if (!strcmp(key, "noexec")) {
                fc->sb_flags |= MS_NOEXEC;
                fc->mnt_flags |= MNT_NOEXEC;
        } else if (!strcmp(key, "exec")) {
                fc->sb_flags &= ~MS_NOEXEC;
                fc->mnt_flags &= ~MNT_NOEXEC;
        } else if (!strcmp(key, "noatime")) {
                fc->sb_flags |= MS_NOATIME;
                fc->mnt_flags &= ~MNT_ATIME_MASK;
                fc->mnt_flags |= MNT_NOATIME;
        } else if (!strcmp(key, "atime")) {
                fc->sb_flags &= ~MS_NOATIME;
                fc->mnt_flags &= ~MNT_ATIME_MASK;
                fc->mnt_flags |= MNT_RELATIME;
        } else if (!strcmp(key, "nodiratime")) {
                fc->sb_flags |= MS_NODIRATIME;
                fc->mnt_flags |= MNT_NODIRATIME;
        } else if (!strcmp(key, "diratime")) {
                fc->sb_flags &= ~MS_NODIRATIME;
                fc->mnt_flags &= ~MNT_NODIRATIME;
        } else if (!strcmp(key, "relatime")) {
                fc->sb_flags |= MS_RELATIME;
                fc->mnt_flags &= ~MNT_ATIME_MASK;
                fc->mnt_flags |= MNT_RELATIME;
        } else if (!strcmp(key, "strictatime")) {
                fc->sb_flags |= MS_STRICTATIME;
                fc->mnt_flags &= ~MNT_ATIME_MASK;
        } else if (!strcmp(key, "nolazytime")) {
                fc->sb_flags &= ~MS_LAZYTIME;
        } else if (!strcmp(key, "lazytime")) {
                fc->sb_flags |= MS_LAZYTIME;
        } else if (!strcmp(key, "silent")) {
                fc->sb_flags |= MS_SILENT;
        } else if (!strcmp(key, "loud")) {
                fc->sb_flags &= ~MS_SILENT;
        } else if (!strcmp(key, "sync")) {
                fc->sb_flags |= MS_SYNCHRONOUS;
        } else if (!strcmp(key, "async")) {
                fc->sb_flags &= ~MS_SYNCHRONOUS;
        } else if (!strcmp(key, "mand")) {
                fc->sb_flags |= MS_MANDLOCK;
        } else if (!strcmp(key, "nomand")) {
                fc->sb_flags &= ~MS_MANDLOCK;
        } else if (!strcmp(key, "iversion")) {
                fc->sb_flags |= MS_I_VERSION;
        } else if (!strcmp(key, "noiversion")) {
                fc->sb_flags &= ~MS_I_VERSION;
        } else {
                return -EINVAL;
        }
        return 0;
}

static void fc_apply_mount_flags(struct vfsmount *mnt,
                                 unsigned int mnt_flags,
                                 unsigned int sb_flags)
{
        unsigned int old = mnt->mnt_flags;

        mnt_flags &= MNT_USER_SETTABLE_MASK;
        mnt->mnt_flags = (old & ~MNT_USER_SETTABLE_MASK) | mnt_flags;

        if (sb_flags & MS_RDONLY)
                mnt->mnt_flags |= MNT_READONLY;
        else
                mnt->mnt_flags &= ~MNT_READONLY;
}

static int fc_mount_attr_to_flags(unsigned int attr,
                                  unsigned int *mnt_flags,
                                  unsigned int *sb_flags)
{
        unsigned int atime;

        if (attr & MOUNT_ATTR_IDMAP)
                return -EINVAL;
        if (attr & MOUNT_ATTR_NOSYMFOLLOW)
                return -EINVAL;
        if (attr & ~(MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |
                     MOUNT_ATTR_NOEXEC | MOUNT_ATTR__ATIME |
                     MOUNT_ATTR_NODIRATIME))
                return -EINVAL;

        if (attr & MOUNT_ATTR_RDONLY)
                *sb_flags |= MS_RDONLY;
        if (attr & MOUNT_ATTR_NOSUID)
                *sb_flags |= MS_NOSUID;
        if (attr & MOUNT_ATTR_NODEV)
                *sb_flags |= MS_NODEV;
        if (attr & MOUNT_ATTR_NOEXEC)
                *sb_flags |= MS_NOEXEC;
        if (attr & MOUNT_ATTR_NODIRATIME)
                *sb_flags |= MS_NODIRATIME;

        if (attr & MOUNT_ATTR_RDONLY)
                *mnt_flags |= MNT_READONLY;
        if (attr & MOUNT_ATTR_NOSUID)
                *mnt_flags |= MNT_NOSUID;
        if (attr & MOUNT_ATTR_NODEV)
                *mnt_flags |= MNT_NODEV;
        if (attr & MOUNT_ATTR_NOEXEC)
                *mnt_flags |= MNT_NOEXEC;
        if (attr & MOUNT_ATTR_NODIRATIME)
                *mnt_flags |= MNT_NODIRATIME;

        atime = attr & MOUNT_ATTR__ATIME;
        if (atime) {
                *mnt_flags &= ~MNT_ATIME_MASK;
                switch (atime) {
                case MOUNT_ATTR_RELATIME:
                        *mnt_flags |= MNT_RELATIME;
                        break;
                case MOUNT_ATTR_NOATIME:
                        *mnt_flags |= MNT_NOATIME;
                        *sb_flags |= MS_NOATIME;
                        break;
                case MOUNT_ATTR_STRICTATIME:
                        *sb_flags |= MS_STRICTATIME;
                        break;
                default:
                        return -EINVAL;
                }
        }
        return 0;
}

struct fs_context *fs_context_new(struct file_system_type *fs_type,
                                  unsigned int flags)
{
        struct fs_context *fc;

        fc = kzalloc(sizeof(*fc), GFP_KERNEL);
        if (!fc)
                return ERR_PTR(-ENOMEM);

        fc->fs_type = fs_type;
        fc->state = FS_CONTEXT_CREATE_PARAMS;
        fc->sb_flags = 0;
        fc->mnt_flags = 0;
        return fc;
}

void fs_context_free(struct fs_context *fc)
{
        if (!fc)
                return;
        if (fc->root_mnt)
                mntput(fc->root_mnt);
        kfree(fc->data);
        kfree(fc->source);
        put_filesystem(fc->fs_type);
        kfree(fc);
}

int fs_context_set_source(struct fs_context *fc, const char *source)
{
        char *s;

        if (fc->state != FS_CONTEXT_CREATE_PARAMS &&
            fc->state != FS_CONTEXT_IS_ALIVE)
                return -EBUSY;

        s = kstrdup(source, GFP_KERNEL);
        if (!s)
                return -ENOMEM;
        kfree(fc->source);
        fc->source = s;
        return 0;
}

int fs_context_set_string(struct fs_context *fc, const char *key,
                          const char *value)
{
        if (fc->state != FS_CONTEXT_CREATE_PARAMS &&
            fc->state != FS_CONTEXT_IS_ALIVE)
                return -EBUSY;
        if (!key || !*key)
                return -EINVAL;
        if (!value)
                return -EINVAL;

        if (!strcmp(key, "source"))
                return fs_context_set_source(fc, value);

        return fc_append_option(fc, key, value);
}

int fs_context_set_flag(struct fs_context *fc, const char *key)
{
        if (fc->state != FS_CONTEXT_CREATE_PARAMS &&
            fc->state != FS_CONTEXT_IS_ALIVE)
                return -EBUSY;
        if (!key || !*key)
                return -EINVAL;

        return fc_parse_flag(fc, key);
}

int fs_context_create(struct fs_context *fc)
{
        struct vfsmount *mnt;
        const char *source;
        void *data;

        if (fc->state != FS_CONTEXT_CREATE_PARAMS)
                return -EBUSY;
        if (!fc_may_mount())
                return -EPERM;

        source = fc->source ? : fc->fs_type->name;
        data = (fc->data && fc->data[0]) ? fc->data : NULL;

        mnt = vfs_kern_mount(fc->fs_type, fc->sb_flags, source, data);
        if (IS_ERR(mnt))
                return PTR_ERR(mnt);

        fc->root_mnt = mnt;
        fc_apply_mount_flags(mnt, fc->mnt_flags, fc->sb_flags);
        if (!(mnt->mnt_flags & MNT_ATIME_MASK))
                mnt->mnt_flags |= MNT_RELATIME;
        fc->state = FS_CONTEXT_IS_ALIVE;
        return 0;
}

int fs_context_reconfigure(struct fs_context *fc)
{
        struct super_block *sb;
        void *data;
        int flags;
        int ret;

        if (fc->state != FS_CONTEXT_IS_ALIVE)
                return -EINVAL;
        if (!fc_may_mount())
                return -EPERM;
        if (!fc->root_mnt)
                return -EINVAL;

        sb = fc->root_mnt->mnt_sb;
        data = (fc->data && fc->data[0]) ? fc->data : NULL;
        flags = fc->sb_flags | MS_REMOUNT;

        ret = do_remount_sb2(fc->root_mnt, sb, flags, data, 0);
        if (ret)
                return ret;

        fc_apply_mount_flags(fc->root_mnt, fc->mnt_flags, fc->sb_flags);
        return 0;
}

int fs_context_apply_mount_attrs(struct fs_context *fc,
                                 unsigned int attr_flags)
{
        unsigned int mnt_flags = fc->mnt_flags;
        unsigned int sb_flags = fc->sb_flags;
        int ret;

        if (fc->state != FS_CONTEXT_IS_ALIVE)
                return -EINVAL;
        if (!fc->root_mnt)
                return -EINVAL;

        ret = fc_mount_attr_to_flags(attr_flags, &mnt_flags, &sb_flags);
        if (ret)
                return ret;

        fc->mnt_flags = mnt_flags;
        fc->sb_flags = sb_flags;

        if (sb_flags & MS_RDONLY)
                fc->root_mnt->mnt_sb->s_flags |= MS_RDONLY;
        else
                fc->root_mnt->mnt_sb->s_flags &= ~MS_RDONLY;

        fc_apply_mount_flags(fc->root_mnt, mnt_flags, sb_flags);
        return 0;
}
