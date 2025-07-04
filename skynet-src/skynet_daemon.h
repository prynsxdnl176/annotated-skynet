/*
 * skynet_daemon.h - skynet守护进程管理头文件
 * 提供守护进程初始化和退出的接口
 */

#ifndef skynet_daemon_h
#define skynet_daemon_h

/*
 * 初始化守护进程
 * 将当前进程转换为守护进程，并创建PID文件
 * @param pidfile: PID文件路径
 * @return: 成功返回0，失败返回-1
 */
int daemon_init(const char *pidfile);

/*
 * 退出守护进程
 * 删除PID文件并清理资源
 * @param pidfile: PID文件路径
 * @return: 成功返回0，失败返回-1
 */
int daemon_exit(const char *pidfile);

#endif
