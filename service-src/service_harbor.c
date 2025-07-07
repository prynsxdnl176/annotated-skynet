#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_socket.h"
#include "skynet_handle.h"

/*
	harbor listen the PTYPE_HARBOR (in text)
	harbor 监听 PTYPE_HARBOR 类型的消息（文本格式）
	N name : update the global name
	N name : 更新全局名称
	S fd id: connect to new harbor , we should send self_id to fd first , and then recv a id (check it), and at last send queue.
	S fd id: 连接到新的harbor，我们应该先发送self_id到fd，然后接收一个id（检查它），最后发送队列
	A fd id: accept new harbor , we should send self_id to fd , and then send queue.
	A fd id: 接受新的harbor，我们应该发送self_id到fd，然后发送队列

	If the fd is disconnected, send message to slave in PTYPE_TEXT.  D id
	如果fd断开连接，向slave发送PTYPE_TEXT消息。D id
	If we don't known a globalname, send message to slave in PTYPE_TEXT. Q name
	如果我们不知道全局名称，向slave发送PTYPE_TEXT消息。Q name
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#define HASH_SIZE 4096           // 哈希表大小
#define DEFAULT_QUEUE_SIZE 1024  // 默认队列大小

// 12 is sizeof(struct remote_message_header)
// 12是远程消息头的大小
#define HEADER_COOKIE_LENGTH 12

/*
	message type (8bits) is in destination high 8bits
	消息类型（8位）在destination的高8位
	harbor id (8bits) is also in that place , but remote message doesn't need harbor id.
	harbor id（8位）也在那个位置，但远程消息不需要harbor id
 */
// 远程消息头结构，用于跨节点通信
struct remote_message_header {
	uint32_t source;      // 源服务句柄
	uint32_t destination; // 目标服务句柄
	uint32_t session;     // 会话ID
};

// harbor消息结构，包含消息头和数据
struct harbor_msg {
	struct remote_message_header header; // 消息头
	void * buffer;                       // 消息数据缓冲区
	size_t size;                         // 消息数据大小
};

// harbor消息队列结构，用于缓存待发送的消息
struct harbor_msg_queue {
	int size;                    // 队列容量
	int head;                    // 队列头索引
	int tail;                    // 队列尾索引
	struct harbor_msg * data;    // 消息数组
};

// 全局名称键值对结构，用于名称到服务句柄的映射
struct keyvalue {
	struct keyvalue * next;                  // 链表下一个节点
	char key[GLOBALNAME_LENGTH];             // 全局名称键
	uint32_t hash;                           // 哈希值
	uint32_t value;                          // 服务句柄值
	struct harbor_msg_queue * queue;         // 等待该名称解析的消息队列
};

// 哈希映射结构，管理全局名称映射
struct hashmap {
	struct keyvalue *node[HASH_SIZE];        // 哈希桶数组
};

// 从节点连接状态定义
#define STATUS_WAIT 0        // 等待连接
#define STATUS_HANDSHAKE 1   // 握手阶段
#define STATUS_HEADER 2      // 读取消息头阶段
#define STATUS_CONTENT 3     // 读取消息内容阶段
#define STATUS_DOWN 4        // 连接断开

// 从节点结构，表示一个远程harbor节点
struct slave {
	int fd;                              // socket文件描述符
	struct harbor_msg_queue *queue;      // 待发送消息队列
	int status;                          // 连接状态
	int length;                          // 当前消息长度
	int read;                            // 已读取字节数
	uint8_t size[4];                     // 消息大小缓冲区
	char * recv_buffer;                  // 接收缓冲区
};

// harbor主结构，管理整个集群通信
struct harbor {
	struct skynet_context *ctx;          // skynet上下文
	int id;                              // 本节点ID
	uint32_t slave;                      // 从节点服务句柄
	struct hashmap * map;                // 全局名称映射表
	struct slave s[REMOTE_MAX];          // 远程节点数组
};

// hash table
// 哈希表相关函数

// 向消息队列中添加消息
static void
push_queue_msg(struct harbor_msg_queue * queue, struct harbor_msg * m) {
	// If there is only 1 free slot which is reserved to distinguish full/empty
	// of circular buffer, expand it.
	// 如果只剩1个空闲槽位（用于区分满/空的循环缓冲区），则扩展队列
	if (((queue->tail + 1) % queue->size) == queue->head) {
		// 队列已满，需要扩展
		struct harbor_msg * new_buffer = skynet_malloc(queue->size * 2 * sizeof(struct harbor_msg));
		int i;
		// 复制现有消息到新缓冲区
		for (i=0;i<queue->size-1;i++) {
			new_buffer[i] = queue->data[(i+queue->head) % queue->size];
		}
		skynet_free(queue->data);    // 释放旧缓冲区
		queue->data = new_buffer;    // 使用新缓冲区
		queue->head = 0;             // 重置头指针
		queue->tail = queue->size - 1; // 重置尾指针
		queue->size *= 2;            // 队列大小翻倍
	}
	struct harbor_msg * slot = &queue->data[queue->tail];  // 获取尾部槽位
	*slot = *m;  // 复制消息到槽位
	queue->tail = (queue->tail + 1) % queue->size;  // 移动尾指针
}

// 向队列中添加消息（带消息头）
static void
push_queue(struct harbor_msg_queue * queue, void * buffer, size_t sz, struct remote_message_header * header) {
	struct harbor_msg m;
	m.header = *header;  // 复制消息头
	m.buffer = buffer;   // 设置消息缓冲区
	m.size = sz;         // 设置消息大小
	push_queue_msg(queue, &m);  // 添加到队列
}

// 从队列中弹出消息
static struct harbor_msg *
pop_queue(struct harbor_msg_queue * queue) {
	if (queue->head == queue->tail) {
		// 队列为空
		return NULL;
	}
	struct harbor_msg * slot = &queue->data[queue->head];  // 获取头部消息
	queue->head = (queue->head + 1) % queue->size;         // 移动头指针
	return slot;
}

// 创建新的消息队列
static struct harbor_msg_queue *
new_queue() {
	struct harbor_msg_queue * queue = skynet_malloc(sizeof(*queue));
	queue->size = DEFAULT_QUEUE_SIZE;  // 设置默认大小
	queue->head = 0;                   // 初始化头指针
	queue->tail = 0;                   // 初始化尾指针
	queue->data = skynet_malloc(DEFAULT_QUEUE_SIZE * sizeof(struct harbor_msg));  // 分配数据缓冲区

	return queue;
}

// 释放消息队列
static void
release_queue(struct harbor_msg_queue *queue) {
	if (queue == NULL)
		return;
	struct harbor_msg * m;
	// 释放队列中所有消息的缓冲区
	while ((m=pop_queue(queue)) != NULL) {
		skynet_free(m->buffer);
	}
	skynet_free(queue->data);  // 释放队列数据缓冲区
	skynet_free(queue);        // 释放队列结构
}

// 在哈希表中搜索指定名称的键值对
static struct keyvalue *
hash_search(struct hashmap * hash, const char name[GLOBALNAME_LENGTH]) {
	uint32_t *ptr = (uint32_t*) name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];  // 计算哈希值
	struct keyvalue * node = hash->node[h % HASH_SIZE];  // 获取哈希桶
	while (node) {
		if (node->hash == h && strncmp(node->key, name, GLOBALNAME_LENGTH) == 0) {
			return node;
		}
		node = node->next;
	}
	return NULL;
}

/*

// Don't support erase name yet

static struct void
hash_erase(struct hashmap * hash, char name[GLOBALNAME_LENGTH) {
	uint32_t *ptr = name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
	struct keyvalue ** ptr = &hash->node[h % HASH_SIZE];
	while (*ptr) {
		struct keyvalue * node = *ptr;
		if (node->hash == h && strncmp(node->key, name, GLOBALNAME_LENGTH) == 0) {
			_release_queue(node->queue);
			*ptr->next = node->next;
			skynet_free(node);
			return;
		}
		*ptr = &(node->next);
	}
}
*/

// 向哈希表中插入新的键值对
static struct keyvalue *
hash_insert(struct hashmap * hash, const char name[GLOBALNAME_LENGTH]) {
	uint32_t *ptr = (uint32_t *)name;
	uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];  // 计算哈希值
	struct keyvalue ** pkv = &hash->node[h % HASH_SIZE];  // 获取哈希桶位置
	struct keyvalue * node = skynet_malloc(sizeof(*node));  // 分配新节点
	memcpy(node->key, name, GLOBALNAME_LENGTH);  // 复制键名
	node->next = *pkv;    // 链表头插法
	node->queue = NULL;   // 初始化队列为空
	node->hash = h;       // 保存哈希值
	node->value = 0;      // 初始化值为0
	*pkv = node;          // 更新哈希桶头指针

	return node;
}

// 创建新的哈希映射
static struct hashmap *
hash_new() {
	struct hashmap * h = skynet_malloc(sizeof(struct hashmap));
	memset(h,0,sizeof(*h));  // 清零所有哈希桶
	return h;
}

// 删除哈希映射并释放所有资源
static void
hash_delete(struct hashmap *hash) {
	int i;
	for (i=0;i<HASH_SIZE;i++) {
		// 遍历每个哈希桶
		struct keyvalue * node = hash->node[i];
		while (node) {
			struct keyvalue * next = node->next;
			release_queue(node->queue);  // 释放节点的消息队列
			skynet_free(node);           // 释放节点
			node = next;
		}
	}
	skynet_free(hash);  // 释放哈希表结构
}

///////////////
// Harbor管理函数

// 关闭指定ID的harbor连接
static void
close_harbor(struct harbor *h, int id) {
	struct slave *s = &h->s[id];
	s->status = STATUS_DOWN;  // 设置状态为下线
	if (s->fd) {
		skynet_socket_close(h->ctx, s->fd);  // 关闭socket连接
		s->fd = 0;
	}
	if (s->queue) {
		release_queue(s->queue);  // 释放消息队列
		s->queue = NULL;
	}
}

// 报告harbor节点下线
static void
report_harbor_down(struct harbor *h, int id) {
	char down[64];
	int n = sprintf(down, "D %d",id);  // 格式化下线消息

	skynet_send(h->ctx, 0, h->slave, PTYPE_TEXT, 0, down, n);  // 发送下线通知给slave服务
}

// 创建harbor实例
struct harbor *
harbor_create(void) {
	struct harbor * h = skynet_malloc(sizeof(*h));
	memset(h,0,sizeof(*h));  // 清零结构体
	h->map = hash_new();     // 创建哈希映射表
	return h;
}


// 关闭所有远程harbor连接
static void
close_all_remotes(struct harbor *h) {
	int i;
	for (i=1;i<REMOTE_MAX;i++) {
		close_harbor(h,i);  // 关闭每个远程连接
		// don't call report_harbor_down.
		// never call skynet_send during module exit, because of dead lock
		// 不调用report_harbor_down
		// 模块退出时不要调用skynet_send，因为会死锁
	}
}

// 释放harbor实例
void
harbor_release(struct harbor *h) {
	close_all_remotes(h);  // 关闭所有远程连接
	hash_delete(h->map);   // 删除哈希映射表
	skynet_free(h);        // 释放harbor结构
}

// 将32位整数转换为大端字节序
static inline void
to_bigendian(uint8_t *buffer, uint32_t n) {
	buffer[0] = (n >> 24) & 0xff;  // 最高字节
	buffer[1] = (n >> 16) & 0xff;  // 次高字节
	buffer[2] = (n >> 8) & 0xff;   // 次低字节
	buffer[3] = n & 0xff;          // 最低字节
}

// 将消息头转换为网络字节序的消息格式
static inline void
header_to_message(const struct remote_message_header * header, uint8_t * message) {
	to_bigendian(message , header->source);       // 转换源地址
	to_bigendian(message+4 , header->destination); // 转换目标地址
	to_bigendian(message+8 , header->session);     // 转换会话ID
}

// 从大端字节序转换为本地字节序
static inline uint32_t
from_bigendian(uint32_t n) {
	union {
		uint32_t big;
		uint8_t bytes[4];
	} u;
	u.big = n;
	return u.bytes[0] << 24 | u.bytes[1] << 16 | u.bytes[2] << 8 | u.bytes[3];
}

// 将网络消息转换为消息头结构
static inline void
message_to_header(const uint32_t *message, struct remote_message_header *header) {
	header->source = from_bigendian(message[0]);       // 转换源地址
	header->destination = from_bigendian(message[1]);  // 转换目标地址
	header->session = from_bigendian(message[2]);      // 转换会话ID
}

// socket package
// socket数据包处理

// 转发本地消息
static void
forward_local_messsage(struct harbor *h, void *msg, int sz) {
	const char * cookie = msg;
	cookie += sz - HEADER_COOKIE_LENGTH;  // 定位到消息尾部的头信息
	struct remote_message_header header;
	message_to_header((const uint32_t *)cookie, &header);  // 解析消息头

	uint32_t destination = header.destination;
	int type = destination >> HANDLE_REMOTE_SHIFT;  // 提取消息类型
	destination = (destination & HANDLE_MASK) | ((uint32_t)h->id << HANDLE_REMOTE_SHIFT);  // 重构目标地址

	if (skynet_send(h->ctx, header.source, destination, type | PTYPE_TAG_DONTCOPY , (int)header.session, (void *)msg, sz-HEADER_COOKIE_LENGTH) < 0) {
		// 发送失败处理
		if (type != PTYPE_ERROR) {
			// don't need report error when type is error
			// 当类型不是错误时才需要报告错误
			skynet_send(h->ctx, destination, header.source , PTYPE_ERROR, (int)header.session, NULL, 0);
		}
		skynet_error(h->ctx, "Unknown destination :%x from :%x type(%d)", destination, header.source, type);
	}
}

// 发送远程消息
static void
send_remote(struct skynet_context * ctx, int fd, const char * buffer, size_t sz, struct remote_message_header * cookie) {
	size_t sz_header = sz+sizeof(*cookie);  // 计算总大小（消息+头）
	if (sz_header > UINT32_MAX) {
		skynet_error(ctx, "remote message from :%08x to :%08x is too large.", cookie->source, cookie->destination);
		return;
	}
	uint8_t sendbuf[sz_header+4];  // 分配发送缓冲区（包含4字节长度前缀）
	to_bigendian(sendbuf, (uint32_t)sz_header);  // 写入消息长度（大端序）
	memcpy(sendbuf+4, buffer, sz);               // 复制消息内容
	header_to_message(cookie, sendbuf+4+sz);     // 写入消息头到末尾

	struct socket_sendbuffer tmp;
	tmp.id = fd;                          // socket文件描述符
	tmp.type = SOCKET_BUFFER_RAWPOINTER;  // 原始指针类型
	tmp.buffer = sendbuf;                 // 发送缓冲区
	tmp.sz = sz_header+4;                 // 总大小

	// ignore send error, because if the connection is broken, the mainloop will recv a message.
	// 忽略发送错误，因为如果连接断开，主循环会收到消息
	skynet_socket_sendbuffer(ctx, &tmp);
}

// 分发名称队列中的消息
static void
dispatch_name_queue(struct harbor *h, struct keyvalue * node) {
	struct harbor_msg_queue * queue = node->queue;
	uint32_t handle = node->value;
	int harbor_id = handle >> HANDLE_REMOTE_SHIFT;  // 提取harbor ID
	struct skynet_context * context = h->ctx;
	struct slave *s = &h->s[harbor_id];
	int fd = s->fd;
	if (fd == 0) {
		// 连接不存在
		if (s->status == STATUS_DOWN) {
			// harbor节点已下线，丢弃消息
			char tmp [GLOBALNAME_LENGTH+1];
			memcpy(tmp, node->key, GLOBALNAME_LENGTH);
			tmp[GLOBALNAME_LENGTH] = '\0';
			skynet_error(context, "Drop message to %s (in harbor %d)",tmp,harbor_id);
		} else {
			// harbor节点未连接，将消息加入等待队列
			if (s->queue == NULL) {
				s->queue = node->queue;  // 直接使用节点的队列
				node->queue = NULL;
			} else {
				// 合并队列
				struct harbor_msg * m;
				while ((m = pop_queue(queue))!=NULL) {
					push_queue_msg(s->queue, m);
				}
			}
			if (harbor_id == (h->slave >> HANDLE_REMOTE_SHIFT)) {
				// the harbor_id is local
				// harbor_id是本地的
				struct harbor_msg * m;
				while ((m = pop_queue(s->queue)) != NULL) {
					int type = m->header.destination >> HANDLE_REMOTE_SHIFT;
					skynet_send(context, m->header.source, handle , type | PTYPE_TAG_DONTCOPY, m->header.session, m->buffer, m->size);
				}
				release_queue(s->queue);  // 释放队列
				s->queue = NULL;
			}
		}
		return;
	}
	// 有连接，直接发送队列中的消息
	struct harbor_msg * m;
	while ((m = pop_queue(queue)) != NULL) {
		m->header.destination |= (handle & HANDLE_MASK);  // 设置完整的目标地址
		send_remote(context, fd, m->buffer, m->size, &m->header);  // 发送远程消息
		skynet_free(m->buffer);  // 释放消息缓冲区
	}
}

// 分发指定harbor节点的消息队列
static void
dispatch_queue(struct harbor *h, int id) {
	struct slave *s = &h->s[id];
	int fd = s->fd;
	assert(fd != 0);  // 确保连接存在

	struct harbor_msg_queue *queue = s->queue;
	if (queue == NULL)
		return;  // 队列为空

	// 发送队列中的所有消息
	struct harbor_msg * m;
	while ((m = pop_queue(queue)) != NULL) {
		send_remote(h->ctx, fd, m->buffer, m->size, &m->header);  // 发送远程消息
		skynet_free(m->buffer);  // 释放消息缓冲区
	}
	release_queue(queue);  // 释放队列
	s->queue = NULL;
}

// 处理socket接收到的数据
static void
push_socket_data(struct harbor *h, const struct skynet_socket_message * message) {
	assert(message->type == SKYNET_SOCKET_TYPE_DATA);
	int fd = message->id;
	int i;
	int id = 0;
	struct slave * s = NULL;
	// 根据fd查找对应的slave节点
	for (i=1;i<REMOTE_MAX;i++) {
		if (h->s[i].fd == fd) {
			s = &h->s[i];
			id = i;
			break;
		}
	}
	if (s == NULL) {
		skynet_error(h->ctx, "Invalid socket fd (%d) data", fd);
		return;
	}
	uint8_t * buffer = (uint8_t *)message->buffer;
	int size = message->ud;

	// 状态机处理接收数据
	for (;;) {
		switch(s->status) {
		case STATUS_HANDSHAKE: {
			// check id
			// 检查握手ID
			uint8_t remote_id = buffer[0];
			if (remote_id != id) {
				skynet_error(h->ctx, "Invalid shakehand id (%d) from fd = %d , harbor = %d", id, fd, remote_id);
				close_harbor(h,id);
				return;
			}
			++buffer;  // 跳过ID字节
			--size;
			s->status = STATUS_HEADER;  // 切换到读取消息头状态

			dispatch_queue(h, id);  // 分发等待队列中的消息

			if (size == 0) {
				break;  // 数据处理完毕
			}
			// go though
			// 继续处理
		}
		case STATUS_HEADER: {
			// big endian 4 bytes length, the first one must be 0.
			// 大端序4字节长度，第一个字节必须是0
			int need = 4 - s->read;  // 还需要读取的字节数
			if (size < need) {
				// 数据不够，先缓存
				memcpy(s->size + s->read, buffer, size);
				s->read += size;
				return;
			} else {
				// 数据足够，读取完整的长度信息
				memcpy(s->size + s->read, buffer, need);
				buffer += need;
				size -= need;

				if (s->size[0] != 0) {
					// 消息长度超出限制
					skynet_error(h->ctx, "Message is too long from harbor %d", id);
					close_harbor(h,id);
					return;
				}
				s->length = s->size[1] << 16 | s->size[2] << 8 | s->size[3];  // 解析消息长度
				s->read = 0;
				s->recv_buffer = skynet_malloc(s->length);  // 分配接收缓冲区
				s->status = STATUS_CONTENT;  // 切换到读取内容状态
				if (size == 0) {
					return;
				}
			}
		}
		// go though
		// 继续处理
		case STATUS_CONTENT: {
			int need = s->length - s->read;  // 还需要读取的字节数
			if (size < need) {
				// 数据不够，先缓存
				memcpy(s->recv_buffer + s->read, buffer, size);
				s->read += size;
				return;
			}
			// 数据足够，读取完整消息
			memcpy(s->recv_buffer + s->read, buffer, need);
			forward_local_messsage(h, s->recv_buffer, s->length);  // 转发本地消息
			s->length = 0;
			s->read = 0;
			s->recv_buffer = NULL;
			size -= need;
			buffer += need;
			s->status = STATUS_HEADER;  // 重置为读取消息头状态
			if (size == 0)
				return;
			break;
		}
		default:
			return;
		}
	}
}

// 更新全局名称映射
static void
update_name(struct harbor *h, const char name[GLOBALNAME_LENGTH], uint32_t handle) {
	struct keyvalue * node = hash_search(h->map, name);  // 搜索现有映射
	if (node == NULL) {
		node = hash_insert(h->map, name);  // 插入新映射
	}
	node->value = handle;  // 更新句柄值
	if (node->queue) {
		// 如果有等待队列，分发消息
		dispatch_name_queue(h, node);
		release_queue(node->queue);
		node->queue = NULL;
	}
}

// 按句柄发送远程消息
static int
remote_send_handle(struct harbor *h, uint32_t source, uint32_t destination, int type, int session, const char * msg, size_t sz) {
	int harbor_id = destination >> HANDLE_REMOTE_SHIFT;  // 提取harbor ID
	struct skynet_context * context = h->ctx;
	if (harbor_id == h->id) {
		// local message
		// 本地消息，直接发送
		skynet_send(context, source, destination , type | PTYPE_TAG_DONTCOPY, session, (void *)msg, sz);
		return 1;
	}

	struct slave * s = &h->s[harbor_id];
	if (s->fd == 0 || s->status == STATUS_HANDSHAKE) {
		// 连接不存在或正在握手
		if (s->status == STATUS_DOWN) {
			// throw an error return to source
			// report the destination is dead
			// 向源发送错误响应，报告目标已死
			skynet_send(context, destination, source, PTYPE_ERROR, session, NULL, 0);
			skynet_error(context, "Drop message to harbor %d from %x to %x (session = %d, msgsz = %d)",harbor_id, source, destination,session,(int)sz);
		} else {
			// 加入等待队列
			if (s->queue == NULL) {
				s->queue = new_queue();
			}
			struct remote_message_header header;
			header.source = source;
			header.destination = (type << HANDLE_REMOTE_SHIFT) | (destination & HANDLE_MASK);
			header.session = (uint32_t)session;
			push_queue(s->queue, (void *)msg, sz, &header);
			return 1;
		}
	} else {
		// 连接正常，直接发送
		struct remote_message_header cookie;
		cookie.source = source;
		cookie.destination = (destination & HANDLE_MASK) | ((uint32_t)type << HANDLE_REMOTE_SHIFT);
		cookie.session = (uint32_t)session;
		send_remote(context, s->fd, msg,sz,&cookie);
	}

	return 0;
}

// 按名称发送远程消息
static int
remote_send_name(struct harbor *h, uint32_t source, const char name[GLOBALNAME_LENGTH], int type, int session, const char * msg, size_t sz) {
	struct keyvalue * node = hash_search(h->map, name);  // 搜索名称映射
	if (node == NULL) {
		node = hash_insert(h->map, name);  // 插入新的名称映射
	}
	if (node->value == 0) {
		// 名称还未解析，加入等待队列
		if (node->queue == NULL) {
			node->queue = new_queue();
		}
		struct remote_message_header header;
		header.source = source;
		header.destination = type << HANDLE_REMOTE_SHIFT;
		header.session = (uint32_t)session;
		push_queue(node->queue, (void *)msg, sz, &header);
		// 向slave服务查询名称
		char query[2+GLOBALNAME_LENGTH+1] = "Q ";
		query[2+GLOBALNAME_LENGTH] = 0;
		memcpy(query+2, name, GLOBALNAME_LENGTH);
		skynet_send(h->ctx, 0, h->slave, PTYPE_TEXT, 0, query, strlen(query));
		return 1;
	} else {
		// 名称已解析，直接按句柄发送
		return remote_send_handle(h, source, node->value, type, session, msg, sz);
	}
}

// 发送握手消息
static void
handshake(struct harbor *h, int id) {
	struct slave *s = &h->s[id];
	uint8_t handshake[1] = { (uint8_t)h->id };  // 发送本节点ID
	struct socket_sendbuffer tmp;
	tmp.id = s->fd;
	tmp.type = SOCKET_BUFFER_RAWPOINTER;
	tmp.buffer = handshake;
	tmp.sz = 1;
	skynet_socket_sendbuffer(h->ctx, &tmp);
}

// 处理harbor命令
static void
harbor_command(struct harbor * h, const char * msg, size_t sz, int session, uint32_t source) {
	const char * name = msg + 2;  // 跳过命令前缀
	int s = (int)sz;
	s -= 2;
	switch(msg[0]) {
	case 'N' : {
		// 名称注册命令
		if (s <=0 || s>= GLOBALNAME_LENGTH) {
			skynet_error(h->ctx, "Invalid global name %s", name);
			return;
		}
		struct remote_name rn;
		memset(&rn, 0, sizeof(rn));
		memcpy(rn.name, name, s);  // 复制名称
		rn.handle = source;        // 设置句柄
		update_name(h, rn.name, rn.handle);  // 更新名称映射
		break;
	}
	case 'S' :
	case 'A' : {
		// 连接建立命令（Accept/Start）
		char buffer[s+1];
		memcpy(buffer, name, s);
		buffer[s] = 0;
		int fd=0, id=0;
		sscanf(buffer, "%d %d",&fd,&id);  // 解析fd和harbor ID
		if (fd == 0 || id <= 0 || id>=REMOTE_MAX) {
			skynet_error(h->ctx, "Invalid command %c %s", msg[0], buffer);
			return;
		}
		struct slave * slave = &h->s[id];
		if (slave->fd != 0) {
			skynet_error(h->ctx, "Harbor %d alreay exist", id);
			return;
		}
		slave->fd = fd;  // 设置socket文件描述符

		skynet_socket_start(h->ctx, fd);  // 启动socket
		handshake(h, id);                 // 发送握手消息
		if (msg[0] == 'S') {
			// Start命令：等待对方握手
			slave->status = STATUS_HANDSHAKE;
		} else {
			// Accept命令：直接进入消息头读取状态
			slave->status = STATUS_HEADER;
			dispatch_queue(h,id);  // 分发等待队列中的消息
		}
		break;
	}
	default:
		skynet_error(h->ctx, "Unknown command %s", msg);
		return;
	}
}

// 根据socket fd查找harbor ID
static int
harbor_id(struct harbor *h, int fd) {
	int i;
	for (i=1;i<REMOTE_MAX;i++) {
		struct slave *s = &h->s[i];
		if (s->fd == fd) {
			return i;  // 返回harbor ID
		}
	}
	return 0;  // 未找到
}

// harbor服务的主消息循环
static int
mainloop(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct harbor * h = ud;
	switch (type) {
	case PTYPE_SOCKET: {
		// 处理socket消息
		const struct skynet_socket_message * message = msg;
		switch(message->type) {
		case SKYNET_SOCKET_TYPE_DATA:
			// 接收到socket数据
			push_socket_data(h, message);  // 处理接收的数据
			skynet_free(message->buffer);  // 释放缓冲区
			break;
		case SKYNET_SOCKET_TYPE_ERROR:
		case SKYNET_SOCKET_TYPE_CLOSE: {
			// socket连接错误或关闭
			int id = harbor_id(h, message->id);
			if (id) {
				report_harbor_down(h,id);  // 报告harbor节点下线
			} else {
				skynet_error(context, "Unknown fd (%d) closed", message->id);
			}
			break;
		}
		case SKYNET_SOCKET_TYPE_CONNECT:
			// fd forward to this service
			// fd转发到此服务
			break;
		case SKYNET_SOCKET_TYPE_WARNING: {
			// socket发送缓冲区警告
			int id = harbor_id(h, message->id);
			if (id) {
				skynet_error(context, "message havn't send to Harbor (%d) reach %d K", id, message->ud);
			}
			break;
		}
		default:
			skynet_error(context, "recv invalid socket message type %d", type);
			break;
		}
		return 0;
	}
	case PTYPE_HARBOR: {
		// 处理harbor命令消息
		harbor_command(h, msg,sz,session,source);
		return 0;
	}
	case PTYPE_SYSTEM : {
		// remote message out
		// 远程消息输出
		const struct remote_message *rmsg = msg;
		if (rmsg->destination.handle == 0) {
			// 按名称发送远程消息
			if (remote_send_name(h, source , rmsg->destination.name, rmsg->type, session, rmsg->message, rmsg->sz)) {
				return 0;
			}
		} else {
			// 按句柄发送远程消息
			if (remote_send_handle(h, source , rmsg->destination.handle, rmsg->type, session, rmsg->message, rmsg->sz)) {
				return 0;
			}
		}
		skynet_free((void *)rmsg->message);  // 释放消息内存
		return 0;
	}
	default:
		// 无效的消息类型
		skynet_error(context, "recv invalid message from %x,  type = %d", source, type);
		if (session != 0 && type != PTYPE_ERROR) {
			// 发送错误响应
			skynet_send(context,0,source,PTYPE_ERROR, session, NULL, 0);
		}
		return 0;
	}
}

// harbor服务的初始化函数
int
harbor_init(struct harbor *h, struct skynet_context *ctx, const char * args) {
	h->ctx = ctx;  // 设置上下文
	int harbor_id = 0;
	uint32_t slave = 0;
	sscanf(args,"%d %u", &harbor_id, &slave);  // 解析参数：harbor ID和slave服务句柄
	if (slave == 0) {
		// slave服务句柄无效
		return 1;  // 初始化失败
	}
	h->id = harbor_id;    // 设置harbor节点ID
	h->slave = slave;     // 设置slave服务句柄
	if (harbor_id == 0) {
		// 如果是主harbor节点，关闭所有远程连接
		close_all_remotes(h);
	}
	skynet_callback(ctx, h, mainloop);  // 设置消息处理回调
	skynet_harbor_start(ctx);           // 启动harbor服务

	return 0;  // 初始化成功
}
