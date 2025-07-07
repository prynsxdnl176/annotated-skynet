#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// 日志服务结构，用于管理日志文件的写入
struct logger {
	FILE * handle;      // 日志文件句柄
	char * filename;    // 日志文件名
	uint32_t starttime; // 服务启动时间
	int close;          // 是否需要关闭文件句柄的标志
};

// 创建日志服务实例
struct logger *
logger_create(void) {
	struct logger * inst = skynet_malloc(sizeof(*inst));
	inst->handle = NULL;    // 初始化文件句柄为空
	inst->close = 0;        // 初始化关闭标志为0
	inst->filename = NULL;  // 初始化文件名为空

	return inst;
}

// 释放日志服务实例及其资源
void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);  // 如果需要关闭文件，则关闭文件句柄
	}
	skynet_free(inst->filename);  // 释放文件名内存
	skynet_free(inst);            // 释放日志实例内存
}

#define SIZETIMEFMT	250  // 时间格式字符串的最大长度

// 生成时间字符串，返回毫秒部分
static int
timestring(struct logger *inst, char tmp[SIZETIMEFMT]) {
	uint64_t now = skynet_now();                    // 获取当前时间（厘秒）
	time_t ti = now/100 + inst->starttime;          // 转换为秒并加上启动时间
	struct tm info;
	(void)localtime_r(&ti,&info);                   // 转换为本地时间结构
	strftime(tmp, SIZETIMEFMT, "%d/%m/%y %H:%M:%S", &info);  // 格式化时间字符串
	return now % 100;                               // 返回毫秒部分（0-99）
}

// 日志服务的消息回调函数，处理系统消息和文本日志消息
static int
logger_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	switch (type) {
	case PTYPE_SYSTEM:
		// 处理系统消息：重新打开日志文件（用于日志轮转）
		if (inst->filename) {
			inst->handle = freopen(inst->filename, "a", inst->handle);  // 以追加模式重新打开文件
		}
		break;
	case PTYPE_TEXT:
		// 处理文本消息：写入日志内容
		if (inst->filename) {
			// 如果有文件名，添加时间戳
			char tmp[SIZETIMEFMT];
			int csec = timestring(ud, tmp);                           // 生成时间字符串
			fprintf(inst->handle, "%s.%02d ", tmp, csec);             // 写入时间戳（精确到厘秒）
		}
		fprintf(inst->handle, "[:%08x] ", source);  // 写入消息源服务的句柄
		fwrite(msg, sz , 1, inst->handle);          // 写入日志消息内容
		fprintf(inst->handle, "\n");               // 添加换行符
		fflush(inst->handle);                      // 立即刷新缓冲区，确保日志及时写入
		break;
	}

	return 0;  // 返回0表示正常处理完成
}

// 初始化日志服务，设置日志文件和回调函数
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	const char * r = skynet_command(ctx, "STARTTIME", NULL);  // 获取skynet启动时间
	inst->starttime = strtoul(r, NULL, 10);                   // 保存启动时间用于时间戳计算
	if (parm) {
		// 如果指定了文件名，打开日志文件
		inst->handle = fopen(parm,"a");  // 以追加模式打开文件
		if (inst->handle == NULL) {
			return 1;  // 文件打开失败
		}
		inst->filename = skynet_malloc(strlen(parm)+1);  // 分配内存保存文件名
		strcpy(inst->filename, parm);                    // 复制文件名
		inst->close = 1;                                 // 标记需要关闭文件
	} else {
		// 如果没有指定文件名，使用标准输出
		inst->handle = stdout;
	}
	if (inst->handle) {
		skynet_callback(ctx, inst, logger_cb);  // 注册消息回调函数
		return 0;  // 初始化成功
	}
	return 1;  // 初始化失败
}
