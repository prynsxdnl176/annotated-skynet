/*
 * socket_buffer.h - socket缓冲区管理头文件
 * 定义了socket发送缓冲区的数据结构和类型
 */

#ifndef socket_buffer_h
#define socket_buffer_h

#include <stdlib.h>

// socket缓冲区类型定义
#define SOCKET_BUFFER_MEMORY 0      // 内存缓冲区
#define SOCKET_BUFFER_OBJECT 1      // 对象缓冲区
#define SOCKET_BUFFER_RAWPOINTER 2  // 原始指针缓冲区

/*
 * socket发送缓冲区结构体
 * 用于封装不同类型的发送数据
 */
struct socket_sendbuffer {
	int id;                 // socket ID
	int type;               // 缓冲区类型（见上面的宏定义）
	const void *buffer;     // 缓冲区指针
	size_t sz;              // 缓冲区大小
};

#endif
