/*
 * socket_kqueue.h - BSD kqueue事件模型实现头文件
 * 提供基于kqueue的高性能I/O多路复用接口（适用于FreeBSD、macOS等）
 */

#ifndef poll_socket_kqueue_h
#define poll_socket_kqueue_h

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * 检查kqueue文件描述符是否无效
 * @param kfd: kqueue文件描述符
 * @return: 无效返回true，有效返回false
 */
static bool
sp_invalid(int kfd) {
	return kfd == -1;
}

/*
 * 创建kqueue实例
 * @return: kqueue文件描述符，失败返回-1
 */
static int
sp_create() {
	return kqueue();
}

/*
 * 释放kqueue实例
 * @param kfd: kqueue文件描述符
 */
static void
sp_release(int kfd) {
	close(kfd);
}

/*
 * 从kqueue实例删除socket
 * 删除读写事件监听
 * @param kfd: kqueue文件描述符
 * @param sock: socket文件描述符
 */
static void
sp_del(int kfd, int sock) {
	struct kevent ke;
	// 删除读事件监听
	EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	kevent(kfd, &ke, 1, NULL, 0, NULL);
	// 删除写事件监听
	EV_SET(&ke, sock, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	kevent(kfd, &ke, 1, NULL, 0, NULL);
}

/*
 * 向kqueue实例添加socket
 * 添加读写事件监听，默认禁用写事件
 * @param kfd: kqueue文件描述符
 * @param sock: socket文件描述符
 * @param ud: 用户数据指针
 * @return: 成功返回0，失败返回1
 */
static int
sp_add(int kfd, int sock, void *ud) {
	struct kevent ke;
	// 添加读事件监听
	EV_SET(&ke, sock, EVFILT_READ, EV_ADD, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 ||	ke.flags & EV_ERROR) {
		return 1;
	}
	// 添加写事件监听
	EV_SET(&ke, sock, EVFILT_WRITE, EV_ADD, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 ||	ke.flags & EV_ERROR) {
		// 失败时清理读事件
		EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		kevent(kfd, &ke, 1, NULL, 0, NULL);
		return 1;
	}
	// 默认禁用写事件
	EV_SET(&ke, sock, EVFILT_WRITE, EV_DISABLE, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 ||	ke.flags & EV_ERROR) {
		sp_del(kfd, sock);
		return 1;
	}
	return 0;
}

/*
 * 修改socket的事件监听状态
 * @param kfd: kqueue文件描述符
 * @param sock: socket文件描述符
 * @param ud: 用户数据指针
 * @param read_enable: 是否启用读事件监听
 * @param write_enable: 是否启用写事件监听
 * @return: 成功返回0，失败返回非0
 */
static int
sp_enable(int kfd, int sock, void *ud, bool read_enable, bool write_enable) {
	int ret = 0;
	struct kevent ke;
	// 设置读事件状态
	EV_SET(&ke, sock, EVFILT_READ, read_enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
		ret |= 1;
	}
	// 设置写事件状态
	EV_SET(&ke, sock, EVFILT_WRITE, write_enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
	if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
		ret |= 1;
	}
	return ret;
}

/*
 * 等待kqueue事件
 * @param kfd: kqueue文件描述符
 * @param e: 事件数组
 * @param max: 最大事件数量
 * @return: 实际事件数量
 */
static int
sp_wait(int kfd, struct event *e, int max) {
	struct kevent ev[max];
	int n = kevent(kfd, NULL, 0, ev, max, NULL);  // 阻塞等待事件

	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].udata;
		unsigned filter = ev[i].filter;
		bool eof = (ev[i].flags & EV_EOF) != 0;
		e[i].write = (filter == EVFILT_WRITE) && (!eof);  // 可写事件（排除EOF）
		e[i].read = (filter == EVFILT_READ);              // 可读事件
		e[i].error = (ev[i].flags & EV_ERROR) != 0;       // 错误事件
		e[i].eof = eof;                                   // 连接断开事件
	}

	return n;
}

/*
 * 设置文件描述符为非阻塞模式
 * @param fd: 文件描述符
 */
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
