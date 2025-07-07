#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "rwlock.h"
#include "skynet_malloc.h"
#include "atomic.h"

// STM（软件事务内存）对象结构
struct stm_object {
	struct rwlock lock;     // 读写锁
	ATOM_INT reference;     // 原子引用计数
	struct stm_copy * copy; // 数据副本指针
};

// STM 数据副本结构
struct stm_copy {
	ATOM_INT reference;  // 原子引用计数
	uint32_t sz;         // 数据大小
	void * msg;          // 数据指针
};

// msg should alloc by skynet_malloc
// 消息应该由 skynet_malloc 分配
// 创建新的数据副本
static struct stm_copy *
stm_newcopy(void * msg, int32_t sz) {
	struct stm_copy * copy = skynet_malloc(sizeof(*copy));
	ATOM_INIT(&copy->reference, 1);  // 初始引用计数为1
	copy->sz = sz;
	copy->msg = msg;

	return copy;
}

// 创建新的 STM 对象
static struct stm_object *
stm_new(void * msg, int32_t sz) {
	struct stm_object * obj = skynet_malloc(sizeof(*obj));
	rwlock_init(&obj->lock);         // 初始化读写锁
	ATOM_INIT(&obj->reference , 1);  // 初始引用计数为1
	obj->copy = stm_newcopy(msg, sz); // 创建初始副本

	return obj;
}

// 释放数据副本
static void
stm_releasecopy(struct stm_copy *copy) {
	if (copy == NULL)
		return;
	if (ATOM_FDEC(&copy->reference) <= 1) {
		// 引用计数降到0，释放资源
		skynet_free(copy->msg);
		skynet_free(copy);
	}
}

// 释放 STM 对象（写者调用）
static void
stm_release(struct stm_object *obj) {
	assert(obj->copy);
	rwlock_wlock(&obj->lock);  // 获取写锁
	// writer release the stm object, so release the last copy .
	// 写者释放 STM 对象，所以释放最后的副本
	stm_releasecopy(obj->copy);
	obj->copy = NULL;
	if (ATOM_FDEC(&obj->reference) > 1) {
		// stm object grab by readers, reset the copy to NULL.
		// STM 对象被读者持有，将副本重置为 NULL
		rwlock_wunlock(&obj->lock);
		return;
	}
	// no one grab the stm object, no need to unlock wlock.
	// 没有人持有 STM 对象，无需解锁写锁
	skynet_free(obj);
}

// 释放 STM 对象（读者调用）
static void
stm_releasereader(struct stm_object *obj) {
	rwlock_rlock(&obj->lock);  // 获取读锁
	if (ATOM_FDEC(&obj->reference) == 1) {
		// last reader, no writer. so no need to unlock
		// 最后一个读者，没有写者，无需解锁
		assert(obj->copy == NULL);
		skynet_free(obj);
		return;
	}
	rwlock_runlock(&obj->lock);
}

// 增加 STM 对象引用计数
static void
stm_grab(struct stm_object *obj) {
	rwlock_rlock(&obj->lock);   // 获取读锁
	int ref = ATOM_FINC(&obj->reference);  // 原子递增引用计数
	rwlock_runlock(&obj->lock);
	assert(ref > 0);
}

// 获取 STM 对象的数据副本
static struct stm_copy *
stm_copy(struct stm_object *obj) {
	rwlock_rlock(&obj->lock);   // 获取读锁
	struct stm_copy * ret = obj->copy;
	if (ret) {
		int ref = ATOM_FINC(&ret->reference);  // 增加副本引用计数
		assert(ref > 0);
	}
	rwlock_runlock(&obj->lock);
	
	return ret;
}

static void
stm_update(struct stm_object *obj, void *msg, int32_t sz) {
	struct stm_copy *copy = stm_newcopy(msg, sz);
	rwlock_wlock(&obj->lock);
	struct stm_copy *oldcopy = obj->copy;
	obj->copy = copy;
	rwlock_wunlock(&obj->lock);

	stm_releasecopy(oldcopy);
}

// lua binding

struct boxstm {
	struct stm_object * obj;
};

static int
lcopy(lua_State *L) {
	struct boxstm * box = lua_touserdata(L, 1);
	stm_grab(box->obj);
	lua_pushlightuserdata(L, box->obj);
	return 1;
}

static int
lnewwriter(lua_State *L) {
	void * msg;
	size_t sz;
	if (lua_isuserdata(L,1)) {
		msg = lua_touserdata(L, 1);
		sz = (size_t)luaL_checkinteger(L, 2);
	} else {
		const char * tmp = luaL_checklstring(L,1,&sz);
		msg = skynet_malloc(sz);
		memcpy(msg, tmp, sz);
	}
	struct boxstm * box = lua_newuserdatauv(L, sizeof(*box), 0);
	box->obj = stm_new(msg,sz);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);

	return 1;
}

static int
ldeletewriter(lua_State *L) {
	struct boxstm * box = lua_touserdata(L, 1);
	stm_release(box->obj);
	box->obj = NULL;

	return 0;
}

static int
lupdate(lua_State *L) {
	struct boxstm * box = lua_touserdata(L, 1);
	void * msg;
	size_t sz;
	if (lua_isuserdata(L, 2)) {
		msg = lua_touserdata(L, 2);
		sz = (size_t)luaL_checkinteger(L, 3);
	} else {
		const char * tmp = luaL_checklstring(L,2,&sz);
		msg = skynet_malloc(sz);
		memcpy(msg, tmp, sz);
	}
	stm_update(box->obj, msg, sz);

	return 0;
}

struct boxreader {
	struct stm_object *obj;
	struct stm_copy *lastcopy;
};

static int
lnewreader(lua_State *L) {
	struct boxreader * box = lua_newuserdatauv(L, sizeof(*box), 0);
	box->obj = lua_touserdata(L, 1);
	box->lastcopy = NULL;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);

	return 1;
}

static int
ldeletereader(lua_State *L) {
	struct boxreader * box = lua_touserdata(L, 1);
	stm_releasereader(box->obj);
	box->obj = NULL;
	stm_releasecopy(box->lastcopy);
	box->lastcopy = NULL;

	return 0;
}

static int
lread(lua_State *L) {
	struct boxreader * box = lua_touserdata(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	struct stm_copy * copy = stm_copy(box->obj);
	if (copy == box->lastcopy) {
		// not update
		stm_releasecopy(copy);
		lua_pushboolean(L, 0);
		return 1;
	}

	stm_releasecopy(box->lastcopy);
	box->lastcopy = copy;
	if (copy) {
		lua_settop(L, 3);
		lua_replace(L, 1);
		lua_settop(L, 2);
		lua_pushlightuserdata(L, copy->msg);
		lua_pushinteger(L, copy->sz);
		lua_pushvalue(L, 1);
		lua_call(L, 3, LUA_MULTRET);
		lua_pushboolean(L, 1);
		lua_replace(L, 1);
		return lua_gettop(L);
	} else {
		lua_pushboolean(L, 0);
		return 1;
	}
}

// STM 模块初始化函数
LUAMOD_API int
luaopen_skynet_stm(lua_State *L) {
	luaL_checkversion(L);
	lua_createtable(L, 0, 3);

	// 添加 copy 函数
	lua_pushcfunction(L, lcopy);
	lua_setfield(L, -2, "copy");

	// 创建写者元表和函数
	luaL_Reg writer[] = {
		{ "new", lnewwriter },  // 创建新写者
		{ NULL, NULL },
	};
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, ldeletewriter),  // 垃圾回收方法
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, lupdate),        // 更新方法
	lua_setfield(L, -2, "__call");
	luaL_setfuncs(L, writer, 1);

	// 创建读者元表和函数
	luaL_Reg reader[] = {
		{ "newcopy", lnewreader },  // 创建新读者
		{ NULL, NULL },
	};
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, ldeletereader),  // 垃圾回收方法
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, lread),          // 读取方法
	lua_setfield(L, -2, "__call");
	luaL_setfuncs(L, reader, 1);

	return 1;
}
