/*
 * malloc_hook.h - skynet内存分配钩子头文件
 * 提供内存使用统计、调试和控制接口
 */

#ifndef SKYNET_MALLOC_HOOK_H
#define SKYNET_MALLOC_HOOK_H

#include <stdlib.h>
#include <stdbool.h>
#include <lua.h>

/*
 * 内存统计接口
 */

// 获取已使用的内存总量
extern size_t malloc_used_memory(void);

// 获取内存块数量
extern size_t malloc_memory_block(void);

/*
 * 内存调试和转储接口
 */

// 转储内存信息到文件或标准输出
extern void memory_info_dump(const char *opts);

/*
 * jemalloc控制接口（如果使用jemalloc）
 */

// 控制64位整数配置项
extern size_t mallctl_int64(const char* name, size_t* newval);

// 控制选项配置项
extern int mallctl_opt(const char* name, int* newval);

// 控制布尔配置项
extern bool mallctl_bool(const char* name, bool* newval);

// 执行控制命令
extern int mallctl_cmd(const char* name);

// 转储C内存使用情况
extern void dump_c_mem(void);

// Lua接口：转储内存使用情况
extern int dump_mem_lua(lua_State *L);

// 获取当前内存使用量
extern size_t malloc_current_memory(void);


#endif /* SKYNET_MALLOC_HOOK_H */

