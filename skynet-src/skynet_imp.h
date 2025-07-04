/*
 * skynet_imp.h - skynet内部实现头文件
 * 定义了skynet的配置结构、线程类型和内部工具函数
 */

#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

#include <string.h>

/*
 * skynet配置结构体
 * 包含启动skynet所需的所有配置参数
 */
struct skynet_config {
	int thread;                 // 工作线程数量
	int harbor;                 // 节点ID（用于集群）
	int profile;                // 是否启用性能分析
	const char * daemon;        // 守护进程PID文件路径
	const char * module_path;   // 模块搜索路径
	const char * bootstrap;     // 启动脚本路径
	const char * logger;        // 日志服务名称
	const char * logservice;    // 日志服务类型
};

// 线程类型定义
#define THREAD_WORKER 0     // 工作线程
#define THREAD_MAIN 1       // 主线程
#define THREAD_SOCKET 2     // socket线程
#define THREAD_TIMER 3      // 定时器线程
#define THREAD_MONITOR 4    // 监控线程

/*
 * skynet启动函数
 * @param config: skynet配置结构指针
 */
void skynet_start(struct skynet_config * config);

/*
 * 字符串复制工具函数（指定长度）
 * 类似于POSIX strndup函数
 * @param str: 源字符串
 * @param size: 复制长度
 * @return: 新分配的字符串，失败返回NULL
 */
static inline char *
skynet_strndup(const char *str, size_t size) {
	char * ret = skynet_malloc(size+1);
	if (ret == NULL) return NULL;
	memcpy(ret, str, size);
	ret[size] = '\0';
	return ret;
}

/*
 * 字符串复制工具函数
 * 类似于POSIX strdup函数
 * @param str: 源字符串
 * @return: 新分配的字符串，失败返回NULL
 */
static inline char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	return skynet_strndup(str, sz);
}

#endif
