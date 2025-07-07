#define LUA_LIB

#include "skynet.h"

#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <string.h>

#include "atomic.h"

// 多播包结构
struct mc_package {
	ATOM_INT reference;  // 原子引用计数
	uint32_t size;       // 数据大小
	void *data;          // 数据指针
};

// 打包数据为多播包
static int
pack(lua_State *L, void *data, size_t size) {
	struct mc_package * pack = skynet_malloc(sizeof(struct mc_package));
	ATOM_INIT(&pack->reference, 0);  // 初始化引用计数为0
	pack->size = (uint32_t)size;
	pack->data = data;
	struct mc_package ** ret = skynet_malloc(sizeof(*ret));
	*ret = pack;
	lua_pushlightuserdata(L, ret);      // 返回包指针的指针
	lua_pushinteger(L, sizeof(ret));    // 返回指针大小
	return 2;
}

/*
	lightuserdata
	integer size

	return lightuserdata, sizeof(struct mc_package *)

	参数：
	lightuserdata 轻量用户数据
	size 大小

	返回：lightuserdata轻量用户数据, sizeof(struct mc_package *)
 */
// Lua 接口：打包本地数据
static int
mc_packlocal(lua_State *L) {
	void * data = lua_touserdata(L, 1);
	size_t size = (size_t)luaL_checkinteger(L, 2);
	if (size != (uint32_t)size) {
		return luaL_error(L, "Size should be 32bit integer");
	}
	return pack(L, data, size);  // 直接使用数据指针
}

/*
	lightuserdata
	integer size

	return lightuserdata, sizeof(struct mc_package *)

	参数：轻量用户数据，整数大小
	返回：轻量用户数据，sizeof(struct mc_package *)
 */
// Lua 接口：打包远程数据（复制数据）
static int
mc_packremote(lua_State *L) {
	void * data = lua_touserdata(L, 1);
	size_t size = (size_t)luaL_checkinteger(L, 2);
	if (size != (uint32_t)size) {
		return luaL_error(L, "Size should be 32bit integer");
	}
	void * msg = skynet_malloc(size);  // 分配新内存
	memcpy(msg, data, size);           // 复制数据
	return pack(L, msg, size);
}

/*
	lightuserdata struct mc_package **
	integer size (must be sizeof(struct mc_package *)

	return package, lightuserdata, size

	参数：
	轻量用户数据 struct mc_package **，
	整数大小（必须是 sizeof(struct mc_package *)）
	
	返回：包，轻量用户数据，大小
 */
// Lua 接口：解包本地多播包
static int
mc_unpacklocal(lua_State *L) {
	struct mc_package ** pack = lua_touserdata(L,1);
	int sz = luaL_checkinteger(L,2);
	if (sz != sizeof(pack)) {
		return luaL_error(L, "Invalid multicast package size %d", sz);
	}
	lua_pushlightuserdata(L, *pack);           // 返回包指针
	lua_pushlightuserdata(L, (*pack)->data);   // 返回数据指针
	lua_pushinteger(L, (lua_Integer)((*pack)->size));  // 返回数据大小
	return 3;
}

/*
	lightuserdata struct mc_package **
	integer reference

	return mc_package *

	参数：轻量用户数据 struct mc_package **，整数引用计数
	返回：mc_package *
 */
// Lua 接口：绑定引用计数
static int
mc_bindrefer(lua_State *L) {
	struct mc_package ** pack = lua_touserdata(L,1);
	int ref = luaL_checkinteger(L,2);
	if (ATOM_LOAD(&(*pack)->reference) != 0) {
		return luaL_error(L, "Can't bind a multicast package more than once");
	}
	ATOM_STORE(&(*pack)->reference , ref);  // 设置引用计数

	lua_pushlightuserdata(L, *pack);

	skynet_free(pack);  // 释放包指针的指针

	return 1;
}

/*
	lightuserdata struct mc_package *

	参数：轻量用户数据 struct mc_package *
 */
// Lua 接口：关闭本地多播包
static int
mc_closelocal(lua_State *L) {
	struct mc_package *pack = lua_touserdata(L,1);

	int ref = ATOM_FDEC(&pack->reference)-1;  // 原子递减引用计数
	if (ref <= 0) {
		// 引用计数为0，释放资源
		skynet_free(pack->data);
		skynet_free(pack);
		if (ref < 0) {
			return luaL_error(L, "Invalid multicast package reference %d", ref);
		}
	}

	return 0;
}

/*
	lightuserdata struct mc_package **
	return lightuserdata/size

	参数：轻量用户数据 struct mc_package **
	返回：轻量用户数据/大小
 */
// Lua 接口：处理远程多播包
static int
mc_remote(lua_State *L) {
	struct mc_package **ptr = lua_touserdata(L,1);
	struct mc_package *pack = *ptr;
	lua_pushlightuserdata(L, pack->data);      // 返回数据指针
	lua_pushinteger(L, (lua_Integer)(pack->size));  // 返回数据大小
	skynet_free(pack);  // 释放包
	skynet_free(ptr);   // 释放指针
	return 2;
}

// Lua 接口：生成下一个多播ID
static int
mc_nextid(lua_State *L) {
	uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
	id += 256;  // 增加256
	// remove the highest bit, see #1139
	// 移除最高位，参见 #1139
	lua_pushinteger(L, id & 0x7fffffffu);

	return 1;
}

// multicast 核心模块初始化函数
LUAMOD_API int
luaopen_skynet_multicast_core(lua_State *L) {
	luaL_Reg l[] = {
		{ "pack", mc_packlocal },      // 打包本地数据
		{ "unpack", mc_unpacklocal },  // 解包本地数据
		{ "bind", mc_bindrefer },      // 绑定引用计数
		{ "close", mc_closelocal },    // 关闭本地包
		{ "remote", mc_remote },       // 处理远程包
		{ "packremote", mc_packremote }, // 打包远程数据
		{ "nextid", mc_nextid },       // 生成下一个ID
		{ NULL, NULL },
	};
	luaL_checkversion(L);
	luaL_newlib(L,l);
	return 1;
}
