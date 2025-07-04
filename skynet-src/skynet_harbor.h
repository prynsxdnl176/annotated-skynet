/*
 * skynet_harbor.h - skynet跨节点通信头文件
 * 定义了集群节点间消息传递的数据结构和接口
 */

#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

// 全局服务名称最大长度
#define GLOBALNAME_LENGTH 16

// 最大远程节点数量
#define REMOTE_MAX 256

/*
 * 远程服务名称结构体
 * 用于标识远程节点上的服务
 */
struct remote_name {
	char name[GLOBALNAME_LENGTH];  // 服务名称
	uint32_t handle;               // 服务handle
};

/*
 * 远程消息结构体
 * 用于跨节点消息传递
 */
struct remote_message {
	struct remote_name destination;  // 目标服务信息
	const void * message;           // 消息内容
	size_t sz;                      // 消息大小
	int type;                       // 消息类型
};

/*
 * harbor系统接口
 */

// 发送远程消息
void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);

// 检查handle是否为远程服务
int skynet_harbor_message_isremote(uint32_t handle);

// 初始化harbor系统
void skynet_harbor_init(int harbor);

// 启动harbor服务
void skynet_harbor_start(void * ctx);

// 退出harbor系统
void skynet_harbor_exit();

#endif
