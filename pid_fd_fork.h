// Abstraction over platform-specific process descriptor APIs.
// With this abstraction, you get an FD that you can poll on for *readability*,
// i.e. you can insert it into abstracted event loops (libevent etc.)
// WARNING: exit code will always return 0 on Linux before 5.4.
// Avoid relying on exit code.
#pragma once

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
	int poll_fd;
	int proc_fd;
} pid_fork_t;

#if defined(__FreeBSD__)

#include <sys/procdesc.h>
#include <sys/event.h>

static inline pid_t pid_fork_start(pid_fork_t *pf) {
	pid_t pid = pdfork(&pf->proc_fd, 0);
	if (pid > 0) {
		pf->poll_fd = kqueue();
		if (pf->poll_fd == -1) {
			pdkill(pf->proc_fd, SIGKILL);
			return -1;
		}
		struct kevent event;
		EV_SET(&event, pf->proc_fd, EVFILT_PROCDESC, EV_ADD | EV_CLEAR, NOTE_EXIT, 0, NULL);
		int kret = kevent(pf->poll_fd, &event, 1, NULL, 0, NULL);
		if (kret == -1 || event.flags & EV_ERROR) {
			pdkill(pf->proc_fd, SIGKILL);
			return -1;
		}
	}
	return pid;
}

static inline int pid_fork_signal(pid_fork_t *pf, int signum) {
	return pdkill(pf->proc_fd, signum);
}

static inline int pid_fork_exit_code(pid_fork_t *pf, int *code) {
	struct kevent event;
	int ret = kevent(pf->poll_fd, NULL, 0, &event, 1, NULL);
	if (ret < 1) return -1;
	*code = WEXITSTATUS(event.data);
	close(pf->proc_fd);
	close(pf->poll_fd);
	pf->poll_fd = -1;
	pf->proc_fd = -1;
	return 0;
}

#elif defined(__linux__)

#include <sys/syscall.h>
#include <linux/sched.h>

#ifndef CLONE_PIDFD
#error "Linux (or uapi headers specifically) too old, at least 5.2 required"
#endif

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

// XXX: UNTESTED!
static inline pid_t pid_fork_start(pid_fork_t *pf) {
	struct clone_args args = {
		.pidfd = ptr_to_u64(&pf->proc_fd),
		.flags = CLONE_PIDFD,
		.exit_signal = 0,
	};
	pid_t pid = syscall(__NR_clone3, args, sizeof(struct clone_args));
	pf->poll_fd = pf->proc_fd;
	return pid;
}

static inline int pid_fork_signal(pid_fork_t *pf, int signum) {
	return syscall(__NR_pidfd_send_signal, pf->proc_fd, signum, NULL, 0);
}

static inline int pid_fork_exit_code(pid_fork_t *pf, int *code) {
	*code = 0;
	siginfo_t info;
	if (waitid(P_PIDFD, pf->proc_fd, &info, 0) != 0) {
		*code = info.si_status;
	}
	// Don't error because error might be Linux being too old
	close(pf->poll_fd);
	pf->poll_fd = -1;
	pf->proc_fd = -1;
	return 0;
}

#else
#error "pid_fd_fork: port me"
#endif
