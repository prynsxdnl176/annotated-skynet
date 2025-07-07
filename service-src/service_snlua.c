#include "skynet.h"
#include "atomic.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/task.h>
#include <mach/mach.h>
#endif

#define NANOSEC 1000000000   // 纳秒转换常量
#define MICROSEC 1000000     // 微秒转换常量

// #define DEBUG_LOG

#define MEMORY_WARNING_REPORT (1024 * 1024 * 32)  // 内存警告阈值：32MB

// snlua服务结构，封装Lua虚拟机和相关状态
struct snlua {
	lua_State * L;              // 主Lua虚拟机状态
	struct skynet_context * ctx; // skynet上下文
	size_t mem;                 // 当前内存使用量
	size_t mem_report;          // 内存报告阈值
	size_t mem_limit;           // 内存使用限制
	lua_State * activeL;        // 当前活跃的Lua状态（可能是协程）
	ATOM_INT trap;              // 原子陷阱标志，用于中断Lua执行
};

// LUA_CACHELIB may defined in patched lua for shared proto
// LUA_CACHELIB 可能在修补的lua中定义，用于共享原型
#ifdef LUA_CACHELIB

#define codecache luaopen_cache

#else

// 空的清理函数，用于兼容性
static int
cleardummy(lua_State *L) {
  return 0;
}

// 代码缓存模块，提供loadfile功能的包装
static int
codecache(lua_State *L) {
	luaL_Reg l[] = {
		{ "clear", cleardummy },  // 清理缓存（空实现）
		{ "mode", cleardummy },   // 设置模式（空实现）
		{ NULL, NULL },
	};
	luaL_newlib(L,l);                    // 创建新的库表
	lua_getglobal(L, "loadfile");        // 获取全局loadfile函数
	lua_setfield(L, -2, "loadfile");     // 设置为库的loadfile字段
	return 1;  // 返回库表
}

#endif

// 信号钩子函数，用于中断Lua执行
static void
signal_hook(lua_State *L, lua_Debug *ar) {
	void *ud = NULL;
	lua_getallocf(L, &ud);                // 获取分配器的用户数据
	struct snlua *l = (struct snlua *)ud; // 转换为snlua结构

	lua_sethook (L, NULL, 0, 0);  // 清除钩子函数
	if (ATOM_LOAD(&l->trap)) {
		ATOM_STORE(&l->trap , 0);     // 重置陷阱标志
		luaL_error(L, "signal 0");    // 抛出Lua错误，中断执行
	}
}

// 切换活跃的Lua状态，并设置信号钩子
static void
switchL(lua_State *L, struct snlua *l) {
	l->activeL = L;  // 设置当前活跃的Lua状态
	if (ATOM_LOAD(&l->trap)) {
		// 如果设置了陷阱，安装信号钩子，每执行1条指令就检查一次
		lua_sethook(L, signal_hook, LUA_MASKCOUNT, 1);
	}
}

// 扩展的lua_resume函数，支持信号中断
static int
lua_resumeX(lua_State *L, lua_State *from, int nargs, int *nresults) {
	void *ud = NULL;
	lua_getallocf(L, &ud);                // 获取分配器用户数据
	struct snlua *l = (struct snlua *)ud; // 转换为snlua结构
	switchL(L, l);                        // 切换到目标Lua状态
	int err = lua_resume(L, from, nargs, nresults);  // 恢复协程执行
	if (ATOM_LOAD(&l->trap)) {
		// wait for lua_sethook. (l->trap == -1)
		// 等待lua_sethook完成（l->trap == -1）
		while (ATOM_LOAD(&l->trap) >= 0) ;
	}
	switchL(from, l);  // 切换回原来的Lua状态
	return err;        // 返回执行结果
}

// 获取当前线程的CPU时间，用于性能统计
static double
get_time() {
#if  !defined(__APPLE__)
	// Linux/Unix系统：使用clock_gettime获取线程CPU时间
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	int sec = ti.tv_sec & 0xffff;  // 取秒数的低16位
	int nsec = ti.tv_nsec;         // 纳秒部分

	return (double)sec + (double)nsec / NANOSEC;  // 转换为浮点秒数
#else
	// macOS系统：使用task_info获取任务时间信息
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;  // 获取失败返回0
	}

	int sec = aTaskInfo.user_time.seconds & 0xffff;     // 用户态时间秒数
	int msec = aTaskInfo.user_time.microseconds;        // 微秒部分

	return (double)sec + (double)msec / MICROSEC;  // 转换为浮点秒数
#endif
}

// 计算时间差，处理时间回绕的情况
static inline double
diff_time(double start) {
	double now = get_time();
	if (now < start) {
		// 处理时间回绕（当计数器溢出时）
		return now + 0x10000 - start;
	} else {
		return now - start;
	}
}

// coroutine lib, add profile
// 协程库，添加性能分析功能

/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors.
** 恢复协程执行。对于非错误情况返回结果数量，错误时返回-1
*/
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status, nres;
  if (!lua_checkstack(co, narg)) {
    // 检查协程栈空间是否足够
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */ /* 错误标志 */
  }
  lua_xmove(L, co, narg);  // 移动参数到协程栈
  status = lua_resumeX(co, L, narg, &nres);  // 恢复协程执行
  if (status == LUA_OK || status == LUA_YIELD) {
    // 协程正常完成或让出
    if (!lua_checkstack(L, nres + 1)) {
      lua_pop(co, nres);  /* remove results anyway */ /* 无论如何都移除结果 */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */ /* 错误标志 */
    }
    lua_xmove(co, L, nres);  /* move yielded values */ /* 移动让出的值 */
    return nres;  // 返回结果数量
  }
  else {
    // 协程执行出错
    lua_xmove(co, L, 1);  /* move error message */ /* 移动错误消息 */
    return -1;  /* error flag */ /* 错误标志 */
  }
}

// 检查协程是否启用了性能计时，并获取开始时间
static int
timing_enable(lua_State *L, int co_index, lua_Number *start_time) {
	lua_pushvalue(L, co_index);        // 将协程对象压栈
	lua_rawget(L, lua_upvalueindex(1)); // 从upvalue表1中获取开始时间
	if (lua_isnil(L, -1)) {		// check start time
		// 如果没有开始时间记录，说明未启用计时
		lua_pop(L, 1);
		return 0;  // 未启用计时
	}
	*start_time = lua_tonumber(L, -1);  // 获取开始时间
	lua_pop(L,1);
	return 1;  // 已启用计时
}

// 获取协程的总执行时间
static double
timing_total(lua_State *L, int co_index) {
	lua_pushvalue(L, co_index);        // 将协程对象压栈
	lua_rawget(L, lua_upvalueindex(2)); // 从upvalue表2中获取总时间
	double total_time = lua_tonumber(L, -1);  // 转换为数值
	lua_pop(L,1);
	return total_time;  // 返回总时间
}

// 带性能计时的协程恢复函数
static int
timing_resume(lua_State *L, int co_index, int n) {
	lua_State *co = lua_tothread(L, co_index);  // 获取协程对象
	lua_Number start_time = 0;
	if (timing_enable(L, co_index, &start_time)) {
		// 如果启用了计时，记录开始时间
		start_time = get_time();
#ifdef DEBUG_LOG
		double ti = diff_time(start_time);
		fprintf(stderr, "PROFILE [%p] resume %lf\n", co, ti);
#endif
		lua_pushvalue(L, co_index);
		lua_pushnumber(L, start_time);
		lua_rawset(L, lua_upvalueindex(1));	// set start time
		// 设置开始时间到upvalue表1
	}

	int r = auxresume(L, co, n);  // 恢复协程执行

	if (timing_enable(L, co_index, &start_time)) {
		// 协程让出后，计算执行时间并累加到总时间
		double total_time = timing_total(L, co_index);  // 获取已有总时间
		double diff = diff_time(start_time);            // 计算本次执行时间
		total_time += diff;                             // 累加到总时间
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] yield (%lf/%lf)\n", co, diff, total_time);
#endif
		lua_pushvalue(L, co_index);
		lua_pushnumber(L, total_time);
		lua_rawset(L, lua_upvalueindex(2));  // 更新总时间到upvalue表2
	}

	return r;  // 返回协程恢复结果
}

// Lua协程恢复函数的C实现（带性能分析）
static int luaB_coresume (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTHREAD);  // 检查第一个参数是协程
  int r = timing_resume(L, 1, lua_gettop(L) - 1);  // 调用带计时的恢复函数
  if (r < 0) {
    // 协程执行出错
    lua_pushboolean(L, 0);  // 压入false
    lua_insert(L, -2);      // 将false插入到错误消息前
    return 2;  /* return false + error message */ /* 返回false + 错误消息 */
  }
  else {
    // 协程执行成功
    lua_pushboolean(L, 1);     // 压入true
    lua_insert(L, -(r + 1));   // 将true插入到返回值前
    return r + 1;  /* return true + 'resume' returns */ /* 返回true + resume的返回值 */
  }
}

// 协程包装器的辅助函数
static int luaB_auxwrap (lua_State *L) {
  lua_State *co = lua_tothread(L, lua_upvalueindex(3));  // 获取协程对象
  int r = timing_resume(L, lua_upvalueindex(3), lua_gettop(L));  // 恢复协程执行
  if (r < 0) {
    // 协程执行出错
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD)
      lua_closethread(co, L);  /* close variables in case of errors */ /* 出错时关闭变量 */
    if (lua_type(L, -1) == LUA_TSTRING) {  /* error object is a string? */ /* 错误对象是字符串吗？ */
      luaL_where(L, 1);  /* add extra info, if available */ /* 如果可用，添加额外信息 */
      lua_insert(L, -2);
      lua_concat(L, 2);  // 连接错误信息
    }
    return lua_error(L);  /* propagate error */ /* 传播错误 */
  }
  return r;  // 返回结果数量
}

// 创建新协程的Lua函数实现
static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  luaL_checktype(L, 1, LUA_TFUNCTION);  // 检查参数是函数
  NL = lua_newthread(L);                // 创建新协程
  lua_pushvalue(L, 1);  /* move function to top */ /* 将函数移到栈顶 */
  lua_xmove(L, NL, 1);  /* move function from L to NL */ /* 将函数从L移到NL */
  return 1;  // 返回新协程
}

// 创建协程包装器函数
static int luaB_cowrap (lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));  // 推入upvalue1
  lua_pushvalue(L, lua_upvalueindex(2));  // 推入upvalue2
  luaB_cocreate(L);                       // 创建协程
  lua_pushcclosure(L, luaB_auxwrap, 3);   // 创建闭包，包含3个upvalue
  return 1;  // 返回包装器函数
}

// profile lib
// 性能分析库

// 开始性能分析的Lua函数
static int
lstart(lua_State *L) {
	if (lua_gettop(L) != 0) {
		// 如果有参数，检查是否为协程
		lua_settop(L,1);
		luaL_checktype(L, 1, LUA_TTHREAD);
	} else {
		// 如果没有参数，使用当前协程
		lua_pushthread(L);
	}
	lua_Number start_time = 0;
	if (timing_enable(L, 1, &start_time)) {
		// 如果已经开始了性能分析，报错
		return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
	}

	// reset total time
	// 重置总时间
	lua_pushvalue(L, 1);
	lua_pushnumber(L, 0);
	lua_rawset(L, lua_upvalueindex(2));  // 设置总时间为0

	// set start time
	// 设置开始时间
	lua_pushvalue(L, 1);
	start_time = get_time();  // 获取当前时间
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] start\n", L);
#endif
	lua_pushnumber(L, start_time);
	lua_rawset(L, lua_upvalueindex(1));  // 设置开始时间

	return 0;  // 无返回值
}

// 停止性能分析的Lua函数
static int
lstop(lua_State *L) {
	if (lua_gettop(L) != 0) {
		// 如果有参数，检查是否为协程
		lua_settop(L,1);
		luaL_checktype(L, 1, LUA_TTHREAD);
	} else {
		// 如果没有参数，使用当前协程
		lua_pushthread(L);
	}
	lua_Number start_time = 0;
	if (!timing_enable(L, 1, &start_time)) {
		// 如果没有开始性能分析，报错
		return luaL_error(L, "Call profile.start() before profile.stop()");
	}
	double ti = diff_time(start_time);      // 计算最后一段时间
	double total_time = timing_total(L,1);  // 获取总时间

	// 清除开始时间记录
	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));  // 设置开始时间为nil

	// 清除总时间记录
	lua_pushvalue(L, 1);	// push coroutine
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(2));  // 设置总时间为nil

	total_time += ti;  // 加上最后一段时间
	lua_pushnumber(L, total_time);  // 返回总执行时间
#ifdef DEBUG_LOG
	fprintf(stderr, "PROFILE [%p] stop (%lf/%lf)\n", lua_tothread(L,1), ti, total_time);
#endif

	return 1;  // 返回总时间
}

// 初始化性能分析库
static int
init_profile(lua_State *L) {
	luaL_Reg l[] = {
		{ "start", lstart },           // 开始性能分析
		{ "stop", lstop },             // 停止性能分析
		{ "resume", luaB_coresume },   // 带性能分析的协程恢复
		{ "wrap", luaB_cowrap },       // 协程包装器
		{ NULL, NULL },
	};
	luaL_newlibtable(L,l);
	lua_newtable(L);	// table thread->start time  // 协程->开始时间映射表
	lua_newtable(L);	// table thread->total time  // 协程->总时间映射表

	lua_newtable(L);	// weak table
	                    // 弱引用表
	lua_pushliteral(L, "kv");
	lua_setfield(L, -2, "__mode");  // 设置元表的__mode字段为"kv"

	lua_pushvalue(L, -1);
	lua_setmetatable(L, -3);  // 设置弱引用表的元表
	lua_setmetatable(L, -3);  // 设置协程表的元表

	luaL_setfuncs(L,l,2);  // 注册协程相关函数，带2个upvalue

	return 1;
}

/// end of coroutine
/// 协程部分结束

// Lua错误回溯函数
static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);  // 获取错误消息
	if (msg)
		luaL_traceback(L, L, msg, 1);  // 生成完整的错误回溯信息
	else {
		lua_pushliteral(L, "(no error message)");  // 没有错误消息时的默认提示
	}
	return 1;
}

// 向launcher服务报告错误
static void
report_launcher_error(struct skynet_context *ctx) {
	// sizeof "ERROR" == 5
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);  // 发送错误通知
}

// 获取可选的字符串配置项
static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);  // 从环境变量获取配置
	if (ret == NULL) {
		return str;  // 如果没有配置则返回默认值
	}
	return ret;  // 返回配置值
}

// snlua服务的初始化回调函数
static int
init_cb(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {
	lua_State *L = l->L;
	l->ctx = ctx;  // 设置上下文
	lua_gc(L, LUA_GCSTOP, 0);  // 停止垃圾回收
	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */
	/* 向库发出忽略环境变量的信号 */
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
	luaL_openlibs(L);  // 打开标准Lua库
	luaL_requiref(L, "skynet.profile", init_profile, 0);  // 加载性能分析库

	int profile_lib = lua_gettop(L);
	// replace coroutine.resume / coroutine.wrap
	// 替换协程的resume和wrap函数为带性能分析的版本
	lua_getglobal(L, "coroutine");
	lua_getfield(L, profile_lib, "resume");
	lua_setfield(L, -2, "resume");  // 替换coroutine.resume
	lua_getfield(L, profile_lib, "wrap");
	lua_setfield(L, -2, "wrap");    // 替换coroutine.wrap

	lua_settop(L, profile_lib-1);  // 清理栈

	// 设置skynet上下文到注册表
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	luaL_requiref(L, "skynet.codecache", codecache , 0);  // 加载代码缓存库
	lua_pop(L,1);

	lua_gc(L, LUA_GCGEN, 0, 0);  // 启用分代垃圾回收

	// 设置Lua路径相关的全局变量
	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");  // 设置Lua模块搜索路径
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");  // 设置C模块搜索路径
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);
	lua_setglobal(L, "LUA_SERVICE");  // 设置服务脚本搜索路径
	const char *preload = skynet_command(ctx, "GETENV", "preload");
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");  // 设置预加载模块

	lua_pushcfunction(L, traceback);  // 压入错误跟踪函数
	assert(lua_gettop(L) == 1);

	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	int r = luaL_loadfile(L,loader);  // 加载Lua加载器脚本
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		report_launcher_error(ctx);  // 报告启动错误
		return 1;  // 初始化失败
	}
	lua_pushlstring(L, args, sz);  // 将参数字符串压入栈
	r = lua_pcall(L,1,0,1);        // 调用加载器函数，传入参数
	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(ctx);  // 报告加载器错误
		return 1;
	}
	lua_settop(L,0);  // 清空栈
	// 检查是否设置了内存限制
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
		size_t limit = lua_tointeger(L, -1);  // 获取内存限制值
		l->mem_limit = limit;                 // 设置内存限制
		skynet_error(ctx, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");  // 清除注册表中的memlimit
	}
	lua_pop(L, 1);  // 弹出栈顶元素

	lua_gc(L, LUA_GCRESTART, 0);

	return 0;
}

// 启动回调函数，处理服务的第一条消息
static int
launch_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);  // 确保是启动消息
	struct snlua *l = ud;
	skynet_callback(context, NULL, NULL);  // 清除回调函数
	int err = init_cb(l, context, msg, sz); // 执行初始化
	if (err) {
		skynet_command(context, "EXIT", NULL);  // 初始化失败则退出服务
	}

	return 0;
}

// snlua服务的初始化函数
int
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args);
	char * tmp = skynet_malloc(sz);  // 分配参数内存
	memcpy(tmp, args, sz);           // 复制参数
	skynet_callback(ctx, l , launch_cb);  // 设置启动回调
	const char * self = skynet_command(ctx, "REG", NULL);  // 获取自己的句柄
	uint32_t handle_id = strtoul(self+1, NULL, 16);       // 解析句柄ID
	// it must be first message
	// 这必须是第一条消息
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY,0, tmp, sz);  // 发送启动消息给自己
	return 0;
}

// Lua内存分配器，跟踪内存使用并实施限制
static void *
lalloc(void * ud, void *ptr, size_t osize, size_t nsize) {
	struct snlua *l = ud;
	size_t mem = l->mem;      // 保存当前内存使用量
	l->mem += nsize;          // 增加新分配的内存
	if (ptr)
		l->mem -= osize;      // 减去释放的内存
	if (l->mem_limit != 0 && l->mem > l->mem_limit) {
		// 检查内存限制
		if (ptr == NULL || nsize > osize) {
			// 新分配或扩大内存时超出限制，拒绝分配
			l->mem = mem;     // 恢复内存计数
			return NULL;      // 分配失败
		}
	}
	if (l->mem > l->mem_report) {
		// 内存使用超过报告阈值，发出警告
		l->mem_report *= 2;   // 下次报告阈值翻倍
		skynet_error(l->ctx, "Memory warning %.2f M", (float)l->mem / (1024 * 1024));
	}
	return skynet_lalloc(ptr, osize, nsize);  // 调用实际的内存分配函数
}

// 创建snlua服务实例
struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));
	memset(l,0,sizeof(*l));                    // 清零结构体
	l->mem_report = MEMORY_WARNING_REPORT;     // 设置内存警告阈值
	l->mem_limit = 0;                          // 初始无内存限制
	l->L = lua_newstate(lalloc, l);            // 创建Lua虚拟机，使用自定义分配器
	l->activeL = NULL;                         // 初始无活跃Lua状态
	ATOM_INIT(&l->trap , 0);                   // 初始化陷阱标志
	return l;
}

// 释放snlua实例
void
snlua_release(struct snlua *l) {
	lua_close(l->L);  // 关闭Lua虚拟机
	skynet_free(l);   // 释放snlua结构体
}

// 处理信号
void
snlua_signal(struct snlua *l, int signal) {
	skynet_error(l->ctx, "recv a signal %d", signal);
	if (signal == 0) {
		// 信号0：设置中断陷阱
		if (ATOM_LOAD(&l->trap) == 0) {
			// only one thread can set trap ( l->trap 0->1 )
			// 只有一个线程可以设置陷阱（l->trap 0->1）
			if (!ATOM_CAS(&l->trap, 0, 1))
				return;
			lua_sethook (l->activeL, signal_hook, LUA_MASKCOUNT, 1);  // 设置Lua钩子函数
			// finish set ( l->trap 1 -> -1 )
			// 完成设置（l->trap 1 -> -1）
			ATOM_CAS(&l->trap, 1, -1);
		}
	} else if (signal == 1) {
		skynet_error(l->ctx, "Current Memory %.3fK", (float)l->mem / 1024);
	}
}
