/*
 * socket_poll.h - socket轮询抽象层头文件
 * 提供跨平台的I/O多路复用统一接口
 * 根据不同平台自动选择最优的事件模型（epoll/kqueue）
 */

#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>

// 轮询文件描述符类型定义
typedef int poll_fd;

/*
 * 事件结构体
 * 描述socket上发生的各种事件
 */
struct event {
	void * s;       // socket对象指针
	bool read;      // 可读事件
	bool write;     // 可写事件
	bool error;     // 错误事件
	bool eof;       // 连接结束事件
};

/*
 * 跨平台轮询接口函数声明
 * 具体实现由平台相关的头文件提供
 */

// 检查轮询描述符是否无效
static bool sp_invalid(poll_fd fd);

// 创建轮询实例
static poll_fd sp_create();

// 释放轮询实例
static void sp_release(poll_fd fd);

// 添加socket到轮询实例
static int sp_add(poll_fd fd, int sock, void *ud);

// 从轮询实例删除socket
static void sp_del(poll_fd fd, int sock);

// 启用/禁用socket事件监听
static int sp_enable(poll_fd, int sock, void *ud, bool read_enable, bool write_enable);

// 等待事件发生
static int sp_wait(poll_fd, struct event *e, int max);

// 设置socket为非阻塞模式
static void sp_nonblocking(int sock);

/*
 * 平台相关实现包含
 * 根据编译平台自动选择最优的事件模型
 */

#ifdef __linux__
#include "socket_epoll.h"  // Linux平台使用epoll
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#include "socket_kqueue.h"  // BSD系列平台使用kqueue
#endif

#endif
