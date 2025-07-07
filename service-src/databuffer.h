#ifndef skynet_databuffer_h
#define skynet_databuffer_h

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MESSAGEPOOL 1023  // 消息池大小，每个池包含1023个消息节点

// 消息节点结构，用于存储单个数据块
struct message {
	char * buffer;        // 数据缓冲区指针
	int size;            // 数据大小
	struct message * next; // 指向下一个消息节点的指针
};

// 数据缓冲区结构，用于管理消息链表
struct databuffer {
	int header;          // 消息头长度（2或4字节）
	int offset;          // 当前消息节点的读取偏移量
	int size;            // 缓冲区中剩余的总数据大小
	struct message * head; // 消息链表头指针
	struct message * tail; // 消息链表尾指针
};

// 消息池链表节点，用于管理多个消息池
struct messagepool_list {
	struct messagepool_list *next; // 指向下一个消息池的指针
	struct message pool[MESSAGEPOOL]; // 消息池数组
};

// 消息池管理结构，用于复用消息节点以减少内存分配
struct messagepool {
	struct messagepool_list * pool; // 消息池链表头指针
	struct message * freelist;     // 空闲消息节点链表头指针
};

// use memset init struct
// 使用 memset 初始化结构体

// 释放消息池中的所有内存
static void
messagepool_free(struct messagepool *pool) {
	struct messagepool_list *p = pool->pool;
	while(p) {
		struct messagepool_list *tmp = p;
		p=p->next;
		skynet_free(tmp);  // 释放消息池节点内存
	}
	pool->pool = NULL;     // 清空池链表头指针
	pool->freelist = NULL; // 清空空闲链表头指针
}

// 将已使用的消息节点归还到消息池中
static inline void
_return_message(struct databuffer *db, struct messagepool *mp) {
	struct message *m = db->head;
	if (m->next == NULL) {
		assert(db->tail == m);
		db->head = db->tail = NULL; // 链表为空，清空头尾指针
	} else {
		db->head = m->next;         // 移动头指针到下一个节点
	}
	skynet_free(m->buffer);         // 释放消息数据缓冲区
	m->buffer = NULL;               // 清空缓冲区指针
	m->size = 0;                    // 重置大小
	m->next = mp->freelist;         // 将节点加入空闲链表
	mp->freelist = m;               // 更新空闲链表头指针
}

// 从数据缓冲区中读取指定大小的数据
static void
databuffer_read(struct databuffer *db, struct messagepool *mp, char * buffer, int sz) {
	assert(db->size >= sz);  // 确保缓冲区有足够的数据
	db->size -= sz;          // 减少缓冲区剩余数据大小
	for (;;) {
		struct message *current = db->head;
		int bsz = current->size - db->offset; // 当前消息节点剩余数据大小
		if (bsz > sz) {
			// 当前节点数据足够，直接复制并更新偏移量
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset += sz;
			return;
		}
		if (bsz == sz) {
			// 当前节点数据刚好够用，复制后归还节点
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset = 0;
			_return_message(db, mp);
			return;
		} else {
			// 当前节点数据不够，复制全部并继续下一个节点
			memcpy(buffer, current->buffer + db->offset, bsz);
			_return_message(db, mp);
			db->offset = 0;
			buffer+=bsz;  // 移动目标缓冲区指针
			sz-=bsz;      // 减少剩余需要读取的大小
		}
	}
}

// 向数据缓冲区添加新的数据
static void
databuffer_push(struct databuffer *db, struct messagepool *mp, void *data, int sz) {
	struct message * m;
	if (mp->freelist) {
		// 从空闲链表中获取消息节点
		m = mp->freelist;
		mp->freelist = m->next;
	} else {
		// 空闲链表为空，分配新的消息池
		struct messagepool_list * mpl = skynet_malloc(sizeof(*mpl));
		struct message * temp = mpl->pool;
		int i;
		// 初始化新消息池，将所有节点链接成空闲链表
		for (i=1;i<MESSAGEPOOL;i++) {
			temp[i].buffer = NULL;
			temp[i].size = 0;
			temp[i].next = &temp[i+1];
		}
		temp[MESSAGEPOOL-1].next = NULL; // 最后一个节点的next为NULL
		mpl->next = mp->pool;            // 将新池加入池链表
		mp->pool = mpl;
		m = &temp[0];                    // 使用第一个节点
		mp->freelist = &temp[1];         // 其余节点加入空闲链表
	}
	m->buffer = data;  // 设置数据指针
	m->size = sz;      // 设置数据大小
	m->next = NULL;    // 清空next指针
	db->size += sz;    // 增加缓冲区总大小
	if (db->head == NULL) {
		// 缓冲区为空，设置头尾指针
		assert(db->tail == NULL);
		db->head = db->tail = m;
	} else {
		// 将新节点添加到链表尾部
		db->tail->next = m;
		db->tail = m;
	}
}

// 读取并解析消息头，返回消息体长度
static int
databuffer_readheader(struct databuffer *db, struct messagepool *mp, int header_size) {
	if (db->header == 0) {
		// parser header (2 or 4)
		// 解析消息头（2字节或4字节）
		if (db->size < header_size) {
			return -1;  // 数据不足，无法读取完整头部
		}
		uint8_t plen[4];
		databuffer_read(db,mp,(char *)plen,header_size);
		// big-endian
		// 大端字节序解析头部长度
		if (header_size == 2) {
			db->header = plen[0] << 8 | plen[1];  // 2字节头部
		} else {
			db->header = plen[0] << 24 | plen[1] << 16 | plen[2] << 8 | plen[3];  // 4字节头部
		}
	}
	if (db->size < db->header)
		return -1;  // 消息体数据不完整
	return db->header;  // 返回消息体长度
}

// 重置数据缓冲区的消息头状态
static inline void
databuffer_reset(struct databuffer *db) {
	db->header = 0;  // 清空已解析的消息头长度
}

// 清空数据缓冲区，释放所有消息节点
static void
databuffer_clear(struct databuffer *db, struct messagepool *mp) {
	while (db->head) {
		_return_message(db,mp);  // 归还所有消息节点到消息池
	}
	memset(db, 0, sizeof(*db));  // 重置缓冲区结构体
}

#endif
