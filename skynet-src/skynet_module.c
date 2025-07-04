/*
 * skynet_module.c - skynet模块管理器
 * 负责动态加载C服务模块，管理模块的生命周期和实例创建
 */

#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32  // 最大模块类型数量

/*
 * 模块管理器结构体
 * 管理所有已加载的C服务模块
 */
struct modules {
	int count;                                  // 已加载的模块数量
	struct spinlock lock;                       // 自旋锁，保护并发访问
	const char * path;                          // 模块搜索路径
	struct skynet_module m[MAX_MODULE_TYPE];    // 模块数组
};

// 全局模块管理器实例
static struct modules * M = NULL;

/*
 * 尝试打开指定名称的动态库
 * 在配置的搜索路径中查找并加载动态库
 * @param m: 模块管理器
 * @param name: 模块名称
 * @return: 动态库句柄，失败返回NULL
 */
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	//search path
	// 在搜索路径中查找模块
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		while (*path == ';') path++;  // 跳过分隔符
		if (*path == '\0') break;     // 路径结束
		l = strchr(path, ';');        // 查找下一个分隔符
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		// 构建完整的模块路径，'?'是模块名的占位符
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size);  // 插入模块名
		if (path[i] == '?') {
			// 复制'?'后面的部分（如文件扩展名）
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		// 尝试加载动态库
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;  // 移动到下一个路径
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

/*
 * 查询已加载的模块
 * 在模块数组中按名称查找指定模块
 * @param name: 模块名称
 * @return: 模块指针，未找到返回NULL
 */
static struct skynet_module *
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

/*
 * 获取模块的API函数指针
 * 根据模块名和API名称构建符号名，然后从动态库中获取函数指针
 * @param mod: 模块结构
 * @param api_name: API函数名称
 * @return: 函数指针，未找到返回NULL
 */
static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	// 构建完整的符号名：模块名 + API名
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	// 如果模块名包含'.'，只使用最后一部分作为符号前缀
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	// 从动态库中获取符号
	return dlsym(mod->module, ptr);
}

static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");
	mod->init = get_api(mod, "_init");
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

struct skynet_module *
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	SPIN_LOCK(M)

	result = _query(name); // double check 双重检查

	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	SPIN_UNLOCK(M)

	return result;
}

void *
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

void
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path);

	SPIN_INIT(m)

	M = m;
}
