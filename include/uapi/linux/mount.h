/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MOUNT_H
#define _UAPI_LINUX_MOUNT_H

#include <linux/types.h>

/*
 * New mount API file descriptor setup flags.
 */
#define FSOPEN_CLOEXEC          0x00000001
#define FSMOUNT_CLOEXEC         0x00000001
#define FSPICK_CLOEXEC          0x00000001
#define FSPICK_SYNTAX_BITS      0x00000006
#define FSPICK_NO_AUTOMOUNT     0x00000000 /* tri-state flag */
#define FSPICK_EMPTY_PATH       0x00000001 /* bit 0 of syntax */

/*
 * fsconfig() command IDs.
 */
enum fsconfig_command {
        FSCONFIG_SET_FLAG       = 0,    /* Set parameter, supplying no value */
        FSCONFIG_SET_STRING     = 1,    /* Set parameter, supplying a string value */
        FSCONFIG_SET_BINARY     = 2,    /* Set parameter, supplying a binary blob value */
        FSCONFIG_SET_PATH       = 3,    /* Set parameter, supplying an object path */
        FSCONFIG_SET_PATH_EMPTY = 4,    /* Set parameter, supplying an object path */
        FSCONFIG_SET_FD         = 5,    /* Set parameter, supplying a file descriptor */
        FSCONFIG_CMD_CREATE     = 6,    /* Invoke superblock creation */
        FSCONFIG_CMD_RECONFIGURE = 7,   /* Invoke superblock reconfiguration */
        FSCONFIG_CMD_CREATE_EXCL = 8,   /* Create new superblock, fail if existing */
};

/*
 * open_tree() flags.
 */
#define OPEN_TREE_CLONE         0x01    /* Clone the target tree */
#define OPEN_TREE_CLOEXEC       0x02    /* Close the file descriptor on exec */

/*
 * move_mount() flags.
 */
#define MOVE_MOUNT_F_SYMLINKS   0x01    /* Follow symlinks on from path */
#define MOVE_MOUNT_F_AUTOMOUNTS 0x02    /* Follow automounts on from path */
#define MOVE_MOUNT_F_EMPTY_PATH 0x04    /* Empty from path allowed */
#define MOVE_MOUNT_T_SYMLINKS   0x10    /* Follow symlinks on to path */
#define MOVE_MOUNT_T_AUTOMOUNTS 0x20    /* Follow automounts on to path */
#define MOVE_MOUNT_T_EMPTY_PATH 0x40    /* Empty to path allowed */
#define MOVE_MOUNT_BENEATH      0x200   /* Move beneath another mount */

/*
 * Mount attributes (fsmount() attrs parameter).
 */
#define MOUNT_ATTR_RDONLY       0x00000001
#define MOUNT_ATTR_NOSUID       0x00000002
#define MOUNT_ATTR_NODEV        0x00000004
#define MOUNT_ATTR_NOEXEC       0x00000008
#define MOUNT_ATTR__ATIME       0x00000070
#define MOUNT_ATTR_RELATIME     0x00000000
#define MOUNT_ATTR_NOATIME      0x00000010
#define MOUNT_ATTR_STRICTATIME  0x00000020
#define MOUNT_ATTR_NODIRATIME   0x00000080
#define MOUNT_ATTR_IDMAP        0x00100000
#define MOUNT_ATTR_NOSYMFOLLOW  0x00200000

/*
 * mount_setattr()
 */
struct mount_attr {
        __u64 attr_set;
        __u64 attr_clr;
        __u64 propagation;
        __u64 userns_fd;
};

/* List of all mount_attr versions. */
#define MOUNT_ATTR_SIZE_VER0    32 /* sizeof first published struct */

#endif /* _UAPI_LINUX_MOUNT_H */
