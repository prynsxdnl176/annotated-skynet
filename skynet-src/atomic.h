/*
 * atomic.h - skynet原子操作封装头文件
 * 提供跨平台的原子操作接口，支持C11标准原子操作和GCC内建原子操作
 */

#ifndef SKYNET_ATOMIC_H
#define SKYNET_ATOMIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __STDC_NO_ATOMICS__

/*
 * 不支持C11原子操作时，使用GCC内建原子操作
 */

// 原子类型定义（使用volatile关键字）
#define ATOM_INT volatile int                    // 原子整数
#define ATOM_POINTER volatile uintptr_t          // 原子指针
#define ATOM_SIZET volatile size_t               // 原子size_t
#define ATOM_ULONG volatile unsigned long        // 原子无符号长整数

// 原子操作宏定义（使用GCC内建函数）
#define ATOM_INIT(ptr, v) (*(ptr) = v)                                          // 初始化
#define ATOM_LOAD(ptr) (*(ptr))                                                 // 加载值
#define ATOM_STORE(ptr, v) (*(ptr) = v)                                         // 存储值
#define ATOM_CAS(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval) // 比较并交换
#define ATOM_CAS_ULONG(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_CAS_SIZET(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_CAS_POINTER(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_FINC(ptr) __sync_fetch_and_add(ptr, 1)                             // 获取并递增
#define ATOM_FDEC(ptr) __sync_fetch_and_sub(ptr, 1)                             // 获取并递减
#define ATOM_FADD(ptr,n) __sync_fetch_and_add(ptr, n)                           // 获取并加
#define ATOM_FSUB(ptr,n) __sync_fetch_and_sub(ptr, n)                           // 获取并减
#define ATOM_FAND(ptr,n) __sync_fetch_and_and(ptr, n)                           // 获取并按位与

#else

/*
 * 支持C11原子操作时，使用标准原子操作
 */

#if defined (__cplusplus)
#include <atomic>
#define STD_ std::
#define atomic_value_type_(p, v) decltype((p)->load())(v)
#else
#include <stdatomic.h>
#define STD_
#define atomic_value_type_(p, v) v
#endif

// 原子类型定义（使用C11标准原子类型）
#define ATOM_INT  STD_ atomic_int                // 原子整数
#define ATOM_POINTER STD_ atomic_uintptr_t       // 原子指针
#define ATOM_SIZET STD_ atomic_size_t            // 原子size_t
#define ATOM_ULONG STD_ atomic_ulong             // 原子无符号长整数

// 基本原子操作宏
#define ATOM_INIT(ref, v) STD_ atomic_init(ref, v)    // 初始化
#define ATOM_LOAD(ptr) STD_ atomic_load(ptr)          // 加载值
#define ATOM_STORE(ptr, v) STD_ atomic_store(ptr, v)  // 存储值

/*
 * 比较并交换操作（CAS）- 整数类型
 */
static inline int
ATOM_CAS(STD_ atomic_int *ptr, int oval, int nval) {
	return STD_ atomic_compare_exchange_weak(ptr, &(oval), nval);
}

/*
 * 比较并交换操作（CAS）- size_t类型
 */
static inline int
ATOM_CAS_SIZET(STD_ atomic_size_t *ptr, size_t oval, size_t nval) {
	return STD_ atomic_compare_exchange_weak(ptr, &(oval), nval);
}

/*
 * 比较并交换操作（CAS）- unsigned long类型
 */
static inline int
ATOM_CAS_ULONG(STD_ atomic_ulong *ptr, unsigned long oval, unsigned long nval) {
	return STD_ atomic_compare_exchange_weak(ptr, &(oval), nval);
}

/*
 * 比较并交换操作（CAS）- 指针类型
 */
static inline int
ATOM_CAS_POINTER(STD_ atomic_uintptr_t *ptr, uintptr_t oval, uintptr_t nval) {
	return STD_ atomic_compare_exchange_weak(ptr, &(oval), nval);
}

// 获取并修改操作宏
#define ATOM_FINC(ptr) STD_ atomic_fetch_add(ptr, atomic_value_type_(ptr,1))    // 获取并递增
#define ATOM_FDEC(ptr) STD_ atomic_fetch_sub(ptr, atomic_value_type_(ptr, 1))   // 获取并递减
#define ATOM_FADD(ptr,n) STD_ atomic_fetch_add(ptr, atomic_value_type_(ptr, n)) // 获取并加
#define ATOM_FSUB(ptr,n) STD_ atomic_fetch_sub(ptr, atomic_value_type_(ptr, n)) // 获取并减
#define ATOM_FAND(ptr,n) STD_ atomic_fetch_and(ptr, atomic_value_type_(ptr, n)) // 获取并按位与

#endif

#endif
