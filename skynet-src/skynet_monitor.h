/*
 * skynet_monitor.h - skynet监控系统头文件
 * 提供死循环检测和服务监控功能
 */

#ifndef SKYNET_MONITOR_H
#define SKYNET_MONITOR_H

#include <stdint.h>

// 前向声明
struct skynet_monitor;

/*
 * 创建监控器
 * @return: 监控器实例指针
 */
struct skynet_monitor * skynet_monitor_new();

/*
 * 删除监控器
 * @param monitor: 监控器实例指针
 */
void skynet_monitor_delete(struct skynet_monitor *);

/*
 * 触发监控检查
 * 记录服务间的消息传递，用于死循环检测
 * @param monitor: 监控器实例指针
 * @param source: 消息源handle
 * @param destination: 消息目标handle
 */
void skynet_monitor_trigger(struct skynet_monitor *, uint32_t source, uint32_t destination);

/*
 * 执行监控检查
 * 检查是否有服务陷入死循环
 * @param monitor: 监控器实例指针
 */
void skynet_monitor_check(struct skynet_monitor *);

#endif
