/*
 * skynet_monitor.c - skynet监控系统
 * 用于检测服务间的消息处理是否陷入死循环或长时间阻塞
 * 通过版本号机制监控消息处理的进度
 */

#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>

/*
 * skynet监控器结构体
 * 用于跟踪消息处理状态，检测潜在的死循环
 */
struct skynet_monitor {
	ATOM_INT version;       // 版本号（原子操作），每次消息处理时递增
	int check_version;      // 上次检查时的版本号
	uint32_t source;        // 消息来源服务handle
	uint32_t destination;   // 消息目标服务handle
};

/*
 * 创建新的监控器实例
 * @return: 新创建的监控器指针
 */
struct skynet_monitor *
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));  // 初始化为0
	return ret;
}

/*
 * 删除监控器实例
 * @param sm: 要删除的监控器
 */
void
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}

/*
 * 触发监控器，记录消息处理开始
 * 在消息分发开始和结束时调用，用于更新监控状态
 * @param sm: 监控器实例
 * @param source: 消息来源服务handle（0表示处理结束）
 * @param destination: 消息目标服务handle
 */
void
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;
	sm->destination = destination;
	ATOM_FINC(&sm->version);  // 原子递增版本号
}

/*
 * 检查监控器状态，检测是否存在死循环
 * 如果版本号在两次检查间没有变化，说明消息处理可能陷入死循环
 * @param sm: 监控器实例
 */
void
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {
		// 版本号未变化，可能存在死循环
		if (sm->destination) {
			// 标记目标服务为无限循环状态
			skynet_context_endless(sm->destination);
			skynet_error(NULL, "error: A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		// 版本号已变化，更新检查版本号
		sm->check_version = sm->version;
	}
}
