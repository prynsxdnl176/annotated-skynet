#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "atomic.h"

// 键类型定义
#define KEYTYPE_INTEGER 0  // 整数键
#define KEYTYPE_STRING 1   // 字符串键

// 值类型定义
#define VALUETYPE_NIL 0     // 空值
#define VALUETYPE_REAL 1    // 实数
#define VALUETYPE_STRING 2  // 字符串
#define VALUETYPE_BOOLEAN 3 // 布尔值
#define VALUETYPE_TABLE 4   // 表
#define VALUETYPE_INTEGER 5 // 整数

struct table;

// 值联合体
union value {
	lua_Number n;       // 数字值
	lua_Integer d;      // 整数值
	struct table * tbl; // 表指针
	int string;         // 字符串索引
	int boolean;        // 布尔值
};

// 哈希节点结构
struct node {
	union value v;       // 值
	int key;	         // integer key or index of string table
	                     // 整数键或字符串表索引
	int next;	         // next slot index
	                     // 下一个槽位索引
	uint32_t keyhash;    // 键的哈希值
	uint8_t keytype;	 // key type must be integer or string
	                     // 键类型必须是整数或字符串
	uint8_t valuetype;	 // value type can be number/string/boolean/table
	                     // 值类型可以是数字/字符串/布尔值/表
	uint8_t nocolliding; // 0 means colliding slot
	                     // 0表示冲突槽位
};

// 共享数据状态
struct state {
	int dirty;           // 脏标记
	ATOM_INT ref;        // 原子引用计数
	struct table * root; // 根表指针
};

// 表结构
struct table {
	int sizearray;       // 数组部分大小
	int sizehash;        // 哈希部分大小
	uint8_t *arraytype;  // 数组类型数组
	union value * array; // 数组值数组
	struct node * hash;  // 哈希节点数组
	lua_State * L;       // Lua 状态机
};

// 上下文结构
struct context {
	lua_State * L;       // Lua 状态机
	struct table * tbl;  // 当前表
	int string_index;    // 字符串索引
};

// 控制结构
struct ctrl {
	struct table * root;   // 根表
	struct table * update; // 更新表
};

// 计算哈希表大小（排除数组部分的键）
static int
countsize(lua_State *L, int sizearray) {
	int n = 0;
	lua_pushnil(L);
	// 遍历表中的所有键值对
	while (lua_next(L, 1) != 0) {
		int type = lua_type(L, -2);
		++n;
		if (type == LUA_TNUMBER) {
			if (!lua_isinteger(L, -2)) {
				luaL_error(L, "Invalid key %f", lua_tonumber(L, -2));
			}
			lua_Integer nkey = lua_tointeger(L, -2);
			if (nkey > 0 && nkey <= sizearray) {
				// 属于数组部分的键，不计入哈希表
				--n;
			}
		} else if (type != LUA_TSTRING && type != LUA_TTABLE) {
			luaL_error(L, "Invalid key type %s", lua_typename(L, type));
		}
		lua_pop(L, 1);
	}

	return n;
}

// 计算字符串哈希值
static uint32_t
calchash(const char * str, size_t l) {
	uint32_t h = (uint32_t)l;  // 初始哈希值为字符串长度
	size_t l1;
	size_t step = (l >> 5) + 1;  // 步长
	// 按步长采样字符计算哈希
	for (l1 = l; l1 >= step; l1 -= step) {
		h = h ^ ((h<<5) + (h>>2) + (uint8_t)(str[l1 - 1]));
	}
	return h;
}

// 获取字符串在字符串表中的索引
static int
stringindex(struct context *ctx, const char * str, size_t sz) {
	lua_State *L = ctx->L;
	lua_pushlstring(L, str, sz);  // 推送字符串
	lua_pushvalue(L, -1);         // 复制字符串作为键
	lua_rawget(L, 1);             // 在字符串表中查找
	int index;
	// stringmap(1) str index
	// 字符串映射表(1) 字符串 索引
	if (lua_isnil(L, -1)) {
		// 字符串不存在，分配新索引
		index = ++ctx->string_index;
		lua_pop(L, 1);
		lua_pushinteger(L, index);  // 推送新索引
		lua_rawset(L, 1);           // 设置映射关系
	} else {
		// 字符串已存在，获取索引
		index = lua_tointeger(L, -1);
		lua_pop(L, 2);  // 弹出索引和字符串
	}
	return index;
}

static int convtable(lua_State *L);

// 设置节点的值（根据 Lua 值类型转换为内部表示）
static void
setvalue(struct context * ctx, lua_State *L, int index, struct node *n) {
	int vt = lua_type(L, index);  // 获取值类型
	switch(vt) {
	case LUA_TNIL:
		n->valuetype = VALUETYPE_NIL;  // 空值
		break;
	case LUA_TNUMBER:
		if (lua_isinteger(L, index)) {
			n->v.d = lua_tointeger(L, index);  // 整数
			n->valuetype = VALUETYPE_INTEGER;
		} else {
			n->v.n = lua_tonumber(L, index);   // 浮点数
			n->valuetype = VALUETYPE_REAL;
		}
		break;
	case LUA_TSTRING: {
		size_t sz = 0;
		const char * str = lua_tolstring(L, index, &sz);
		n->v.string = stringindex(ctx, str, sz);  // 字符串索引
		n->valuetype = VALUETYPE_STRING;
		break;
	}
	case LUA_TBOOLEAN:
		n->v.boolean = lua_toboolean(L, index);  // 布尔值
		n->valuetype = VALUETYPE_BOOLEAN;
		break;
	case LUA_TTABLE: {
		// 表类型，需要递归转换
		struct table *tbl = ctx->tbl;
		ctx->tbl = (struct table *)malloc(sizeof(struct table));
		if (ctx->tbl == NULL) {
			ctx->tbl = tbl;
			luaL_error(L, "memory error");
			// never get here
			// 永远不会到达这里
		}
		memset(ctx->tbl, 0, sizeof(struct table));  // 初始化表结构
		int absidx = lua_absindex(L, index);        // 获取绝对索引

		lua_pushcfunction(L, convtable);  // 推送转换函数
		lua_pushvalue(L, absidx);         // 推送表
		lua_pushlightuserdata(L, ctx);    // 推送上下文

		lua_call(L, 2, 0);  // 调用转换函数

		n->v.tbl = ctx->tbl;           // 设置表指针
		n->valuetype = VALUETYPE_TABLE;

		ctx->tbl = tbl;  // 恢复原表指针

		break;
	}
	default:
		luaL_error(L, "Unsupport value type %s", lua_typename(L, vt));
		break;
	}
}

// 设置数组元素
static void
setarray(struct context *ctx, lua_State *L, int index, int key) {
	struct node n;
	setvalue(ctx, L, index, &n);  // 转换值
	struct table *tbl = ctx->tbl;
	--key;	// base 0  // 转换为基于0的索引
	tbl->arraytype[key] = n.valuetype;  // 设置类型
	tbl->array[key] = n.v;              // 设置值
}

// 判断键是否为哈希键（非数组索引）
static int
ishashkey(struct context * ctx, lua_State *L, int index, int *key, uint32_t *keyhash, int *keytype) {
	int sizearray = ctx->tbl->sizearray;
	int kt = lua_type(L, index);  // 键的类型
	if (kt == LUA_TNUMBER) {
		*key = lua_tointeger(L, index);
		if (*key > 0 && *key <= sizearray) {
			return 0;  // 是数组索引
		}
		*keyhash = (uint32_t)*key;     // 数字键的哈希值
		*keytype = KEYTYPE_INTEGER;
	} else {
		// 字符串键
		size_t sz = 0;
		const char * s = lua_tolstring(L, index, &sz);
		*keyhash = calchash(s, sz);           // 计算哈希值
		*key = stringindex(ctx, s, sz);       // 获取字符串索引
		*keytype = KEYTYPE_STRING;
	}
	return 1;  // 是哈希键
}

// 填充无冲突的哈希表项
static void
fillnocolliding(lua_State *L, struct context *ctx) {
	struct table * tbl = ctx->tbl;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {  // 遍历表
		int key;
		int keytype;
		uint32_t keyhash;
		if (!ishashkey(ctx, L, -2, &key, &keyhash, &keytype)) {
			// 数组索引
			setarray(ctx, L, -1, key);
		} else {
			// 哈希键，计算主位置
			struct node * n = &tbl->hash[keyhash % tbl->sizehash];
			if (n->valuetype == VALUETYPE_NIL) {
				// 主位置为空，直接插入
				n->key = key;
				n->keytype = keytype;
				n->keyhash = keyhash;
				n->next = -1;           // 无下一个节点
				n->nocolliding = 1;     // 标记为无冲突
				setvalue(ctx, L, -1, n);	// set n->v , n->valuetype
				                        // 设置 n->v, n->valuetype
			}
		}
		lua_pop(L,1);
	}
}

// 填充有冲突的哈希表项
static void
fillcolliding(lua_State *L, struct context *ctx) {
	struct table * tbl = ctx->tbl;
	int sizehash = tbl->sizehash;
	int emptyslot = 0;  // 空槽位搜索起始位置
	int i;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {  // 遍历表
		int key;
		int keytype;
		uint32_t keyhash;
		if (ishashkey(ctx, L, -2, &key, &keyhash, &keytype)) {
			struct node * mainpos = &tbl->hash[keyhash % tbl->sizehash];
			if (!(mainpos->keytype == keytype && mainpos->key == key)) {
				// the key has not insert
				// 键尚未插入
				struct node * n = NULL;
				// 寻找空槽位
				for (i=emptyslot;i<sizehash;i++) {
					if (tbl->hash[i].valuetype == VALUETYPE_NIL) {
						n = &tbl->hash[i];
						emptyslot = i + 1;  // 更新搜索起始位置
						break;
					}
				}
				assert(n);
				// 链接到冲突链表
				n->next = mainpos->next;
				mainpos->next = n - tbl->hash;
				mainpos->nocolliding = 0;  // 标记主位置有冲突
				n->key = key;
				n->keytype = keytype;
				n->keyhash = keyhash;
				n->nocolliding = 0;        // 标记为冲突节点
				setvalue(ctx, L, -1, n);	// set n->v , n->valuetype
				                        // 设置 n->v, n->valuetype
			}
		}
		lua_pop(L,1);
	}
}

// table need convert
// struct context * ctx
// 需要转换的表
// 结构体 context * ctx
// 转换 Lua 表为内部表示
static int
convtable(lua_State *L) {
	int i;
	struct context *ctx = lua_touserdata(L,2);
	struct table *tbl = ctx->tbl;

	tbl->L = ctx->L;  // 设置 Lua 状态机

	int sizearray = lua_rawlen(L, 1);  // 获取数组部分大小
	if (sizearray) {
		// 分配数组类型数组
		tbl->arraytype = (uint8_t *)malloc(sizearray * sizeof(uint8_t));
		if (tbl->arraytype == NULL) {
			goto memerror;
		}
		// 初始化数组类型为 NIL
		for (i=0;i<sizearray;i++) {
			tbl->arraytype[i] = VALUETYPE_NIL;
		}
		// 分配数组值数组
		tbl->array = (union value *)malloc(sizearray * sizeof(union value));
		if (tbl->array == NULL) {
			goto memerror;
		}
		tbl->sizearray = sizearray;
	}
	int sizehash = countsize(L, sizearray);  // 计算哈希部分大小
	if (sizehash) {
		// 分配哈希表
		tbl->hash = (struct node *)malloc(sizehash * sizeof(struct node));
		if (tbl->hash == NULL) {
			goto memerror;
		}
		// 初始化哈希表节点
		for (i=0;i<sizehash;i++) {
			tbl->hash[i].valuetype = VALUETYPE_NIL;
			tbl->hash[i].nocolliding = 0;
			tbl->hash[i].next = -1;
		}
		tbl->sizehash = sizehash;

		fillnocolliding(L, ctx);  // 填充无冲突项
		fillcolliding(L, ctx);    // 填充冲突项
	} else {
		// 只有数组部分
		int i;
		for (i=1;i<=sizearray;i++) {
			lua_rawgeti(L, 1, i);     // 获取数组元素
			setarray(ctx, L, -1, i);  // 设置数组元素
			lua_pop(L,1);
		}
	}

	return 0;
memerror:
	return luaL_error(L, "memory error");
}

// 删除表（递归释放内存）
static void
delete_tbl(struct table *tbl) {
	int i;
	// 删除数组部分的子表
	for (i=0;i<tbl->sizearray;i++) {
		if (tbl->arraytype[i] == VALUETYPE_TABLE) {
			delete_tbl(tbl->array[i].tbl);  // 递归删除子表
		}
	}
	// 删除哈希部分的子表
	for (i=0;i<tbl->sizehash;i++) {
		if (tbl->hash[i].valuetype == VALUETYPE_TABLE) {
			delete_tbl(tbl->hash[i].v.tbl);  // 递归删除子表
		}
	}
	// 释放表的各个部分
	free(tbl->arraytype);  // 释放数组类型数组
	free(tbl->array);      // 释放数组值数组
	free(tbl->hash);       // 释放哈希表
	free(tbl);             // 释放表结构本身
}

// 转换函数（在独立的 Lua 状态机中执行）
static int
pconv(lua_State *L) {
	struct context *ctx = lua_touserdata(L,1);  // 获取上下文
	lua_State * pL = lua_touserdata(L, 2);      // 获取父状态机
	int ret;

	lua_settop(L, 0);  // 清空栈

	// init L (may throw memory error)
	// create a table for string map
	// 初始化 L（可能抛出内存错误）
	// 为字符串映射创建表
	lua_newtable(L);

	lua_pushcfunction(pL, convtable);     // 推送转换表函数
	lua_pushvalue(pL,1);                  // 推送要转换的表
	lua_pushlightuserdata(pL, ctx);       // 推送上下文

	ret = lua_pcall(pL, 2, 0, 0);  // 调用转换函数
	if (ret != LUA_OK) {
		// 转换失败，传递错误信息
		size_t sz = 0;
		const char * error = lua_tolstring(pL, -1, &sz);
		lua_pushlstring(L, error, sz);
		lua_error(L);
		// never get here
		// 永远不会到这里
	}

	luaL_checkstack(L, ctx->string_index + 3, NULL);  // 检查栈空间
	lua_settop(L,1);  // 设置栈顶

	return 1;
}

// 转换字符串映射表
static void
convert_stringmap(struct context *ctx, struct table *tbl) {
	lua_State *L = ctx->L;
	lua_checkstack(L, ctx->string_index + LUA_MINSTACK);  // 检查栈空间
	lua_settop(L, ctx->string_index + 1);  // 设置栈顶位置
	lua_pushvalue(L, 1);  // 复制字符串映射表
	struct state * s = lua_newuserdatauv(L, sizeof(*s), 1);  // 创建状态用户数据
	s->dirty = 0;         // 初始化脏标志
	ATOM_INIT(&s->ref , 0);  // 初始化原子引用计数
	s->root = tbl;        // 设置根表
	lua_replace(L, 1);    // 替换位置1的值
	lua_replace(L, -2);   // 替换倒数第二个值

	lua_pushnil(L);  // 推送 nil 作为初始键
	// ... stringmap nil
	// ... 字符串映射表 nil
	while (lua_next(L, -2) != 0) {  // 遍历字符串映射表
		int idx = lua_tointeger(L, -1);  // 获取索引
		lua_pop(L, 1);        // 弹出索引值
		lua_pushvalue(L, -1); // 复制字符串
		lua_replace(L, idx);  // 将字符串放到对应索引位置
	}

	lua_pop(L, 1);  // 弹出字符串映射表

	lua_gc(L, LUA_GCCOLLECT, 0);  // 执行垃圾回收
}

// Lua 接口：创建新的共享数据配置
static int
lnewconf(lua_State *L) {
	int ret;
	struct context ctx;
	struct table * tbl = NULL;
	luaL_checktype(L,1,LUA_TTABLE);  // 检查参数为表
	ctx.L = luaL_newstate();         // 创建新的 Lua 状态机
	ctx.tbl = NULL;
	ctx.string_index = 1;	// 1 reserved for dirty flag
	                        // 1 保留给脏标志
	if (ctx.L == NULL) {
		lua_pushliteral(L, "memory error");
		goto error;
	}
	tbl = (struct table *)malloc(sizeof(struct table));
	if (tbl == NULL) {
		// lua_pushliteral may fail because of memory error, close first.
		// lua_pushliteral 可能因内存错误失败，先关闭
		lua_close(ctx.L);
		ctx.L = NULL;
		lua_pushliteral(L, "memory error");
		goto error;
	}
	memset(tbl, 0, sizeof(struct table));  // 初始化表结构
	ctx.tbl = tbl;

	lua_pushcfunction(ctx.L, pconv);       // 推送转换函数
	lua_pushlightuserdata(ctx.L , &ctx);   // 推送上下文
	lua_pushlightuserdata(ctx.L , L);      // 推送原始状态机

	ret = lua_pcall(ctx.L, 2, 1, 0);       // 调用转换函数

	if (ret != LUA_OK) {
		// 转换失败，获取错误信息
		size_t sz = 0;
		const char * error = lua_tolstring(ctx.L, -1, &sz);
		lua_pushlstring(L, error, sz);
		goto error;
	}

	convert_stringmap(&ctx, tbl);  // 转换字符串映射表

	lua_pushlightuserdata(L, tbl);	// 返回表指针

	return 1;
error:
	if (ctx.L) {
		lua_close(ctx.L);  // 关闭 Lua 状态机
	}
	if (tbl) {
		delete_tbl(tbl);   // 删除表
	}
	lua_error(L);
	return -1;
}

// 获取表对象（从用户数据）
static struct table *
get_table(lua_State *L, int index) {
	struct table *tbl = lua_touserdata(L,index);
	if (tbl == NULL) {
		luaL_error(L, "Need a conf object");
	}
	return tbl;
}

// Lua 接口：删除共享数据配置
static int
ldeleteconf(lua_State *L) {
	struct table *tbl = get_table(L,1);
	lua_close(tbl->L);  // 关闭 Lua 状态机
	delete_tbl(tbl);    // 删除表
	return 0;
}

// 推送值到 Lua 栈（根据类型转换）
static void
pushvalue(lua_State *L, lua_State *sL, uint8_t vt, union value *v) {
	switch(vt) {
	case VALUETYPE_REAL:
		lua_pushnumber(L, v->n);    // 浮点数
		break;
	case VALUETYPE_INTEGER:
		lua_pushinteger(L, v->d);   // 整数
		break;
	case VALUETYPE_STRING: {
		// 字符串，从字符串表中获取
		size_t sz = 0;
		const char *str = lua_tolstring(sL, v->string, &sz);
		lua_pushlstring(L, str, sz);
		break;
	}
	case VALUETYPE_BOOLEAN:
		lua_pushboolean(L, v->boolean);  // 布尔值
		break;
	case VALUETYPE_TABLE:
		lua_pushlightuserdata(L, v->tbl);  // 表指针
		break;
	default:
		lua_pushnil(L);  // 其他类型推送 nil
		break;
	}
}

static struct node *
lookup_key(struct table *tbl, uint32_t keyhash, int key, int keytype, const char *str, size_t sz) {
	if (tbl->sizehash == 0)
		return NULL;
	struct node *n = &tbl->hash[keyhash % tbl->sizehash];
	if (keyhash != n->keyhash && n->nocolliding)
		return NULL;
	for (;;) {
		if (keyhash == n->keyhash) {
			if (n->keytype == KEYTYPE_INTEGER) {
				if (keytype == KEYTYPE_INTEGER && n->key == key) {
					return n;
				}
			} else {
				// n->keytype == KEYTYPE_STRING
				if (keytype == KEYTYPE_STRING) {
					size_t sz2 = 0;
					const char * str2 = lua_tolstring(tbl->L, n->key, &sz2);
					if (sz == sz2 && memcmp(str,str2,sz) == 0) {
						return n;
					}
				}
			}
		}
		if (n->next < 0) {
			return NULL;
		}
		n = &tbl->hash[n->next];		
	}
}

// Lua 接口：索引共享数据配置
static int
lindexconf(lua_State *L) {
	struct table *tbl = get_table(L,1);
	int kt = lua_type(L,2);  // 键的类型
	uint32_t keyhash;
	int key = 0;
	int keytype;
	size_t sz = 0;
	const char * str = NULL;
	if (kt == LUA_TNIL) {
		return 0;  // nil 键返回 nil
	} else if (kt == LUA_TNUMBER) {
		if (!lua_isinteger(L, 2)) {
			return luaL_error(L, "Invalid key %f", lua_tonumber(L, 2));
		}
		key = (int)lua_tointeger(L, 2);
		if (key > 0 && key <= tbl->sizearray) {
			// 数组索引
			--key;  // 转换为基于0的索引
			pushvalue(L, tbl->L, tbl->arraytype[key], &tbl->array[key]);
			return 1;
		}
		keytype = KEYTYPE_INTEGER;
		keyhash = (uint32_t)key;
	} else {
		// 字符串键
		str = luaL_checklstring(L, 2, &sz);
		keyhash = calchash(str, sz);  // 计算哈希值
		keytype = KEYTYPE_STRING;
	}

	struct node *n = lookup_key(tbl, keyhash, key, keytype, str, sz);
	if (n) {
		pushvalue(L, tbl->L, n->valuetype, &n->v);  // 推送找到的值
		return 1;
	} else {
		return 0;  // 未找到，返回 nil
	}
}

// 推送键到 Lua 栈
static void
pushkey(lua_State *L, lua_State *sL, struct node *n) {
	if (n->keytype == KEYTYPE_INTEGER) {
		lua_pushinteger(L, n->key);  // 整数键
	} else {
		// 字符串键，从字符串表中获取
		size_t sz = 0;
		const char * str = lua_tolstring(sL, n->key, &sz);
		lua_pushlstring(L, str, sz);
	}
}

static int
pushfirsthash(lua_State *L, struct table * tbl) {
	if (tbl->sizehash) {
		pushkey(L, tbl->L, &tbl->hash[0]);
		return 1;
	} else {
		return 0;
	}
}

static int
lnextkey(lua_State *L) {
	struct table *tbl = get_table(L,1);
	if (lua_isnoneornil(L,2)) {
		if (tbl->sizearray > 0) {
			int i;
			for (i=0;i<tbl->sizearray;i++) {
				if (tbl->arraytype[i] != VALUETYPE_NIL) {
					lua_pushinteger(L, i+1);
					return 1;
				}
			}
		}
		return pushfirsthash(L, tbl);
	}
	int kt = lua_type(L,2);
	uint32_t keyhash;
	int key = 0;
	int keytype;
	size_t sz=0;
	const char *str = NULL;
	int sizearray = tbl->sizearray;
	if (kt == LUA_TNUMBER) {
		if (!lua_isinteger(L, 2)) {
			return 0;
		}
		key = (int)lua_tointeger(L, 2);
		if (key > 0 && key <= sizearray) {
			lua_Integer i;
			for (i=key;i<sizearray;i++) {
				if (tbl->arraytype[i] != VALUETYPE_NIL) {
					lua_pushinteger(L, i+1);
					return 1;
				}
			}
			return pushfirsthash(L, tbl);
		}
		keyhash = (uint32_t)key;
		keytype = KEYTYPE_INTEGER;
	} else {
		str = luaL_checklstring(L, 2, &sz);
		keyhash = calchash(str, sz);
		keytype = KEYTYPE_STRING;
	}

	struct node *n = lookup_key(tbl, keyhash, key, keytype, str, sz);
	if (n) {
		++n;
		int index = n-tbl->hash;
		if (index == tbl->sizehash) {
			return 0;
		}
		pushkey(L, tbl->L, n);
		return 1;
	} else {
		return 0;
	}
}

// Lua 接口：获取数组长度
static int
llen(lua_State *L) {
	struct table *tbl = get_table(L,1);
	lua_pushinteger(L, tbl->sizearray);  // 返回数组大小
	return 1;
}

// Lua 接口：获取哈希表长度
static int
lhashlen(lua_State *L) {
	struct table *tbl = get_table(L,1);
	lua_pushinteger(L, tbl->sizehash);  // 返回哈希表大小
	return 1;
}

// 释放对象（垃圾回收时调用）
static int
releaseobj(lua_State *L) {
	struct ctrl *c = lua_touserdata(L, 1);
	struct table *tbl = c->root;
	struct state *s = lua_touserdata(tbl->L, 1);
	ATOM_FDEC(&s->ref);  // 原子递减引用计数
	c->root = NULL;      // 清空根表指针
	c->update = NULL;    // 清空更新指针

	return 0;
}

// Lua 接口：装箱配置（创建控制结构）
static int
lboxconf(lua_State *L) {
	struct table * tbl = get_table(L,1);
	struct state * s = lua_touserdata(tbl->L, 1);
	ATOM_FINC(&s->ref);  // 原子递增引用计数

	struct ctrl * c = lua_newuserdatauv(L, sizeof(*c), 1);  // 创建控制结构
	c->root = tbl;       // 设置根表
	c->update = NULL;    // 初始化更新指针
	if (luaL_newmetatable(L, "confctrl")) {
		lua_pushcfunction(L, releaseobj);  // 设置垃圾回收函数
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);  // 设置元表

	return 1;
}

// Lua 接口：标记为脏数据
static int
lmarkdirty(lua_State *L) {
	struct table *tbl = get_table(L,1);
	struct state * s = lua_touserdata(tbl->L, 1);
	s->dirty = 1;  // 设置脏标志
	return 0;
}

static int
lisdirty(lua_State *L) {
	struct table *tbl = get_table(L,1);
	struct state * s = lua_touserdata(tbl->L, 1);
	int d = s->dirty;
	lua_pushboolean(L, d);
	
	return 1;
}

static int
lgetref(lua_State *L) {
	struct table *tbl = get_table(L,1);
	struct state * s = lua_touserdata(tbl->L, 1);
	lua_pushinteger(L , ATOM_LOAD(&s->ref));

	return 1;
}

static int
lincref(lua_State *L) {
	struct table *tbl = get_table(L,1);
	struct state * s = lua_touserdata(tbl->L, 1);
	int ref = ATOM_FINC(&s->ref)+1;
	lua_pushinteger(L , ref);

	return 1;
}

// Lua 接口：减少引用计数
static int
ldecref(lua_State *L) {
	struct table *tbl = get_table(L,1);
	struct state * s = lua_touserdata(tbl->L, 1);
	int ref = ATOM_FDEC(&s->ref)-1;  // 原子递减引用计数
	lua_pushinteger(L , ref);

	return 1;
}

// Lua 接口：检查是否需要更新
static int
lneedupdate(lua_State *L) {
	struct ctrl * c = lua_touserdata(L, 1);
	if (c->update) {
		// 有更新数据
		lua_pushlightuserdata(L, c->update);
		lua_getiuservalue(L, 1, 1);  // 获取关联的用户值
		return 2;
	}
	return 0;  // 无更新
}

// Lua 接口：更新共享数据
static int
lupdate(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);      // 控制结构
	luaL_checktype(L, 2, LUA_TLIGHTUSERDATA); // 新表指针
	luaL_checktype(L, 3, LUA_TTABLE);         // 字符串表
	struct ctrl * c= lua_touserdata(L, 1);
	struct table *n = lua_touserdata(L, 2);
	if (c->root == n) {
		return luaL_error(L, "You should update a new object");
	}
	lua_settop(L, 3);
	lua_setiuservalue(L, 1, 1);  // 设置用户值
	c->update = n;               // 设置更新表

	return 0;
}

// sharedata 核心模块初始化函数
LUAMOD_API int
luaopen_skynet_sharedata_core(lua_State *L) {
	luaL_Reg l[] = {
		// used by host
		// 主机端使用的函数
		{ "new", lnewconf },        // 创建新配置
		{ "delete", ldeleteconf },  // 删除配置
		{ "markdirty", lmarkdirty }, // 标记为脏
		{ "getref", lgetref },      // 获取引用计数
		{ "incref", lincref },      // 增加引用计数
		{ "decref", ldecref },      // 减少引用计数

		// used by client
		// 客户端使用的函数
		{ "box", lboxconf },        // 装箱配置
		{ "index", lindexconf },    // 索引访问
		{ "nextkey", lnextkey },    // 下一个键
		{ "len", llen },            // 数组长度
		{ "hashlen", lhashlen },    // 哈希长度
		{ "isdirty", lisdirty },    // 检查是否脏
		{ "needupdate", lneedupdate }, // 检查是否需要更新
		{ "update", lupdate },      // 更新数据
		{ NULL, NULL },
	};
	luaL_checkversion(L);
	luaL_newlib(L, l);

	return 1;
}
