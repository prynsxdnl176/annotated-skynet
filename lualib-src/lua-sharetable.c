#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "lgc.h"

#ifdef makeshared

// 递归标记表为共享
static void
mark_shared(lua_State *L) {
	if (lua_type(L, -1) != LUA_TTABLE) {
		luaL_error(L, "Not a table, it's a %s.", lua_typename(L, lua_type(L, -1)));
	}
	Table * t = (Table *)lua_topointer(L, -1);
	if (isshared(t))
		return;  // 已经是共享表
	makeshared(t);  // 标记为共享
	luaL_checkstack(L, 4, NULL);
	if (lua_getmetatable(L, -1)) {
		luaL_error(L, "Can't share metatable");
	}
	lua_pushnil(L);
	// 遍历表中的所有键值对
	while (lua_next(L, -2) != 0) {
		int i;
		// 处理键和值
		for (i=0;i<2;i++) {
			int idx = -i-1;
			int t = lua_type(L, idx);
			switch (t) {
			case LUA_TTABLE:
				mark_shared(L);  // 递归处理嵌套表
				break;
			case LUA_TNUMBER:
			case LUA_TBOOLEAN:
			case LUA_TLIGHTUSERDATA:
				break;  // 这些类型可以直接共享
			case LUA_TFUNCTION:
				if (lua_getupvalue(L, idx, 1) != NULL) {
					luaL_error(L, "Invalid function with upvalue");
				} else if (!lua_iscfunction(L, idx)) {
					LClosure *f = (LClosure *)lua_topointer(L, idx);
					makeshared(f);  // 标记 Lua 函数为共享
					lua_sharefunction(L, idx);
				}
				break;
			case LUA_TSTRING:
				lua_sharestring(L, idx);  // 共享字符串
				break;
			default:
				luaL_error(L, "Invalid type [%s]", lua_typename(L, t));
				break;
			}
		}
		lua_pop(L, 1);
	}
}

// Lua 接口：检查是否为共享表
static int
lis_sharedtable(lua_State* L) {
	int b = 0;
	if(lua_type(L, 1) == LUA_TTABLE) {
		Table * t = (Table *)lua_topointer(L, 1);
		b = isshared(t);  // 检查共享状态
	}
	lua_pushboolean(L, b);
	return 1;
}

// 创建共享矩阵
static int
make_matrix(lua_State *L) {
	// turn off gc , because marking shared will prevent gc mark.
	// 关闭 GC，因为标记共享会阻止 GC 标记
	lua_gc(L, LUA_GCSTOP, 0);
	mark_shared(L);  // 标记为共享
	Table * t = (Table *)lua_topointer(L, -1);
	lua_pushlightuserdata(L, t);  // 返回表指针
	return 1;
}

// Lua 接口：克隆表
static int
clone_table(lua_State *L) {
	lua_clonetable(L, lua_touserdata(L, 1));  // 克隆共享表

	return 1;
}

// Lua 接口：获取协程栈值
static int
lco_stackvalues(lua_State* L) {
    lua_State *cL = lua_tothread(L, 1);  // 获取协程
    luaL_argcheck(L, cL, 1, "thread expected");
    int n = 0;
    if(cL != L) {  // 如果不是当前线程
        luaL_checktype(L, 2, LUA_TTABLE);  // 检查第二个参数为表
        n = lua_gettop(cL);  // 获取协程栈顶位置
        if(n > 0) {
            luaL_checkstack(L, n+1, NULL);  // 检查栈空间
            int top = lua_gettop(L);
            lua_xmove(cL, L, n);  // 移动值从协程到主线程
            int i=0;
            for(i=1; i<=n; i++) {
                lua_pushvalue(L, top+i);  // 复制值
                lua_seti(L, 2, i);  // 设置到表中
            }
            lua_xmove(L, cL, n);  // 移动值回协程
        }
    }

    lua_pushinteger(L, n);  // 返回栈值数量
    return 1;
}


// 状态用户数据结构
struct state_ud {
	lua_State *L;
};

// 关闭状态
static int
close_state(lua_State *L) {
	struct state_ud *ud = (struct state_ud *)luaL_checkudata(L, 1, "BOXMATRIXSTATE");
	if (ud->L) {
		lua_close(ud->L);  // 关闭 Lua 状态机
		ud->L = NULL;
	}
	return 0;
}

// 获取矩阵指针
static int
get_matrix(lua_State *L) {
	struct state_ud *ud = (struct state_ud *)luaL_checkudata(L, 1, "BOXMATRIXSTATE");
	if (ud->L) {
		const void * v = lua_topointer(ud->L, 1);  // 获取矩阵指针
		lua_pushlightuserdata(L, (void *)v);  // 推送轻量用户数据
		return 1;
	}
	return 0;
}

// 获取内存大小
static int
get_size(lua_State *L) {
	struct state_ud *ud = (struct state_ud *)luaL_checkudata(L, 1, "BOXMATRIXSTATE");
	if (ud->L) {
		lua_Integer sz = lua_gc(ud->L, LUA_GCCOUNT, 0);  // 获取内存使用量（KB）
		sz *= 1024;  // 转换为字节
		sz += lua_gc(ud->L, LUA_GCCOUNTB, 0);  // 加上余数字节
		lua_pushinteger(L, sz);
	} else {
		lua_pushinteger(L, 0);  // 状态已关闭，返回0
	}
	return 1;
}


// 装箱状态（创建状态用户数据）
static int
box_state(lua_State *L, lua_State *mL) {
	struct state_ud *ud = (struct state_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->L = mL;  // 保存状态指针
	if (luaL_newmetatable(L, "BOXMATRIXSTATE")) {  // 创建元表
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");  // 设置索引元方法
		lua_pushcfunction(L, close_state);
		lua_setfield(L, -2, "close");  // 关闭方法
		lua_pushcfunction(L, get_matrix);
		lua_setfield(L, -2, "getptr");  // 获取指针方法
		lua_pushcfunction(L, get_size);
		lua_setfield(L, -2, "size");  // 获取大小方法
	}
	lua_setmetatable(L, -2);  // 设置元表

	return 1;
}

// 加载矩阵文件
static int
load_matrixfile(lua_State *L) {
	luaL_openlibs(L);  // 打开标准库
	const char * source = (const char *)lua_touserdata(L, 1);
	if (source[0] == '@') {
		// 从文件加载
		if (luaL_loadfilex_(L, source+1, NULL) != LUA_OK)
			lua_error(L);
	} else {
		// 从字符串加载
		if (luaL_loadstring(L, source) != LUA_OK)
			lua_error(L);
	}
	lua_replace(L, 1);  // 替换源码参数
	if (lua_pcall(L, lua_gettop(L) - 1, 1, 0) != LUA_OK)  // 执行代码
		lua_error(L);
	lua_gc(L, LUA_GCCOLLECT, 0);  // 垃圾回收
	lua_pushcfunction(L, make_matrix);  // 推送矩阵创建函数
	lua_insert(L, -2);  // 插入到结果前面
	lua_call(L, 1, 1);  // 调用创建矩阵
	return 1;
}

// Lua 接口：从文件创建矩阵
static int
matrix_from_file(lua_State *L) {
	lua_State *mL = luaL_newstate();  // 创建新的 Lua 状态机
	if (mL == NULL) {
		return luaL_error(L, "luaL_newstate failed");
	}
	const char * source = luaL_checkstring(L, 1);  // 获取源码字符串
	int top = lua_gettop(L);  // 获取参数数量
	lua_pushcfunction(mL, load_matrixfile);  // 推送加载函数
	lua_pushlightuserdata(mL, (void *)source);  // 推送源码
	if (top > 1) {  // 如果有额外参数
		if (!lua_checkstack(mL, top + 1)) {
			return luaL_error(L, "Too many argument %d", top);
		}
		int i;
		// 复制参数到新状态机
		for (i=2;i<=top;i++) {
			switch(lua_type(L, i)) {
			case LUA_TBOOLEAN:
				lua_pushboolean(mL, lua_toboolean(L, i));  // 布尔值
				break;
			case LUA_TNUMBER:
				if (lua_isinteger(L, i)) {
					lua_pushinteger(mL, lua_tointeger(L, i));  // 整数
				} else {
					lua_pushnumber(mL, lua_tonumber(L, i));  // 浮点数
				}
				break;
			case LUA_TLIGHTUSERDATA:
				lua_pushlightuserdata(mL, lua_touserdata(L, i));  // 轻量用户数据
				break;
			case LUA_TFUNCTION:
				if (lua_iscfunction(L, i) && lua_getupvalue(L, i, 1) == NULL) {
					lua_pushcfunction(mL, lua_tocfunction(L, i));  // C函数（无上值）
					break;
				}
				return luaL_argerror(L, i, "Only support light C function");
			default:
				return luaL_argerror(L, i, "Type invalid");
			}
		}
	}
	int ok = lua_pcall(mL, top, 1, 0);  // 调用加载函数
	if (ok != LUA_OK) {
		lua_pushstring(L, lua_tostring(mL, -1));  // 获取错误信息
		lua_close(mL);  // 关闭状态机
		lua_error(L);  // 抛出错误
	}
	return box_state(L, mL);  // 装箱状态并返回
}

// sharetable 核心模块初始化函数
LUAMOD_API int
luaopen_skynet_sharetable_core(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "clone", clone_table },         // 克隆共享表
		{ "stackvalues", lco_stackvalues }, // 获取协程栈值
		{ "matrix", matrix_from_file },   // 从文件创建矩阵
		{ "is_sharedtable", lis_sharedtable }, // 检查是否为共享表
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

#else

// 不支持共享字符串表时的模块初始化
LUAMOD_API int
luaopen_skynet_sharetable_core(lua_State *L) {
	return luaL_error(L, "No share string table support");
}

#endif