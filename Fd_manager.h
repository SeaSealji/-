#ifndef _FD_MANAGER_H_
#define _FD_MANAGER_H_
#include<sys/stat.h>
#include <memory>
#include <shared_mutex>
#include<vector>
#include "thread.h"
/*
含有FdCtx和FdManager两个类
管理文件描述符fd上下文和相关操作
*/

namespace sylar {

	//管理文件描述符相关状态操作
	class FdCtx :public std::enable_shared_from_this<FdCtx>
	{
	private:
		bool m_isInit = false;//标记文件描述符是否已初始化
		bool m_isSocket = false;//标记文件描述符是否为一个套接字
		bool m_sysNonblock = false;//标记文件描述符是否为系统非阻塞
		bool m_userNonblock = false;//标记文件描述符是否为用户非阻塞
        bool m_isClosed = false;//标记文件描述符是否已关闭
		int m_fd;//文件描述符值

		uint64_t m_recvTimeout = (uint64_t)-1; //读事件 超时时间 默认-1，表示没有超时限制
		uint64_t m_sendTimeout = (uint64_t)-1; //写事件 超时时间 默认-1，表示没有超时限制
    public:
        FdCtx(int fd);
		~FdCtx();

		bool init();//初始化
		bool isInit()const { return m_isInit; }
		bool isSocket()const { return m_isSocket; }
		bool isClosed()const {return m_isClosed; }

		void setUderNonblock(bool v) { m_userNonblock = v; }//设置获取用户层非阻塞状态
        bool getUserNonblock()const { return m_userNonblock; }

        void setSysNonblock(bool v) { m_sysNonblock = v; }//设置获取系统层非阻塞状态
		bool getSysNonblock()const { return m_sysNonblock; }
		//设置获取超时时间，type区分读写，v=ms
		void setTimeout(int type, uint64_t v);
		uint64_t getTimeout(int type);

	};


	//管理Fdctx
	class FdManager
	{
	public:
		FdManager();

		//获取指定文件描述符的FdCtx，auto_create表示如果不存在是否自动创建Fdctx
		std::shared_ptr<FdCtx> get(int fd,bool auto_create=false);
        void del(int fd);//删除指定文件描述符的FdCtx
	private:
		std::shared_mutex m_mutex;//保护对m_datas的访问，
		std::vector<std::shared_ptr<FdCtx>> m_datas;//FdCtx对象共享指针
	};

	template<class T>
	class Singleton {
	private:
		static T* instance;// 对外提供的实例
		static std::mutex mutex;//互斥锁
		Singleton();
		~Singleton();
        Singleton(const Singleton&) = delete;
		Singleton& operator=(const Singleton&) = delete;
	public:
		static T* GetInstance() {
			std::lock_guard < std::mutex > lock(mutex);
			if (instance == nullptr) {
			instance = new T();
			}
			return instance;
		}
		static void DestroyInstance() {
			std::lock_guard<std::mutex> lock(mutex);
			if (instance) {
				delete instance;
				instance = nullptr;
			}
		}
	};

	typedef Singleton<FdManager> FdMgr;//FdManager单例
}
#endif