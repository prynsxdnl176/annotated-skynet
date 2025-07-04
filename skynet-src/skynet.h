/*
 * skynet.h - skynet框架核心头文件
 * 定义了skynet框架的基本数据类型、消息类型和核心API接口
 */

#ifndef SKYNET_H
#define SKYNET_H

#include "skynet_malloc.h"

#include <stddef.h>
#include <stdint.h>

// 消息协议类型定义
#define PTYPE_TEXT 0            // 文本消息
#define PTYPE_RESPONSE 1        // 响应消息
#define PTYPE_MULTICAST 2       // 多播消息
#define PTYPE_CLIENT 3          // 客户端消息
#define PTYPE_SYSTEM 4          // 系统消息
#define PTYPE_HARBOR 5          // 跨节点消息
#define PTYPE_SOCKET 6          // socket消息
// read lualib/skynet.lua examples/simplemonitor.lua
#define PTYPE_ERROR 7           // 错误消息（参考lualib/skynet.lua examples/simplemonitor.lua）
// read lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua
// 保留的消息类型（参考lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua）
#define PTYPE_RESERVED_QUEUE 8  // 队列保留类型
#define PTYPE_RESERVED_DEBUG 9  // 调试保留类型
#define PTYPE_RESERVED_LUA 10   // Lua保留类型
#define PTYPE_RESERVED_SNAX 11  // SNAX保留类型

// 消息标签定义
#define PTYPE_TAG_DONTCOPY 0x10000      // 不复制消息数据标签
#define PTYPE_TAG_ALLOCSESSION 0x20000  // 分配会话ID标签

// 前向声明
struct skynet_context;

/*
 * 核心API函数声明
 */

// 错误日志输出
void skynet_error(struct skynet_context * context, const char *msg, ...);

// 服务命令执行
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);

// 服务名称查询
uint32_t skynet_queryname(struct skynet_context * context, const char * name);

// 消息发送（通过handle）
int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);

// 消息发送（通过名称）
int skynet_sendname(struct skynet_context * context, uint32_t source, const char * destination , int type, int session, void * msg, size_t sz);

// 检查handle是否为远程服务
int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

// 消息处理回调函数类型定义
typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

// 设置消息处理回调
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

// 获取当前服务handle
uint32_t skynet_current_handle(void);

// 获取当前时间（毫秒）
uint64_t skynet_now(void);

// 调试用内存信息输出
void skynet_debug_memory(const char *info);	// for debug use, output current service memory to stderr

#endif
