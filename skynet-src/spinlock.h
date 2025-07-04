/*
 * spinlock.h - skynet自旋锁实现头文件
 * 提供跨平台的自旋锁实现，支持原子操作和pthread互斥锁两种方式
 */

#ifndef SKYNET_SPINLOCK_H
#define SKYNET_SPINLOCK_H

// 自旋锁操作宏定义
#define SPIN_INIT(q) spinlock_init(&(q)->lock);      // 初始化自旋锁
#define SPIN_LOCK(q) spinlock_lock(&(q)->lock);      // 加锁
#define SPIN_UNLOCK(q) spinlock_unlock(&(q)->lock);  // 解锁
#define SPIN_DESTROY(q) spinlock_destroy(&(q)->lock);// 销毁自旋锁

#ifndef USE_PTHREAD_LOCK

/*
 * 使用原子操作实现自旋锁
 */

#ifdef __STDC_NO_ATOMICS__

/*
 * 不支持C11原子操作时，使用GCC内建原子操作
 */

#define atomic_flag_ int                                                // 原子标志类型
#define ATOMIC_FLAG_INIT_ 0                                             // 原子标志初始值
#define atomic_flag_test_and_set_(ptr) __sync_lock_test_and_set(ptr, 1) // 测试并设置
#define atomic_flag_clear_(ptr) __sync_lock_release(ptr)                // 清除标志

/*
 * 自旋锁结构体（GCC内建原子操作版本）
 */
struct spinlock {
	atomic_flag_ lock;  // 锁标志
};

/*
 * 初始化自旋锁
 */
static inline void
spinlock_init(struct spinlock *lock) {
	atomic_flag_ v = ATOMIC_FLAG_INIT_;
	lock->lock = v;
}

/*
 * 加锁（忙等待）
 */
static inline void
spinlock_lock(struct spinlock *lock) {
	while (atomic_flag_test_and_set_(&lock->lock)) {}
}

/*
 * 尝试加锁（非阻塞）
 * @return: 成功返回1，失败返回0
 */
static inline int
spinlock_trylock(struct spinlock *lock) {
	return atomic_flag_test_and_set_(&lock->lock) == 0;
}

/*
 * 解锁
 */
static inline void
spinlock_unlock(struct spinlock *lock) {
	atomic_flag_clear_(&lock->lock);
}

/*
 * 销毁自旋锁（空操作）
 */
static inline void
spinlock_destroy(struct spinlock *lock) {
	(void) lock;
}

#else  // __STDC_NO_ATOMICS__

/*
 * 支持C11原子操作时，使用标准原子操作
 */

#include "atomic.h"

#define atomic_test_and_set_(ptr) STD_ atomic_exchange_explicit(ptr, 1, STD_ memory_order_acquire)
#define atomic_clear_(ptr) STD_ atomic_store_explicit(ptr, 0, STD_ memory_order_release);
#define atomic_load_relaxed_(ptr) STD_ atomic_load_explicit(ptr, STD_ memory_order_relaxed)

// CPU暂停指令，减少自旋时的功耗
#if defined(__x86_64__)
#include <immintrin.h> // For _mm_pause
#define atomic_pause_() _mm_pause()
#else
#define atomic_pause_() ((void)0)
#endif

/*
 * 自旋锁结构体（C11原子操作版本）
 */
struct spinlock {
	STD_ atomic_int lock;  // 原子整数锁
};

/*
 * 初始化自旋锁
 */
static inline void
spinlock_init(struct spinlock *lock) {
	STD_ atomic_init(&lock->lock, 0);
}

/*
 * 加锁（优化的忙等待，包含CPU暂停）
 */
static inline void
spinlock_lock(struct spinlock *lock) {
	for (;;) {
		if (!atomic_test_and_set_(&lock->lock))
			return;
		// 在锁被占用时暂停CPU，减少功耗和总线争用
		while (atomic_load_relaxed_(&lock->lock))
			atomic_pause_();
	}
}

/*
 * 尝试加锁（非阻塞）
 * @return: 成功返回1，失败返回0
 */
static inline int
spinlock_trylock(struct spinlock *lock) {
	return !atomic_load_relaxed_(&lock->lock) &&
		!atomic_test_and_set_(&lock->lock);
}

/*
 * 解锁
 */
static inline void
spinlock_unlock(struct spinlock *lock) {
	atomic_clear_(&lock->lock);
}

/*
 * 销毁自旋锁（空操作）
 */
static inline void
spinlock_destroy(struct spinlock *lock) {
	(void) lock;
}

#endif  // __STDC_NO_ATOMICS__

#else

/*
 * 使用pthread互斥锁实现（某些情况下使用互斥锁而非自旋锁）
 * 也可以替换为pthread_spinlock
 */

#include <pthread.h>

// we use mutex instead of spinlock for some reason
// you can also replace to pthread_spinlock
/*
 * 自旋锁结构体（pthread互斥锁版本）
 */
struct spinlock {
	pthread_mutex_t lock;  // pthread互斥锁
};

/*
 * 初始化自旋锁
 */
static inline void
spinlock_init(struct spinlock *lock) {
	pthread_mutex_init(&lock->lock, NULL);
}

/*
 * 加锁
 */
static inline void
spinlock_lock(struct spinlock *lock) {
	pthread_mutex_lock(&lock->lock);
}

/*
 * 尝试加锁（非阻塞）
 * @return: 成功返回1，失败返回0
 */
static inline int
spinlock_trylock(struct spinlock *lock) {
	return pthread_mutex_trylock(&lock->lock) == 0;
}

/*
 * 解锁
 */
static inline void
spinlock_unlock(struct spinlock *lock) {
	pthread_mutex_unlock(&lock->lock);
}

/*
 * 销毁自旋锁
 */
static inline void
spinlock_destroy(struct spinlock *lock) {
	pthread_mutex_destroy(&lock->lock);
}

#endif

#endif
