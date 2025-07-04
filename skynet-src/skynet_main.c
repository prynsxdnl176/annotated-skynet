/*
 * skynet_main.c - skynet框架的主入口文件
 * 负责解析配置文件、初始化环境变量、启动skynet框架
 */

#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

/*
 * 从环境变量中获取整数配置项
 * @param key: 配置项的键名
 * @param opt: 默认值
 * @return: 配置项的整数值，如果不存在则返回默认值并设置到环境变量中
 */
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		// 如果环境变量中没有该配置项，则使用默认值并设置到环境变量中
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	// 将字符串转换为整数返回
	return strtol(str, NULL, 10);
}

/*
 * 从环境变量中获取布尔配置项
 * @param key: 配置项的键名
 * @param opt: 默认值
 * @return: 配置项的布尔值，如果不存在则返回默认值并设置到环境变量中
 */
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		// 如果环境变量中没有该配置项，则使用默认值并设置到环境变量中
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	// 判断字符串是否为"true"
	return strcmp(str,"true")==0;
}

/*
 * 从环境变量中获取字符串配置项
 * @param key: 配置项的键名
 * @param opt: 默认值
 * @return: 配置项的字符串值，如果不存在则返回默认值并设置到环境变量中
 */
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			// 如果有默认值，则设置到环境变量中
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

/*
 * 从lua配置表中读取配置项并设置到环境变量中
 * @param L: lua虚拟机状态，栈顶应该是配置表
 */
static void
_init_env(lua_State *L) {
	/* 压入第一个键，用于遍历表 */
	lua_pushnil(L);  /* first key */
	// 遍历配置表中的所有键值对
	while (lua_next(L, -2) != 0) {
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			// 配置表的键必须是字符串类型
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			// 如果值是布尔类型，转换为字符串存储
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			// 其他类型转换为字符串存储
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		lua_pop(L,1);  // 弹出值，保留键用于下次迭代
	}
	lua_pop(L,1);  // 弹出配置表
}

/*
 * 忽略SIGPIPE信号
 * 防止在写入已关闭的socket时程序被意外终止
 * @return: 总是返回0
 */
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;  // 设置信号处理器为忽略
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);  // 忽略SIGPIPE信号
	return 0;
}

/*
 * 用于加载配置文件的lua代码
 * 这段代码会被加载到lua虚拟机中执行，用于解析skynet的配置文件
 * 支持include指令来包含其他配置文件，支持环境变量替换
 */
static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

/*
 * skynet框架的主入口函数
 * 负责解析配置文件、初始化全局环境、启动skynet框架
 * @param argc: 命令行参数个数
 * @param argv: 命令行参数数组
 * @return: 程序退出码，0表示正常退出，1表示异常退出
 */
int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
		// 获取配置文件路径
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	// 初始化全局节点数据
	skynet_globalinit();
	// 初始化环境变量系统
	skynet_env_init();

	// 忽略SIGPIPE信号，防止写数据时被意外终止进程
	sigign();

	struct skynet_config config;

#ifdef LUA_CACHELIB
	// init the lock of code cache
	// 初始化lua代码缓存的锁
	luaL_initcodecache();
#endif

	// 创建一个lua虚拟机来加载配置文件
	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);	// link lua lib

	// 将配置加载代码加载到虚拟机中
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	lua_pushstring(L, config_file);

	// 使用lua代码解析配置文件
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	// 从配置文件中读取内容，写入到环境变量中
	_init_env(L);
	// 配置解析完成，关闭lua虚拟机
	lua_close(L);

	// 从环境变量中获取各项配置，设置到config结构中
	config.thread =  optint("thread",8);                                    // 工作线程数量，默认8个
	config.module_path = optstring("cpath","./cservice/?.so");              // C模块搜索路径
	config.harbor = optint("harbor", 1);                                    // 节点ID，用于集群
	config.bootstrap = optstring("bootstrap","snlua bootstrap");            // 启动服务命令
	config.daemon = optstring("daemon", NULL);                              // 守护进程配置
	config.logger = optstring("logger", NULL);                              // 日志服务参数
	config.logservice = optstring("logservice", "logger");                  // 日志服务名称
	config.profile = optboolean("profile", 1);                              // 是否开启性能分析

	// 通过config结构中的内容，开始正式启动skynet
	skynet_start(&config);
	// 退出全局环境
	skynet_globalexit();

	return 0;
}
