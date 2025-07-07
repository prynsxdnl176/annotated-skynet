#define LUA_LIB

// only for debug use
// 仅用于调试
#include <lua.h>
#include <lauxlib.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "spinlock.h"

#define METANAME "debugchannel"  // 元表名称

// 命令结构
struct command {
	struct command * next;  // 下一个命令
	size_t sz;              // 命令大小
};

// 调试通道结构
struct channel {
	struct spinlock lock;   // 自旋锁
	int ref;                // 引用计数
	struct command * head;  // 命令队列头
	struct command * tail;  // 命令队列尾
};

// 创建新通道
static struct channel *
channel_new() {
	struct channel * c = malloc(sizeof(*c));
	memset(c, 0 , sizeof(*c));
	c->ref = 1;         // 初始引用计数为1
	SPIN_INIT(c)        // 初始化自旋锁

	return c;
}

// 连接到通道（增加引用计数）
static struct channel *
channel_connect(struct channel *c) {
	struct channel * ret = NULL;
	SPIN_LOCK(c)
	if (c->ref == 1) {
		// 只有引用计数为1时才允许连接
		++c->ref;
		ret = c;
	}
	SPIN_UNLOCK(c)
	return ret;
}

// 释放通道（减少引用计数）
static struct channel *
channel_release(struct channel *c) {
	SPIN_LOCK(c)
	--c->ref;  // 减少引用计数
	if (c->ref > 0) {
		// 还有其他引用，不释放
		SPIN_UNLOCK(c)
		return c;
	}
	// never unlock while reference is 0
	// 引用计数为0时不再解锁
	struct command * p = c->head;
	c->head = NULL;
	c->tail = NULL;
	// 释放所有命令
	while(p) {
		struct command *next = p->next;
		free(p);
		p = next;
	}
	SPIN_UNLOCK(c)
	SPIN_DESTROY(c)  // 销毁自旋锁
	free(c);         // 释放通道内存
	return NULL;
}

// call free after channel_read
// 调用 channel_read 后需要释放返回的命令
static struct command *
channel_read(struct channel *c, double timeout) {
	struct command * ret = NULL;
	SPIN_LOCK(c)
	if (c->head == NULL) {
		// 队列为空，等待一段时间
		SPIN_UNLOCK(c)
		int ti = (int)(timeout * 100000);  // 转换为微秒
		usleep(ti);  // 休眠等待
		return NULL;
	}
	// 从队列头取出命令
	ret = c->head;
	c->head = ret->next;
	if (c->head == NULL) {
		c->tail = NULL;  // 队列已空
	}
	SPIN_UNLOCK(c)

	return ret;
}

// 向通道写入命令
static void
channel_write(struct channel *c, const char * s, size_t sz) {
	// 分配命令内存（结构体 + 数据）
	struct command * cmd = malloc(sizeof(*cmd)+ sz);
	cmd->sz = sz;
	cmd->next = NULL;
	memcpy(cmd+1, s, sz);  // 复制数据到命令后面
	SPIN_LOCK(c)
	if (c->tail == NULL) {
		// 队列为空
		c->head = c->tail = cmd;
	} else {
		c->tail->next = cmd;
		c->tail = cmd;
	}
	SPIN_UNLOCK(c)
}

// 通道包装结构（用于 Lua 用户数据）
struct channel_box {
	struct channel *c;
};

// Lua 接口：从通道读取数据
static int
lread(lua_State *L) {
	struct channel_box *cb = luaL_checkudata(L,1, METANAME);
	double ti = luaL_optnumber(L, 2, 0);  // 可选的超时时间
	struct command * c = channel_read(cb->c, ti);  // 读取命令
	if (c == NULL)
		return 0;  // 没有数据可读
	lua_pushlstring(L, (const char *)(c+1), c->sz);  // 推送命令数据
	free(c);  // 释放命令内存
	return 1;
}

// Lua 接口：向通道写入数据
static int
lwrite(lua_State *L) {
	struct channel_box *cb = luaL_checkudata(L,1, METANAME);
	size_t sz;
	const char * str = luaL_checklstring(L, 2, &sz);  // 获取要写入的字符串
	channel_write(cb->c, str, sz);  // 写入通道
	return 0;
}

// Lua 接口：释放通道（垃圾回收时调用）
static int
lrelease(lua_State *L) {
	struct channel_box *cb = lua_touserdata(L, 1);
	if (cb) {
		if (channel_release(cb->c) == NULL) {  // 释放通道
			cb->c = NULL;  // 清空指针
		}
	}

	return 0;
}

// 创建新的通道用户数据
static struct channel *
new_channel(lua_State *L, struct channel *c) {
	if (c == NULL) {
		c = channel_new();  // 创建新通道
	} else {
		c = channel_connect(c);  // 连接到现有通道
	}
	if (c == NULL) {
		luaL_error(L, "new channel failed");
		// never go here
		// 永远不会到这里
	}
	struct channel_box * cb = lua_newuserdatauv(L, sizeof(*cb), 0);  // 创建用户数据
	cb->c = c;
	if (luaL_newmetatable(L, METANAME)) {  // 创建元表
		luaL_Reg l[]={
			{ "read", lread },    // 读取方法
			{ "write", lwrite },  // 写入方法
			{ NULL, NULL },
		};
		luaL_newlib(L,l);  // 创建方法库
		lua_setfield(L, -2, "__index");  // 设置索引元方法
		lua_pushcfunction(L, lrelease);  // 设置垃圾回收方法
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);  // 设置元表
	return c;
}

// Lua 接口：创建新通道
static int
lcreate(lua_State *L) {
	struct channel *c = new_channel(L, NULL);  // 创建新通道
	lua_pushlightuserdata(L, c);  // 返回通道指针
	return 2;  // 返回用户数据和轻量用户数据
}

// Lua 接口：连接到现有通道
static int
lconnect(lua_State *L) {
	struct channel *c = lua_touserdata(L, 1);  // 获取通道指针
	if (c == NULL)
		return luaL_error(L, "Invalid channel pointer");
	new_channel(L, c);  // 连接到通道

	return 1;
}

// 钩子键（用于注册表）
static const int HOOKKEY = 0;

/*
** Auxiliary function used by several library functions: check for
** an optional thread as function's first argument and set 'arg' with
** 1 if this argument is present (so that functions can skip it to
** access their other arguments)
**
** 辅助函数，被多个库函数使用：检查第一个参数是否为可选的线程，
** 如果存在则将 'arg' 设置为 1（这样函数可以跳过它来访问其他参数）
*/
static lua_State *getthread (lua_State *L, int *arg) {
  if (lua_isthread(L, 1)) {
    *arg = 1;
    return lua_tothread(L, 1);  // 返回指定的线程
  }
  else {
    *arg = 0;
    return L;  /* function will operate over current thread */
               /* 函数将在当前线程上操作 */
  }
}

/*
** Call hook function registered at hook table for the current
** thread (if there is one)
**
** 调用为当前线程注册在钩子表中的钩子函数（如果有的话）
*/
static void hookf (lua_State *L, lua_Debug *ar) {
  static const char *const hooknames[] =
    {"call", "return", "line", "count", "tail call"};
  lua_rawgetp(L, LUA_REGISTRYINDEX, &HOOKKEY);  // 获取钩子表
  lua_pushthread(L);  // 推送当前线程
  if (lua_rawget(L, -2) == LUA_TFUNCTION) {  /* is there a hook function? */
                                             /* 是否有钩子函数？ */
    lua_pushstring(L, hooknames[(int)ar->event]);  /* push event name */
                                                   /* 推送事件名 */
    if (ar->currentline >= 0)
      lua_pushinteger(L, ar->currentline);  /* push current line */
                                            /* 推送当前行号 */
    else lua_pushnil(L);
    lua_call(L, 2, 1);  /* call hook function */
                        /* 调用钩子函数 */
	int yield = lua_toboolean(L, -1);  // 检查是否需要让出
	lua_pop(L,1);
	if (yield) {
		lua_yield(L, 0);  // 让出协程
	}
  }
}

/*
** Convert a string mask (for 'sethook') into a bit mask
*/
// 根据字符串掩码创建调试掩码
static int makemask (const char *smask, int count) {
  int mask = 0;
  if (strchr(smask, 'c')) mask |= LUA_MASKCALL;  // 函数调用
  if (strchr(smask, 'r')) mask |= LUA_MASKRET;   // 函数返回
  if (strchr(smask, 'l')) mask |= LUA_MASKLINE;  // 行执行
  if (count > 0) mask |= LUA_MASKCOUNT;          // 指令计数
  return mask;
}

// 设置调试钩子
static int db_sethook (lua_State *L) {
  int arg, mask, count;
  lua_Hook func;
  lua_State *L1 = getthread(L, &arg);
  if (lua_isnoneornil(L, arg+1)) {  /* no hook? */
                                    /* 没有钩子？ */
    lua_settop(L, arg+1);
    func = NULL; mask = 0; count = 0;  /* turn off hooks */
                                       /* 关闭钩子 */
  }
  else {
    const char *smask = luaL_checkstring(L, arg+2);
    luaL_checktype(L, arg+1, LUA_TFUNCTION);
    count = (int)luaL_optinteger(L, arg + 3, 0);
    func = hookf; mask = makemask(smask, count);
  }
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &HOOKKEY) == LUA_TNIL) {
    lua_createtable(L, 0, 2);  /* create a hook table */
                               /* 创建钩子表 */
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &HOOKKEY);  /* set it in position */
                                                  /* 设置到注册表 */
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");  /** hooktable.__mode = "k" */
                                    /** 设置弱引用模式 */
    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2);  /* setmetatable(hooktable) = hooktable */
                              /* 设置元表 */
  }
  lua_pushthread(L1); lua_xmove(L1, L, 1);  /* key (thread) */
                                            /* 键（线程） */
  lua_pushvalue(L, arg + 1);  /* value (hook function) */
                              /* 值（钩子函数） */
  lua_rawset(L, -3);  /* hooktable[L1] = new Lua hook */
                      /* 设置新的 Lua 钩子 */
  lua_sethook(L1, func, mask, count);
  return 0;
}

// debugchannel 模块初始化函数
LUAMOD_API int
luaopen_skynet_debugchannel(lua_State *L) {
	luaL_Reg l[] = {
		{ "create", lcreate },	// for write
		                        // 用于写入
		{ "connect", lconnect },	// for read
		                        // 用于读取
		{ "release", lrelease },    // 释放通道
		{ "sethook", db_sethook },  // 设置调试钩子
		{ NULL, NULL },
	};
	luaL_checkversion(L);
	luaL_newlib(L,l);
	return 1;
}
