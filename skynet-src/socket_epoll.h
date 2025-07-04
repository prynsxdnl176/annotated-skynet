/*
 * socket_epoll.h - Linux epoll事件模型实现头文件
 * 提供基于epoll的高性能I/O多路复用接口
 */

#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/*
 * 检查epoll文件描述符是否无效
 * @param efd: epoll文件描述符
 * @return: 无效返回true，有效返回false
 */
static bool
sp_invalid(int efd) {
	return efd == -1;
}

/*
 * 创建epoll实例
 * @return: epoll文件描述符，失败返回-1
 */
static int
sp_create() {
	return epoll_create(1024);
}

/*
 * 释放epoll实例
 * @param efd: epoll文件描述符
 */
static void
sp_release(int efd) {
	close(efd);
}

/*
 * 向epoll实例添加socket
 * @param efd: epoll文件描述符
 * @param sock: socket文件描述符
 * @param ud: 用户数据指针
 * @return: 成功返回0，失败返回1
 */
static int
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN;  // 默认监听读事件
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

/*
 * 从epoll实例删除socket
 * @param efd: epoll文件描述符
 * @param sock: socket文件描述符
 */
static void
sp_del(int efd, int sock) {
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

/*
 * 修改socket的事件监听状态
 * @param efd: epoll文件描述符
 * @param sock: socket文件描述符
 * @param ud: 用户数据指针
 * @param read_enable: 是否启用读事件监听
 * @param write_enable: 是否启用写事件监听
 * @return: 成功返回0，失败返回1
 */
static int
sp_enable(int efd, int sock, void *ud, bool read_enable, bool write_enable) {
	struct epoll_event ev;
	ev.events = (read_enable ? EPOLLIN : 0) | (write_enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

/*
 * 等待epoll事件
 * @param efd: epoll文件描述符
 * @param e: 事件数组
 * @param max: 最大事件数量
 * @return: 实际事件数量
 */
static int
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];
	int n = epoll_wait(efd , ev, max, -1);  // 阻塞等待事件
	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].data.ptr;
		unsigned flag = ev[i].events;
		e[i].write = (flag & EPOLLOUT) != 0;  // 可写事件
		e[i].read = (flag & EPOLLIN) != 0;    // 可读事件
		e[i].error = (flag & EPOLLERR) != 0;  // 错误事件
		e[i].eof = (flag & EPOLLHUP) != 0;    // 连接断开事件
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
