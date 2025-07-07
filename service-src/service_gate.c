#include "skynet.h"
#include "skynet_socket.h"
#include "databuffer.h"
#include "hashid.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define BACKLOG 128  // 监听队列的最大长度

// 连接结构，表示一个客户端连接
struct connection {
	int id;	// skynet_socket id  // socket连接的ID
	uint32_t agent;               // 代理服务的句柄
	uint32_t client;              // 客户端服务的句柄
	char remote_name[32];         // 远程客户端地址信息
	struct databuffer buffer;     // 数据缓冲区，用于存储接收的数据
};

// 网关结构，管理所有客户端连接
struct gate {
	struct skynet_context *ctx;   // skynet上下文
	int listen_id;                // 监听socket的ID
	uint32_t watchdog;            // 看门狗服务句柄，用于监控连接状态
	uint32_t broker;              // 代理服务句柄，用于转发消息
	int client_tag;               // 客户端消息类型标签
	int header_size;              // 消息头大小（2或4字节）
	int max_connection;           // 最大连接数
	struct hashid hash;           // 哈希表，用于快速查找连接
	struct connection *conn;      // 连接数组
	// todo: save message pool ptr for release
	// 待办：保存消息池指针用于释放
	struct messagepool mp;        // 消息池，用于管理消息节点的内存分配
};

// 创建网关实例
struct gate *
gate_create(void) {
	struct gate * g = skynet_malloc(sizeof(*g));
	memset(g,0,sizeof(*g));  // 初始化为0
	g->listen_id = -1;       // 初始化监听ID为-1（无效值）
	return g;
}

// 释放网关实例及其资源
void
gate_release(struct gate *g) {
	int i;
	struct skynet_context *ctx = g->ctx;
	// 关闭所有客户端连接
	for (i=0;i<g->max_connection;i++) {
		struct connection *c = &g->conn[i];
		if (c->id >=0) {
			skynet_socket_close(ctx, c->id);  // 关闭socket连接
		}
	}
	// 关闭监听socket
	if (g->listen_id >= 0) {
		skynet_socket_close(ctx, g->listen_id);
	}
	messagepool_free(&g->mp);  // 释放消息池
	hashid_clear(&g->hash);    // 清理哈希表
	skynet_free(g->conn);      // 释放连接数组
	skynet_free(g);            // 释放网关结构体
}

// 解析命令参数，移除命令前缀，保留参数部分
static void
_parm(char *msg, int sz, int command_sz) {
	// 跳过命令后的空格
	while (command_sz < sz) {
		if (msg[command_sz] != ' ')
			break;
		++command_sz;
	}
	int i;
	// 将参数部分移动到字符串开头
	for (i=command_sz;i<sz;i++) {
		msg[i-command_sz] = msg[i];
	}
	msg[i-command_sz] = '\0';  // 添加字符串结束符
}

// 为指定连接设置代理和客户端句柄
static void
_forward_agent(struct gate * g, int fd, uint32_t agentaddr, uint32_t clientaddr) {
	int id = hashid_lookup(&g->hash, fd);  // 查找连接ID
	if (id >=0) {
		struct connection * agent = &g->conn[id];
		agent->agent = agentaddr;   // 设置代理服务句柄
		agent->client = clientaddr; // 设置客户端服务句柄
	}
}

// 处理控制命令（kick、forward、broker、start、close等）
static void
_ctrl(struct gate * g, const void * msg, int sz) {
	struct skynet_context * ctx = g->ctx;
	char tmp[sz+1];
	memcpy(tmp, msg, sz);  // 复制消息内容
	tmp[sz] = '\0';        // 添加字符串结束符
	char * command = tmp;
	int i;
	if (sz == 0)
		return;  // 空消息，直接返回
	// 查找命令和参数的分隔符（空格）
	for (i=0;i<sz;i++) {
		if (command[i]==' ') {
			break;
		}
	}
	// 处理kick命令：踢掉指定连接
	if (memcmp(command,"kick",i)==0) {
		_parm(tmp, sz, i);  // 解析参数
		int uid = strtol(command , NULL, 10);  // 获取连接ID
		int id = hashid_lookup(&g->hash, uid); // 查找连接
		if (id>=0) {
			skynet_socket_close(ctx, uid);  // 关闭连接
		}
		return;
	}
	// 处理forward命令：设置连接的代理和客户端句柄
	if (memcmp(command,"forward",i)==0) {
		_parm(tmp, sz, i);  // 解析参数
		char * client = tmp;
		char * idstr = strsep(&client, " ");  // 分离连接ID
		if (client == NULL) {
			return;
		}
		int id = strtol(idstr , NULL, 10);    // 解析连接ID
		char * agent = strsep(&client, " ");  // 分离代理句柄
		if (client == NULL) {
			return;
		}
		uint32_t agent_handle = strtoul(agent+1, NULL, 16);   // 解析代理句柄（跳过前缀）
		uint32_t client_handle = strtoul(client+1, NULL, 16); // 解析客户端句柄（跳过前缀）
		_forward_agent(g, id, agent_handle, client_handle);   // 设置转发代理
		return;
	}
	// 处理broker命令：设置代理服务
	if (memcmp(command,"broker",i)==0) {
		_parm(tmp, sz, i);  // 解析参数
		g->broker = skynet_queryname(ctx, command);  // 查询并设置代理服务句柄
		return;
	}
	// 处理start命令：启动指定连接
	if (memcmp(command,"start",i) == 0) {
		_parm(tmp, sz, i);  // 解析参数
		int uid = strtol(command , NULL, 10);  // 获取连接ID
		int id = hashid_lookup(&g->hash, uid); // 查找连接
		if (id>=0) {
			skynet_socket_start(ctx, uid);  // 启动socket连接
		}
		return;
	}
	// 处理close命令：关闭监听socket
	if (memcmp(command, "close", i) == 0) {
		if (g->listen_id >= 0) {
			skynet_socket_close(ctx, g->listen_id);  // 关闭监听socket
			g->listen_id = -1;  // 重置监听ID
		}
		return;
	}
	skynet_error(ctx, "[gate] Unknown command : %s", command);  // 未知命令错误
}

// 向看门狗服务报告连接状态信息
static void
_report(struct gate * g, const char * data, ...) {
	if (g->watchdog == 0) {
		return;  // 没有设置看门狗服务，直接返回
	}
	struct skynet_context * ctx = g->ctx;
	va_list ap;
	va_start(ap, data);
	char tmp[1024];
	int n = vsnprintf(tmp, sizeof(tmp), data, ap);  // 格式化报告消息
	va_end(ap);

	skynet_send(ctx, 0, g->watchdog, PTYPE_TEXT,  0, tmp, n);  // 发送报告给看门狗
}

// 转发客户端消息到相应的服务
static void
_forward(struct gate *g, struct connection * c, int size) {
	struct skynet_context * ctx = g->ctx;
	int fd = c->id;
	if (fd <= 0) {
		// socket error
		// socket错误，直接返回
		return;
	}
	if (g->broker) {
		// 有代理服务，转发给代理
		void * temp = skynet_malloc(size);
		databuffer_read(&c->buffer,&g->mp,(char *)temp, size);  // 从缓冲区读取数据
		skynet_send(ctx, 0, g->broker, g->client_tag | PTYPE_TAG_DONTCOPY, fd, temp, size);  // 发送给代理服务
		return;
	}
	if (c->agent) {
		// 有专门的代理服务，转发给代理
		void * temp = skynet_malloc(size);
		databuffer_read(&c->buffer,&g->mp,(char *)temp, size);  // 从缓冲区读取数据
		skynet_send(ctx, c->client, c->agent, g->client_tag | PTYPE_TAG_DONTCOPY, fd , temp, size);  // 发送给代理服务
	} else if (g->watchdog) {
		// 没有代理服务，发送给看门狗处理
		char * tmp = skynet_malloc(size + 32);
		int n = snprintf(tmp,32,"%d data ",c->id);  // 添加连接ID前缀
		databuffer_read(&c->buffer,&g->mp,tmp+n,size);  // 读取数据到缓冲区
		skynet_send(ctx, 0, g->watchdog, PTYPE_TEXT | PTYPE_TAG_DONTCOPY, fd, tmp, size + n);  // 发送给看门狗
	}
}

// 处理接收到的消息数据，解析消息头并转发完整消息
static void
dispatch_message(struct gate *g, struct connection *c, int id, void * data, int sz) {
	databuffer_push(&c->buffer,&g->mp, data, sz);  // 将数据添加到连接的缓冲区
	for (;;) {
		// 尝试读取消息头，获取消息体长度
		int size = databuffer_readheader(&c->buffer, &g->mp, g->header_size);
		if (size < 0) {
			return;  // 数据不足，等待更多数据
		} else if (size > 0) {
			if (size >= 0x1000000) {
				// 消息过大（超过16M），关闭连接
				struct skynet_context * ctx = g->ctx;
				databuffer_clear(&c->buffer,&g->mp);  // 清空缓冲区
				skynet_socket_close(ctx, id);         // 关闭连接
				skynet_error(ctx, "Recv socket message > 16M");
				return;
			} else {
				_forward(g, c, size);           // 转发完整消息
				databuffer_reset(&c->buffer);  // 重置缓冲区状态，准备读取下一条消息
			}
		}
	}
}

// 分发处理socket消息，根据消息类型执行相应操作
static void
dispatch_socket_message(struct gate *g, const struct skynet_socket_message * message, int sz) {
	struct skynet_context * ctx = g->ctx;
	switch(message->type) {
	case SKYNET_SOCKET_TYPE_DATA: {
		// 处理数据消息：接收到客户端发送的数据
		int id = hashid_lookup(&g->hash, message->id);  // 查找连接ID
		if (id>=0) {
			struct connection *c = &g->conn[id];
			dispatch_message(g, c, message->id, message->buffer, message->ud);  // 分发消息数据
		} else {
			// 未知连接，丢弃消息并关闭连接
			skynet_error(ctx, "Drop unknown connection %d message", message->id);
			skynet_socket_close(ctx, message->id);
			skynet_free(message->buffer);  // 释放消息缓冲区
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_CONNECT: {
		// 处理连接消息：socket连接建立成功
		if (message->id == g->listen_id) {
			// start listening
			// 监听socket开始监听，无需特殊处理
			break;
		}
		int id = hashid_lookup(&g->hash, message->id);  // 查找连接ID
		if (id<0) {
			// 未知连接，关闭它
			skynet_error(ctx, "Close unknown connection %d", message->id);
			skynet_socket_close(ctx, message->id);
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_CLOSE:
	case SKYNET_SOCKET_TYPE_ERROR: {
		// 处理关闭和错误消息：连接断开或出错
		int id = hashid_remove(&g->hash, message->id);  // 从哈希表中移除连接
		if (id>=0) {
			struct connection *c = &g->conn[id];
			databuffer_clear(&c->buffer,&g->mp);  // 清空数据缓冲区
			memset(c, 0, sizeof(*c));             // 重置连接结构
			c->id = -1;                           // 标记为无效连接
			_report(g, "%d close", message->id);  // 向看门狗报告连接关闭
			skynet_socket_close(ctx, message->id); // 关闭socket
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_ACCEPT:
		// report accept, then it will be get a SKYNET_SOCKET_TYPE_CONNECT message
		// 报告接受连接，然后会收到一个SKYNET_SOCKET_TYPE_CONNECT消息
		assert(g->listen_id == message->id);  // 确保是监听socket的接受事件
		if (hashid_full(&g->hash)) {
			// 连接数已满，直接关闭新连接
			skynet_socket_close(ctx, message->ud);
		} else {
			// 为新连接分配连接槽位
			struct connection *c = &g->conn[hashid_insert(&g->hash, message->ud)];
			if (sz >= sizeof(c->remote_name)) {
				sz = sizeof(c->remote_name) - 1;  // 防止缓冲区溢出
			}
			c->id = message->ud;                      // 设置连接ID
			memcpy(c->remote_name, message+1, sz);   // 复制远程地址信息
			c->remote_name[sz] = '\0';               // 添加字符串结束符
			_report(g, "%d open %d %s:0",c->id, c->id, c->remote_name);  // 向看门狗报告新连接
			skynet_error(ctx, "socket open: %x", c->id);  // 记录连接打开日志
		}
		break;
	case SKYNET_SOCKET_TYPE_WARNING:
		// 处理警告消息：发送缓冲区过大
		skynet_error(ctx, "fd (%d) send buffer (%d)K", message->id, message->ud);
		break;
	}
}

// 网关服务的消息回调函数，处理不同类型的消息
static int
_cb(struct skynet_context * ctx, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct gate *g = ud;
	switch(type) {
	case PTYPE_TEXT:
		// 处理文本消息：控制命令
		_ctrl(g , msg , (int)sz);
		break;
	case PTYPE_CLIENT: {
		// 处理客户端消息：向指定连接发送数据
		if (sz <=4 ) {
			skynet_error(ctx, "Invalid client message from %x",source);
			break;
		}
		// The last 4 bytes in msg are the id of socket, write following bytes to it
		// 消息的最后4字节是socket的id，将前面的字节写入到该socket
		const uint8_t * idbuf = msg + sz - 4;
		uint32_t uid = idbuf[0] | idbuf[1] << 8 | idbuf[2] << 16 | idbuf[3] << 24;  // 小端字节序解析连接ID
		int id = hashid_lookup(&g->hash, uid);  // 查找连接
		if (id>=0) {
			// don't send id (last 4 bytes)
			// 不发送id（最后4字节）
			skynet_socket_send(ctx, uid, (void*)msg, sz-4);  // 发送数据到客户端连接
			// return 1 means don't free msg
			// 返回1表示不释放消息内存
			return 1;
		} else {
			skynet_error(ctx, "Invalid client id %d from %x",(int)uid,source);
			break;
		}
	}
	case PTYPE_SOCKET:
		// recv socket message from skynet_socket
		// 接收来自skynet_socket的socket消息
		dispatch_socket_message(g, msg, (int)(sz-sizeof(struct skynet_socket_message)));
		break;
	}
	return 0;  // 返回0表示正常处理完成
}

// 启动监听服务，解析地址并创建监听socket
static int
start_listen(struct gate *g, char * listen_addr) {
	struct skynet_context * ctx = g->ctx;
	char * portstr = strrchr(listen_addr,':');  // 查找最后一个冒号，分离主机和端口
	const char * host = "";                     // 默认主机为空（监听所有接口）
	int port;
	if (portstr == NULL) {
		// 没有冒号，整个字符串都是端口号
		port = strtol(listen_addr, NULL, 10);
		if (port <= 0) {
			skynet_error(ctx, "Invalid gate address %s",listen_addr);
			return 1;  // 端口号无效
		}
	} else {
		// 有冒号，分离主机和端口
		port = strtol(portstr + 1, NULL, 10);  // 解析端口号
		if (port <= 0) {
			skynet_error(ctx, "Invalid gate address %s",listen_addr);
			return 1;  // 端口号无效
		}
		portstr[0] = '\0';  // 截断字符串，分离主机部分
		host = listen_addr; // 设置主机地址
	}
	g->listen_id = skynet_socket_listen(ctx, host, port, BACKLOG);  // 创建监听socket
	if (g->listen_id < 0) {
		return 1;  // 监听失败
	}
	skynet_socket_start(ctx, g->listen_id);  // 启动监听
	return 0;  // 成功
}

// 初始化网关服务，解析参数并设置各种配置
int
gate_init(struct gate *g , struct skynet_context * ctx, char * parm) {
	if (parm == NULL)
		return 1;  // 参数为空，初始化失败
	int max = 0;
	int sz = strlen(parm)+1;
	char watchdog[sz];  // 看门狗服务名
	char binding[sz];   // 绑定地址
	int client_tag = 0; // 客户端消息标签
	char header;        // 消息头类型（S=2字节，L=4字节）
	// 解析参数：消息头类型 看门狗服务 绑定地址 客户端标签 最大连接数
	int n = sscanf(parm, "%c %s %s %d %d", &header, watchdog, binding, &client_tag, &max);
	if (n<4) {
		skynet_error(ctx, "Invalid gate parm %s",parm);
		return 1;  // 参数格式错误
	}
	if (max <=0 ) {
		skynet_error(ctx, "Need max connection");
		return 1;  // 最大连接数必须大于0
	}
	if (header != 'S' && header !='L') {
		skynet_error(ctx, "Invalid data header style");
		return 1;  // 消息头类型只能是S或L
	}

	if (client_tag == 0) {
		client_tag = PTYPE_CLIENT;  // 默认客户端消息类型
	}
	if (watchdog[0] == '!') {
		g->watchdog = 0;  // '!'表示不使用看门狗
	} else {
		g->watchdog = skynet_queryname(ctx, watchdog);  // 查询看门狗服务句柄
		if (g->watchdog == 0) {
			skynet_error(ctx, "Invalid watchdog %s",watchdog);
			return 1;  // 看门狗服务不存在
		}
	}

	g->ctx = ctx;  // 设置上下文

	hashid_init(&g->hash, max);  // 初始化哈希表
	g->conn = skynet_malloc(max * sizeof(struct connection));  // 分配连接数组
	memset(g->conn, 0, max *sizeof(struct connection));        // 清零连接数组
	g->max_connection = max;  // 设置最大连接数
	int i;
	// 初始化所有连接为无效状态
	for (i=0;i<max;i++) {
		g->conn[i].id = -1;
	}

	g->client_tag = client_tag;                    // 设置客户端消息标签
	g->header_size = header=='S' ? 2 : 4;          // 设置消息头大小

	skynet_callback(ctx,g,_cb);  // 注册消息回调函数

	return start_listen(g,binding);  // 启动监听服务
}
