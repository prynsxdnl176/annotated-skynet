/*
 * skynet_env.c - skynet环境变量管理模块
 * 使用Lua状态机管理全局环境变量，提供线程安全的读写接口
 */

#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

/*
 * skynet环境变量管理器结构体
 * 使用Lua虚拟机存储环境变量，提供线程安全访问
 */
struct skynet_env {
	struct spinlock lock;   // 自旋锁，保护并发访问
	lua_State *L;           // Lua状态机，存储环境变量
};

// 全局环境变量管理器实例
static struct skynet_env *E = NULL;

/*
 * 获取环境变量值
 * @param key: 环境变量名
 * @return: 环境变量值，不存在返回NULL
 */
const char *
skynet_getenv(const char *key) {
	SPIN_LOCK(E)

	lua_State *L = E->L;

	lua_getglobal(L, key);                      // 获取全局变量
	const char * result = lua_tostring(L, -1);  // 转换为字符串
	lua_pop(L, 1);                              // 弹出栈顶元素

	SPIN_UNLOCK(E)

	return result;
}

/*
 * 设置环境变量值
 * 注意：只能设置新的环境变量，不能修改已存在的变量
 * @param key: 环境变量名
 * @param value: 环境变量值
 */
void
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)

	lua_State *L = E->L;
	lua_getglobal(L, key);          // 检查变量是否已存在
	assert(lua_isnil(L, -1));       // 确保变量不存在（只能设置新变量）
	lua_pop(L,1);                   // 弹出nil值
	lua_pushstring(L,value);        // 压入新值
	lua_setglobal(L,key);           // 设置全局变量

	SPIN_UNLOCK(E)
}

/*
 * 初始化环境变量系统
 * 创建Lua状态机和管理器实例
 */
void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	SPIN_INIT(E)  // 初始化自旋锁
	E->L = luaL_newstate();
}
