/*
 * skynet_daemon.c - skynet守护进程管理模块
 * 提供将skynet进程转为守护进程的功能，包括PID文件管理
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include "skynet_daemon.h"

/*
 * 检查PID文件中的进程是否仍在运行
 * @param pidfile: PID文件路径
 * @return: 如果进程仍在运行返回PID，否则返回0
 */
static int
check_pid(const char *pidfile) {
	int pid = 0;
	FILE *f = fopen(pidfile,"r");
	if (f == NULL)
		return 0;  // PID文件不存在

	int n = fscanf(f,"%d", &pid);
	fclose(f);

	if (n !=1 || pid == 0 || pid == getpid()) {
		return 0;  // 读取失败、PID无效或是当前进程
	}

	// 检查进程是否存在（发送信号0不会实际发送信号，只检查权限）
	if (kill(pid, 0) && errno == ESRCH)
		return 0;  // 进程不存在

	return pid;  // 进程仍在运行
}

/*
 * 写入当前进程PID到文件
 * 使用文件锁确保只有一个实例运行
 * @param pidfile: PID文件路径
 * @return: 成功返回1，失败返回0
 */
static int
write_pid(const char *pidfile) {
	FILE *f;
	int pid = 0;
	int fd = open(pidfile, O_RDWR|O_CREAT, 0644);
	if (fd == -1) {
		fprintf(stderr, "Can't create pidfile [%s].\n", pidfile);
		return 0;
	}
	f = fdopen(fd, "w+");
	if (f == NULL) {
		fprintf(stderr, "Can't open pidfile [%s].\n", pidfile);
		return 0;
	}

	// 尝试获取排他锁（非阻塞）
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		int n = fscanf(f, "%d", &pid);
		fclose(f);
		if (n != 1) {
			fprintf(stderr, "Can't lock and read pidfile.\n");
		} else {
			fprintf(stderr, "Can't lock pidfile, lock is held by pid %d.\n", pid);
		}
		return 0;
	}

	pid = getpid();
	if (!fprintf(f,"%d\n", pid)) {
		fprintf(stderr, "Can't write pid.\n");
		close(fd);
		return 0;
	}
	fflush(f);

	return pid;
}

/*
 * 重定向标准文件描述符到/dev/null
 * 守护进程需要关闭标准输入、输出和错误输出
 * @return: 成功返回0，失败返回-1
 */
static int
redirect_fds() {
	int nfd = open("/dev/null", O_RDWR);
	if (nfd == -1) {
		perror("Unable to open /dev/null: ");
		return -1;
	}
	// 重定向标准输入到/dev/null
	if (dup2(nfd, 0) < 0) {
		perror("Unable to dup2 stdin(0): ");
		return -1;
	}
	// 重定向标准输出到/dev/null
	if (dup2(nfd, 1) < 0) {
		perror("Unable to dup2 stdout(1): ");
		return -1;
	}
	// 重定向标准错误输出到/dev/null
	if (dup2(nfd, 2) < 0) {
		perror("Unable to dup2 stderr(2): ");
		return -1;
	}

	close(nfd);

	return 0;
}

/*
 * 初始化守护进程
 * 检查是否已有实例运行，创建守护进程，写入PID文件，重定向文件描述符
 * @param pidfile: PID文件路径
 * @return: 成功返回0，失败返回1
 */
int
daemon_init(const char *pidfile) {
	int pid = check_pid(pidfile);

	if (pid) {
		fprintf(stderr, "Skynet is already running, pid = %d.\n", pid);
		return 1;
	}

#ifdef __APPLE__
	// macOS上daemon函数已废弃，建议使用launchd
	fprintf(stderr, "'daemon' is deprecated: first deprecated in OS X 10.5 , use launchd instead.\n");
#else
	// 创建守护进程（保持当前目录，保持文件描述符）
	if (daemon(1,1)) {
		fprintf(stderr, "Can't daemonize.\n");
		return 1;
	}
#endif

	// 写入PID文件
	pid = write_pid(pidfile);
	if (pid == 0) {
		return 1;
	}

	// 重定向标准文件描述符
	if (redirect_fds()) {
		return 1;
	}

	return 0;
}

/*
 * 退出守护进程
 * 删除PID文件，清理守护进程资源
 * @param pidfile: PID文件路径
 * @return: 成功返回0，失败返回-1
 */
int
daemon_exit(const char *pidfile) {
	return unlink(pidfile);  // 删除PID文件
}
