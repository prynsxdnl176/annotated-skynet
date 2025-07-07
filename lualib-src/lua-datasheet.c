#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>

// 缓存键名定义
#define NODECACHE "_ctable"   // 节点缓存
#define PROXYCACHE "_proxy"   // 代理缓存
#define TABLES "_ctables"     // 表缓存

// 值类型定义
#define VALUE_NIL 0       // 空值
#define VALUE_INTEGER 1   // 整数
#define VALUE_REAL 2      // 实数
#define VALUE_BOOLEAN 3   // 布尔值
#define VALUE_TABLE 4     // 表
#define VALUE_STRING 5    // 字符串
#define VALUE_INVALID 6   // 无效值

#define INVALID_OFFSET 0xffffffff  // 无效偏移量

// 代理结构
struct proxy {
	const char * data;  // 数据指针
	int index;          // 索引
};

// 文档结构
struct document {
	uint32_t strtbl;    // 字符串表偏移
	uint32_t n;         // 表数量
	uint32_t index[1];  // 表索引数组
	// table[n]         // 表数据
	// strings          // 字符串数据
};

// 表结构
struct table {
	uint32_t array;     // 数组元素数量
	uint32_t dict;      // 字典元素数量
	uint8_t type[1];    // 类型数组
	// value[array]     // 数组值
	// kvpair[dict]     // 键值对
};

// 根据索引获取表结构
static inline const struct table *
gettable(const struct document *doc, int index) {
	if (doc->index[index] == INVALID_OFFSET) {
		return NULL;  // 无效索引
	}
	// 计算表在文档中的位置
	return (const struct table *)((const char *)doc + sizeof(uint32_t) + sizeof(uint32_t) + doc->n * sizeof(uint32_t) + doc->index[index]);
}

// 创建表代理
static void
create_proxy(lua_State *L, const void *data, int index) {
	const struct table * t = gettable(data, index);
	if (t == NULL) {
		luaL_error(L, "Invalid index %d", index);
	}
	// 检查节点缓存中是否已存在
	lua_getfield(L, LUA_REGISTRYINDEX, NODECACHE);
	if (lua_rawgetp(L, -1, t) == LUA_TTABLE) {
		// 已存在，直接返回
		lua_replace(L, -2);
		return;
	}
	lua_pop(L, 1);
	// 创建新表
	lua_newtable(L);
	lua_pushvalue(L, lua_upvalueindex(1));  // 设置元表
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -1);
	// NODECACHE, table, table
	lua_rawsetp(L, -3, t);  // 缓存表
	// NODECACHE, table
	lua_getfield(L, LUA_REGISTRYINDEX, PROXYCACHE);
	// NODECACHE, table, PROXYCACHE
	lua_pushvalue(L, -2);
	// NODECACHE, table, PROXYCACHE, table
	struct proxy * p = lua_newuserdatauv(L, sizeof(struct proxy), 0);
	// NODECACHE, table, PROXYCACHE, table, proxy
	p->data = data;   // 设置数据指针
	p->index = index; // 设置索引
	lua_rawset(L, -3);
	// NODECACHE, table, PROXYCACHE
	lua_pop(L, 1);
	// NODECACHE, table
	lua_replace(L, -2);
	// table
}

// 清理表缓存
static void
clear_table(lua_State *L) {
	int t = lua_gettop(L);	// clear top table
	                        // 清理栈顶表
	if (lua_type(L, t) != LUA_TTABLE) {
		luaL_error(L, "Invalid cache");
	}
	lua_pushnil(L);
	// 遍历表并清空所有键值对
	while (lua_next(L, t) != 0) {
		// key value
		lua_pop(L, 1);        // 弹出值
		lua_pushvalue(L, -1); // 复制键
		lua_pushnil(L);       // 推入 nil
		// key key nil
		lua_rawset(L, t);     // 设置键为 nil
		// key
	}
}

// 更新缓存（当数据更新时同步更新所有相关的代理表）
static void
update_cache(lua_State *L, const void *data, const void * newdata) {
	lua_getfield(L, LUA_REGISTRYINDEX, NODECACHE);  // 获取节点缓存
	int t = lua_gettop(L);
	lua_getfield(L, LUA_REGISTRYINDEX, PROXYCACHE);  // 获取代理缓存
	int pt = t + 1;
	lua_newtable(L);	// temp table
	                    // 临时表
	int nt = pt + 1;
	lua_pushnil(L);
	while (lua_next(L, t) != 0) {  // 遍历节点缓存
		// pointer (-2) -> table (-1)
		// 指针 (-2) -> 表 (-1)
		lua_pushvalue(L, -1);
		if (lua_rawget(L, pt) == LUA_TUSERDATA) {  // 在代理缓存中查找
			// pointer, table, proxy
			// 指针，表，代理
			struct proxy * p = lua_touserdata(L, -1);
			if (p->data == data) {  // 如果是需要更新的数据
				// update to newdata
				// 更新为新数据
				p->data = newdata;
				const struct table * newt = gettable(newdata, p->index);
				lua_pop(L, 1);
				// pointer, table
				// 指针，表
				clear_table(L);  // 清空表内容
				lua_pushvalue(L, lua_upvalueindex(1));
				// pointer, table, meta
				// 指针，表，元表
				lua_setmetatable(L, -2);  // 设置元表
				// pointer, table
				// 指针，表
				if (newt) {
					lua_rawsetp(L, nt, newt);  // 保存到临时表
				} else {
					lua_pop(L, 1);
				}
				// pointer
				// 指针
				lua_pushvalue(L, -1);
				lua_pushnil(L);
				lua_rawset(L, t);  // 从原缓存中移除
			} else {
				lua_pop(L, 2);
			}
		} else {
			lua_pop(L, 2);
			// pointer
			// 指针
		}
	}
	// copy nt to t
	// 将临时表复制到节点缓存
	lua_pushnil(L);
	while (lua_next(L, nt) != 0) {
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		// key key value
		// 键 键 值
		lua_rawset(L, t);
	}
	// NODECACHE PROXYCACHE TEMP
	lua_pop(L, 3);
}

// Lua 接口：更新数据表
static int
lupdate(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, PROXYCACHE);
	lua_pushvalue(L, 1);
	// PROXYCACHE, table
	if (lua_rawget(L, -2) != LUA_TUSERDATA) {
		luaL_error(L, "Invalid proxy table %p", lua_topointer(L, 1));
	}
	struct proxy * p = lua_touserdata(L, -1);  // 获取代理对象
	luaL_checktype(L, 2, LUA_TLIGHTUSERDATA);  // 检查新数据类型
	const char * newdata = lua_touserdata(L, 2);  // 获取新数据
	update_cache(L, p->data, newdata);  // 更新缓存
	return 1;
}

// 获取32位无符号整数（处理字节序）
static inline uint32_t
getuint32(const void *v) {
	union {
		uint32_t d;
		uint8_t t[4];
	} test = { 1 };
	if (test.t[0] == 0) {
		// big endian
		// 大端序
		test.d = *(const uint32_t *)v;
		return test.t[0] | test.t[1] << 4 | test.t[2] << 8 | test.t[3] << 12;
	} else {
		return *(const uint32_t *)v;  // 小端序直接返回
	}
}

// 获取浮点数（处理字节序）
static inline float
getfloat(const void *v) {
	union {
		uint32_t d;
		float f;
		uint8_t t[4];
	} test = { 1 };
	if (test.t[0] == 0) {
		// big endian
		// 大端序
		test.d = *(const uint32_t *)v;
		test.d = test.t[0] | test.t[1] << 4 | test.t[2] << 8 | test.t[3] << 12;
		return test.f;
	} else {
		return *(const float *)v;  // 小端序直接返回
	}
}

// 推送值到 Lua 栈（根据类型转换）
static void
pushvalue(lua_State *L, const void *v, int type, const struct document * doc) {
	switch (type) {
	case VALUE_NIL:
		lua_pushnil(L);  // 空值
		break;
	case VALUE_INTEGER:
		lua_pushinteger(L, (int32_t)getuint32(v));  // 整数
		break;
	case VALUE_REAL:
		lua_pushnumber(L, getfloat(v));  // 浮点数
		break;
	case VALUE_BOOLEAN:
		lua_pushboolean(L, getuint32(v));  // 布尔值
		break;
	case VALUE_TABLE:
		create_proxy(L, doc, getuint32(v));  // 表（创建代理）
		break;
	case VALUE_STRING:
		lua_pushstring(L,  (const char *)doc + doc->strtbl + getuint32(v));  // 字符串
		break;
	default:
		luaL_error(L, "Invalid type %d at %p", type, v);
	}
}

// 复制表数据到 Lua 表
static void
copytable(lua_State *L, int tbl, struct proxy *p) {
	const struct document * doc = (const struct document *)p->data;
	if (p->index < 0 || p->index >= doc->n) {
		luaL_error(L, "Invalid proxy (index = %d, total = %d)", p->index, (int)doc->n);
	}
	const struct table * t = gettable(doc, p->index);  // 获取表结构
	if (t == NULL) {
		luaL_error(L, "Invalid proxy (index = %d)", p->index);
	}
	// 计算值数组的起始位置（考虑对齐）
	const uint32_t * v = (const uint32_t *)((const char *)t + sizeof(uint32_t) + sizeof(uint32_t) + ((t->array + t->dict + 3) & ~3));
	int i;
	// 复制数组部分
	for (i=0;i<t->array;i++) {
		pushvalue(L, v++, t->type[i], doc);
		lua_rawseti(L, tbl, i+1);  // 设置数组元素（基于1的索引）
	}
	// 复制字典部分
	for (i=0;i<t->dict;i++) {
		pushvalue(L, v++, VALUE_STRING, doc);  // 键（字符串）
		pushvalue(L, v++, t->type[t->array+i], doc);  // 值
		lua_rawset(L, tbl);  // 设置键值对
	}
}

// Lua 接口：创建新的数据表代理
static int
lnew(lua_State *L) {
	luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
	const char * data = lua_touserdata(L, 1);
	// hold ref to data
	// 持有数据引用
	lua_getfield(L, LUA_REGISTRYINDEX, TABLES);
	lua_pushvalue(L, 1);
	lua_rawsetp(L, -2, data);  // 保存数据引用

	create_proxy(L, data, 0);  // 创建根表的代理
	return 1;
}

// 从数据复制到 Lua 表
static void
copyfromdata(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, PROXYCACHE);
	lua_pushvalue(L, 1);
	// PROXYCACHE, table
	if (lua_rawget(L, -2) != LUA_TUSERDATA) {
		luaL_error(L, "Invalid proxy table %p", lua_topointer(L, 1));
	}
	struct proxy * p = lua_touserdata(L, -1);  // 获取代理对象
	lua_pop(L, 2);
	copytable(L, 1, p);  // 复制表数据
	lua_pushnil(L);
	lua_setmetatable(L, 1);	// remove metatable
	                        // 移除元表
}

// Lua 接口：索引操作
static int
lindex(lua_State *L) {
	copyfromdata(L);  // 复制数据到表
	lua_rawget(L, 1);  // 获取索引值
	return 1;
}

// Lua 接口：next 迭代器
static int
lnext(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_settop(L, 2);  /* create a 2nd argument if there isn't one */
	                   /* 如果没有第二个参数则创建一个 */
	if (lua_next(L, 1))
		return 2;  // 返回键值对
	else {
		lua_pushnil(L);
		return 1;  // 迭代结束
	}
}

// Lua 接口：pairs 迭代器
static int
lpairs(lua_State *L) {
	copyfromdata(L);  // 复制数据到表
	lua_pushcfunction(L, lnext);  // 推送 next 函数
	lua_pushvalue(L, 1);  // 推送表
	lua_pushnil(L);  // 推送初始键
	return 3;  // 返回迭代器三元组
}

// Lua 接口：获取表长度
static int
llen(lua_State *L) {
	copyfromdata(L);  // 复制数据到表
	lua_pushinteger(L, lua_rawlen(L, 1));  // 返回表长度
	return 1;
}

// 创建弱引用表
static void
new_weak_table(lua_State *L, const char *mode) {
	lua_newtable(L);	// NODECACHE { pointer:table }
	                    // 节点缓存 { 指针:表 }

	lua_createtable(L, 0, 1);	// weak meta table
	                            // 弱引用元表
	lua_pushstring(L, mode);
	lua_setfield(L, -2, "__mode");  // 设置弱引用模式

	lua_setmetatable(L, -2);	// make NODECACHE weak
	                            // 使节点缓存成为弱引用
}

// 生成元表
static void
gen_metatable(lua_State *L) {
	new_weak_table(L, "kv");	// NODECACHE { pointer:table }
	                            // 节点缓存 { 指针:表 }
	lua_setfield(L, LUA_REGISTRYINDEX, NODECACHE);

	new_weak_table(L, "k");	// PROXYCACHE { table:userdata }
	                        // 代理缓存 { 表:用户数据 }
	lua_setfield(L, LUA_REGISTRYINDEX, PROXYCACHE);

	lua_newtable(L);  // 表引用缓存
	lua_setfield(L, LUA_REGISTRYINDEX, TABLES);

	lua_createtable(L, 0, 1);	// mod table
	                            // 模块表

	lua_createtable(L, 0, 2);	// metatable
	                            // 元表
	luaL_Reg l[] = {
		{ "__index", lindex },  // 索引访问
		{ "__pairs", lpairs },  // 遍历支持
		{ "__len", llen },      // 长度操作
		{ NULL, NULL },
	};
	lua_pushvalue(L, -1);
	luaL_setfuncs(L, l, 1);  // 设置元方法
}

// Lua 接口：获取字符串指针
static int
lstringpointer(lua_State *L) {
	const char * str = luaL_checkstring(L, 1);
	lua_pushlightuserdata(L, (void *)str);  // 返回字符串指针
	return 1;
}

// datasheet 核心模块初始化函数
LUAMOD_API int
luaopen_skynet_datasheet_core(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "new", lnew },        // 创建新数据表
		{ "update", lupdate },  // 更新数据表
		{ NULL, NULL },
	};

	luaL_newlibtable(L,l);
	gen_metatable(L);       // 生成元表
	luaL_setfuncs(L, l, 1); // 设置函数（带元表作为 upvalue）
	lua_pushcfunction(L, lstringpointer);
	lua_setfield(L, -2, "stringpointer");  // 添加字符串指针函数
	return 1;
}
