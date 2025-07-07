#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>

#include "malloc_hook.h"

// Lua 接口：获取总内存使用量
static int
ltotal(lua_State *L) {
	size_t t = malloc_used_memory();  // 获取已使用内存
	lua_pushinteger(L, (lua_Integer)t);

	return 1;
}

// Lua 接口：获取内存块数量
static int
lblock(lua_State *L) {
	size_t t = malloc_memory_block();  // 获取内存块数量
	lua_pushinteger(L, (lua_Integer)t);

	return 1;
}

// Lua 接口：转储内存信息
static int
ldumpinfo(lua_State *L) {
	const char *opts = NULL;
	if (lua_isstring(L, 1)) {
		opts = luaL_checkstring(L,1);  // 获取选项字符串
	}
	memory_info_dump(opts);  // 转储内存信息

	return 0;
}

// Lua 接口：获取 jemalloc 统计信息
static int
ljestat(lua_State *L) {
	static const char* names[] = {
		"stats.allocated",  // 已分配内存
		"stats.resident",   // 驻留内存
		"stats.retained",   // 保留内存
		"stats.mapped",     // 映射内存
		"stats.active" };   // 活跃内存
	static size_t flush = 1;
	mallctl_int64("epoch", &flush); // refresh je.stats.cache
	                                // 刷新 jemalloc 统计缓存
	lua_newtable(L);
	int i;
	// 遍历所有统计项
	for (i = 0; i < (sizeof(names)/sizeof(names[0])); i++) {
		lua_pushstring(L, names[i]);
		lua_pushinteger(L,  (lua_Integer) mallctl_int64(names[i], NULL));
		lua_settable(L, -3);
	}
	return 1;
}

// Lua 接口：调用 mallctl 获取指定统计信息
static int
lmallctl(lua_State *L) {
	const char *name = luaL_checkstring(L,1);
	lua_pushinteger(L, (lua_Integer) mallctl_int64(name, NULL));
	return 1;
}

// Lua 接口：转储 C 内存信息
static int
ldump(lua_State *L) {
	dump_c_mem();  // 转储 C 内存

	return 0;
}

// Lua 接口：获取当前内存使用量
static int
lcurrent(lua_State *L) {
	lua_pushinteger(L, malloc_current_memory());  // 获取当前内存
	return 1;
}

// Lua 接口：转储堆信息
static int
ldumpheap(lua_State *L) {
	mallctl_cmd("prof.dump");  // 执行堆转储命令
	return 0;
}

// Lua 接口：控制性能分析器状态
static int
lprofactive(lua_State *L) {
	bool *pval, active;
	if (lua_isnone(L, 1)) {
		pval = NULL;  // 只读取状态
	} else {
		active = lua_toboolean(L, 1) ? true : false;
		pval = &active;  // 设置新状态
	}
	bool ret = mallctl_bool("prof.active", pval);
	lua_pushboolean(L, ret);  // 返回当前状态
	return 1;
}

// memory 模块初始化函数
LUAMOD_API int
luaopen_skynet_memory(lua_State *L) {
	luaL_checkversion(L);

	luaL_Reg l[] = {
		{ "total", ltotal },           // 总内存使用量
		{ "block", lblock },           // 内存块数量
		{ "dumpinfo", ldumpinfo },     // 转储内存信息
		{ "jestat", ljestat },         // jemalloc 统计
		{ "mallctl", lmallctl },       // mallctl 接口
		{ "dump", ldump },             // 转储 C 内存
		{ "info", dump_mem_lua },      // Lua 内存信息
		{ "current", lcurrent },       // 当前内存使用量
		{ "dumpheap", ldumpheap },     // 转储堆信息
		{ "profactive", lprofactive }, // 性能分析器控制
		{ NULL, NULL },
	};

	luaL_newlib(L,l);

	return 1;
}
