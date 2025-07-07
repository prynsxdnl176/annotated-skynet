#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "skynet.h"

/*
	uint32_t/string addr
	uint32_t/session session
	lightuserdata msg
	uint32_t sz

	return
		string request
		uint32_t next_session

	集群通信参数说明：
		uint32_t/string addr - 目标地址（数字ID或字符串名称）
		uint32_t/session session - 会话ID
		lightuserdata msg - 消息数据指针
		uint32_t sz - 消息大小
	返回值：
		string request - 打包后的请求数据
		uint32_t next_session - 下一个会话ID
 */

#define TEMP_LENGTH 0x8200  // 临时缓冲区长度 (33280 字节)
#define MULTI_PART 0x8000   // 多部分消息阈值 (32768 字节)

// 将 32 位整数以小端序填入缓冲区
static void
fill_uint32(uint8_t * buf, uint32_t n) {
	buf[0] = n & 0xff;
	buf[1] = (n >> 8) & 0xff;
	buf[2] = (n >> 16) & 0xff;
	buf[3] = (n >> 24) & 0xff;
}

// 填充包头，使用大端序存储大小
static void
fill_header(lua_State *L, uint8_t *buf, int sz) {
	assert(sz < 0x10000);
	buf[0] = (sz >> 8) & 0xff;  // 高字节
	buf[1] = sz & 0xff;         // 低字节
}

/*
	The request package :
		first WORD is size of the package with big-endian
		DWORD in content is small-endian
	请求包格式：
	首个 WORD 是包大小（大端序）
	内容中的 DWORD 是小端序

	size <= 0x8000 (32K) and address is id
		WORD sz+9
		BYTE 0
		DWORD addr
		DWORD session
		PADDING msg(sz)
	大小 <= 0x8000 (32K) 且地址是数字ID：
		WORD sz+9 - 包大小
		BYTE 0 - 类型标识
		DWORD addr - 目标地址
		DWORD session - 会话ID
		PADDING msg(sz) - 消息内容

	size > 0x8000 and address is id
		WORD 13
		BYTE 1	; multireq	, 0x41: multi push
		DWORD addr
		DWORD session
		DWORD sz
	大小 > 0x8000 且地址是数字ID：
		WORD 13 - 包大小
		BYTE 1 - 多请求标识，0x41: 多推送
		DWORD addr - 目标地址
		DWORD session - 会话ID
		DWORD sz - 消息大小

	size <= 0x8000 (32K) and address is string
		WORD sz+6+namelen
		BYTE 0x80
		BYTE namelen
		STRING name
		DWORD session
		PADDING msg(sz)
	大小 <= 0x8000 (32K) 且地址是字符串：
		WORD sz+6+namelen - 包大小
		BYTE 0x80 - 字符串地址标识
		BYTE namelen - 名称长度
		STRING name - 名称字符串
		DWORD session - 会话ID
		PADDING msg(sz) - 消息内容

	size > 0x8000 and address is string
		WORD 10 + namelen
		BYTE 0x81	; 0xc1 : multi push
		BYTE namelen
		STRING name
		DWORD session
		DWORD sz
	大小 > 0x8000 且地址是字符串：
		WORD 10 + namelen - 包大小
		BYTE 0x81 - 字符串多请求标识，0xc1: 多推送
		BYTE namelen - 名称长度
		STRING name - 名称字符串
		DWORD session - 会话ID
		DWORD sz - 消息大小

	multi req
		WORD sz + 5
		BYTE 2/3 ; 2:multipart, 3:multipart end
		DWORD SESSION
		PADDING msgpart(sz)
	多部分请求：
		WORD sz + 5 - 包大小
		BYTE 2/3 - 2:多部分，3:多部分结束
		DWORD SESSION - 会话ID
		PADDING msgpart(sz) - 消息部分内容

	trace
		WORD stringsz + 1
		BYTE 4
		STRING tag
	跟踪信息：
		WORD stringsz + 1 - 包大小
		BYTE 4 - 跟踪标识
		STRING tag - 跟踪标签
 */
// 打包数字地址的请求
static int
packreq_number(lua_State *L, int session, void * msg, uint32_t sz, int is_push) {
	uint32_t addr = (uint32_t)lua_tointeger(L,1);  // 获取目标地址
	uint8_t buf[TEMP_LENGTH];
	if (sz < MULTI_PART) {
		// 小消息：直接打包
		fill_header(L, buf, sz+9);
		buf[2] = 0;  // 类型标识
		fill_uint32(buf+3, addr);  // 目标地址
		fill_uint32(buf+7, is_push ? 0 : (uint32_t)session);  // 会话ID（推送时为0）
		memcpy(buf+11,msg,sz);  // 消息内容

		lua_pushlstring(L, (const char *)buf, sz+11);
		return 0;  // 不需要多部分传输
	} else {
		// 大消息：需要多部分传输
		int part = (sz - 1) / MULTI_PART + 1;  // 计算需要的部分数
		fill_header(L, buf, 13);
		buf[2] = is_push ? 0x41 : 1;	// multi push or request
		                                // 多推送或多请求标识
		fill_uint32(buf+3, addr);       // 目标地址
		fill_uint32(buf+7, (uint32_t)session);  // 会话ID
		fill_uint32(buf+11, sz);        // 消息总大小
		lua_pushlstring(L, (const char *)buf, 15);
		return part;  // 返回需要的部分数
	}
}

// 打包字符串地址的请求
static int
packreq_string(lua_State *L, int session, void * msg, uint32_t sz, int is_push) {
	size_t namelen = 0;
	const char *name = lua_tolstring(L, 1, &namelen);
	if (name == NULL || namelen < 1 || namelen > 255) {
		// 名称验证失败
		skynet_free(msg);
		if (name == NULL) {
			luaL_error(L, "name is not a string, it's a %s", lua_typename(L, lua_type(L, 1)));
		} else {
			luaL_error(L, "name length is invalid, must be between 1 and 255 characters: %s", name);
		}
	}

	uint8_t buf[TEMP_LENGTH];
	if (sz < MULTI_PART) {
		// 小消息：直接打包
		fill_header(L, buf, sz+6+namelen);
		buf[2] = 0x80;  // 字符串地址标识
		buf[3] = (uint8_t)namelen;  // 名称长度
		memcpy(buf+4, name, namelen);  // 名称字符串
		fill_uint32(buf+4+namelen, is_push ? 0 : (uint32_t)session);  // 会话ID
		memcpy(buf+8+namelen,msg,sz);  // 消息内容

		lua_pushlstring(L, (const char *)buf, sz+8+namelen);
		return 0;  // 不需要多部分传输
	} else {
		// 大消息：需要多部分传输
		int part = (sz - 1) / MULTI_PART + 1;  // 计算需要的部分数
		fill_header(L, buf, 10+namelen);
		buf[2] = is_push ? 0xc1 : 0x81;	// multi push or request
		                                    // 字符串多推送或多请求标识
		buf[3] = (uint8_t)namelen;      // 名称长度
		memcpy(buf+4, name, namelen);   // 名称字符串
		fill_uint32(buf+4+namelen, (uint32_t)session);  // 会话ID
		fill_uint32(buf+8+namelen, sz); // 消息总大小

		lua_pushlstring(L, (const char *)buf, 12+namelen);
		return part;  // 返回需要的部分数
	}
}

// 打包多部分消息
static void
packreq_multi(lua_State *L, int session, void * msg, uint32_t sz) {
	uint8_t buf[TEMP_LENGTH];
	int part = (sz - 1) / MULTI_PART + 1;  // 计算部分数
	int i;
	char *ptr = msg;
	for (i=0;i<part;i++) {
		uint32_t s;
		if (sz > MULTI_PART) {
			// 不是最后一部分
			s = MULTI_PART;
			buf[2] = 2;  // 多部分标识
		} else {
			// 最后一部分
			s = sz;
			buf[2] = 3;	// the last multi part
			            // 最后一个多部分标识
		}
		fill_header(L, buf, s+5);
		fill_uint32(buf+3, (uint32_t)session);  // 会话ID
		memcpy(buf+7, ptr, s);  // 复制消息部分
		lua_pushlstring(L, (const char *)buf, s+7);
		lua_rawseti(L, -2, i+1);  // 存入表中
		sz -= s;
		ptr += s;
	}
}

// 打包请求的通用函数
static int
packrequest(lua_State *L, int is_push) {
	void *msg = lua_touserdata(L,3);  // 消息数据指针
	if (msg == NULL) {
		return luaL_error(L, "Invalid request message");
	}
	uint32_t sz = (uint32_t)luaL_checkinteger(L,4);  // 消息大小
	int session = luaL_checkinteger(L,2);  // 会话ID
	if (session <= 0) {
		skynet_free(msg);
		return luaL_error(L, "Invalid request session %d", session);
	}
	int addr_type = lua_type(L,1);  // 地址类型
	int multipak;
	if (addr_type == LUA_TNUMBER) {
		// 数字地址
		multipak = packreq_number(L, session, msg, sz, is_push);
	} else {
		// 字符串地址
		multipak = packreq_string(L, session, msg, sz, is_push);
	}
	// 计算下一个会话ID
	uint32_t new_session = (uint32_t)session + 1;
	if (new_session > INT32_MAX) {
		new_session = 1;  // 回绕到1
	}
	lua_pushinteger(L, new_session);
	if (multipak) {
		// 需要多部分传输
		lua_createtable(L, multipak, 0);
		packreq_multi(L, session, msg, sz);
		skynet_free(msg);
		return 3;  // 返回：请求数据、下一个会话ID、多部分表
	} else {
		// 单部分传输
		skynet_free(msg);
		return 2;  // 返回：请求数据、下一个会话ID
	}
}

// Lua 接口：打包请求
static int
lpackrequest(lua_State *L) {
	return packrequest(L, 0);  // 非推送模式
}

// Lua 接口：打包推送
static int
lpackpush(lua_State *L) {
	return packrequest(L, 1);  // 推送模式
}

// Lua 接口：打包跟踪信息
static int
lpacktrace(lua_State *L) {
	size_t sz;
	const char * tag = luaL_checklstring(L, 1, &sz);  // 获取跟踪标签
	if (sz > 0x8000) {
		return luaL_error(L, "trace tag is too long : %d", (int) sz);
	}
	uint8_t buf[TEMP_LENGTH];
	buf[2] = 4;  // 跟踪标识
	fill_header(L, buf, sz+1);
	memcpy(buf+3, tag, sz);  // 复制标签内容
	lua_pushlstring(L, (const char *)buf, sz+3);
	return 1;
}

/*
	string packed message
	return
		uint32_t or string addr
		int session
		lightuserdata msg
		int sz
		boolean padding
		boolean is_push

	解包消息参数说明：
	string packed message - 打包的字符串消息数据
	返回值：
		uint32_t or string addr - 目标地址（数字ID或字符串名称）
		int session - 会话ID
		lightuserdata msg - 消息数据指针
		int sz - 消息大小
		boolean padding - 是否有填充
		boolean is_push - 是否为推送消息
 */

// 从缓冲区解包 32 位整数（小端序）
static inline uint32_t
unpack_uint32(const uint8_t * buf) {
	return buf[0] | buf[1]<<8 | buf[2]<<16 | buf[3]<<24;
}

// 返回消息缓冲区给 Lua
static void
return_buffer(lua_State *L, const char * buffer, int sz) {
	void * ptr = skynet_malloc(sz);  // 分配新内存
	memcpy(ptr, buffer, sz);         // 复制数据
	lua_pushlightuserdata(L, ptr);   // 推入指针
	lua_pushinteger(L, sz);          // 推入大小
}

// 解包数字地址的请求
static int
unpackreq_number(lua_State *L, const uint8_t * buf, int sz) {
	if (sz < 9) {
		return luaL_error(L, "Invalid cluster message (size=%d)", sz);
	}
	uint32_t address = unpack_uint32(buf+1);  // 解包地址
	uint32_t session = unpack_uint32(buf+5);  // 解包会话ID
	lua_pushinteger(L, address);
	lua_pushinteger(L, session);

	return_buffer(L, (const char *)buf+9, sz-9);  // 返回消息内容
	if (session == 0) {
		// 推送消息，无需响应
		lua_pushnil(L);
		lua_pushboolean(L,1);	// is_push, no reponse
		                        // 是推送，无响应
		return 6;
	}

	return 4;  // 返回：地址、会话ID、消息指针、消息大小
}

// 解包多部分请求（数字地址）
static int
unpackmreq_number(lua_State *L, const uint8_t * buf, int sz, int is_push) {
	if (sz != 13) {
		return luaL_error(L, "Invalid cluster message size %d (multi req must be 13)", sz);
	}
	uint32_t address = unpack_uint32(buf+1);  // 解包地址
	uint32_t session = unpack_uint32(buf+5);  // 解包会话ID
	uint32_t size = unpack_uint32(buf+9);     // 解包数据大小
	lua_pushinteger(L, address);
	lua_pushinteger(L, session);
	lua_pushnil(L);                           // 无数据内容
	lua_pushinteger(L, size);
	lua_pushboolean(L, 1);	// padding multi part
	                        // 填充多部分标志
	lua_pushboolean(L, is_push);              // 是否为推送

	return 6;
}

// 解包多部分请求的数据部分
static int
unpackmreq_part(lua_State *L, const uint8_t * buf, int sz) {
	if (sz < 5) {
		return luaL_error(L, "Invalid cluster multi part message");
	}
	int padding = (buf[0] == 2);              // 检查是否为填充部分
	uint32_t session = unpack_uint32(buf+1);  // 解包会话ID
	lua_pushboolean(L, 0);	// no address
	                        // 无地址
	lua_pushinteger(L, session);
	return_buffer(L, (const char *)buf+5, sz-5);  // 返回数据缓冲区
	lua_pushboolean(L, padding);                  // 返回填充标志

	return 5;
}

// 解包跟踪消息
static int
unpacktrace(lua_State *L, const char * buf, int sz) {
	lua_pushlstring(L, buf + 1, sz - 1);  // 跳过类型字节，返回跟踪数据
	return 1;
}

// 解包请求（字符串地址）
static int
unpackreq_string(lua_State *L, const uint8_t * buf, int sz) {
	if (sz < 2) {
		return luaL_error(L, "Invalid cluster message (size=%d)", sz);
	}
	size_t namesz = buf[1];  // 服务名长度
	if (sz < namesz + 6) {
		return luaL_error(L, "Invalid cluster message (size=%d)", sz);
	}
	lua_pushlstring(L, (const char *)buf+2, namesz);  // 服务名压入堆栈
	uint32_t session = unpack_uint32(buf + namesz + 2);  // 解包会话ID
	lua_pushinteger(L, (uint32_t)session);
	return_buffer(L, (const char *)buf+2+namesz+4, sz - namesz - 6);  // 返回数据
	if (session == 0) {
		lua_pushnil(L);
		lua_pushboolean(L,1);	// is_push, no reponse
		return 6;
	}

	return 4;
}

// 解包多部分请求（字符串地址）
static int
unpackmreq_string(lua_State *L, const uint8_t * buf, int sz, int is_push) {
	if (sz < 2) {
		return luaL_error(L, "Invalid cluster message (size=%d)", sz);
	}
	size_t namesz = buf[1];  // 服务名长度
	if (sz < namesz + 10) {
		return luaL_error(L, "Invalid cluster message (size=%d)", sz);
	}
	lua_pushlstring(L, (const char *)buf+2, namesz);  // 服务名压入堆栈
	uint32_t session = unpack_uint32(buf + namesz + 2);  // 解包会话ID
	uint32_t size = unpack_uint32(buf + namesz + 6);     // 解包数据大小
	lua_pushinteger(L, session);
	lua_pushnil(L);                                      // 无数据内容
	lua_pushinteger(L, size);
	lua_pushboolean(L, 1);	// padding multipart
	                        // 填充多部分标志
	lua_pushboolean(L, is_push);                         // 是否为推送

	return 6;
}

// Lua 接口：解包集群请求
static int
lunpackrequest(lua_State *L) {
	int sz;
	const char *msg;
	if (lua_type(L, 1) == LUA_TLIGHTUSERDATA) {
		// 轻量用户数据形式的消息
		msg = (const char *)lua_touserdata(L, 1);
		sz = luaL_checkinteger(L, 2);
	} else {
		// 字符串形式的消息
		size_t ssz;
		msg = luaL_checklstring(L,1,&ssz);
		sz = (int)ssz;
	}
	if (sz == 0)
		return luaL_error(L, "Invalid req package. size == 0");
	// 根据消息类型字节分发到不同的解包函数
	switch (msg[0]) {
	case 0:
		return unpackreq_number(L, (const uint8_t *)msg, sz);
	case 1:
		return unpackmreq_number(L, (const uint8_t *)msg, sz, 0);	// request
		                                                            // 请求
	case '\x41':
		return unpackmreq_number(L, (const uint8_t *)msg, sz, 1);	// push
		                                                            // 推送
	case 2:
	case 3:
		return unpackmreq_part(L, (const uint8_t *)msg, sz);        // 多部分数据
	case 4:
		return unpacktrace(L, msg, sz);                             // 跟踪消息
	case '\x80':
		return unpackreq_string(L, (const uint8_t *)msg, sz);       // 字符串地址请求
	case '\x81':
		return unpackmreq_string(L, (const uint8_t *)msg, sz, 0 );	// request
		                                                            // 字符串地址请求
	case '\xc1':
		return unpackmreq_string(L, (const uint8_t *)msg, sz, 1 );	// push
		                                                            // 字符串地址推送
	default:
		return luaL_error(L, "Invalid req package type %d", msg[0]);
	}
}

/*
	The response package :
	WORD size (big endian)
	DWORD session
	BYTE type
		0: error
		1: ok
		2: multi begin
		3: multi part
		4: multi end
	PADDING msg
		type = 0, error msg
		type = 1, msg
		type = 2, DWORD size
		type = 3/4, msg
 */
/*
	int session
	boolean ok
	lightuserdata msg
	int sz
	return string response

	int session - 会话ID
	boolean ok - 是否成功
	lightuserdata msg - 消息数据指针
	int sz - 消息大小
	返回值：string response - 打包后的响应数据
 */
// Lua 接口：打包响应
static int
lpackresponse(lua_State *L) {
	uint32_t session = (uint32_t)luaL_checkinteger(L,1);  // 会话ID
	// clusterd.lua:command.socket call lpackresponse,
	// and the msg/sz is return by skynet.rawcall , so don't free(msg)
	// clusterd.lua:command.socket 调用 lpackresponse，
	// msg/sz 由 skynet.rawcall 返回，所以不要释放 msg
	int ok = lua_toboolean(L,2);  // 成功标志
	void * msg;
	size_t sz;

	if (lua_type(L,3) == LUA_TSTRING) {
		// 消息是字符串
		msg = (void *)lua_tolstring(L, 3, &sz);
	} else {
		// 消息是用户数据指针
		msg = lua_touserdata(L,3);
		sz = (size_t)luaL_checkinteger(L, 4);
	}

	if (!ok) {
		// 错误响应
		if (sz > MULTI_PART) {
			// truncate the error msg if too long
			// 如果错误消息太长则截断
			sz = MULTI_PART;
		}
	} else {
		// 成功响应
		if (sz > MULTI_PART) {
			// return
			// 需要多部分传输
			int part = (sz - 1) / MULTI_PART + 1;
			lua_createtable(L, part+1, 0);
			uint8_t buf[TEMP_LENGTH];

			// multi part begin
			// 多部分开始
			fill_header(L, buf, 9);
			fill_uint32(buf+2, session);
			buf[6] = 2;  // 多部分开始标识
			fill_uint32(buf+7, (uint32_t)sz);
			lua_pushlstring(L, (const char *)buf, 11);
			lua_rawseti(L, -2, 1);

			char * ptr = msg;
			int i;
			for (i=0;i<part;i++) {
				int s;
				if (sz > MULTI_PART) {
					s = MULTI_PART;
					buf[6] = 3;  // 多部分中间
				} else {
					s = sz;
					buf[6] = 4;  // 多部分结束
				}
				fill_header(L, buf, s+5);
				fill_uint32(buf+2, session);
				memcpy(buf+7,ptr,s);
				lua_pushlstring(L, (const char *)buf, s+7);
				lua_rawseti(L, -2, i+2);
				sz -= s;
				ptr += s;
			}
			return 1;
		}
	}

	// 单部分响应
	uint8_t buf[TEMP_LENGTH];
	fill_header(L, buf, sz+5);  // 填充头部
	fill_uint32(buf+2, session);  // 填充会话ID
	buf[6] = ok;  // 成功/失败标识
	memcpy(buf+7,msg,sz);  // 复制消息内容

	lua_pushlstring(L, (const char *)buf, sz+7);  // 打包后的响应压入堆栈

	return 1;
}

/*
	string packed response
	return integer session
		boolean ok
		string msg
		boolean padding

	参数：字符串打包响应
	返回：整数会话
		成功标志
		字符串消息
		填充标志
 */
// Lua 接口：解包集群响应
static int
lunpackresponse(lua_State *L) {
	size_t sz;
	const char * buf = luaL_checklstring(L, 1, &sz);
	if (sz < 5) {
		return 0;  // 消息太短
	}
	uint32_t session = unpack_uint32((const uint8_t *)buf);  // 解包会话ID
	lua_pushinteger(L, (lua_Integer)session);
	switch(buf[4]) {  // 根据状态字节处理
	case 0:	// error
	        // 错误
		lua_pushboolean(L, 0);  // 失败
		lua_pushlstring(L, buf+5, sz-5);  // 错误消息
		return 3;
	case 1:	// ok
	        // 成功
	case 4:	// multi end
	        // 多部分结束
		lua_pushboolean(L, 1);  // 成功
		lua_pushlstring(L, buf+5, sz-5);  // 响应数据
		return 3;
	case 2:	// multi begin
	        // 多部分开始
		if (sz != 9) {
			return 0;  // 大小不正确
		}
		sz = unpack_uint32((const uint8_t *)buf+5);  // 解包总大小
		lua_pushboolean(L, 1);  // 成功
		lua_pushinteger(L, sz);  // 总大小
		lua_pushboolean(L, 1);  // 填充标志
		return 4;
	case 3:	// multi part
	        // 多部分数据
		lua_pushboolean(L, 1);  // 成功
		lua_pushlstring(L, buf+5, sz-5);  // 部分数据
		lua_pushboolean(L, 1);  // 填充标志
		return 4;
	default:
		return 0;  // 未知状态
	}
}

/*
	table
	pointer
	sz

	push (pointer/sz) as string into table, and free pointer

	参数：
	表
	指针
	大小

	将（指针/大小）作为字符串推入表中，并释放指针
 */
static int
lappend(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	int n = lua_rawlen(L, 1);
	if (lua_isnil(L, 2)) {
		lua_settop(L, 3);
		lua_seti(L, 1, n + 1);
		return 0;
	}
	void * buffer = lua_touserdata(L, 2);
	if (buffer == NULL)
		return luaL_error(L, "Need lightuserdata");
	int sz = luaL_checkinteger(L, 3);
	lua_pushlstring(L, (const char *)buffer, sz);
	skynet_free((void *)buffer);
	lua_seti(L, 1, n+1);
	return 0;
}

static int
lconcat(lua_State *L) {
	if (!lua_istable(L,1))
		return 0;  // 不是表，返回失败
	if (lua_geti(L,1,1) != LUA_TNUMBER)
		return 0;  // 第一个元素不是数字，返回失败
	int sz = lua_tointeger(L,-1);  // 获取总大小
	lua_pop(L,1);
	char * buff = skynet_malloc(sz);  // 分配缓冲区
	int idx = 2;
	int offset = 0;
	// 连接所有字符串片段
	while(lua_geti(L,1,idx) == LUA_TSTRING) {
		size_t s;
		const char * str = lua_tolstring(L, -1, &s);
		if (s+offset > sz) {
			// 大小超出预期
			skynet_free(buff);
			return 0;
		}
		memcpy(buff+offset, str, s);  // 复制字符串片段
		lua_pop(L,1);
		offset += s;
		++idx;
	}
	if (offset != sz) {
		// 实际大小与预期不符
		skynet_free(buff);
		return 0;
	}
	// buff/sz will send to other service, See clusterd.lua
	// 缓冲区/大小将发送给其他服务，参见 clusterd.lua
	lua_pushlightuserdata(L, buff);
	lua_pushinteger(L, sz);
	return 2;
}

// 检查是否为集群名称（以 @ 开头）
static int
lisname(lua_State *L) {
	const char * name = lua_tostring(L, 1);
	if (name && name[0] == '@') {
		lua_pushboolean(L, 1);
		return 1;
	}
	return 0;
}

// 获取节点名称（主机名+进程ID）
static int
lnodename(lua_State *L) {
	pid_t pid = getpid();  // 获取进程ID
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname))==0) {
		// 成功获取主机名
		int i;
		for (i=0; hostname[i]; i++) {
			if (hostname[i] <= ' ')
				hostname[i] = '_';  // 替换空白字符为下划线
		}
		lua_pushfstring(L, "%s%d", hostname, (int)pid);
	} else {
		// 获取主机名失败，使用默认名称
		lua_pushfstring(L, "noname%d", (int)pid);
	}
	return 1;
}

// 模块初始化函数
LUAMOD_API int
luaopen_skynet_cluster_core(lua_State *L) {
	luaL_Reg l[] = {
		{ "packrequest", lpackrequest },    // 打包请求
		{ "packpush", lpackpush },          // 打包推送
		{ "packtrace", lpacktrace },        // 打包跟踪
		{ "unpackrequest", lunpackrequest }, // 解包请求
		{ "packresponse", lpackresponse },   // 打包响应
		{ "unpackresponse", lunpackresponse }, // 解包响应
		{ "append", lappend },              // 追加数据
		{ "concat", lconcat },              // 连接数据
		{ "isname", lisname },              // 检查是否为集群名称
		{ "nodename", lnodename },          // 获取节点名称
		{ NULL, NULL },
	};
	luaL_checkversion(L);
	luaL_newlib(L,l);  // 创建模块表

	return 1;
}
