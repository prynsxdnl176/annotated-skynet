/*
 * skynet_server.h - skynet服务管理器头文件
 * 定义了服务上下文管理、消息分发和全局初始化相关的接口
 */

#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>
#include <stdlib.h>

// 前向声明
struct skynet_context;
struct skynet_message;
struct skynet_monitor;

/*
 * 服务上下文管理接口
 */

// 创建新的服务上下文
struct skynet_context * skynet_context_new(const char * name, const char * parm);

// 增加服务引用计数
void skynet_context_grab(struct skynet_context *);

// 保留服务上下文
void skynet_context_reserve(struct skynet_context *ctx);

// 释放服务上下文
struct skynet_context * skynet_context_release(struct skynet_context *);

// 获取服务handle
uint32_t skynet_context_handle(struct skynet_context *);

// 向指定handle推送消息
int skynet_context_push(uint32_t handle, struct skynet_message *message);

// 发送消息
void skynet_context_send(struct skynet_context * context, void * msg, size_t sz, uint32_t source, int type, int session);

// 创建新的会话ID
int skynet_context_newsession(struct skynet_context *);

// 消息分发处理（返回下一个队列）
struct message_queue * skynet_context_message_dispatch(struct skynet_monitor *, struct message_queue *, int weight);	// return next queue

// 获取当前活跃服务总数
int skynet_context_total();

// 分发服务的所有消息（用于退出前的错误输出）
void skynet_context_dispatchall(struct skynet_context * context);	// for skynet_error output before exit

// 标记服务为无限循环状态（用于监控）
void skynet_context_endless(uint32_t handle);	// for monitor

/*
 * 全局初始化接口
 */

// 全局初始化
void skynet_globalinit(void);

// 全局退出清理
void skynet_globalexit(void);

// 初始化工作线程
void skynet_initthread(int m);

// 启用/禁用性能分析
void skynet_profile_enable(int enable);

#endif
