/*
 * skynet_start.c - skynet框架的启动和线程管理模块
 * 负责初始化各个子系统、创建和管理工作线程
 */

#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/*
 * 监控器结构体，用于管理所有工作线程
 */
struct monitor {
	int count;                      // 工作线程总数
	struct skynet_monitor ** m;     // 每个工作线程对应的监控器数组
	pthread_cond_t cond;            // 条件变量，用于线程间同步
	pthread_mutex_t mutex;          // 互斥锁，保护共享数据
	int sleep;                      // 当前睡眠的线程数量
	int quit;                       // 退出标志
};

/*
 * 工作线程参数结构体
 */
struct worker_parm {
	struct monitor *m;              // 指向监控器的指针
	int id;                         // 线程ID
	int weight;                     // 工作权重，决定每次处理的消息数量
};

// 全局信号标志，用于处理SIGHUP信号
static volatile int SIG = 0;

/*
 * SIGHUP信号处理函数
 * 用于重新打开日志文件
 */
static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

// 检查是否应该退出的宏，当没有活跃的服务时退出
#define CHECK_ABORT if (skynet_context_total()==0) break;

/*
 * 创建线程的辅助函数
 * @param thread: 线程ID指针
 * @param start_routine: 线程启动函数
 * @param arg: 传递给线程函数的参数
 */
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

/*
 * 唤醒睡眠中的工作线程
 * @param m: 监控器指针
 * @param busy: 当前忙碌的线程数量
 */
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		// 如果睡眠线程数量足够多，则唤醒一个线程
		// "虚假唤醒"是无害的
		pthread_cond_signal(&m->cond);
	}
}

/*
 * socket线程函数
 * 负责处理网络I/O事件，轮询socket状态
 * @param p: 监控器指针
 * @return: NULL
 */
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);  // 初始化线程类型为socket线程
	for (;;) {
		int r = skynet_socket_poll();  // 轮询socket事件
		if (r==0)
			break;  // 没有更多socket事件，退出
		if (r<0) {
			CHECK_ABORT  // 检查是否应该退出
			continue;
		}
		wakeup(m,0);  // 有socket事件时唤醒工作线程处理
	}
	return NULL;
}

/*
 * 释放监控器资源
 * @param m: 监控器指针
 */
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	// 删除所有线程监控器
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	// 销毁同步原语
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	// 释放内存
	skynet_free(m->m);
	skynet_free(m);
}

/*
 * 监控线程函数
 * 负责监控所有工作线程的状态，检测死锁等异常情况
 * @param p: 监控器指针
 * @return: NULL
 */
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);  // 初始化线程类型为监控线程
	for (;;) {
		CHECK_ABORT  // 检查是否应该退出
		// 检查所有工作线程的状态
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		// 每5秒检查一次
		// 使用循环分开调用是为了更快的触发 abort
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

/*
 * 处理SIGHUP信号，重新打开日志文件
 * 向logger服务发送系统消息
 */
static void
signal_hup() {
	// make log file reopen
	// 构造系统消息
	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	// 查找logger服务
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		// 向logger服务推送消息，触发日志文件重新打开
		skynet_context_push(logger, &smsg);
	}
}

/*
 * 定时器线程函数
 * 负责更新系统时间、处理定时器事件、处理信号
 * @param p: 监控器指针
 * @return: NULL
 */
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);  // 初始化线程类型为定时器线程
	for (;;) {
		skynet_updatetime();        // 更新系统时间
		skynet_socket_updatetime(); // 更新socket超时时间
		CHECK_ABORT                 // 检查是否应该退出
		wakeup(m,m->count-1);       // 唤醒工作线程处理定时器事件
		usleep(2500);               // 休眠2.5毫秒，控制定时器精度
		if (SIG) {
			signal_hup();           // 处理SIGHUP信号
			SIG = 0;
		}
	}
	// wakeup socket thread
	// 退出时的清理工作
	skynet_socket_exit();           // 通知socket线程退出
	// wakeup all worker thread
	// 唤醒所有工作线程退出
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

/*
 * 工作线程函数
 * 负责处理服务的消息队列，执行具体的业务逻辑
 * @param p: 工作线程参数指针
 * @return: NULL
 */
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;                        // 线程ID
	int weight = wp->weight;                // 工作权重
	struct monitor *m = wp->m;              // 监控器
	struct skynet_monitor *sm = m->m[id];   // 该线程对应的监控器
	skynet_initthread(THREAD_WORKER);       // 初始化线程类型为工作线程
	struct message_queue * q = NULL;        // 当前处理的消息队列
	while (!m->quit) {
		// 分发消息，处理服务的消息队列
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			// 没有消息需要处理，进入睡眠状态
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;  // 增加睡眠线程计数
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				// "虚假唤醒"是无害的，因为skynet_context_message_dispatch()可以随时调用
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);  // 等待被唤醒
				-- m->sleep;  // 减少睡眠线程计数
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

/*
 * 启动所有线程
 * 创建监控线程、定时器线程、socket线程和工作线程
 * @param thread: 工作线程数量
 */
static void
start(int thread) {
	pthread_t pid[thread+3];  // 存储所有线程ID，包括3个系统线程和N个工作线程

	// 创建并初始化监控器
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;  // 工作线程总数
	m->sleep = 0;       // 初始睡眠线程数为0

	// 为每个工作线程分配一个监控器
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}
	// 初始化互斥锁
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	// 初始化条件变量
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 创建系统线程
	create_thread(&pid[0], thread_monitor, m);  // 监控线程
	create_thread(&pid[1], thread_timer, m);    // 定时器线程
	create_thread(&pid[2], thread_socket, m);   // socket线程

	/*
	 * 工作线程权重配置
	 * -1: 每次只处理一条消息
	 *  0: 处理队列中的全部消息
	 *  1: 处理队列中消息数量的1/2
	 *  2: 处理队列中消息数量的1/4
	 *  3: 处理队列中消息数量的1/8
	 */
	static int weight[] = {
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	// 创建工作线程
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];  // 使用预定义权重
		} else {
			wp[i].weight = 0;         // 超出范围的线程使用权重0
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	// 等待所有线程结束
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL);
	}

	// 清理监控器资源
	free_monitor(m);
}

/*
 * 启动引导服务
 * 解析启动命令行，创建第一个服务（通常是bootstrap服务）
 * @param logger: logger服务的上下文
 * @param cmdline: 启动命令行，格式为"服务名 参数"
 */
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];  // 服务名
	char args[sz+1];  // 服务参数
	int arg_pos;

	// 解析服务名
	sscanf(cmdline, "%s", name);
	arg_pos = strlen(name);

	// 解析服务参数
	if (arg_pos < sz) {
		// 跳过空格
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		// 复制参数部分
		strncpy(args, cmdline + arg_pos, sz);
	} else {
		// 没有参数
		args[0] = '\0';
	}

	// 创建引导服务
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);  // 处理logger的剩余消息
		exit(1);
	}
}

/*
 * skynet框架启动函数
 * 初始化所有子系统，创建logger服务，启动引导服务，启动所有线程
 * @param config: 配置结构体指针
 */
void
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	// 注册SIGHUP信号处理器，用于重新打开日志文件
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	// 如果配置了守护进程模式，则初始化守护进程
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	// 初始化各个子系统
	skynet_harbor_init(config->harbor);        // 初始化节点管理器
	skynet_handle_init(config->harbor);        // 初始化handle存储器
	skynet_mq_init();                          // 初始化全局消息队列
	skynet_module_init(config->module_path);   // 初始化C模块管理器，设置查找路径
	skynet_timer_init();                       // 初始化全局时间系统
	skynet_socket_init();                      // 初始化socket管理器
	skynet_profile_enable(config->profile);   // 设置是否开启性能分析

	// 创建logger服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	// 注册logger服务的名字，方便其他服务查找
	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

	// 启动引导服务，执行配置文件中的bootstrap命令
	bootstrap(ctx, config->bootstrap);

	// 启动所有线程（监控、定时器、socket、工作线程）
	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	// 清理工作：harbor_exit可能会调用socket发送，所以应该在socket_free之前退出
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
