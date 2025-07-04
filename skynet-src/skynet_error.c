/*
 * skynet_error.c - skynet错误处理模块
 * 提供统一的错误日志输出功能，将错误信息发送给logger服务
 */

#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_MESSAGE_SIZE 256  // 日志消息最大长度

/*
 * 尝试使用vasprintf格式化字符串
 * 特殊处理lua-skynet.c中lerror函数的"%*s"格式
 * @param strp: 输出字符串指针
 * @param fmt: 格式字符串
 * @param ap: 可变参数列表
 * @return: 格式化后的字符串长度，失败返回-1
 */
static int
log_try_vasprintf(char **strp, const char *fmt, va_list ap) {
	if (strcmp(fmt, "%*s") == 0) {
		// for `lerror` in lua-skynet.c
		// 特殊处理lua-skynet.c中的lerror函数
		const int len = va_arg(ap, int);
		const char *tmp = va_arg(ap, const char*);
		*strp = skynet_strndup(tmp, len);
		return *strp != NULL ? len : -1;
	}

	// 使用固定大小缓冲区格式化
	char tmp[LOG_MESSAGE_SIZE];
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, fmt, ap);
	if (len >= 0 && len < LOG_MESSAGE_SIZE) {
		*strp = skynet_strndup(tmp, len);
		if (*strp == NULL) return -1;
	}
	return len;
}

/*
 * skynet错误日志输出函数
 * 将错误信息格式化后发送给logger服务进行处理
 * @param context: 产生错误的服务上下文（可为NULL）
 * @param msg: 错误消息格式字符串
 * @param ...: 可变参数
 */
void
skynet_error(struct skynet_context * context, const char *msg, ...) {
	static uint32_t logger = 0;
	if (logger == 0) {
		// 查找logger服务的handle
		logger = skynet_handle_findname("logger");
	}
	if (logger == 0) {
		// logger服务不存在，直接返回
		return;
	}

	char *data = NULL;

	va_list ap;

	// 第一次尝试格式化
	va_start(ap, msg);
	int len = log_try_vasprintf(&data, msg, ap);
	va_end(ap);
	if (len < 0) {
		perror("vasprintf error :");
		return;
	}

	if (data == NULL) { // unlikely
		// 第一次格式化失败，使用动态分配的缓冲区重试
		data = skynet_malloc(len + 1);
		va_start(ap, msg);
		len = vsnprintf(data, len + 1, msg, ap);
		va_end(ap);
		if (len < 0) {
			skynet_free(data);
			perror("vsnprintf error :");
			return;
		}
	}

	// 构造错误消息并发送给logger服务
	struct skynet_message smsg;
	if (context == NULL) {
		smsg.source = 0;  // 系统错误
	} else {
		smsg.source = skynet_context_handle(context);  // 服务错误
	}
	smsg.session = 0;
	smsg.data = data;
	smsg.sz = len | ((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);  // 文本类型消息
	skynet_context_push(logger, &smsg);
}
