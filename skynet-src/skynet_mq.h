/*
 * skynet_mq.h - skynet消息队列头文件
 * 定义了消息结构和消息队列的管理接口
 */

#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

/*
 * skynet消息结构体
 * 用于服务间通信的基本消息单元
 */
struct skynet_message {
	uint32_t source;  // 消息源handle
	int session;      // 会话ID
	void * data;      // 消息数据指针
	size_t sz;        // 消息大小（高8位编码消息类型）
};

// type is encoding in skynet_message.sz high 8bit
// 消息类型编码在skynet_message.sz字段的高8位
// 消息大小掩码
#define MESSAGE_TYPE_MASK (SIZE_MAX >> 8)
// 消息类型位移
#define MESSAGE_TYPE_SHIFT ((sizeof(size_t)-1) * 8)

// 前向声明
struct message_queue;

/*
 * 全局消息队列管理
 */

// 将消息队列推入全局队列
void skynet_globalmq_push(struct message_queue * queue);

// 从全局队列弹出消息队列
struct message_queue * skynet_globalmq_pop(void);

/*
 * 消息队列生命周期管理
 */

// 创建消息队列
struct message_queue * skynet_mq_create(uint32_t handle);

// 标记消息队列为待释放状态
void skynet_mq_mark_release(struct message_queue *q);

// 消息丢弃回调函数类型
typedef void (*message_drop)(struct skynet_message *, void *);

// 释放消息队列
void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud);

// 获取消息队列对应的handle
uint32_t skynet_mq_handle(struct message_queue *);

/*
 * 消息队列操作
 */

// 0 for success
// 从消息队列弹出消息（成功返回0）
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);

// 向消息队列推入消息
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

/*
 * 消息队列状态查询（用于调试）
 */

// return the length of message queue, for debug
// 返回消息队列长度
int skynet_mq_length(struct message_queue *q);

// 检查消息队列是否过载
int skynet_mq_overload(struct message_queue *q);

/*
 * 消息队列系统初始化
 */
void skynet_mq_init();

#endif
