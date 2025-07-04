/*
 * skynet_log.c - skynet日志系统
 * 提供服务级别的消息日志记录功能，用于调试和监控
 */

#include "skynet_log.h"
#include "skynet_timer.h"
#include "skynet.h"
#include "skynet_socket.h"
#include <string.h>
#include <time.h>

/*
 * 打开服务的日志文件
 * 为指定handle的服务创建专用的日志文件
 * @param ctx: 服务上下文
 * @param handle: 服务handle
 * @return: 日志文件指针，失败返回NULL
 */
FILE *
skynet_log_open(struct skynet_context * ctx, uint32_t handle) {
	const char * logpath = skynet_getenv("logpath");
	if (logpath == NULL)
		return NULL;  // 未配置日志路径

	size_t sz = strlen(logpath);
	char tmp[sz + 16];
	sprintf(tmp, "%s/%08x.log", logpath, handle);  // 生成日志文件名

	FILE *f = fopen(tmp, "ab");  // 以追加模式打开
	if (f) {
		// 记录日志文件打开时间
		uint32_t starttime = skynet_starttime();
		uint64_t currenttime = skynet_now();
		time_t ti = starttime + currenttime/100;
		skynet_error(ctx, "Open log file %s", tmp);
		fprintf(f, "open time: %u %s", (uint32_t)currenttime, ctime(&ti));
		fflush(f);
	} else {
		skynet_error(ctx, "Open log file %s fail", tmp);
	}
	return f;
}

/*
 * 关闭服务的日志文件
 * @param ctx: 服务上下文
 * @param f: 日志文件指针
 * @param handle: 服务handle
 */
void
skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle) {
	skynet_error(ctx, "Close log file :%08x", handle);
	fprintf(f, "close time: %u\n", (uint32_t)skynet_now());  // 记录关闭时间
	fclose(f);
}

/*
 * 以16进制格式记录二进制数据
 * @param f: 日志文件指针
 * @param buffer: 数据缓冲区
 * @param sz: 数据大小
 */
static void
log_blob(FILE *f, void * buffer, size_t sz) {
	size_t i;
	uint8_t * buf = buffer;
	for (i=0;i!=sz;i++) {
		fprintf(f, "%02x", buf[i]);  // 每个字节输出为两位16进制
	}
}

/*
 * 记录socket消息到日志
 * @param f: 日志文件指针
 * @param message: socket消息
 * @param sz: 消息大小
 */
static void
log_socket(FILE * f, struct skynet_socket_message * message, size_t sz) {
	fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

	if (message->buffer == NULL) {
		const char *buffer = (const char *)(message + 1);
		sz -= sizeof(*message);
		const char * eol = memchr(buffer, '\0', sz);
		if (eol) {
			sz = eol - buffer;
		}
		fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
	} else {
		sz = message->ud;
		log_blob(f, message->buffer, sz);
	}
	fprintf(f, "\n");
	fflush(f);
}

/*
 * 输出消息到日志文件
 * 根据消息类型选择不同的日志格式
 * @param f: 日志文件指针
 * @param source: 消息源handle
 * @param type: 消息类型
 * @param session: 会话ID
 * @param buffer: 消息数据缓冲区
 * @param sz: 消息大小
 */
void
skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz) {
	if (type == PTYPE_SOCKET) {
		// socket消息使用特殊格式
		log_socket(f, buffer, sz);
	} else {
		// 普通消息格式：源handle 类型 会话ID 时间戳 数据
		uint32_t ti = (uint32_t)skynet_now();
		fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
		log_blob(f, buffer, sz);  // 以16进制输出数据
		fprintf(f,"\n");
		fflush(f);
	}
}
