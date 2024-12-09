#include"Fd_manager.h"
#include "Hook.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
namespace sylar {

	template class Singleton <FdManager>;//全局唯一单例实例
	//这些行代码定义了 Singleton 类模板的静态成员变量 instance 和 mutex。
	// 静态成员变量需要在类外部定义和初始化。
	template<typename T>
	T* Singleton<T>::instance = nullptr;
	template<typename T>
	std::mutex Singleton<T>::mutex;

	FdCtx::FdCtx(int fd) :m_fd(fd) {
		init();
	}
	FdCtx::~FdCtx() {
	}
	bool FdCtx::init() {
		if (m_isInit) {//如果已经初始化
			return true;
		}
		struct stat statbuf;
		//fstat函数用于获取与文件描述符 m_fd 关联的文件状态信息存放到statbuf中。
		// 如果 fstat() 返回 -1，表示文件描述符无效或出现错误。
		if (-1 == fstat(m_fd, &statbuf)) {
			m_isInit = false;
			m_isSocket = false;
		}
		else {
			m_isInit = true;
			m_isSocket = S_ISSOCK(statbuf.st_mode);//S_ISSOCK(statbuf.st_mode) 用于判断文件类型是否为套接字
		}
		if (m_isSocket) {//表示与m_fd关联文件是套接字
			int flags = fcntl_f(m_fd, F_GETFL, 0);//获取文件描述符状态
			if (!(flag & O_NONBLOCK)) {//如果文件描述符不是非阻塞
			fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK);//设置文件描述符为非阻塞
			}
			m_sysNonblock = true;//hook非阻塞设置成功
		}
		else {
			m_sysNonblock = false;//非socket无需设置非阻塞
		}
		return m_isInit;
	}

	void FdCtx::setTimeout(int type, uint64_t v) {
		if (type == SO_RCVTIMEO)
		{
			m_recvTimeout = v;
		}
		else
		{
            m_sendTimeout = v;
		}
	}
	uint64_t FdCtx::getTimeout(int type) {
		if (type == SO_RCVTIMEO) {
			return m_recvTimeout;
		}
		else {
			return m_sendTimeout;
		}
	}

	FdManager::FdManager() {
		m_datas.resize(64);
	}

	std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create) {
		if (fd == -1)
		{
			return nullptr;
		}
		std::shared_lock<std::shared_mutex>read_lock(m_mutex);
		//如果fd超过datas范围，auto——create为false，返回nullptr，表示没有新建对象需求
		if (fd >= m_datas.size()) {
			if (!auto_create) {
				return nullptr;
			}
		}
		else
		{
			if (m_datas[fd] || !auto_create) {
			 return m_datas[fd];
			}
		}
		//fd大小超过data，auto——create为true，新建对象
		read_lock.unlock();
		std::unique_lock<std::shared_mutex>write_lock(m_mutex);
		if (m_datas.size() < fd)
		{
			m_datas.resize(fd *1.5);
		}
		m_datas[fd] = std::make_shared<FdCtx>(fd);
		return m_datas[fd];
	}

	void FdManager::del(int fd)
	{
		std::unique_lock<std::shared_mutex>write_lock(m_mutex);
		if (fd >= m_datas.size()) {
			return;
		}
		m_datas[fd].reset();
	}
}s