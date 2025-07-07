#define LUA_LIB

#include "skynet.h"
#include "lua-seri.h"

// 终端颜色控制码
#define KNRM  "\x1B[0m"  // 正常颜色
#define KRED  "\x1B[31m" // 红色

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <time.h>

#if defined(__APPLE__)
#include <sys/time.h>
#endif

#include "skynet.h"

// return nsec
// 返回纳秒时间戳
static int64_t
get_time() {
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);  // 使用单调时钟
	return (int64_t)1000000000 * ti.tv_sec + ti.tv_nsec;
#else
	// macOS 兼容性处理
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)1000000000 * tv.tv_sec + tv.tv_usec * 1000;
#endif
}

// Lua 错误回溯函数
static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);  // 生成错误回溯信息
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

// 回调上下文结构
struct callback_context {
	lua_State *L;  // Lua 状态机指针
};

// skynet 消息回调函数
static int
_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct callback_context *cb_ctx = (struct callback_context *)ud;
	lua_State *L = cb_ctx->L;
	int trace = 1;  // 启用错误回溯
	int r;
	lua_pushvalue(L,2);  // 推入回调函数

	// 推入回调参数
	lua_pushinteger(L, type);    // 消息类型
	lua_pushlightuserdata(L, (void *)msg);  // 消息数据指针
	lua_pushinteger(L,sz);       // 消息大小
	lua_pushinteger(L, session); // 会话ID
	lua_pushinteger(L, source);  // 源地址

	r = lua_pcall(L, 5, 0 , trace);  // 调用 Lua 回调函数

	if (r == LUA_OK) {
		return 0;  // 成功执行
	}

	// 处理 Lua 执行错误
	const char * self = skynet_command(context, "REG", NULL);
	switch (r) {
	case LUA_ERRRUN:
		// 运行时错误
		skynet_error(context, "lua call [%x to %s : %d msgsz = %d] error : " KRED "%s" KNRM, source , self, session, sz, lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		// 内存错误
		skynet_error(context, "lua memory error : [%x to %s : %d]", source , self, session);
		break;
	case LUA_ERRERR:
		// 错误处理函数中的错误
		skynet_error(context, "lua error in error : [%x to %s : %d]", source , self, session);
		break;
	};

	lua_pop(L,1);  // 弹出错误信息

	return 0;
}

// 转发模式的回调函数
static int
forward_cb(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	_cb(context, ud, type, session, source, msg, sz);
	// don't delete msg in forward mode.
	// 转发模式下不删除消息
	return 1;
}

// 清理上一个回调上下文
static void
clear_last_context(lua_State *L) {
	if (lua_getfield(L, LUA_REGISTRYINDEX, "callback_context") == LUA_TUSERDATA) {
		lua_pushnil(L);
		lua_setiuservalue(L, -2, 2);  // 清空用户值
	}
	lua_pop(L, 1);
}

// 回调预处理函数
static int
_cb_pre(struct skynet_context * context, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct callback_context *cb_ctx = (struct callback_context *)ud;
	clear_last_context(cb_ctx->L);  // 清理上一个上下文
	skynet_callback(context, ud, _cb);  // 设置正式回调函数
	return _cb(context, cb_ctx, type, session, source, msg, sz);
}

// 转发模式预处理函数
static int
_forward_pre(struct skynet_context *context, void *ud, int type, int session, uint32_t source, const void *msg, size_t sz) {
	struct callback_context *cb_ctx = (struct callback_context *)ud;
	clear_last_context(cb_ctx->L);  // 清理上一个上下文
	skynet_callback(context, ud, forward_cb);  // 设置转发回调函数
	return forward_cb(context, cb_ctx, type, session, source, msg, sz);
}

// Lua 接口：设置回调函数
static int
lcallback(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int forward = lua_toboolean(L, 2);  // 是否为转发模式
	luaL_checktype(L,1,LUA_TFUNCTION);  // 检查第一个参数是函数
	lua_settop(L,1);

	// 创建回调上下文
	struct callback_context * cb_ctx = (struct callback_context *)lua_newuserdatauv(L, sizeof(*cb_ctx), 2);
	cb_ctx->L = lua_newthread(L);  // 创建新的 Lua 线程
	lua_pushcfunction(cb_ctx->L, traceback);  // 设置错误回溯函数
	lua_setiuservalue(L, -2, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "callback_context");
	lua_setiuservalue(L, -2, 2);
	lua_setfield(L, LUA_REGISTRYINDEX, "callback_context");
	lua_xmove(L, cb_ctx->L, 1);  // 移动回调函数到新线程

	// 设置回调函数（转发模式或普通模式）
	skynet_callback(context, cb_ctx, (forward)?(_forward_pre):(_cb_pre));
	return 0;
}

// Lua 接口：执行 skynet 命令
static int
lcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);  // 命令名称
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {
		parm = luaL_checkstring(L,2);  // 命令参数
	}

	result = skynet_command(context, cmd, parm);  // 执行命令
	if (result) {
		lua_pushstring(L, result);  // 返回结果字符串
		return 1;
	}
	return 0;
}

// Lua 接口：执行返回地址的命令
static int
laddresscommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);  // 命令名称
	const char * result;
	const char * parm = NULL;
	if (lua_gettop(L) == 2) {
		parm = luaL_checkstring(L,2);  // 命令参数
	}
	result = skynet_command(context, cmd, parm);  // 执行命令
	if (result && result[0] == ':') {
		// 解析十六进制地址（格式：:xxxxxxxx）
		int i;
		uint32_t addr = 0;
		for (i=1;result[i];i++) {
			int c = result[i];
			if (c>='0' && c<='9') {
				c = c - '0';
			} else if (c>='a' && c<='f') {
				c = c - 'a' + 10;
			} else if (c>='A' && c<='F') {
				c = c - 'A' + 10;
			} else {
				return 0;  // 无效字符
			}
			addr = addr * 16 + c;  // 累积十六进制值
		}
		lua_pushinteger(L, addr);  // 返回地址整数
		return 1;
	}
	return 0;
}

// Lua 接口：执行返回整数的命令
static int
lintcommand(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * cmd = luaL_checkstring(L,1);  // 命令名称
	const char * result;
	const char * parm = NULL;
	char tmp[64];	// for integer parm
	                // 用于整数参数的临时缓冲区
	if (lua_gettop(L) == 2) {
		if (lua_isnumber(L, 2)) {
			// 整数参数转换为字符串
			int32_t n = (int32_t)luaL_checkinteger(L,2);
			sprintf(tmp, "%d", n);
			parm = tmp;
		} else {
			parm = luaL_checkstring(L,2);  // 字符串参数
		}
	}

	result = skynet_command(context, cmd, parm);  // 执行命令
	if (result) {
		// 尝试解析为整数
		char *endptr = NULL;
		lua_Integer r = strtoll(result, &endptr, 0);
		if (endptr == NULL || *endptr != '\0') {
			// may be real number
			// 可能是实数
			double n = strtod(result, &endptr);
			if (endptr == NULL || *endptr != '\0') {
				return luaL_error(L, "Invalid result %s", result);
			} else {
				lua_pushnumber(L, n);  // 返回浮点数
			}
		} else {
			lua_pushinteger(L, r);  // 返回整数
		}
		return 1;
	}
	return 0;
}

// Lua 接口：生成新的会话ID
static int
lgenid(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int session = skynet_send(context, 0, 0, PTYPE_TAG_ALLOCSESSION , 0 , NULL, 0);
	lua_pushinteger(L, session);
	return 1;
}

// 获取目标地址字符串
static const char *
get_dest_string(lua_State *L, int index) {
	const char * dest_string = lua_tostring(L, index);
	if (dest_string == NULL) {
		luaL_error(L, "dest address type (%s) must be a string or number.", lua_typename(L, lua_type(L,index)));
	}
	return dest_string;
}

// 发送消息的核心函数
static int
send_message(lua_State *L, int source, int idx_type) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t dest = (uint32_t)lua_tointeger(L, 1);  // 目标地址
	const char * dest_string = NULL;
	if (dest == 0) {
		if (lua_type(L,1) == LUA_TNUMBER) {
			return luaL_error(L, "Invalid service address 0");
		}
		dest_string = get_dest_string(L, 1);  // 获取字符串形式的目标地址
	}

	int type = luaL_checkinteger(L, idx_type+0);  // 消息类型
	int session = 0;
	if (lua_isnil(L,idx_type+1)) {
		type |= PTYPE_TAG_ALLOCSESSION;  // 自动分配会话ID
	} else {
		session = luaL_checkinteger(L,idx_type+1);  // 指定会话ID
	}

	int mtype = lua_type(L,idx_type+2);  // 消息数据类型
	switch (mtype) {
	case LUA_TSTRING: {
		size_t len = 0;
		void * msg = (void *)lua_tolstring(L,idx_type+2,&len);  // 字符串消息
		if (len == 0) {
			msg = NULL;
		}
		if (dest_string) {
			// 按名称发送消息
			session = skynet_sendname(context, source, dest_string, type, session , msg, len);
		} else {
			// 按地址发送消息
			session = skynet_send(context, source, dest, type, session , msg, len);
		}
		break;
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,idx_type+2);  // 轻量用户数据消息
		int size = luaL_checkinteger(L,idx_type+3);
		if (dest_string) {
			// 按名称发送消息（不复制数据）
			session = skynet_sendname(context, source, dest_string, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		} else {
			// 按地址发送消息（不复制数据）
			session = skynet_send(context, source, dest, type | PTYPE_TAG_DONTCOPY, session, msg, size);
		}
		break;
	}
	default:
		luaL_error(L, "invalid param %s", lua_typename(L, lua_type(L,idx_type+2)));
	}
	if (session < 0) {
		if (session == -2) {
			// package is too large
			// 包太大
			lua_pushboolean(L, 0);
			return 1;
		}
		// send to invalid address
		// todo: maybe throw an error would be better
		// 发送到无效地址
		// 待办：也许抛出错误会更好
		return 0;
	}
	lua_pushinteger(L,session);  // 返回会话ID
	return 1;
}

/*
	uint32 address
	 string address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len

	参数：uint32地址或字符串地址，整数类型，整数会话，字符串消息或轻量用户数据消息指针和长度
 */
// Lua 接口：发送消息
static int
lsend(lua_State *L) {
	return send_message(L, 0, 2);  // 源地址为0（当前服务）
}

/*
	uint32 address
	 string address
	integer source_address
	integer type
	integer session
	string message
	 lightuserdata message_ptr
	 integer len

	参数：uint32地址或字符串地址，整数源地址，整数类型，整数会话，字符串消息或轻量用户数据消息指针和长度
 */
// Lua 接口：重定向消息（指定源地址）
static int
lredirect(lua_State *L) {
	uint32_t source = (uint32_t)luaL_checkinteger(L,2);  // 指定的源地址
	return send_message(L, source, 3);
}

// Lua 接口：输出错误信息
static int
lerror(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	int n = lua_gettop(L);  // 参数个数
	if (n <= 1) {
		// 单个参数
		lua_settop(L, 1);
		size_t len;
		const char *s = luaL_tolstring(L, 1, &len);
		skynet_error(context, "%*s", (int)len, s);
		return 0;
	}
	// 多个参数，拼接成字符串
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	int i;
	for (i=1; i<=n; i++) {
		luaL_tolstring(L, i, NULL);  // 转换为字符串
		luaL_addvalue(&b);           // 添加到缓冲区
		if (i<n) {
			luaL_addchar(&b, ' ');   // 添加空格分隔符
		}
	}
	luaL_pushresult(&b);  // 生成最终字符串
	size_t len;
	const char *s = luaL_tolstring(L, -1, &len);
	skynet_error(context, "%*s", (int)len, s);  // 输出错误信息
	return 0;
}

// Lua 接口：将用户数据转换为字符串
static int
ltostring(lua_State *L) {
	if (lua_isnoneornil(L,1)) {
		return 0;  // 参数为空，返回0个值
	}
	char * msg = lua_touserdata(L,1);  // 获取用户数据指针
	int sz = luaL_checkinteger(L,2);   // 获取数据大小
	lua_pushlstring(L,msg,sz);         // 创建指定长度的字符串
	return 1;
}

// Lua 接口：获取服务的 harbor 信息
static int
lharbor(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	uint32_t handle = (uint32_t)luaL_checkinteger(L,1);  // 服务句柄
	int harbor = 0;
	int remote = skynet_isremote(context, handle, &harbor);  // 检查是否为远程服务
	lua_pushinteger(L,harbor);   // 返回 harbor ID
	lua_pushboolean(L, remote);  // 返回是否为远程服务

	return 2;
}

// Lua 接口：打包数据为字符串
static int
lpackstring(lua_State *L) {
	luaseri_pack(L);  // 调用序列化函数
	char * str = (char *)lua_touserdata(L, -2);  // 获取序列化后的数据
	int sz = lua_tointeger(L, -1);                // 获取数据大小
	lua_pushlstring(L, str, sz);                  // 创建字符串
	skynet_free(str);  // 释放临时数据
	return 1;
}

// Lua 接口：释放消息内存
static int
ltrash(lua_State *L) {
	int t = lua_type(L,1);  // 获取参数类型
	switch (t) {
	case LUA_TSTRING: {
		break;  // 字符串不需要释放
	}
	case LUA_TLIGHTUSERDATA: {
		void * msg = lua_touserdata(L,1);  // 获取用户数据指针
		luaL_checkinteger(L,2);            // 检查大小参数
		skynet_free(msg);                  // 释放内存
		break;
	}
	default:
		luaL_error(L, "skynet.trash invalid param %s", lua_typename(L,t));
	}

	return 0;
}

// Lua 接口：获取当前时间（skynet 时间）
static int
lnow(lua_State *L) {
	uint64_t ti = skynet_now();  // 获取 skynet 当前时间
	lua_pushinteger(L, ti);
	return 1;
}

// Lua 接口：获取高精度时间
static int
lhpc(lua_State *L) {
	lua_pushinteger(L, get_time());  // 获取高精度时间
	return 1;
}

#define MAX_LEVEL 3  // 最大跟踪层级

// 源码信息结构
struct source_info {
	const char * source;  // 源文件名
	int line;             // 行号
};

/*
	string tag
	string userstring
	thread co (default nil/current L)
	integer level (default nil)

	参数：字符串标签，字符串用户信息，线程协程（默认nil/当前L），整数层级（默认nil）
 */
// Lua 接口：输出跟踪信息
static int
ltrace(lua_State *L) {
	struct skynet_context * context = lua_touserdata(L, lua_upvalueindex(1));
	const char * tag = luaL_checkstring(L, 1);   // 跟踪标签
	const char * user = luaL_checkstring(L, 2);  // 用户信息
	if (!lua_isnoneornil(L, 3)) {
		lua_State * co = L;
		int level;
		if (lua_isthread(L, 3)) {
			co = lua_tothread (L, 3);              // 指定的协程
			level = luaL_optinteger(L, 4, 1);      // 起始层级
		} else {
			level = luaL_optinteger(L, 3, 1);      // 起始层级
		}
		struct source_info si[MAX_LEVEL];  // 源码信息数组
		lua_Debug d;
		int index = 0;
		// 获取调用栈信息
		do {
			if (!lua_getstack(co, level, &d))
				break;
			lua_getinfo(co, "Sl", &d);  // 获取源文件和行号信息
			level++;
			si[index].source = d.source;      // 源文件
			si[index].line = d.currentline;   // 当前行号
			if (d.currentline >= 0)
				++index;
		} while (index < MAX_LEVEL);
		// 根据获取到的层级数输出不同格式的跟踪信息
		switch (index) {
		case 1:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s : %s:%d", tag, get_time(), user, si[0].source, si[0].line);
			break;
		case 2:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s : %s:%d %s:%d", tag, get_time(), user,
				si[0].source, si[0].line,
				si[1].source, si[1].line
				);
			break;
		case 3:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s : %s:%d %s:%d %s:%d", tag, get_time(), user,
				si[0].source, si[0].line,
				si[1].source, si[1].line,
				si[2].source, si[2].line
				);
			break;
		default:
			skynet_error(context, "<TRACE %s> %" PRId64 " %s", tag, get_time(), user);
			break;
		}
		return 0;
	}
	// 没有指定协程和层级，输出简单的跟踪信息
	skynet_error(context, "<TRACE %s> %" PRId64 " %s", tag, get_time(), user);
	return 0;
}

// skynet 核心模块初始化函数
LUAMOD_API int
luaopen_skynet_core(lua_State *L) {
	luaL_checkversion(L);

	// 需要 skynet_context 的函数
	luaL_Reg l[] = {
		{ "send" , lsend },                    // 发送消息
		{ "genid", lgenid },                   // 生成会话ID
		{ "redirect", lredirect },             // 重定向消息
		{ "command" , lcommand },              // 执行命令
		{ "intcommand", lintcommand },         // 执行返回整数的命令
		{ "addresscommand", laddresscommand }, // 执行返回地址的命令
		{ "error", lerror },                   // 错误日志
		{ "harbor", lharbor },                 // harbor 相关
		{ "callback", lcallback },             // 设置回调函数
		{ "trace", ltrace },                   // 跟踪调试
		{ NULL, NULL },
	};

	// functions without skynet_context
	// 不需要 skynet_context 的函数
	luaL_Reg l2[] = {
		{ "tostring", ltostring },      // 地址转字符串
		{ "pack", luaseri_pack },       // 序列化打包
		{ "unpack", luaseri_unpack },   // 序列化解包
		{ "packstring", lpackstring },  // 字符串打包
		{ "trash" , ltrash },           // 垃圾回收
		{ "now", lnow },                // 当前时间
		{ "hpc", lhpc },	// getHPCounter
		                    // 高精度计数器
		{ NULL, NULL },
	};

	// 创建模块表
	lua_createtable(L, 0, sizeof(l)/sizeof(l[0]) + sizeof(l2)/sizeof(l2[0]) -2);

	// 获取 skynet 上下文
	lua_getfield(L, LUA_REGISTRYINDEX, "skynet_context");
	struct skynet_context *ctx = lua_touserdata(L,-1);
	if (ctx == NULL) {
		return luaL_error(L, "Init skynet context first");
	}

	// 注册需要上下文的函数（将上下文作为 upvalue）
	luaL_setfuncs(L,l,1);

	// 注册不需要上下文的函数
	luaL_setfuncs(L,l2,0);

	return 1;
}
