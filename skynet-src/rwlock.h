/*
 * rwlock.h - skynet读写锁实现头文件
 * 提供跨平台的读写锁实现，支持原子操作和pthread两种方式
 */

#ifndef SKYNET_RWLOCK_H
#define SKYNET_RWLOCK_H

#ifndef USE_PTHREAD_LOCK

/*
 * 使用原子操作实现读写锁
 */

#include "atomic.h"

/*
 * 读写锁结构体（原子操作版本）
 * 使用两个原子整数分别记录写锁状态和读锁计数
 */
struct rwlock {
	ATOM_INT write;  // 写锁状态（0=未锁定，1=已锁定）
	ATOM_INT read;   // 读锁计数器
};

/*
 * 初始化读写锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_init(struct rwlock *lock) {
	ATOM_INIT(&lock->write, 0);  // 写锁初始化为未锁定
	ATOM_INIT(&lock->read, 0);   // 读锁计数初始化为0
}

/*
 * 获取读锁
 * 多个线程可以同时持有读锁，但不能与写锁同时存在
 * @param lock: 读写锁指针
 */
static inline void
rwlock_rlock(struct rwlock *lock) {
	for (;;) {
		// 等待写锁释放
		while(ATOM_LOAD(&lock->write)) {}

		// 增加读锁计数
		ATOM_FINC(&lock->read);

		// 再次检查写锁状态，防止竞争条件
		if (ATOM_LOAD(&lock->write)) {
			// 如果写锁被获取，撤销读锁并重试
			ATOM_FDEC(&lock->read);
		} else {
			// 成功获取读锁
			break;
		}
	}
}

/*
 * 获取写锁
 * 写锁是排他的，不能与任何读锁或写锁同时存在
 * @param lock: 读写锁指针
 */
static inline void
rwlock_wlock(struct rwlock *lock) {
	// 原子地获取写锁
	while (!ATOM_CAS(&lock->write, 0, 1)) {}

	// 等待所有读锁释放
	while(ATOM_LOAD(&lock->read)) {}
}

/*
 * 释放写锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_wunlock(struct rwlock *lock) {
	ATOM_STORE(&lock->write, 0);
}

/*
 * 释放读锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_runlock(struct rwlock *lock) {
	ATOM_FDEC(&lock->read);
}

#else

/*
 * 使用pthread读写锁实现
 * 适用于不支持原子操作的平台
 */

#include <pthread.h>

// only for some platform doesn't have __sync_*
// todo: check the result of pthread api
/*
 * 读写锁结构体（pthread版本）
 */

struct rwlock {
	pthread_rwlock_t lock;  // pthread读写锁
};

/*
 * 初始化读写锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_init(struct rwlock *lock) {
	pthread_rwlock_init(&lock->lock, NULL);
}

/*
 * 获取读锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_rlock(struct rwlock *lock) {
	pthread_rwlock_rdlock(&lock->lock);
}

/*
 * 获取写锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_wlock(struct rwlock *lock) {
	pthread_rwlock_wrlock(&lock->lock);
}

/*
 * 释放写锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_wunlock(struct rwlock *lock) {
	pthread_rwlock_unlock(&lock->lock);
}

/*
 * 释放读锁
 * @param lock: 读写锁指针
 */
static inline void
rwlock_runlock(struct rwlock *lock) {
	pthread_rwlock_unlock(&lock->lock);
}

#endif

#endif
