/*
 * skynet_timer.h - skynet定时器系统头文件
 * 提供基于分层时间轮的定时器功能
 */

#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

/*
 * 添加超时事件
 * 在指定时间后向指定服务发送超时消息
 * @param handle: 目标服务handle
 * @param time: 超时时间（厘秒，1/100秒）
 * @param session: 会话ID
 * @return: 成功返回0，失败返回-1
 */
int skynet_timeout(uint32_t handle, int time, int session);

/*
 * 更新系统时间
 * 处理到期的定时器事件
 */
void skynet_updatetime(void);

/*
 * 获取系统启动时间
 * @return: 启动时间戳（厘秒）
 */
uint32_t skynet_starttime(void);

/*
 * 获取线程时间
 * 用于性能分析
 * @return: 线程时间（微秒）
 */
uint64_t skynet_thread_time(void);	// for profile, in micro second

/*
 * 初始化定时器系统
 */
void skynet_timer_init(void);

#endif
