/*
 * skynet_module.h - skynet动态模块管理头文件
 * 定义了动态模块的接口和管理功能
 */

#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

// 前向声明
struct skynet_context;

/*
 * 模块接口函数类型定义
 */

// 创建模块实例
typedef void * (*skynet_dl_create)(void);

// 初始化模块实例
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);

// 释放模块实例
typedef void (*skynet_dl_release)(void * inst);

// 发送信号给模块实例
typedef void (*skynet_dl_signal)(void * inst, int signal);

/*
 * 模块描述结构体
 * 包含模块的基本信息和接口函数指针
 */
struct skynet_module {
	const char * name;          // 模块名称
	void * module;              // 动态库句柄
	skynet_dl_create create;    // 创建函数指针
	skynet_dl_init init;        // 初始化函数指针
	skynet_dl_release release;  // 释放函数指针
	skynet_dl_signal signal;    // 信号处理函数指针
};

/*
 * 模块管理接口
 */

// 查询模块（按名称）
struct skynet_module * skynet_module_query(const char * name);

// 创建模块实例
void * skynet_module_instance_create(struct skynet_module *);

// 初始化模块实例
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);

// 释放模块实例
void skynet_module_instance_release(struct skynet_module *, void *inst);

// 向模块实例发送信号
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

// 初始化模块系统
void skynet_module_init(const char *path);

#endif
