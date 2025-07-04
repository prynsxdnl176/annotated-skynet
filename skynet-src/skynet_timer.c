/*
 * skynet_timer.c - skynet定时器系统
 * 实现高效的分层时间轮定时器，支持大量定时器的管理
 * 使用多级时间轮算法，提供毫秒级精度的定时功能
 */

#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// 定时器执行函数类型定义
typedef void (*timer_execute_func)(void *ud,void *arg);

// 时间轮配置常量
#define TIME_NEAR_SHIFT 8                   // 近期时间轮的位移（8位，256个槽）
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)    // 近期时间轮大小（256）
#define TIME_LEVEL_SHIFT 6                  // 各级时间轮的位移（6位，64个槽）
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)  // 各级时间轮大小（64）
#define TIME_NEAR_MASK (TIME_NEAR-1)        // 近期时间轮掩码（255）
#define TIME_LEVEL_MASK (TIME_LEVEL-1)      // 各级时间轮掩码（63）

/*
 * 定时器事件结构体
 * 存储定时器触发时要发送的消息信息
 */
struct timer_event {
	uint32_t handle;    // 目标服务的handle
	int session;        // 会话ID
};

/*
 * 定时器节点结构体
 * 时间轮中的基本节点，包含过期时间和链表指针
 */
struct timer_node {
	struct timer_node *next;    // 链表中的下一个节点
	uint32_t expire;            // 过期时间（相对时间）
};

/*
 * 链表结构体
 * 时间轮中每个槽位的链表，存储相同时间槽的定时器
 */
struct link_list {
	struct timer_node head;     // 链表头节点
	struct timer_node *tail;    // 链表尾指针
};

/*
 * 定时器系统结构体
 * 实现分层时间轮的核心数据结构
 */
struct timer {
	struct link_list near[TIME_NEAR];       // 近期时间轮（256个槽，处理0-255的时间）
	struct link_list t[4][TIME_LEVEL];      // 4级时间轮（每级64个槽）
	struct spinlock lock;                   // 自旋锁，保护定时器操作
	uint32_t time;                          // 当前时间（定时器滴答）
	uint32_t starttime;                     // 系统启动时间
	uint64_t current;                       // 当前时间（毫秒）
	uint64_t current_point;                 // 当前时间点
};

// 全局定时器实例
static struct timer * TI = NULL;

static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;
	uint32_t current_time=T->time;
	
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
	} else {
		int i;
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}

		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}

/*
 * 添加定时器到时间轮
 * @param T: 定时器系统
 * @param arg: 定时器事件数据
 * @param sz: 事件数据大小
 * @param time: 延迟时间（相对时间）
 */
static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {
	// 分配定时器节点，节点后面紧跟事件数据
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);  // 复制事件数据到节点后面

	SPIN_LOCK(T);

	node->expire=time+T->time;  // 设置绝对过期时间
	add_node(T,node);           // 将节点添加到合适的时间轮槽位

	SPIN_UNLOCK(T);
}

/*
 * 移动指定级别和索引的定时器列表
 * 当时间轮进位时，将高级时间轮的定时器移动到低级时间轮
 * @param T: 定时器系统
 * @param level: 时间轮级别
 * @param idx: 槽位索引
 */
static void
move_list(struct timer *T, int level, int idx) {
	struct timer_node *current = link_clear(&T->t[level][idx]);
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);  // 重新添加到合适的槽位
		current=temp;
	}
}

/*
 * 时间轮进位操作
 * 当近期时间轮走完一圈时，需要将高级时间轮的定时器移动下来
 * @param T: 定时器系统
 */
static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;
	uint32_t ct = ++T->time;  // 时间前进一个滴答

	if (ct == 0) {
		// 时间溢出，移动最高级时间轮
		move_list(T, 3, 0);
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i=0;

		// 检查哪一级时间轮需要进位
		while ((ct & (mask-1))==0) {
			int idx=time & TIME_LEVEL_MASK;
			if (idx!=0) {
				move_list(T, i, idx);  // 移动对应级别的定时器
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}

/*
 * 分发定时器事件列表
 * 将到期的定时器转换为消息发送给对应的服务
 * @param current: 定时器节点链表头
 */
static inline void
dispatch_list(struct timer_node *current) {
	do {
		// 获取定时器事件数据
		struct timer_event * event = (struct timer_event *)(current+1);

		// 构造定时器消息
		struct skynet_message message;
		message.source = 0;                                           // 来源为0（系统）
		message.session = event->session;                             // 会话ID
		message.data = NULL;                                          // 无数据
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;    // 响应类型消息

		skynet_context_push(event->handle, &message);  // 发送消息到目标服务

		// 释放定时器节点
		struct timer_node * temp = current;
		current=current->next;
		skynet_free(temp);
	} while (current);
}

/*
 * 执行当前时间槽的所有定时器
 * @param T: 定时器系统
 */
static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;  // 计算当前时间槽索引
	
	while (T->near[idx].head.next) {
		struct timer_node *current = link_clear(&T->near[idx]);
		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		// dispatch_list不需要锁定T
		dispatch_list(current);
		SPIN_LOCK(T);
	}
}

static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	// 尝试分发超时时间为0的定时器（罕见情况）
	timer_execute(T);

	// shift time first, and then dispatch timer message
	timer_shift(T);

	timer_execute(T);

	SPIN_UNLOCK(T);
}

static struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	SPIN_INIT(r)

	r->current = 0;

	return r;
}

int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time <= 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second
// 厘秒：1/100秒
static void
systime(uint32_t *sec, uint32_t *cs) {
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
}

static uint64_t
gettime() {
	uint64_t t;
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
	return t;
}

void
skynet_updatetime(void) {
	uint64_t cp = gettime();
	if(cp < TI->current_point) {
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} else if (cp != TI->current_point) {
		uint32_t diff = (uint32_t)(cp - TI->current_point);
		TI->current_point = cp;
		TI->current += diff;
		int i;
		for (i=0;i<diff;i++) {
			timer_update(TI);
		}
	}
}

uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

uint64_t 
skynet_now(void) {
	return TI->current;
}

void 
skynet_timer_init(void) {
	TI = timer_create_timer();
	uint32_t current = 0;
	systime(&TI->starttime, &current);
	TI->current = current;
	TI->current_point = gettime();
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

uint64_t
skynet_thread_time(void) {
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
}
