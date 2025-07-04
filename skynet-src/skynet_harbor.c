/*
 * skynet_harbor.c - skynet跨节点通信模块
 * 处理不同skynet节点间的消息路由和转发
 */

#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

// 远程消息处理服务上下文
static struct skynet_context * REMOTE = 0;
// 当前节点的harbor ID
static unsigned int HARBOR = ~0;

/*
 * 检查消息类型是否为无效类型
 * 只有PTYPE_SYSTEM和PTYPE_HARBOR类型的消息可以跨节点发送
 * @param type: 消息类型
 * @return: 1表示无效类型，0表示有效类型
 */
static inline int
invalid_type(int type) {
	return type != PTYPE_SYSTEM && type != PTYPE_HARBOR;
}

/*
 * 发送远程消息
 * 将消息发送给远程节点处理服务
 * @param rmsg: 远程消息结构
 * @param source: 消息来源handle
 * @param session: 会话ID
 */
void
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
	assert(invalid_type(rmsg->type) && REMOTE);
	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg), source, PTYPE_SYSTEM, session);
}

/*
 * 判断handle是否为远程服务
 * 通过handle的高位判断是否属于其他节点
 * @param handle: 服务handle
 * @return: 1表示远程服务，0表示本地服务
 */
int
skynet_harbor_message_isremote(uint32_t handle) {
	assert(HARBOR != ~0);
	int h = (handle & ~HANDLE_MASK);  // 提取节点ID部分
	return h != HARBOR && h != 0;     // 不是当前节点且不是本地服务
}

/*
 * 初始化harbor系统
 * 设置当前节点的harbor ID
 * @param harbor: 节点ID
 */
void
skynet_harbor_init(int harbor) {
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT;
}

/*
 * 启动harbor服务
 * 设置远程消息处理服务上下文
 * @param ctx: 远程消息处理服务上下文
 */
void
skynet_harbor_start(void *ctx) {
	// the HARBOR must be reserved to ensure the pointer is valid.
	// It will be released at last by calling skynet_harbor_exit
	// 必须保留HARBOR服务以确保指针有效
	// 将在skynet_harbor_exit中最后释放
	skynet_context_reserve(ctx);
	REMOTE = ctx;
}

/*
 * 退出harbor系统
 * 释放远程消息处理服务上下文
 */
void
skynet_harbor_exit() {
	struct skynet_context * ctx = REMOTE;
	REMOTE = NULL;
	if (ctx) {
		skynet_context_release(ctx);
	}
}
