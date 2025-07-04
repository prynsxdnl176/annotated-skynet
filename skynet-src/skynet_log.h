/*
 * skynet_log.h - skynet日志系统头文件
 * 提供服务级别的日志记录和输出接口
 */

#ifndef skynet_log_h
#define skynet_log_h

#include "skynet_env.h"
#include "skynet.h"

#include <stdio.h>
#include <stdint.h>

/*
 * 打开日志文件
 * 为指定服务打开日志文件用于记录
 * @param ctx: 服务上下文
 * @param handle: 服务handle
 * @return: 日志文件指针，失败返回NULL
 */
FILE * skynet_log_open(struct skynet_context * ctx, uint32_t handle);

/*
 * 关闭日志文件
 * 关闭指定服务的日志文件
 * @param ctx: 服务上下文
 * @param f: 日志文件指针
 * @param handle: 服务handle
 */
void skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle);

/*
 * 输出日志内容
 * 将消息内容格式化输出到日志文件
 * @param f: 日志文件指针
 * @param source: 消息源handle
 * @param type: 消息类型
 * @param session: 会话ID
 * @param buffer: 消息内容
 * @param sz: 消息大小
 */
void skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz);

#endif