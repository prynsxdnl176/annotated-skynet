/*
 * skynet_handle.c - skynet服务handle管理模块
 * 负责服务handle的分配、注册、查找和名称管理
 */

#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_imp.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4        // 默认槽位大小
#define MAX_SLOT_SIZE 0x40000000   // 最大槽位大小

/*
 * 服务名称映射结构体
 * 用于将字符串名称映射到handle
 */
struct handle_name {
	char * name;        // 服务名称
	uint32_t handle;    // 对应的handle
};

/*
 * handle存储管理结构体
 * 管理所有服务的handle分配和名称映射
 */
struct handle_storage {
	struct rwlock lock;                 // 读写锁，保护并发访问

	uint32_t harbor;                    // 节点ID（高8位）
	uint32_t handle_index;              // 下一个可分配的handle索引
	int slot_size;                      // 槽位数组大小
	struct skynet_context ** slot;     // 服务上下文槽位数组

	int name_cap;                       // 名称数组容量
	int name_count;                     // 当前名称数量
	struct handle_name *name;           // 名称映射数组
};

// 全局handle存储实例
static struct handle_storage *H = NULL;

/*
 * 注册服务上下文，分配新的handle
 * 使用哈希表存储服务上下文，当哈希冲突时自动扩容
 * @param ctx: 要注册的服务上下文
 * @return: 分配的handle（包含节点ID）
 */
uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);  // 获取写锁

	for (;;) {
		int i;
		uint32_t handle = s->handle_index;
		// 在当前槽位大小范围内查找空闲位置
		for (i=0;i<s->slot_size;i++,handle++) {
			if (handle > HANDLE_MASK) {
				// 0 is reserved
				// handle 0 被系统保留，从1开始分配
				handle = 1;
			}
			int hash = handle & (s->slot_size-1);  // 计算哈希值
			if (s->slot[hash] == NULL) {
				// 找到空闲槽位，注册服务
				s->slot[hash] = ctx;
				s->handle_index = handle + 1;

				rwlock_wunlock(&s->lock);

				handle |= s->harbor;  // 添加节点ID到handle高位
				return handle;
			}
		}
		// 槽位已满，需要扩容（容量翻倍）
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));

		// 重新哈希所有现有服务到新的槽位数组
		for (i=0;i<s->slot_size;i++) {
			if (s->slot[i]) {
				int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
				assert(new_slot[hash] == NULL);
				new_slot[hash] = s->slot[i];
			}
		}
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;  // 容量翻倍
	}
}

/*
 * 注销指定handle的服务
 * 从槽位数组中移除服务，并清理相关的名称映射
 * @param handle: 要注销的服务handle
 * @return: 成功返回1，失败返回0
 */
int
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);  // 获取写锁

	uint32_t hash = handle & (s->slot_size-1);  // 计算哈希值
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		// 找到对应的服务，从槽位中移除
		s->slot[hash] = NULL;
		ret = 1;

		// 清理该handle对应的所有名称映射
		int i;
		int j=0, n=s->name_count;
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				// 释放名称字符串内存
				skynet_free(s->name[i].name);
				continue;  // 跳过这个条目
			} else if (i!=j) {
				// 压缩数组，移除空隙
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;  // 更新名称数量
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);  // 释放写锁

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		// 先释放锁再释放上下文，因为释放上下文可能会调用其他handle函数
		skynet_context_release(ctx);
	}

	return ret;
}

/*
 * 注销所有handle
 * 遍历所有槽位，注销每个活跃的服务
 * 循环执行直到所有服务都被注销
 */
void
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;  // 活跃服务计数
		int i;
		// 遍历所有槽位
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);  // 获取读锁
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx) {
				handle = skynet_context_handle(ctx);
				++n;  // 发现活跃服务
			}
			rwlock_runlock(&s->lock);  // 释放读锁
			if (handle != 0) {
				// 注销找到的服务
				skynet_handle_retire(handle);
			}
		}
		if (n==0)  // 没有活跃服务，退出循环
			return;
	}
}

/*
 * 通过handle获取服务上下文（增加引用计数）
 * 在读锁保护下查找服务，并增加引用计数防止被释放
 * @param handle: 要查找的服务handle
 * @return: 服务上下文指针，未找到返回NULL
 */
struct skynet_context *
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	rwlock_rlock(&s->lock);  // 获取读锁

	uint32_t hash = handle & (s->slot_size-1);  // 计算哈希值
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		// 找到匹配的服务，增加引用计数
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);  // 释放读锁

	return result;
}

/*
 * 通过名称查找handle
 * 使用二分查找在有序的名称数组中查找指定名称
 * @param name: 要查找的服务名称
 * @return: 对应的handle，未找到返回0
 */
uint32_t
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);  // 获取读锁

	uint32_t handle = 0;

	// 二分查找算法
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			// 找到匹配的名称
			handle = n->handle;
			break;
		}
		if (c<0) {
			// 目标名称在右半部分
			begin = mid + 1;
		} else {
			// 目标名称在左半部分
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);  // 释放读锁

	return handle;
}

/*
 * 在指定位置之前插入名称映射
 * 如果数组容量不足会自动扩容，保持数组的有序性
 * @param s: handle存储结构
 * @param name: 服务名称字符串
 * @param handle: 对应的handle
 * @param before: 插入位置索引
 */
static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
		// 容量不足，扩容为原来的2倍
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		// 复制插入位置之前的元素
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		// 复制插入位置之后的元素（留出一个位置）
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		// 容量足够，直接移动元素
		int i;
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	// 插入新的名称映射
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

/*
 * 插入名称映射到有序数组中
 * 使用二分查找确定插入位置，保持数组有序
 * @param s: handle存储结构
 * @param name: 要插入的服务名称
 * @param handle: 对应的handle
 * @return: 成功返回复制的名称字符串，名称已存在返回NULL
 */
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	// 二分查找确定插入位置
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			// 名称已存在，插入失败
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	// 复制名称字符串
	char * result = skynet_strdup(name);

	// 在确定的位置插入名称映射
	_insert_name_before(s, result, handle, begin);

	return result;
}

/*
 * 为handle绑定名称
 * 在写锁保护下将名称和handle的映射关系插入到有序数组中
 * @param handle: 要绑定名称的handle
 * @param name: 要绑定的名称
 * @return: 成功返回复制的名称字符串，失败返回NULL
 */
const char *
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);  // 获取写锁

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);  // 释放写锁

	return ret;
}

/*
 * 初始化handle管理系统
 * 创建全局handle存储结构，初始化槽位数组和名称数组
 * @param harbor: 节点ID（0-255）
 */
void
skynet_handle_init(int harbor) {
	assert(H==NULL);  // 确保只初始化一次
	struct handle_storage * s = skynet_malloc(sizeof(*H));

	// 初始化槽位数组
	s->slot_size = DEFAULT_SLOT_SIZE;
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	// 初始化读写锁
	rwlock_init(&s->lock);
	// reserve 0 for system
	// 设置节点ID（保留handle 0给系统使用）
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;  // 从1开始分配handle

	// 初始化名称数组
	s->name_cap = 2;
	s->name_count = 0;
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;  // 设置全局实例

	// Don't need to free H
	// 注意：H不需要释放，程序结束时由系统回收
}
