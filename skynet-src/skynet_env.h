/*
 * skynet_env.h - skynet环境变量管理头文件
 * 提供基于Lua的环境变量存储和访问接口
 */

#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

/*
 * 获取环境变量值
 * @param key: 环境变量名
 * @return: 环境变量值，不存在返回NULL
 */
const char * skynet_getenv(const char *key);

/*
 * 设置环境变量值
 * @param key: 环境变量名
 * @param value: 环境变量值
 */
void skynet_setenv(const char *key, const char *value);

/*
 * 初始化环境变量系统
 * 创建Lua状态机用于存储环境变量
 */
void skynet_env_init();

#endif
