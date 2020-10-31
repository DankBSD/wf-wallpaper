#include "pid_fd_fork.h"
#include <poll.h>
#include <stdio.h>
#include <assert.h>

int main() {
	pid_fork_t pf;
	int pid = pid_fork_start(&pf);
	if (pid == 0) {
		fprintf(stderr, "child....\n");
		sleep(1);
		fprintf(stderr, "child....done\n");
		return 69;
	}
	assert(pid > 0);
	struct pollfd p = { .fd = pf.poll_fd, .events = POLLRDNORM, .revents = 0 };
	assert(poll(&p, 1, INFTIM) > 0);
	assert(p.revents == POLLRDNORM);
	int code = -420;
	assert(pid_fork_exit_code(&pf, &code) == 0);
	fprintf(stderr, "exit code: %d\n", code);
}
