/*
 * socket_server.h - skynet socket服务器头文件
 * 定义了socket服务器的接口和数据结构，支持TCP/UDP协议
 */

#ifndef skynet_socket_server_h
#define skynet_socket_server_h

#include <stdint.h>
#include "socket_info.h"
#include "socket_buffer.h"

// socket消息类型定义
#define SOCKET_DATA 0       // 数据消息
#define SOCKET_CLOSE 1      // 连接关闭
#define SOCKET_OPEN 2       // 连接打开
#define SOCKET_ACCEPT 3     // 接受新连接
#define SOCKET_ERR 4        // 错误消息
#define SOCKET_EXIT 5       // 退出消息
#define SOCKET_UDP 6        // UDP消息
#define SOCKET_WARNING 7    // 警告消息

// Only for internal use
// 仅供内部使用的消息类型
#define SOCKET_RST 8        // 连接重置
#define SOCKET_MORE 9       // 更多数据

// 前向声明
struct socket_server;

/*
 * socket消息结构体
 * 用于在socket服务器和上层应用间传递消息
 */
struct socket_message {
	int id;             // socket ID
	uintptr_t opaque;   // 不透明指针，用于关联上层对象
	// 用户数据：对于accept是新连接ID，对于data是数据大小
	int ud;	// for accept, ud is new connection id ; for data, ud is size of data 
	char * data;        // 消息数据指针
};

/*
 * socket服务器生命周期管理
 */

// 创建socket服务器
struct socket_server * socket_server_create(uint64_t time);

// 释放socket服务器
void socket_server_release(struct socket_server *);

// 更新服务器时间
void socket_server_updatetime(struct socket_server *, uint64_t time);

// 轮询socket事件
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);

/*
 * socket连接控制
 */

// 退出socket服务器
void socket_server_exit(struct socket_server *);

// 关闭socket连接
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);

// 优雅关闭socket连接
void socket_server_shutdown(struct socket_server *, uintptr_t opaque, int id);

// 启动socket连接
void socket_server_start(struct socket_server *, uintptr_t opaque, int id);

// 暂停socket连接
void socket_server_pause(struct socket_server *, uintptr_t opaque, int id);

/*
 * 数据发送接口
 */

// return -1 when error
// 发送数据（错误时返回-1）
int socket_server_send(struct socket_server *, struct socket_sendbuffer *buffer);

// 低优先级发送数据
int socket_server_send_lowpriority(struct socket_server *, struct socket_sendbuffer *buffer);

/*
 * TCP连接管理（控制命令返回socket ID）
 */

// ctrl command below returns id
// 监听TCP端口
int socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);

// 连接到TCP服务器
int socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);

// 绑定已存在的文件描述符
int socket_server_bind(struct socket_server *, uintptr_t opaque, int fd);

// for tcp
// 设置TCP无延迟选项
void socket_server_nodelay(struct socket_server *, int id);

/*
 * UDP相关接口
 */

// 前向声明
struct socket_udp_address;

// create an udp socket handle, attach opaque with it . udp socket don't need call socket_server_start to recv message
// if port != 0, bind the socket . if addr == NULL, bind ipv4 0.0.0.0 . If you want to use ipv6, addr can be "::" and port 0.
// 创建UDP socket句柄，绑定opaque对象
// UDP socket不需要调用socket_server_start来接收消息
// 如果port != 0，绑定socket；如果addr == NULL，绑定IPv4 0.0.0.0
// 如果要使用IPv6，addr可以是"::"，port为0
int socket_server_udp(struct socket_server *, uintptr_t opaque, const char * addr, int port);
// set default dest address, return 0 when success
// 设置默认目标地址，成功返回0
int socket_server_udp_connect(struct socket_server *, int id, const char * addr, int port);

// create an udp client socket handle, and connect to server addr, return id when success
// 创建UDP客户端socket句柄，连接到服务器地址，成功返回ID
int socket_server_udp_dial(struct socket_server *ss, uintptr_t opaque, const char* addr, int port);
// create an udp server socket handle, and bind the host port, return id when success
// 创建UDP服务器socket句柄，绑定主机端口，成功返回ID
int socket_server_udp_listen(struct socket_server *ss, uintptr_t opaque, const char* addr, int port);

// 发送UDP数据
// If the socket_udp_address is NULL, use last call socket_server_udp_connect address instead
// You can also use socket_server_send 
// 如果socket_udp_address为NULL，使用上次socket_server_udp_connect的地址
// 也可以使用socket_server_send
int socket_server_udp_send(struct socket_server *, const struct socket_udp_address *, struct socket_sendbuffer *buffer);
// extract the address of the message, struct socket_message * should be SOCKET_UDP
// 提取消息的地址信息，socket_message应该是SOCKET_UDP类型
const struct socket_udp_address * socket_server_udp_address(struct socket_server *, struct socket_message *, int *addrsz);

/*
 * 用户对象接口
 * 用于处理SOCKET_BUFFER_OBJECT类型的数据包
 */
struct socket_object_interface {
	const void * (*buffer)(const void *);  // 获取缓冲区指针
	size_t (*size)(const void *);          // 获取对象大小
	void (*free)(void *);                  // 释放对象
};

// if you send package with type SOCKET_BUFFER_OBJECT, use soi.
// 设置用户对象接口（用于发送SOCKET_BUFFER_OBJECT类型的包）
void socket_server_userobject(struct socket_server *, struct socket_object_interface *soi);

// 获取socket信息
struct socket_info * socket_server_info(struct socket_server *);

#endif
