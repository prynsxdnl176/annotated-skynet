#define LUA_LIB

#include "skynet_malloc.h"

#include "skynet_socket.h"

#include <lua.h>
#include <lauxlib.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 队列和哈希表大小定义
#define QUEUESIZE 1024  // 队列大小
#define HASHSIZE 4096   // 哈希表大小
#define SMALLSTRING 2048 // 小字符串大小

// 网络包类型定义
#define TYPE_DATA 1     // 数据包
#define TYPE_MORE 2     // 需要更多数据
#define TYPE_ERROR 3    // 错误
#define TYPE_OPEN 4     // 连接打开
#define TYPE_CLOSE 5    // 连接关闭
#define TYPE_WARNING 6  // 警告
#define TYPE_INIT 7     // 初始化

/*
	Each package is uint16 + data , uint16 (serialized in big-endian) is the number of bytes comprising the data .
	每个包的格式是 uint16 + data，uint16（大端序）表示数据的字节数。
 */

// 网络包结构
struct netpack {
	int id;         // 连接ID
	int size;       // 数据大小
	void * buffer;  // 数据缓冲区
};

// 未完成的包结构
struct uncomplete {
	struct netpack pack;        // 网络包
	struct uncomplete * next;   // 链表下一个节点
	int read;                   // 已读取字节数
	int header;                 // 包头信息
};

// 网络包队列结构
struct queue {
	int cap;                            // 队列容量
	int head;                           // 队列头
	int tail;                           // 队列尾
	struct uncomplete * hash[HASHSIZE]; // 未完成包的哈希表
	struct netpack queue[QUEUESIZE];    // 完成包的队列
};

// 清理未完成包链表
static void
clear_list(struct uncomplete * uc) {
	while (uc) {
		skynet_free(uc->pack.buffer);  // 释放包缓冲区
		void * tmp = uc;
		uc = uc->next;
		skynet_free(tmp);              // 释放节点内存
	}
}

// Lua 接口：清理队列
static int
lclear(lua_State *L) {
	struct queue * q = lua_touserdata(L, 1);
	if (q == NULL) {
		return 0;
	}
	int i;
	// 清理哈希表中的未完成包
	for (i=0;i<HASHSIZE;i++) {
		clear_list(q->hash[i]);
		q->hash[i] = NULL;
	}
	// 清理队列中的完成包
	if (q->head > q->tail) {
		q->tail += q->cap;  // 处理环形队列的回绕
	}
	for (i=q->head;i<q->tail;i++) {
		struct netpack *np = &q->queue[i % q->cap];
		skynet_free(np->buffer);  // 释放包缓冲区
	}
	q->head = q->tail = 0;  // 重置队列指针

	return 0;
}

// 计算文件描述符的哈希值
static inline int
hash_fd(int fd) {
	int a = fd >> 24;  // 高8位
	int b = fd >> 12;  // 中12位
	int c = fd;        // 全部位
	return (int)(((uint32_t)(a + b + c)) % HASHSIZE);
}

// 查找并移除未完成的包
static struct uncomplete *
find_uncomplete(struct queue *q, int fd) {
	if (q == NULL)
		return NULL;
	int h = hash_fd(fd);  // 计算哈希值
	struct uncomplete * uc = q->hash[h];
	if (uc == NULL)
		return NULL;
	if (uc->pack.id == fd) {
		// 找到目标，从链表头移除
		q->hash[h] = uc->next;
		return uc;
	}
	// 在链表中查找
	struct uncomplete * last = uc;
	while (last->next) {
		uc = last->next;
		if (uc->pack.id == fd) {
			// 找到目标，从链表中移除
			last->next = uc->next;
			return uc;
		}
		last = uc;
	}
	return NULL;
}

// 获取或创建队列
static struct queue *
get_queue(lua_State *L) {
	struct queue *q = lua_touserdata(L,1);
	if (q == NULL) {
		// 创建新队列
		q = lua_newuserdatauv(L, sizeof(struct queue), 0);
		q->cap = QUEUESIZE;
		q->head = 0;
		q->tail = 0;
		int i;
		// 初始化哈希表
		for (i=0;i<HASHSIZE;i++) {
			q->hash[i] = NULL;
		}
		lua_replace(L, 1);
	}
	return q;
}

// 扩展队列容量
static void
expand_queue(lua_State *L, struct queue *q) {
	// 创建更大的队列
	struct queue *nq = lua_newuserdatauv(L, sizeof(struct queue) + q->cap * sizeof(struct netpack), 0);
	nq->cap = q->cap + QUEUESIZE;  // 增加容量
	nq->head = 0;
	nq->tail = q->cap;
	// 复制哈希表
	memcpy(nq->hash, q->hash, sizeof(nq->hash));
	memset(q->hash, 0, sizeof(q->hash));
	// 复制队列数据
	int i;
	for (i=0;i<q->cap;i++) {
		int idx = (q->head + i) % q->cap;
		nq->queue[i] = q->queue[idx];
	}
	q->head = q->tail = 0;
	lua_replace(L,1);  // 替换原队列
}

// 推送数据到队列
static void
push_data(lua_State *L, int fd, void *buffer, int size, int clone) {
	if (clone) {
		// 需要克隆数据
		void * tmp = skynet_malloc(size);
		memcpy(tmp, buffer, size);
		buffer = tmp;
	}
	struct queue *q = get_queue(L);
	struct netpack *np = &q->queue[q->tail];
	if (++q->tail >= q->cap)
		q->tail -= q->cap;  // 环形队列回绕
	np->id = fd;
	np->buffer = buffer;
	np->size = size;
	if (q->head == q->tail) {
		// 队列已满，需要扩展
		expand_queue(L, q);
	}
}

// 保存未完成的包
static struct uncomplete *
save_uncomplete(lua_State *L, int fd) {
	struct queue *q = get_queue(L);
	int h = hash_fd(fd);  // 计算哈希值
	struct uncomplete * uc = skynet_malloc(sizeof(struct uncomplete));
	memset(uc, 0, sizeof(*uc));
	uc->next = q->hash[h];  // 插入到链表头
	uc->pack.id = fd;
	q->hash[h] = uc;

	return uc;
}

// 读取包大小（大端序）
static inline int
read_size(uint8_t * buffer) {
	int r = (int)buffer[0] << 8 | (int)buffer[1];  // 大端序转换
	return r;
}

// 处理更多数据包（递归处理多个包）
static void
push_more(lua_State *L, int fd, uint8_t *buffer, int size) {
	if (size == 1) {
		// 只有一个字节，保存为不完整包的头部
		struct uncomplete * uc = save_uncomplete(L, fd);
		uc->read = -1;        // 标记为读取头部状态
		uc->header = *buffer; // 保存头部字节
		return;
	}
	int pack_size = read_size(buffer);  // 读取包大小
	buffer += 2;  // 跳过大小字段
	size -= 2;

	if (size < pack_size) {
		// 数据不完整，保存到不完整包中
		struct uncomplete * uc = save_uncomplete(L, fd);
		uc->read = size;                           // 已读取的字节数
		uc->pack.size = pack_size;                 // 包的总大小
		uc->pack.buffer = skynet_malloc(pack_size); // 分配缓冲区
		memcpy(uc->pack.buffer, buffer, size);     // 复制已有数据
		return;
	}
	push_data(L, fd, buffer, pack_size, 1);  // 推送完整的数据包

	buffer += pack_size;  // 移动到下一个包
	size -= pack_size;
	if (size > 0) {
		push_more(L, fd, buffer, size);  // 递归处理剩余数据
	}
}

// 关闭不完整的连接，释放相关资源
static void
close_uncomplete(lua_State *L, int fd) {
	struct queue *q = lua_touserdata(L,1);
	struct uncomplete * uc = find_uncomplete(q, fd);  // 查找不完整包
	if (uc) {
		skynet_free(uc->pack.buffer);  // 释放缓冲区
		skynet_free(uc);               // 释放不完整包结构
	}
}

// 过滤数据的核心函数（处理不完整包的拼接）
static int
filter_data_(lua_State *L, int fd, uint8_t * buffer, int size) {
	struct queue *q = lua_touserdata(L,1);
	struct uncomplete * uc = find_uncomplete(q, fd);  // 查找不完整包
	if (uc) {
		// fill uncomplete
		// 填充不完整包
		if (uc->read < 0) {
			// read size
			// 读取包大小
			assert(uc->read == -1);
			int pack_size = *buffer;           // 低字节
			pack_size |= uc->header << 8 ;     // 高字节（之前保存的）
			++buffer;
			--size;
			uc->pack.size = pack_size;                 // 设置包大小
			uc->pack.buffer = skynet_malloc(pack_size); // 分配缓冲区
			uc->read = 0;                              // 重置读取位置
		}
		int need = uc->pack.size - uc->read;  // 还需要的字节数
		if (size < need) {
			// 数据仍然不够，继续保存
			memcpy(uc->pack.buffer + uc->read, buffer, size);
			uc->read += size;
			int h = hash_fd(fd);
			uc->next = q->hash[h];  // 重新插入哈希表
			q->hash[h] = uc;
			return 1;
		}
		// 数据足够完成这个包
		memcpy(uc->pack.buffer + uc->read, buffer, need);
		buffer += need;
		size -= need;
		if (size == 0) {
			// 正好完成一个包
			lua_pushvalue(L, lua_upvalueindex(TYPE_DATA));
			lua_pushinteger(L, fd);
			lua_pushlightuserdata(L, uc->pack.buffer);
			lua_pushinteger(L, uc->pack.size);
			skynet_free(uc);  // 释放不完整包结构
			return 5;
		}
		// more data
		// 还有更多数据
		push_data(L, fd, uc->pack.buffer, uc->pack.size, 0);  // 推送完成的包
		skynet_free(uc);                                      // 释放不完整包结构
		push_more(L, fd, buffer, size);                       // 处理剩余数据
		lua_pushvalue(L, lua_upvalueindex(TYPE_MORE));
		return 2;
	} else {
		// 没有不完整包，处理新数据
		if (size == 1) {
			// 只有一个字节，保存为不完整包头部
			struct uncomplete * uc = save_uncomplete(L, fd);
			uc->read = -1;        // 标记为读取头部状态
			uc->header = *buffer; // 保存头部字节
			return 1;
		}
		int pack_size = read_size(buffer);  // 读取包大小
		buffer+=2;
		size-=2;

		if (size < pack_size) {
			// 数据不完整，保存为不完整包
			struct uncomplete * uc = save_uncomplete(L, fd);
			uc->read = size;                           // 已读取字节数
			uc->pack.size = pack_size;                 // 包总大小
			uc->pack.buffer = skynet_malloc(pack_size); // 分配缓冲区
			memcpy(uc->pack.buffer, buffer, size);     // 复制数据
			return 1;
		}
		if (size == pack_size) {
			// just one package
			// 正好一个完整包
			lua_pushvalue(L, lua_upvalueindex(TYPE_DATA));
			lua_pushinteger(L, fd);
			void * result = skynet_malloc(pack_size);  // 分配结果缓冲区
			memcpy(result, buffer, size);              // 复制数据
			lua_pushlightuserdata(L, result);
			lua_pushinteger(L, size);
			return 5;
		}
		// more data
		// 有多个包
		push_data(L, fd, buffer, pack_size, 1);  // 推送第一个包
		buffer += pack_size;
		size -= pack_size;
		push_more(L, fd, buffer, size);          // 处理剩余数据
		lua_pushvalue(L, lua_upvalueindex(TYPE_MORE));
		return 2;
	}
}

// 过滤数据的包装函数（负责释放缓冲区）
static inline int
filter_data(lua_State *L, int fd, uint8_t * buffer, int size) {
	int ret = filter_data_(L, fd, buffer, size);
	// buffer is the data of socket message, it malloc at socket_server.c : function forward_message .
	// it should be free before return,
	// 缓冲区是套接字消息的数据，在 socket_server.c 的 forward_message 函数中分配
	// 返回前应该释放
	skynet_free(buffer);
	return ret;
}

// 推送字符串到 Lua 栈
static void
pushstring(lua_State *L, const char * msg, int size) {
	if (msg) {
		lua_pushlstring(L, msg, size);  // 推送指定长度的字符串
	} else {
		lua_pushliteral(L, "");         // 推送空字符串
	}
}

/*
	userdata queue
	lightuserdata msg
	integer size
	return
		userdata queue
		integer type
		integer fd
		string msg | lightuserdata/integer
 */
// Lua 接口：过滤套接字消息
static int
lfilter(lua_State *L) {
	struct skynet_socket_message *message = lua_touserdata(L,2);
	int size = luaL_checkinteger(L,3);
	char * buffer = message->buffer;
	if (buffer == NULL) {
		buffer = (char *)(message+1);  // 数据紧跟在消息结构后面
		size -= sizeof(*message);
	} else {
		size = -1;  // 使用消息中的 ud 字段作为大小
	}

	lua_settop(L, 1);  // 保留队列参数

	switch(message->type) {
	case SKYNET_SOCKET_TYPE_DATA:
		// ignore listen id (message->id)
		// 忽略监听ID（message->id）
		assert(size == -1);	// never padding string
		                    // 从不填充字符串
		return filter_data(L, message->id, (uint8_t *)buffer, message->ud);
	case SKYNET_SOCKET_TYPE_CONNECT:
		// 连接建立
		lua_pushvalue(L, lua_upvalueindex(TYPE_INIT));
		lua_pushinteger(L, message->id);
		lua_pushlstring(L, buffer, size);
		lua_pushinteger(L, message->ud);
		return 5;
	case SKYNET_SOCKET_TYPE_CLOSE:
		// no more data in fd (message->id)
		// fd（message->id）中没有更多数据
		close_uncomplete(L, message->id);  // 清理不完整包
		lua_pushvalue(L, lua_upvalueindex(TYPE_CLOSE));
		lua_pushinteger(L, message->id);
		return 3;
	case SKYNET_SOCKET_TYPE_ACCEPT:
		// 接受新连接
		lua_pushvalue(L, lua_upvalueindex(TYPE_OPEN));
		// ignore listen id (message->id);
		// 忽略监听ID（message->id）
		lua_pushinteger(L, message->ud);  // 新连接的fd
		pushstring(L, buffer, size);
		return 4;
	case SKYNET_SOCKET_TYPE_ERROR:
		// no more data in fd (message->id)
		// fd（message->id）中没有更多数据
		close_uncomplete(L, message->id);  // 清理不完整包
		lua_pushvalue(L, lua_upvalueindex(TYPE_ERROR));
		lua_pushinteger(L, message->id);
		pushstring(L, buffer, size);      // 错误信息
		return 4;
	case SKYNET_SOCKET_TYPE_WARNING:
		// 警告消息
		lua_pushvalue(L, lua_upvalueindex(TYPE_WARNING));
		lua_pushinteger(L, message->id);
		lua_pushinteger(L, message->ud);
		return 4;
	default:
		// never get here
		// 永远不会到达这里
		return 1;
	}
}

/*
	userdata queue
	return
		integer fd
		lightuserdata msg
		integer size
 */
// Lua 接口：从队列中弹出一个网络包
static int
lpop(lua_State *L) {
	struct queue * q = lua_touserdata(L, 1);
	if (q == NULL || q->head == q->tail)
		return 0;  // 队列为空
	struct netpack *np = &q->queue[q->head];  // 获取队列头部的包
	if (++q->head >= q->cap) {
		q->head = 0;  // 环形队列，回到开头
	}
	lua_pushinteger(L, np->id);           // 推送fd
	lua_pushlightuserdata(L, np->buffer); // 推送消息缓冲区
	lua_pushinteger(L, np->size);         // 推送消息大小

	return 3;
}

/*
	string msg | lightuserdata/integer

	lightuserdata/integer

	参数：字符串消息或轻量用户数据/整数
	返回：轻量用户数据/整数
 */

static const char *
// 获取字符串或用户数据
tolstring(lua_State *L, size_t *sz, int index) {
	const char * ptr;
	if (lua_isuserdata(L,index)) {
		// 用户数据指针 + 大小
		ptr = (const char *)lua_touserdata(L,index);
		*sz = (size_t)luaL_checkinteger(L, index+1);
	} else {
		// Lua 字符串
		ptr = luaL_checklstring(L, index, sz);
	}
	return ptr;
}

// 写入包大小（大端序）
static inline void
write_size(uint8_t * buffer, int len) {
	buffer[0] = (len >> 8) & 0xff;  // 高字节
	buffer[1] = len & 0xff;         // 低字节
}

// Lua 接口：打包数据
static int
lpack(lua_State *L) {
	size_t len;
	const char * ptr = tolstring(L, &len, 1);
	if (len >= 0x10000) {
		return luaL_error(L, "Invalid size (too long) of data : %d", (int)len);
	}

	// 分配缓冲区（2字节头 + 数据）
	uint8_t * buffer = skynet_malloc(len + 2);
	write_size(buffer, len);        // 写入大小头
	memcpy(buffer+2, ptr, len);     // 复制数据

	lua_pushlightuserdata(L, buffer);  // 返回缓冲区指针
	lua_pushinteger(L, len + 2);       // 返回总大小

	return 2;
}

// Lua 接口：用户数据转字符串
static int
ltostring(lua_State *L) {
	void * ptr = lua_touserdata(L, 1);
	int size = luaL_checkinteger(L, 2);
	if (ptr == NULL) {
		lua_pushliteral(L, "");
	} else {
		lua_pushlstring(L, (const char *)ptr, size);
		skynet_free(ptr);  // 释放内存
	}
	return 1;
}

// netpack 模块初始化函数
LUAMOD_API int
luaopen_skynet_netpack(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "pop", lpop },           // 弹出数据包
		{ "pack", lpack },         // 打包数据
		{ "clear", lclear },       // 清理队列
		{ "tostring", ltostring }, // 转换为字符串
		{ NULL, NULL },
	};
	luaL_newlib(L,l);

	// the order is same with macros : TYPE_* (defined top)
	// 顺序与顶部定义的 TYPE_* 宏相同
	lua_pushliteral(L, "data");     // TYPE_DATA
	lua_pushliteral(L, "more");     // TYPE_MORE
	lua_pushliteral(L, "error");    // TYPE_ERROR
	lua_pushliteral(L, "open");     // TYPE_OPEN
	lua_pushliteral(L, "close");    // TYPE_CLOSE
	lua_pushliteral(L, "warning");  // TYPE_WARNING
	lua_pushliteral(L, "init");     // TYPE_INIT

	// 创建 filter 函数（带7个上值）
	lua_pushcclosure(L, lfilter, 7);
	lua_setfield(L, -2, "filter");

	return 1;
}
