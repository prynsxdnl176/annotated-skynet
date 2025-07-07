/*
	modify from https://github.com/cloudwu/lua-serialize
	// 修改自 https://github.com/cloudwu/lua-serialize
 */

#define LUA_LIB

#include "skynet_malloc.h"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

// 数据类型定义
#define TYPE_NIL 0          // nil 类型
#define TYPE_BOOLEAN 1      // 布尔类型
// hibits 0 false 1 true
// 高位 0 表示 false，1 表示 true
#define TYPE_NUMBER 2       // 数字类型
// hibits 0 : 0 , 1: byte, 2:word, 4: dword, 6: qword, 8 : double
// 高位 0: 0值, 1: 字节, 2: 字, 4: 双字, 6: 四字, 8: 双精度浮点数
#define TYPE_NUMBER_ZERO 0  // 数字 0
#define TYPE_NUMBER_BYTE 1  // 字节数字
#define TYPE_NUMBER_WORD 2  // 字数字
#define TYPE_NUMBER_DWORD 4 // 双字数字
#define TYPE_NUMBER_QWORD 6 // 四字数字
#define TYPE_NUMBER_REAL 8  // 实数

#define TYPE_USERDATA 3     // 用户数据类型
#define TYPE_SHORT_STRING 4 // 短字符串类型
// hibits 0~31 : len
// 高位 0~31: 长度
#define TYPE_LONG_STRING 5  // 长字符串类型
#define TYPE_TABLE 6        // 表类型

#define MAX_COOKIE 32       // 最大 cookie 值
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)  // 组合类型和值

#define BLOCK_SIZE 128      // 块大小
#define MAX_DEPTH 32        // 最大深度

// 内存块结构，用于链式存储序列化数据
struct block {
	struct block * next;        // 指向下一个块
	char buffer[BLOCK_SIZE];    // 数据缓冲区
};

// 写入块结构，用于序列化时的数据写入
struct write_block {
	struct block * head;        // 头块指针
	struct block * current;     // 当前块指针
	int len;                    // 总长度
	int ptr;                    // 当前位置指针
};

// 读取块结构，用于反序列化时的数据读取
struct read_block {
	char * buffer;              // 数据缓冲区
	int len;                    // 缓冲区长度
	int ptr;                    // 当前读取位置
};

// 分配一个新的内存块
inline static struct block *
blk_alloc(void) {
	struct block *b = skynet_malloc(sizeof(struct block));
	b->next = NULL;
	return b;
}

// 向写入块中推入数据
inline static void
wb_push(struct write_block *b, const void *buf, int sz) {
	const char * buffer = buf;
	if (b->ptr == BLOCK_SIZE) {  // 当前块已满
_again:
		// 分配新块并链接
		b->current = b->current->next = blk_alloc();
		b->ptr = 0;
	}
	if (b->ptr <= BLOCK_SIZE - sz) {
		// 当前块有足够空间，直接复制
		memcpy(b->current->buffer + b->ptr, buffer, sz);
		b->ptr+=sz;
		b->len+=sz;
	} else {
		// 当前块空间不足，需要分割数据
		int copy = BLOCK_SIZE - b->ptr;
		memcpy(b->current->buffer + b->ptr, buffer, copy);
		buffer += copy;
		b->len += copy;
		sz -= copy;
		goto _again;  // 继续处理剩余数据
	}
}

// 初始化写入块
static void
wb_init(struct write_block *wb , struct block *b) {
	wb->head = b;
	assert(b->next == NULL);
	wb->len = 0;
	wb->current = wb->head;
	wb->ptr = 0;
}

// 释放写入块的内存
static void
wb_free(struct write_block *wb) {
	struct block *blk = wb->head;
	blk = blk->next;	// the first block is on stack
	                    // 第一个块在栈上，不需要释放
	while (blk) {
		struct block * next = blk->next;
		skynet_free(blk);
		blk = next;
	}
	wb->head = NULL;
	wb->current = NULL;
	wb->ptr = 0;
	wb->len = 0;
}

// 初始化读取块
static void
rball_init(struct read_block * rb, char * buffer, int size) {
	rb->buffer = buffer;
	rb->len = size;
	rb->ptr = 0;
}

// 从读取块中读取指定大小的数据
static const void *
rb_read(struct read_block *rb, int sz) {
	if (rb->len < sz) {
		return NULL;  // 数据不足
	}

	int ptr = rb->ptr;
	rb->ptr += sz;
	rb->len -= sz;
	return rb->buffer + ptr;
}

// 写入 nil 值
static inline void
wb_nil(struct write_block *wb) {
	uint8_t n = TYPE_NIL;
	wb_push(wb, &n, 1);
}

// 写入布尔值
static inline void
wb_boolean(struct write_block *wb, int boolean) {
	uint8_t n = COMBINE_TYPE(TYPE_BOOLEAN , boolean ? 1 : 0);
	wb_push(wb, &n, 1);
}

// 写入整数值，根据数值大小选择最优的存储格式
static inline void
wb_integer(struct write_block *wb, lua_Integer v) {
	int type = TYPE_NUMBER;
	if (v == 0) {
		// 特殊处理 0 值
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_ZERO);
		wb_push(wb, &n, 1);
	} else if (v != (int32_t)v) {
		// 需要 64 位存储
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_QWORD);
		int64_t v64 = v;
		wb_push(wb, &n, 1);
		wb_push(wb, &v64, sizeof(v64));
	} else if (v < 0) {
		// 负数用 32 位存储
		int32_t v32 = (int32_t)v;
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);
		wb_push(wb, &n, 1);
		wb_push(wb, &v32, sizeof(v32));
	} else if (v<0x100) {
		// 小于 256，用 1 字节存储
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_BYTE);
		wb_push(wb, &n, 1);
		uint8_t byte = (uint8_t)v;
		wb_push(wb, &byte, sizeof(byte));
	} else if (v<0x10000) {
		// 小于 65536，用 2 字节存储
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_WORD);
		wb_push(wb, &n, 1);
		uint16_t word = (uint16_t)v;
		wb_push(wb, &word, sizeof(word));
	} else {
		// 其他情况用 4 字节存储
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);
		wb_push(wb, &n, 1);
		uint32_t v32 = (uint32_t)v;
		wb_push(wb, &v32, sizeof(v32));
	}
}

// 写入实数（双精度浮点数）
static inline void
wb_real(struct write_block *wb, double v) {
	uint8_t n = COMBINE_TYPE(TYPE_NUMBER , TYPE_NUMBER_REAL);
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

// 写入指针（用户数据）
static inline void
wb_pointer(struct write_block *wb, void *v) {
	uint8_t n = TYPE_USERDATA;
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

// 写入字符串，根据长度选择短字符串或长字符串格式
static inline void
wb_string(struct write_block *wb, const char *str, int len) {
	if (len < MAX_COOKIE) {
		// 短字符串：长度直接编码在类型字节中
		uint8_t n = COMBINE_TYPE(TYPE_SHORT_STRING, len);
		wb_push(wb, &n, 1);
		if (len > 0) {
			wb_push(wb, str, len);
		}
	} else {
		// 长字符串：需要额外的长度字段
		uint8_t n;
		if (len < 0x10000) {
			// 长度用 2 字节存储
			n = COMBINE_TYPE(TYPE_LONG_STRING, 2);
			wb_push(wb, &n, 1);
			uint16_t x = (uint16_t) len;
			wb_push(wb, &x, 2);
		} else {
			// 长度用 4 字节存储
			n = COMBINE_TYPE(TYPE_LONG_STRING, 4);
			wb_push(wb, &n, 1);
			uint32_t x = (uint32_t) len;
			wb_push(wb, &x, 4);
		}
		wb_push(wb, str, len);
	}
}

// 前向声明：打包单个值的函数
static void pack_one(lua_State *L, struct write_block *b, int index, int depth);

// 写入表的数组部分
static int
wb_table_array(lua_State *L, struct write_block * wb, int index, int depth) {
	int array_size = lua_rawlen(L,index);  // 获取数组长度
	if (array_size >= MAX_COOKIE-1) {
		// 数组长度太大，需要额外存储长度信息
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);
		wb_push(wb, &n, 1);
		wb_integer(wb, array_size);
	} else {
		// 数组长度可以直接编码在类型字节中
		uint8_t n = COMBINE_TYPE(TYPE_TABLE, array_size);
		wb_push(wb, &n, 1);
	}

	// 序列化数组元素
	int i;
	for (i=1;i<=array_size;i++) {
		lua_rawgeti(L,index,i);
		pack_one(L, wb, -1, depth);
		lua_pop(L,1);
	}

	return array_size;
}

// 写入表的哈希部分
static void
wb_table_hash(lua_State *L, struct write_block * wb, int index, int depth, int array_size) {
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		if (lua_type(L,-2) == LUA_TNUMBER) {
			if (lua_isinteger(L, -2)) {
				lua_Integer x = lua_tointeger(L,-2);
				if (x>0 && x<=array_size) {
					// 跳过已经在数组部分处理的键
					lua_pop(L,1);
					continue;
				}
			}
		}
		// 序列化键值对
		pack_one(L,wb,-2,depth);  // 键
		pack_one(L,wb,-1,depth);  // 值
		lua_pop(L, 1);
	}
	wb_nil(wb);  // 哈希部分结束标记
}

// 使用元方法 __pairs 处理表
static int
wb_table_metapairs(lua_State *L, struct write_block *wb, int index, int depth) {
	uint8_t n = COMBINE_TYPE(TYPE_TABLE, 0);
	wb_push(wb, &n, 1);
	lua_pushvalue(L, index);
	if (lua_pcall(L, 1, 3,0) != LUA_OK)  // 调用 __pairs 元方法
		return 1;
	for(;;) {
		// 调用迭代器函数
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_copy(L, -5, -3);
		if (lua_pcall(L, 2, 2, 0) != LUA_OK)
			return 1;
		int type = lua_type(L, -2);
		if (type == LUA_TNIL) {
			// 迭代结束
			lua_pop(L, 4);
			break;
		}
		// 序列化键值对
		pack_one(L, wb, -2, depth);
		pack_one(L, wb, -1, depth);
		lua_pop(L, 1);
	}
	wb_nil(wb);  // 结束标记
	return 0;
}

// 写入表数据
static int
wb_table(lua_State *L, struct write_block *wb, int index, int depth) {
	if (!lua_checkstack(L, LUA_MINSTACK)) {
		lua_pushstring(L, "out of memory");
		return 1;
	}
	if (index < 0) {
		// 转换为正索引
		index = lua_gettop(L) + index + 1;
	}
	if (luaL_getmetafield(L, index, "__pairs") != LUA_TNIL) {
		// 表有 __pairs 元方法，使用元方法处理
		return wb_table_metapairs(L, wb, index, depth);
	} else {
		// 标准表处理：先处理数组部分，再处理哈希部分
		int array_size = wb_table_array(L, wb, index, depth);
		wb_table_hash(L, wb, index, depth, array_size);
		return 0;
	}
}

// 打包单个 Lua 值
static void
pack_one(lua_State *L, struct write_block *b, int index, int depth) {
	if (depth > MAX_DEPTH) {
		// 防止无限递归，限制嵌套深度
		wb_free(b);
		luaL_error(L, "serialize can't pack too depth table");
	}
	int type = lua_type(L,index);
	switch(type) {
	case LUA_TNIL:
		wb_nil(b);
		break;
	case LUA_TNUMBER: {
		if (lua_isinteger(L, index)) {
			// 整数
			lua_Integer x = lua_tointeger(L,index);
			wb_integer(b, x);
		} else {
			// 浮点数
			lua_Number n = lua_tonumber(L,index);
			wb_real(b,n);
		}
		break;
	}
	case LUA_TBOOLEAN:
		wb_boolean(b, lua_toboolean(L,index));
		break;
	case LUA_TSTRING: {
		size_t sz = 0;
		const char *str = lua_tolstring(L,index,&sz);
		wb_string(b, str, (int)sz);
		break;
	}
	case LUA_TLIGHTUSERDATA:
		wb_pointer(b, lua_touserdata(L,index));
		break;
	case LUA_TTABLE: {
		if (index < 0) {
			index = lua_gettop(L) + index + 1;
		}
		if (wb_table(L, b, index, depth+1)) {
			wb_free(b);
			lua_error(L);
		}
		break;
	}
	default:
		// 不支持的类型
		wb_free(b);
		luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
	}
}

// 从指定位置开始打包栈上的所有值
static void
pack_from(lua_State *L, struct write_block *b, int from) {
	int n = lua_gettop(L) - from;  // 计算要打包的值的数量
	int i;
	for (i=1;i<=n;i++) {
		pack_one(L, b , from + i, 0);
	}
}

// 报告无效的序列化流错误
static inline void
invalid_stream_line(lua_State *L, struct read_block *rb, int line) {
	int len = rb->len;
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

// 宏定义：报告无效流错误，包含行号信息
#define invalid_stream(L,rb) invalid_stream_line(L,rb,__LINE__)

// 根据 cookie 值读取整数
static lua_Integer
get_integer(lua_State *L, struct read_block *rb, int cookie) {
	switch (cookie) {
	case TYPE_NUMBER_ZERO:
		return 0;  // 特殊值 0
	case TYPE_NUMBER_BYTE: {
		// 1 字节整数
		uint8_t n;
		const uint8_t * pn = (const uint8_t *)rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		n = *pn;
		return n;
	}
	case TYPE_NUMBER_WORD: {
		// 2 字节整数
		uint16_t n;
		const void * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_DWORD: {
		// 4 字节整数
		int32_t n;
		const void * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_QWORD: {
		// 8 字节整数
		int64_t n;
		const void * pn = rb_read(rb,sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	default:
		invalid_stream(L,rb);
		return 0;
	}
}

// 读取双精度浮点数
static double
get_real(lua_State *L, struct read_block *rb) {
	double n;
	const void * pn = rb_read(rb,sizeof(n));
	if (pn == NULL)
		invalid_stream(L,rb);
	memcpy(&n, pn, sizeof(n));
	return n;
}

// 读取指针（用户数据）
static void *
get_pointer(lua_State *L, struct read_block *rb) {
	void * userdata = 0;
	const void * v = rb_read(rb,sizeof(userdata));
	if (v == NULL) {
		invalid_stream(L,rb);
	}
	memcpy(&userdata, v, sizeof(userdata));
	return userdata;
}

// 读取指定长度的缓冲区数据并推入 Lua 栈
static void
get_buffer(lua_State *L, struct read_block *rb, int len) {
	const char * p = (const char *)rb_read(rb,len);
	if (p == NULL) {
		invalid_stream(L,rb);
	}
	lua_pushlstring(L,p,len);  // 创建 Lua 字符串
}

// 前向声明：反序列化单个值的函数
static void unpack_one(lua_State *L, struct read_block *rb);

// 反序列化表数据
static void
unpack_table(lua_State *L, struct read_block *rb, int array_size) {
	if (array_size == MAX_COOKIE-1) {
		// 数组大小需要额外读取
		uint8_t type;
		const uint8_t * t = (const uint8_t *)rb_read(rb, sizeof(type));
		if (t==NULL) {
			invalid_stream(L,rb);
		}
		type = *t;
		int cookie = type >> 3;
		if ((type & 7) != TYPE_NUMBER || cookie == TYPE_NUMBER_REAL) {
			invalid_stream(L,rb);
		}
		array_size = get_integer(L,rb,cookie);
	}
	luaL_checkstack(L,LUA_MINSTACK,NULL);
	lua_createtable(L,array_size,0);  // 创建新表

	// 反序列化数组部分
	int i;
	for (i=1;i<=array_size;i++) {
		unpack_one(L,rb);
		lua_rawseti(L,-2,i);
	}

	// 反序列化哈希部分
	for (;;) {
		unpack_one(L,rb);  // 读取键
		if (lua_isnil(L,-1)) {
			// nil 键表示哈希部分结束
			lua_pop(L,1);
			return;
		}
		unpack_one(L,rb);  // 读取值
		lua_rawset(L,-3);  // 设置键值对
	}
}

// 根据类型和 cookie 推入相应的值到 Lua 栈
static void
push_value(lua_State *L, struct read_block *rb, int type, int cookie) {
	switch(type) {
	case TYPE_NIL:
		lua_pushnil(L);
		break;
	case TYPE_BOOLEAN:
		lua_pushboolean(L,cookie);
		break;
	case TYPE_NUMBER:
		if (cookie == TYPE_NUMBER_REAL) {
			// 浮点数
			lua_pushnumber(L,get_real(L,rb));
		} else {
			// 整数
			lua_pushinteger(L, get_integer(L, rb, cookie));
		}
		break;
	case TYPE_USERDATA:
		lua_pushlightuserdata(L,get_pointer(L,rb));
		break;
	case TYPE_SHORT_STRING:
		// 短字符串，长度在 cookie 中
		get_buffer(L,rb,cookie);
		break;
	case TYPE_LONG_STRING: {
		// 长字符串，需要读取长度
		if (cookie == 2) {
			// 2 字节长度
			const void * plen = rb_read(rb, 2);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint16_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		} else {
			// 4 字节长度
			if (cookie != 4) {
				invalid_stream(L,rb);
			}
			const void * plen = rb_read(rb, 4);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint32_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		}
		break;
	}
	case TYPE_TABLE: {
		unpack_table(L,rb,cookie);
		break;
	}
	default: {
		invalid_stream(L,rb);
		break;
	}
	}
}

static void
// 反序列化单个值
unpack_one(lua_State *L, struct read_block *rb) {
	uint8_t type;
	const uint8_t * t = (const uint8_t *)rb_read(rb, sizeof(type));
	if (t==NULL) {
		invalid_stream(L, rb);
	}
	type = *t;
	push_value(L, rb, type & 0x7, type>>3);  // 解析类型和值
}

// 将链式块数据序列化为连续内存
static void
seri(lua_State *L, struct block *b, int len) {
	uint8_t * buffer = skynet_malloc(len);  // 分配连续内存
	uint8_t * ptr = buffer;
	int sz = len;
	while(len>0) {
		if (len >= BLOCK_SIZE) {
			// 复制完整块
			memcpy(ptr, b->buffer, BLOCK_SIZE);
			ptr += BLOCK_SIZE;
			len -= BLOCK_SIZE;
			b = b->next;
		} else {
			// 复制剩余数据
			memcpy(ptr, b->buffer, len);
			break;
		}
	}

	// 返回缓冲区指针和大小
	lua_pushlightuserdata(L, buffer);
	lua_pushinteger(L, sz);
}

// Lua 接口：反序列化函数
int
luaseri_unpack(lua_State *L) {
	if (lua_isnoneornil(L,1)) {
		return 0;  // 没有数据需要反序列化
	}
	void * buffer;
	int len;
	if (lua_type(L,1) == LUA_TSTRING) {
		// 输入是字符串
		size_t sz;
		 buffer = (void *)lua_tolstring(L,1,&sz);
		len = (int)sz;
	} else {
		// 输入是用户数据指针和长度
		buffer = lua_touserdata(L,1);
		len = luaL_checkinteger(L,2);
	}
	if (len == 0) {
		return 0;  // 空数据
	}
	if (buffer == NULL) {
		return luaL_error(L, "deserialize null pointer");
	}

	lua_settop(L,1);
	struct read_block rb;
	rball_init(&rb, buffer, len);

	// 循环反序列化所有值
	int i;
	for (i=0;;i++) {
		if (i%8==7) {
			// 定期检查栈空间
			luaL_checkstack(L,LUA_MINSTACK,NULL);
		}
		uint8_t type = 0;
		const uint8_t * t = (const uint8_t *)rb_read(&rb, sizeof(type));
		if (t==NULL)
			break;  // 数据读取完毕
		type = *t;
		push_value(L, &rb, type & 0x7, type>>3);
	}

	// Need not free buffer
	// 不需要释放缓冲区（由调用者管理）

	return lua_gettop(L) - 1;  // 返回反序列化的值的数量
}

// Lua 接口：序列化函数
LUAMOD_API int
luaseri_pack(lua_State *L) {
	struct block temp;
	temp.next = NULL;
	struct write_block wb;
	wb_init(&wb, &temp);
	pack_from(L,&wb,0);  // 序列化栈上的所有参数
	assert(wb.head == &temp);
	seri(L, &temp, wb.len);  // 转换为连续内存

	wb_free(&wb);  // 释放临时块

	return 2;  // 返回缓冲区指针和大小
}
