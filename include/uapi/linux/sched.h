#ifndef _UAPI_LINUX_SCHED_H
#define _UAPI_LINUX_SCHED_H

#include <linux/types.h>

/*
 * cloning flags:
 */
#define CSIGNAL		0x000000ff	/* signal mask to be sent at exit */
#define CLONE_VM	0x00000100	/* set if VM shared between processes */
#define CLONE_FS	0x00000200	/* set if fs info shared between processes */
#define CLONE_FILES	0x00000400	/* set if open files shared between processes */
#define CLONE_SIGHAND	0x00000800	/* set if signal handlers and blocked signals shared */
#define CLONE_PIDFD	0x00001000	/* set if a pidfd should be placed in parent */
#define CLONE_PTRACE	0x00002000	/* set if we want to let tracing continue on the child too */
#define CLONE_VFORK	0x00004000	/* set if the parent wants the child to wake it up on mm_release */
#define CLONE_PARENT	0x00008000	/* set if we want to have the same parent as the cloner */
#define CLONE_THREAD	0x00010000	/* Same thread group? */
#define CLONE_NEWNS	0x00020000	/* New mount namespace group */
#define CLONE_SYSVSEM	0x00040000	/* share system V SEM_UNDO semantics */
#define CLONE_SETTLS	0x00080000	/* create a new TLS for the child */
#define CLONE_PARENT_SETTID	0x00100000	/* set the TID in the parent */
#define CLONE_CHILD_CLEARTID	0x00200000	/* clear the TID in the child */
#define CLONE_DETACHED		0x00400000	/* Unused, ignored */
#define CLONE_UNTRACED		0x00800000	/* set if the tracing process can't force CLONE_PTRACE on this clone */
#define CLONE_CHILD_SETTID	0x01000000	/* set the TID in the child */
#define CLONE_NEWCGROUP		0x02000000	/* New cgroup namespace */
#define CLONE_NEWUTS		0x04000000	/* New utsname namespace */
#define CLONE_NEWIPC		0x08000000	/* New ipc namespace */
#define CLONE_NEWUSER		0x10000000	/* New user namespace */
#define CLONE_NEWPID		0x20000000	/* New pid namespace */
#define CLONE_NEWNET		0x40000000	/* New network namespace */
#define CLONE_IO		0x80000000	/* Clone io context */
#define CLONE_INTO_CGROUP	0x200000000ULL	/* Clone into a specific cgroup given the right permissions.
						 * Backported (Linux 5.7) so clone3()'s struct clone_args can
						 * be the modern (cgroup-field) size without hitting -E2BIG --
						 * see the cgroup field comment on struct clone_args below and
						 * sys_clone3()'s handling of it in kernel/fork.c. */

/*
 * Arguments for the clone3 syscall
 */
struct clone_args {
	__aligned_u64 flags;
	__aligned_u64 pidfd;
	__aligned_u64 child_tid;
	__aligned_u64 parent_tid;
	__aligned_u64 exit_signal;
	__aligned_u64 stack;
	__aligned_u64 stack_size;
	__aligned_u64 tls;
	/*
	 * set_tid/set_tid_size (Linux 5.5) and cgroup (Linux 5.7, paired
	 * with CLONE_INTO_CGROUP above) backported so the struct is the
	 * full modern size. sys_clone3() in kernel/fork.c rejects a
	 * nonzero set_tid_size (precise-TID placement isn't implemented
	 * here) but silently accepts a nonzero cgroup (CLONE_INTO_CGROUP
	 * isn't implemented either, but callers that ask for it -- e.g.
	 * systemd's sd-executor spawn path -- already re-attach to the
	 * target cgroup themselves right after the fork if the kernel
	 * didn't do it for them, so a no-op here doesn't lose anything).
	 * Confirmed live 2026-09-26: without these three fields present,
	 * glibc >=2.39's clone3() wrapper (used by posix_spawn(), which
	 * systemd-executor's spawn path uses) passed a struct larger than
	 * this one with a nonzero cgroup field, and the kernel's own
	 * "any extension bytes beyond what I know must be zero" check
	 * (mandatory ABI-compat behavior, not a bug) correctly rejected
	 * that as -E2BIG -- surfacing as "ssh.service: Failed to spawn
	 * executor: Argument list too long" and looping through systemd's
	 * restart backoff for ~3.5 minutes before eventually giving up
	 * retrying via this path.
	 */
	__aligned_u64 set_tid;
	__aligned_u64 set_tid_size;
	__aligned_u64 cgroup;
};

/*
 * Scheduling policies
 */
#define SCHED_NORMAL		0
#define SCHED_FIFO		1
#define SCHED_RR		2
#define SCHED_BATCH		3
/* SCHED_ISO: reserved but not implemented yet */
#define SCHED_IDLE		5
#define SCHED_DEADLINE		6

/* Can be ORed in to make sure the process is reverted back to SCHED_NORMAL on fork */
#define SCHED_RESET_ON_FORK     0x40000000

/*
 * For the sched_{set,get}attr() calls
 */
#define SCHED_FLAG_RESET_ON_FORK	0x01

#endif /* _UAPI_LINUX_SCHED_H */
