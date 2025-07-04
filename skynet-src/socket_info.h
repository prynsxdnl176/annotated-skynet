/*
 * socket_info.h - socket信息管理头文件
 * 定义了socket状态信息的数据结构和管理接口
 */

#ifndef socket_info_h
#define socket_info_h

// socket_info 类型定义
#define SOCKET_INFO_UNKNOWN 0   // 未知类型
#define SOCKET_INFO_LISTEN 1    // 监听socket
#define SOCKET_INFO_TCP 2       // TCP连接
#define SOCKET_INFO_UDP 3       // UDP socket
#define SOCKET_INFO_BIND 4      // 绑定socket
#define SOCKET_INFO_CLOSING 5   // 正在关闭的socket

#include <stdint.h>

/*
 * socket信息结构体
 * 记录socket的状态、统计信息和配置
 */
struct socket_info {
	int id;                     // socket ID
	int type;                   // socket类型（见上面的宏定义）
	uint64_t opaque;            // 不透明指针，关联上层对象
	uint64_t read;              // 已读取字节数
	uint64_t write;             // 已写入字节数
	uint64_t rtime;             // 最后读取时间
	uint64_t wtime;             // 最后写入时间
	int64_t wbuffer;            // 写缓冲区大小
	uint8_t reading;            // 是否正在读取
	uint8_t writing;            // 是否正在写入
	char name[128];             // socket名称或地址信息
	struct socket_info *next;   // 链表指针，用于管理多个socket信息
};

/*
 * socket信息管理接口
 */

// 创建socket信息链表
struct socket_info * socket_info_create(struct socket_info *last);

// 释放socket信息链表
void socket_info_release(struct socket_info *);

#endif
