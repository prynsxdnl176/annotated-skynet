/*
 * skynet_mq.c - skynet消息队列管理模块
 * 负责消息队列的创建、销毁、消息的推送和弹出，以及全局队列的管理
 */

#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64   // 默认队列大小
#define MAX_GLOBAL_MQ 0x10000   // 全局队列最大大小

// 消息队列状态标志
// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.
// 0 表示消息队列不在全局队列中
// 1 表示消息队列在全局队列中，或者消息正在分发中

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024        // 过载阈值

/*
 * 消息队列结构体
 * 每个服务都有一个对应的消息队列
 */
struct message_queue {
	struct spinlock lock;           // 自旋锁，保护队列操作
	uint32_t handle;                // 队列所属服务的handle
	int cap;                        // 队列容量
	int head;                       // 队列头部索引
	int tail;                       // 队列尾部索引
	int release;                    // 释放标志
	int in_global;                  // 是否在全局队列中
	int overload;                   // 过载计数
	int overload_threshold;         // 过载阈值
	struct skynet_message *queue;   // 消息数组
	struct message_queue *next;     // 全局队列中的下一个节点
};

/*
 * 全局消息队列结构体
 * 管理所有有消息待处理的服务队列
 */
struct global_queue {
	struct message_queue *head;     // 队列头部
	struct message_queue *tail;     // 队列尾部
	struct spinlock lock;           // 自旋锁，保护全局队列操作
};

// 全局消息队列实例
static struct global_queue *Q = NULL;

/*
 * 将消息队列推入全局队列
 * 当服务有消息需要处理时，将其队列加入全局队列等待工作线程处理
 * @param queue: 要推入的消息队列
 */
void
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	if(q->tail) {
		// 队列不为空，添加到尾部
		q->tail->next = queue;
		q->tail = queue;
	} else {
		// 队列为空，设置为头尾节点
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

/*
 * 从全局队列弹出一个消息队列
 * 工作线程调用此函数获取有消息待处理的服务队列
 * @return: 消息队列指针，如果全局队列为空则返回NULL
 */
struct message_queue *
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		// 从头部弹出队列
		q->head = mq->next;
		if(q->head == NULL) {
			// 队列变空，重置尾指针
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;  // 清除链表指针
	}
	SPIN_UNLOCK(q)

	return mq;
}

/*
 * 创建新的消息队列
 * 为指定handle的服务创建消息队列
 * @param handle: 服务的handle
 * @return: 新创建的消息队列指针
 */
struct message_queue *
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;                                    // 设置所属服务handle
	q->cap = DEFAULT_QUEUE_SIZE;                           // 设置初始容量
	q->head = 0;                                           // 初始化头部索引
	q->tail = 0;                                           // 初始化尾部索引
	SPIN_INIT(q)                                           // 初始化自旋锁
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	// 队列创建时（总是在服务创建和服务初始化之间），
	// 设置in_global标志避免将其推入全局队列
	// 如果服务初始化成功，skynet_context_new会调用skynet_mq_push将其推入全局队列
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;                                        // 释放标志
	q->overload = 0;                                       // 过载计数
	q->overload_threshold = MQ_OVERLOAD;                   // 过载阈值
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);  // 分配消息数组
	q->next = NULL;                                        // 链表指针

	return q;
}

/*
 * 释放消息队列资源
 * @param q: 要释放的消息队列
 */
static void
_release(struct message_queue *q) {
	assert(q->next == NULL);  // 确保队列不在链表中
	SPIN_DESTROY(q)           // 销毁自旋锁
	skynet_free(q->queue);    // 释放消息数组
	skynet_free(q);           // 释放队列结构体
}

/*
 * 获取消息队列对应的服务handle
 * @param q: 消息队列
 * @return: 服务handle
 */
uint32_t
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

/*
 * 获取消息队列的当前长度
 * 在锁保护下计算环形队列中的消息数量
 * @param q: 消息队列
 * @return: 队列中的消息数量
 */
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)

	if (head <= tail) {
		// 正常情况：尾部在头部之后
		return tail - head;
	}
	// 环形队列回绕情况
	return tail + cap - head;
}

/*
 * 检查并重置消息队列的过载状态
 * 返回过载计数并将其重置为0
 * @param q: 消息队列
 * @return: 过载计数，0表示无过载
 */
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;  // 重置过载计数
		return overload;
	}
	return 0;
}

/*
 * 从消息队列弹出一条消息
 * @param q: 消息队列
 * @param message: 输出参数，存储弹出的消息
 * @return: 0表示成功弹出消息，1表示队列为空
 */
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;  // 默认返回1（队列为空）
	SPIN_LOCK(q)

	if (q->head != q->tail) {
		// 队列不为空，弹出头部消息
		*message = q->queue[q->head++];
		ret = 0;  // 成功弹出
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		// 处理环形队列的头部回绕
		if (head >= cap) {
			q->head = head = 0;
		}

		// 计算当前队列长度
		int length = tail - head;
		if (length < 0) {
			length += cap;  // 处理环形队列的长度计算
		}

		// 动态调整过载阈值
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
		// 队列为空时重置过载阈值
		q->overload_threshold = MQ_OVERLOAD;
	}

	if (ret) {
		// 队列为空，标记不在全局队列中
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

/*
 * 扩展消息队列容量
 * 当队列满时，将容量扩大一倍
 * @param q: 要扩展的消息队列
 */
static void
expand_queue(struct message_queue *q) {
	// 分配新的队列，容量翻倍
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	// 将原队列中的消息按顺序复制到新队列
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;        // 重置头部索引
	q->tail = q->cap;   // 设置尾部索引
	q->cap *= 2;        // 更新容量
	
	skynet_free(q->queue);  // 释放原队列
	q->queue = new_queue;   // 使用新队列
}

/*
 * 向消息队列推送一条消息
 * @param q: 目标消息队列
 * @param message: 要推送的消息
 */
void
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	// 将消息放入队列尾部
	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;  // 环形队列回绕
	}

	// 检查队列是否已满，如果满了则扩展
	if (q->head == q->tail) {
		expand_queue(q);
	}

	// 如果队列不在全局队列中，则将其加入全局队列
	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}

void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
