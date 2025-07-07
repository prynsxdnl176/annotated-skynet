#ifndef skynet_hashid_h
#define skynet_hashid_h

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// 哈希表节点结构，用于存储ID和链表指针
struct hashid_node {
	int id;                    // 存储的ID值
	struct hashid_node *next;  // 指向下一个节点的指针（处理哈希冲突）
};

// 哈希ID管理结构，用于高效存储和查找ID
struct hashid {
	int hashmod;               // 哈希表大小掩码（大小-1）
	int cap;                   // 最大容量
	int count;                 // 当前存储的ID数量
	struct hashid_node *id;    // ID节点数组
	struct hashid_node **hash; // 哈希表数组，存储链表头指针
};

// 初始化哈希ID管理器
static void
hashid_init(struct hashid *hi, int max) {
	int i;
	int hashcap;
	hashcap = 16;
	// 计算哈希表大小，必须是2的幂次方且不小于max
	while (hashcap < max) {
		hashcap *= 2;
	}
	hi->hashmod = hashcap - 1;  // 设置哈希掩码
	hi->cap = max;              // 设置最大容量
	hi->count = 0;              // 初始化计数器
	hi->id = skynet_malloc(max * sizeof(struct hashid_node));  // 分配节点数组
	// 初始化所有节点
	for (i=0;i<max;i++) {
		hi->id[i].id = -1;      // 标记为未使用
		hi->id[i].next = NULL;  // 清空链表指针
	}
	hi->hash = skynet_malloc(hashcap * sizeof(struct hashid_node *));  // 分配哈希表
	memset(hi->hash, 0, hashcap * sizeof(struct hashid_node *));       // 清零哈希表
}

// 清理哈希ID管理器，释放所有内存
static void
hashid_clear(struct hashid *hi) {
	skynet_free(hi->id);    // 释放节点数组内存
	skynet_free(hi->hash);  // 释放哈希表内存
	hi->id = NULL;          // 清空节点数组指针
	hi->hash = NULL;        // 清空哈希表指针
	hi->hashmod = 1;        // 重置哈希掩码
	hi->cap = 0;            // 重置容量
	hi->count = 0;          // 重置计数器
}

// 查找指定ID，返回其在节点数组中的索引
static int
hashid_lookup(struct hashid *hi, int id) {
	int h = id & hi->hashmod;              // 计算哈希值
	struct hashid_node * c = hi->hash[h];  // 获取哈希桶链表头
	while(c) {
		if (c->id == id)
			return c - hi->id;  // 找到ID，返回节点在数组中的索引
		c = c->next;            // 继续查找链表下一个节点
	}
	return -1;  // 未找到，返回-1
}

// 移除指定ID，返回其在节点数组中的索引
static int
hashid_remove(struct hashid *hi, int id) {
	int h = id & hi->hashmod;              // 计算哈希值
	struct hashid_node * c = hi->hash[h];  // 获取哈希桶链表头
	if (c == NULL)
		return -1;  // 哈希桶为空，未找到
	if (c->id == id) {
		// 要删除的是链表头节点
		hi->hash[h] = c->next;
		goto _clear;
	}
	// 在链表中查找要删除的节点
	while(c->next) {
		if (c->next->id == id) {
			struct hashid_node * temp = c->next;
			c->next = temp->next;  // 从链表中移除节点
			c = temp;
			goto _clear;
		}
		c = c->next;
	}
	return -1;  // 未找到要删除的ID
_clear:
	c->id = -1;      // 标记节点为未使用
	c->next = NULL;  // 清空链表指针
	--hi->count;     // 减少计数器
	return c - hi->id;  // 返回节点在数组中的索引
}

// 插入新ID，返回其在节点数组中的索引
static int
hashid_insert(struct hashid * hi, int id) {
	struct hashid_node *c = NULL;
	int i;
	// 使用线性探测法寻找空闲节点
	for (i=0;i<hi->cap;i++) {
		int index = (i+id) % hi->cap;  // 从ID开始线性探测
		if (hi->id[index].id == -1) {
			c = &hi->id[index];  // 找到空闲节点
			break;
		}
	}
	assert(c);        // 确保找到了空闲节点
	++hi->count;      // 增加计数器
	c->id = id;       // 设置节点ID
	assert(c->next == NULL);  // 确保next指针为空
	int h = id & hi->hashmod; // 计算哈希值
	if (hi->hash[h]) {
		// 哈希桶不为空，将新节点插入链表头
		c->next = hi->hash[h];
	}
	hi->hash[h] = c;  // 更新哈希桶头指针

	return c - hi->id;  // 返回节点在数组中的索引
}

// 检查哈希表是否已满
static inline int
hashid_full(struct hashid *hi) {
	return hi->count == hi->cap;  // 当前数量等于最大容量时表示已满
}

#endif
