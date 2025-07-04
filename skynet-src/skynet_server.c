/*
 * skynet_server.c - skynet服务管理核心模块
 * 负责服务的创建、销毁、消息分发、命令处理等核心功能
 */

#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "spinlock.h"
#include "atomic.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

// 调用检查相关宏定义，用于调试模式下检测重入调用
#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif

/*
 * skynet服务上下文结构体
 * 每个服务实例对应一个context，包含服务的所有状态信息
 */
struct skynet_context {
	void * instance;                    // 服务实例指针，指向具体的服务对象
	struct skynet_module * mod;         // 服务模块指针，包含服务的类型信息和函数表
	void * cb_ud;                       // 回调函数的用户数据
	skynet_cb cb;                       // 消息处理回调函数
	struct message_queue *queue;        // 服务的消息队列
	ATOM_POINTER logfile;               // 日志文件指针（原子操作）
	uint64_t cpu_cost;                  // in microsec CPU消耗时间（微秒）
	uint64_t cpu_start;                 // in microsec CPU开始时间（微秒）
	char result[32];                    // 命令执行结果缓冲区
	uint32_t handle;                    // 服务的唯一标识符
	int session_id;                     // 会话ID，用于消息的请求-响应配对
	ATOM_INT ref;                       // 引用计数（原子操作）
	size_t message_count;               // 处理的消息数量统计
	bool init;                          // 是否已初始化
	bool endless;                       // 是否为无限循环服务（用于监控）
	bool profile;                       // 是否开启性能分析

	CHECKCALLING_DECL                   // 调用检查相关字段
};

/*
 * skynet节点全局信息结构体
 * 管理整个节点的全局状态
 */
struct skynet_node {
	ATOM_INT total;                     // 当前活跃的服务总数（原子操作）
	int init;                           // 节点是否已初始化
	uint32_t monitor_exit;              // 监控退出的服务handle
	pthread_key_t handle_key;           // 线程本地存储键，存储当前线程处理的服务handle
	bool profile;                       // default is on 是否开启性能分析（默认开启）
};

// 全局节点实例
static struct skynet_node G_NODE;

/*
 * 获取当前活跃的服务总数
 * @return: 活跃服务数量
 */
int
skynet_context_total() {
	return ATOM_LOAD(&G_NODE.total);
}

/*
 * 增加服务计数
 * 当创建新服务时调用
 */
static void
context_inc() {
	ATOM_FINC(&G_NODE.total);
}

/*
 * 减少服务计数
 * 当服务销毁时调用
 */
static void
context_dec() {
	ATOM_FDEC(&G_NODE.total);
}

/*
 * 获取当前线程正在处理的服务handle
 * @return: 当前服务的handle，如果未初始化则返回主线程标识
 */
uint32_t
skynet_current_handle(void) {
	if (G_NODE.init) {
		// 从线程本地存储中获取当前处理的服务handle
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
		// 未初始化时返回主线程标识
		uint32_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}

/*
 * 将32位ID转换为16进制字符串表示
 * @param str: 输出字符串缓冲区（至少10字节）
 * @param id: 要转换的ID
 */
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';  // 前缀冒号
	// 将32位ID转换为8位16进制字符
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

/*
 * 丢弃消息时的上下文结构体
 */
struct drop_t {
	uint32_t handle;  // 丢弃消息的服务handle
};

/*
 * 丢弃消息的回调函数
 * 当服务退出时，其消息队列中的剩余消息会被丢弃，并向发送方报告错误
 * @param msg: 要丢弃的消息
 * @param ud: 用户数据，包含丢弃消息的服务handle
 */
static void
drop_message(struct skynet_message *msg, void *ud) {
	struct drop_t *d = ud;
	skynet_free(msg->data);  // 释放消息数据
	uint32_t source = d->handle;
	assert(source);
	// report error to the message source
	// 向消息发送方报告错误
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, msg->session, NULL, 0);
}

/*
 * 创建新的服务上下文
 * @param name: 服务模块名称
 * @param param: 传递给服务的初始化参数
 * @return: 成功返回服务上下文指针，失败返回NULL
 */
struct skynet_context *
skynet_context_new(const char * name, const char *param) {
	// 查找指定名称的服务模块
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

	// 创建服务实例
	void *inst = skynet_module_instance_create(mod);
	if (inst == NULL)
		return NULL;

	// 分配并初始化服务上下文
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	CHECKCALLING_INIT(ctx)  // 初始化调用检查

	ctx->mod = mod;                                    // 设置服务模块
	ctx->instance = inst;                              // 设置服务实例
	ATOM_INIT(&ctx->ref , 2);                         // 初始化引用计数为2
	ctx->cb = NULL;                                    // 消息回调函数
	ctx->cb_ud = NULL;                                 // 回调用户数据
	ctx->session_id = 0;                               // 会话ID
	ATOM_INIT(&ctx->logfile, (uintptr_t)NULL);        // 日志文件

	ctx->init = false;                                 // 初始化标志
	ctx->endless = false;                              // 无限循环标志

	ctx->cpu_cost = 0;                                 // CPU消耗时间
	ctx->cpu_start = 0;                                // CPU开始时间
	ctx->message_count = 0;                            // 消息计数
	ctx->profile = G_NODE.profile;                     // 性能分析开关
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	// 必须先设置为0，避免skynet_handle_retireall获取到未初始化的handle
	ctx->handle = 0;
	ctx->handle = skynet_handle_register(ctx);         // 注册服务handle
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);  // 创建消息队列
	// init function maybe use ctx->handle, so it must init at last
	// 初始化函数可能会使用ctx->handle，所以必须最后初始化
	context_inc();  // 增加服务计数

	CHECKCALLING_BEGIN(ctx)
	int r = skynet_module_instance_init(mod, inst, ctx, param);  // 调用服务的初始化函数
	CHECKCALLING_END(ctx)
	if (r == 0) {
		// 初始化成功
		struct skynet_context * ret = skynet_context_release(ctx);
		if (ret) {
			ctx->init = true;  // 标记为已初始化
		}
		skynet_globalmq_push(queue);  // 将消息队列加入全局队列
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "error: launch %s FAILED", name);
		uint32_t handle = ctx->handle;
		skynet_context_release(ctx);
		skynet_handle_retire(handle);
		struct drop_t d = { handle };
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

/*
 * 为服务上下文生成新的会话ID
 * 会话ID用于消息的请求-响应配对，始终为正数
 * @param ctx: 服务上下文
 * @return: 新的会话ID
 */
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	// 会话ID始终为正数
	int session = ++ctx->session_id;
	if (session <= 0) {
		// 溢出时重置为1
		ctx->session_id = 1;
		return 1;
	}
	return session;
}

/*
 * 增加服务上下文的引用计数
 * 防止服务在使用过程中被释放
 * @param ctx: 服务上下文
 */
void
skynet_context_grab(struct skynet_context *ctx) {
	ATOM_FINC(&ctx->ref);  // 原子操作增加引用计数
}

/*
 * 保留服务上下文
 * 增加引用计数但减少全局服务计数，用于在系统关闭时保留特殊服务
 * @param ctx: 服务上下文
 */
void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);  // 增加引用计数
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.
	// 不计入保留的上下文，因为skynet只有在总上下文数为0时才会终止
	// 保留的上下文会在最后释放
	context_dec();
}

/*
 * 删除服务上下文
 * 释放所有相关资源，包括日志文件、服务实例、消息队列等
 * @param ctx: 要删除的服务上下文
 */
static void
delete_context(struct skynet_context *ctx) {
	// 关闭日志文件
	FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (f) {
		fclose(f);
	}
	// 释放服务实例
	skynet_module_instance_release(ctx->mod, ctx->instance);
	// 标记消息队列为待释放状态
	skynet_mq_mark_release(ctx->queue);
	CHECKCALLING_DESTROY(ctx)  // 销毁调用检查
	skynet_free(ctx);          // 释放上下文内存
	context_dec();             // 减少全局服务计数
}

/*
 * 释放服务上下文的引用
 * 减少引用计数，当引用计数为0时删除上下文
 * @param ctx: 服务上下文
 * @return: 如果上下文仍然有效返回ctx，否则返回NULL
 */
struct skynet_context *
skynet_context_release(struct skynet_context *ctx) {
	if (ATOM_FDEC(&ctx->ref) == 1) {
		// 引用计数降为0，删除上下文
		delete_context(ctx);
		return NULL;
	}
	return ctx;  // 上下文仍然有效
}

/*
 * 向指定handle的服务推送消息
 * 通过handle查找服务上下文，将消息推入其消息队列
 * @param handle: 目标服务的handle
 * @param message: 要推送的消息
 * @return: 成功返回0，失败返回-1
 */
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return -1;  // 服务不存在
	}
	skynet_mq_push(ctx->queue, message);  // 推送消息到队列
	skynet_context_release(ctx);          // 释放引用

	return 0;
}

void 
skynet_context_endless(uint32_t handle) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	ctx->endless = true;
	skynet_context_release(ctx);
}

int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	int ret = skynet_harbor_message_isremote(handle);
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}

/*
 * 分发消息到服务
 * 调用服务的消息处理回调函数，处理性能统计和日志记录
 * @param ctx: 服务上下文
 * @param msg: 要分发的消息
 */
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	assert(ctx->init);  // 确保服务已初始化
	CHECKCALLING_BEGIN(ctx)
	// 设置当前线程处理的服务handle到线程本地存储
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));

	// 解析消息类型和大小
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;

	// 如果开启了日志，记录消息
	FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (f) {
		skynet_log_output(f, msg->source, type, msg->session, msg->data, sz);
	}

	++ctx->message_count;  // 增加消息计数
	int reserve_msg;

	if (ctx->profile) {
		// 开启性能分析时，记录CPU消耗时间
		ctx->cpu_start = skynet_thread_time();
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
		uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
		ctx->cpu_cost += cost_time;
	} else {
		// 直接调用消息处理回调
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
	}

	if (!reserve_msg) {
		// 如果服务不保留消息数据，则释放内存
		skynet_free(msg->data);
	}
	CHECKCALLING_END(ctx)
}

/*
 * 分发服务队列中的所有消息
 * 主要用于错误处理，确保服务退出前处理完所有消息
 * @param ctx: 服务上下文
 */
void
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	// 处理队列中的所有消息
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}

struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	if (q == NULL) {
		q = skynet_globalmq_pop();
		if (q==NULL)
			return NULL;
	}

	uint32_t handle = skynet_mq_handle(q);

	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;

	for (i=0;i<n;i++) {
		if (skynet_mq_pop(q,&msg)) {
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
		} else if (i==0 && weight >= 0) {
			n = skynet_mq_length(q);
			n >>= weight;
		}
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "error: May overload, message queue length = %d", overload);
		}

		skynet_monitor_trigger(sm, msg.source , handle);

		if (ctx->cb == NULL) {
			skynet_free(msg.data);
		} else {
			dispatch_message(ctx, &msg);
		}

		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
		// 如果全局消息队列不为空，将当前队列推回，返回下一个队列
		// 否则（全局队列为空或阻塞），不推回当前队列，继续返回当前队列用于下次分发
		skynet_globalmq_push(q);
		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}

uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		return strtoul(name+1,NULL,16);
	case '.':
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "error: Don't support query global name %s",name);
	return 0;
}

static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}
	if (G_NODE.monitor_exit) {
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	skynet_handle_retire(handle);
}

// skynet command
// skynet命令处理

struct command_func {
	const char *name;
	const char * (*func)(struct skynet_context * context, const char * param);
};

static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;
	int ti = strtol(param, &session_ptr, 10);
	int session = skynet_context_newsession(context);
	skynet_timeout(context->handle, ti, session);
	sprintf(context->result, "%d", session);
	return context->result;
}

static const char *
cmd_reg(struct skynet_context * context, const char * param) {
	if (param == NULL || param[0] == '\0') {
		sprintf(context->result, ":%x", context->handle);
		return context->result;
	} else if (param[0] == '.') {
		return skynet_handle_namehandle(context->handle, param + 1);
	} else {
		skynet_error(context, "error: Can't register global name %s in C", param);
		return NULL;
	}
}

static const char *
cmd_query(struct skynet_context * context, const char * param) {
	if (param[0] == '.') {
		uint32_t handle = skynet_handle_findname(param+1);
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	sscanf(param,"%s %s",name,handle);
	if (handle[0] != ':') {
		return NULL;
	}
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	if (name[0] == '.') {
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "error: Can't set global name %s in C", name);
	}
	return NULL;
}

static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	if (param[0] == ':') {
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "error: Can't convert %s to handle",param);
	}

	return handle;
}

static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;
	char * mod = strsep(&args, " \t\r\n");
	args = strsep(&args, "\r\n");
	struct skynet_context * inst = skynet_context_new(mod,args);
	if (inst == NULL) {
		return NULL;
	} else {
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char key[sz+1];
	int i;
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;
	
	skynet_setenv(key,param);
	return NULL;
}

static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	uint32_t sec = skynet_starttime();
	sprintf(context->result,"%u",sec);
	return context->result;
}

static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			// 返回当前监控服务
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

static const char *
cmd_stat(struct skynet_context * context, const char * param) {
	if (strcmp(param, "mqlen") == 0) {
		int len = skynet_mq_length(context->queue);
		sprintf(context->result, "%d", len);
	} else if (strcmp(param, "endless") == 0) {
		if (context->endless) {
			strcpy(context->result, "1");
			context->endless = false;
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "cpu") == 0) {
		double t = (double)context->cpu_cost / 1000000.0;	// microsec 微秒
		sprintf(context->result, "%lf", t);
	} else if (strcmp(param, "time") == 0) {
		if (context->profile) {
			uint64_t ti = skynet_thread_time() - context->cpu_start;
			double t = (double)ti / 1000000.0;	// microsec 微秒
			sprintf(context->result, "%lf", t);
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "message") == 0) {
		sprintf(context->result, "%zu", context->message_count);
	} else {
		context->result[0] = '\0';
	}
	return context->result;
}

static const char *
cmd_logon(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE *f = NULL;
	FILE * lastf = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (lastf == NULL) {
		f = skynet_log_open(context, handle);
		if (f) {
			if (!ATOM_CAS_POINTER(&ctx->logfile, 0, (uintptr_t)f)) {
				// logfile opens in other thread, close this one.
				// 日志文件在其他线程中打开，关闭当前这个
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

static const char *
cmd_logoff(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE * f = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (f) {
		// logfile may close in other thread
		// 日志文件可能在其他线程中关闭
		if (ATOM_CAS_POINTER(&ctx->logfile, (uintptr_t)f, (uintptr_t)NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

static const char *
cmd_signal(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	param = strchr(param, ' ');
	int sig = 0;
	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.
	// 注意：信号函数应该是线程安全的
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "STAT", cmd_stat },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	while(method->name) {
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;
	type &= 0xff;

	if (allocsession) {
		assert(*session == 0);
		*session = skynet_context_newsession(context);
	}

	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}

int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	if ((sz & MESSAGE_TYPE_MASK) != sz) {
		skynet_error(context, "error: The message to %x is too large", destination);
		if (type & PTYPE_TAG_DONTCOPY) {
			skynet_free(data);
		}
		return -2;
	}
	_filter_args(context, type, &session, (void **)&data, &sz);

	if (source == 0) {
		source = context->handle;
	}

	if (destination == 0) {
		if (data) {
			skynet_error(context, "error: Destination address can't be 0");
			skynet_free(data);
			return -1;
		}

		return session;
	}
	if (skynet_harbor_message_isremote(destination)) {
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz & MESSAGE_TYPE_MASK;
		rmsg->type = sz >> MESSAGE_TYPE_SHIFT;
		skynet_harbor_send(rmsg, source, session);
	} else {
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
	if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;
	if (addr[0] == ':') {
		des = strtoul(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} else {
		if ((sz & MESSAGE_TYPE_MASK) != sz) {
			skynet_error(context, "error: The message to %s is too large", addr);
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -2;
		}
		_filter_args(context, type, &session, (void **)&data, &sz);

		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz & MESSAGE_TYPE_MASK;
		rmsg->type = sz >> MESSAGE_TYPE_SHIFT;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}

	return skynet_send(context, source, des, type, session, data, sz);
}

uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}

void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);
}

void 
skynet_globalinit(void) {
	ATOM_INIT(&G_NODE.total , 0);
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key
	// 设置主线程的键
	skynet_initthread(THREAD_MAIN);
}

void 
skynet_globalexit(void) {
	pthread_key_delete(G_NODE.handle_key);
}

void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void
skynet_profile_enable(int enable) {
	G_NODE.profile = (bool)enable;
}
