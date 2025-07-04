/*
 * skynet_malloc.h - skynet内存分配接口头文件
 * 提供统一的内存分配接口，支持内存统计和调试功能
 */

#ifndef skynet_malloc_h
#define skynet_malloc_h

#include <stddef.h>

// 内存分配函数宏定义（可重定向到自定义实现）
#define skynet_malloc malloc                    // 内存分配
#define skynet_calloc calloc                    // 清零内存分配
#define skynet_realloc realloc                  // 内存重新分配
#define skynet_free free                        // 内存释放
#define skynet_memalign memalign                // 对齐内存分配
#define skynet_aligned_alloc aligned_alloc      // 标准对齐内存分配
#define skynet_posix_memalign posix_memalign    // POSIX对齐内存分配

/*
 * skynet内存分配接口函数声明
 */

// 分配指定大小的内存
void * skynet_malloc(size_t sz);

// 分配并清零指定数量和大小的内存块
void * skynet_calloc(size_t nmemb, size_t size);

// 重新分配内存大小
void * skynet_realloc(void *ptr, size_t size);

// 释放内存
void skynet_free(void *ptr);

// Lua专用内存分配器接口
void * skynet_lalloc(void *ptr, size_t osize, size_t nsize);	// use for lua

// 分配指定对齐要求的内存
void * skynet_memalign(size_t alignment, size_t size);

// 分配指定对齐要求的内存（C11标准）
void * skynet_aligned_alloc(size_t alignment, size_t size);

// POSIX对齐内存分配
int skynet_posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
