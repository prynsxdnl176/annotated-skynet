/*
 * skynet_handle.h - skynet服务handle管理头文件
 * 提供服务handle的注册、查找、名称绑定等管理接口
 */

#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// handle格式定义：高8位保留给远程节点ID
#define HANDLE_MASK 0xffffff        // handle掩码（低24位）
#define HANDLE_REMOTE_SHIFT 24      // 远程节点ID位移

// 前向声明
struct skynet_context;

/*
 * handle生命周期管理
 */

// 注册服务上下文，分配新的handle
uint32_t skynet_handle_register(struct skynet_context *);

// 注销指定handle的服务
int skynet_handle_retire(uint32_t handle);

// 通过handle获取服务上下文（增加引用计数）
struct skynet_context * skynet_handle_grab(uint32_t handle);

// 注销所有handle
void skynet_handle_retireall();

/*
 * handle名称管理
 */

// 通过名称查找handle
uint32_t skynet_handle_findname(const char * name);

// 为handle绑定名称
const char * skynet_handle_namehandle(uint32_t handle, const char *name);

/*
 * handle系统初始化
 */

// 初始化handle管理系统
void skynet_handle_init(int harbor);

#endif
