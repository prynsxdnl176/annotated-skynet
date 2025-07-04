/*
 * skynet_socket.h - skynet网络接口头文件
 * 提供TCP/UDP网络通信的高层接口
 */

#ifndef skynet_socket_h
#define skynet_socket_h

#include "socket_info.h"
#include "socket_buffer.h"

// 前向声明
struct skynet_context;

// socket消息类型定义
#define SKYNET_SOCKET_TYPE_DATA 1       // 数据消息
#define SKYNET_SOCKET_TYPE_CONNECT 2    // 连接建立
#define SKYNET_SOCKET_TYPE_CLOSE 3      // 连接关闭
#define SKYNET_SOCKET_TYPE_ACCEPT 4     // 接受新连接
#define SKYNET_SOCKET_TYPE_ERROR 5      // 错误消息
#define SKYNET_SOCKET_TYPE_UDP 6        // UDP消息
#define SKYNET_SOCKET_TYPE_WARNING 7    // 警告消息

/*
 * skynet socket消息结构体
 * 用于在skynet服务和socket系统间传递消息
 */
struct skynet_socket_message {
	int type;       // 消息类型
	int id;         // socket ID
	int ud;         // 用户数据
	char * buffer;  // 消息缓冲区
};

/*
 * socket系统管理
 */

// 初始化socket系统
void skynet_socket_init();

// 退出socket系统
void skynet_socket_exit();

// 释放socket系统资源
void skynet_socket_free();

// 轮询socket事件
int skynet_socket_poll();

// 更新socket系统时间
void skynet_socket_updatetime();

/*
 * TCP连接管理
 */

// 发送数据缓冲区
int skynet_socket_sendbuffer(struct skynet_context *ctx, struct socket_sendbuffer *buffer);

// 低优先级发送数据缓冲区
int skynet_socket_sendbuffer_lowpriority(struct skynet_context *ctx, struct socket_sendbuffer *buffer);

// 监听TCP端口
int skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog);

// 连接到TCP服务器
int skynet_socket_connect(struct skynet_context *ctx, const char *host, int port);

// 绑定已存在的文件描述符
int skynet_socket_bind(struct skynet_context *ctx, int fd);

// 关闭socket连接
void skynet_socket_close(struct skynet_context *ctx, int id);

// 优雅关闭socket连接
void skynet_socket_shutdown(struct skynet_context *ctx, int id);

// 启动socket连接
void skynet_socket_start(struct skynet_context *ctx, int id);

// 暂停socket连接
void skynet_socket_pause(struct skynet_context *ctx, int id);

// 设置TCP无延迟选项
void skynet_socket_nodelay(struct skynet_context *ctx, int id);

/*
 * UDP通信接口
 */

// 创建UDP socket
int skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port);

// 连接UDP目标地址
int skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port);

// 创建UDP客户端
int skynet_socket_udp_dial(struct skynet_context *ctx, const char * addr, int port);

// 创建UDP服务器
int skynet_socket_udp_listen(struct skynet_context *ctx, const char * addr, int port);

// 发送UDP数据缓冲区
int skynet_socket_udp_sendbuffer(struct skynet_context *ctx, const char * address, struct socket_sendbuffer *buffer);

// 获取UDP消息的地址信息
const char * skynet_socket_udp_address(struct skynet_socket_message *, int *addrsz);

/*
 * socket信息查询
 */

// 获取socket信息
struct socket_info * skynet_socket_info();

// legacy APIs
/*
 * 兼容性API（旧版本接口）
 */

// 初始化发送缓冲区结构体
static inline void sendbuffer_init_(struct socket_sendbuffer *buf, int id, const void *buffer, int sz) {
	buf->id = id;
	buf->buffer = buffer;
	if (sz < 0) {
		buf->type = SOCKET_BUFFER_OBJECT;
	} else {
		buf->type = SOCKET_BUFFER_MEMORY;
	}
	buf->sz = (size_t)sz;
}

// 发送数据（兼容接口）
static inline int skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	struct socket_sendbuffer tmp;
	sendbuffer_init_(&tmp, id, buffer, sz);
	return skynet_socket_sendbuffer(ctx, &tmp);
}

// 低优先级发送数据（兼容接口）
static inline int skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	struct socket_sendbuffer tmp;
	sendbuffer_init_(&tmp, id, buffer, sz);
	return skynet_socket_sendbuffer_lowpriority(ctx, &tmp);
}

// 发送UDP数据（兼容接口）
static inline int skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz) {
	struct socket_sendbuffer tmp;
	sendbuffer_init_(&tmp, id, buffer, sz);
	return skynet_socket_udp_sendbuffer(ctx, address, &tmp);
}

#endif
