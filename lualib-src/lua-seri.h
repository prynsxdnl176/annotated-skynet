#ifndef LUA_SERIALIZE_H
#define LUA_SERIALIZE_H

#include <lua.h>

// Lua 序列化函数声明

// 打包 Lua 值为二进制数据
int luaseri_pack(lua_State *L);

// 从二进制数据解包为 Lua 值
int luaseri_unpack(lua_State *L);

#endif
