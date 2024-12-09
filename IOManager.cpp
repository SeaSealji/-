#include"IOManager.h"
#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>


static bool debug = true;
namespace sylar {

	IOManager* IOManager::GetThis()
	{ //与static相比dynamic有检查
		return dynamic_cast<IOManager*>(Scheduler::GetThis());
	}

	IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event)
	{
		assert(event == READ || event == WRITE);//判断时间要么是读，要么是写
		switch (event)
		{
		case sylar::IOManager::READ:
			return read;
		case sylar::IOManager::WRITE:
			return write;
		}
		throw std::invalid_argument("Unsupported event type");
	}

	void IOManager::FdContext::resetEventContext(EventContext& ctx)
	{
		ctx.scheduler = nullptr;
		ctx.fiber.reset();
		ctx.cb = nullptr;
	}

	void IOManager::FdContext::triggerEvent(IOManager::Event event)
	{
		//无指定事件，中断
		assert(events & event);
        
		//delete event,注册IO事件是一次性
		//如果想持续关注某个socket fd读写，每次触发后重新添加
		//对标志位取反就是将event从events去除
		events = (Event)(events & ~event);

		// 触发器
		EventContext& ctx = getEventContext(event);
		if (ctx.cb)
		{
			ctx.scheduler->ScheduleLock(&ctx.cb);
		}
		else
		{
			ctx.scheduler->ScheduleLock(&ctx.fiber);
		}

		resetEventContext(ctx);
		return; 

	}
	IOManager::IOManager(size_t threads, bool use_caller, const std::string& name) :
		Scheduler(threads,  use_caller, name), TimerManager()
	{
		m_epfd = epoll_create(5000);
		assert(m_epfd > 0);//错误就终止程序

		int rt = pipe(m_tickleFds);//创建管道 
		assert(!rt);//错误终止程序

		//管道监听注册到epoll
		epoll_event event;
		event.events = EPOLLIN | EPOLLET;//标志位，采用边缘触发和读
		event.data.fd = m_tickleFds[0];

		//修改管道描述符以非阻塞的方式，配合边缘触发
		rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
		assert(!rt);//每次要判断rt是否成功
		rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);//将 m_tickleFds[0];作为读事件放入到event监听集合中
		assert(!rt);
		contextResize(32);//初始化32个文件描述符上下文的数组

		start();//启动Scheduler，开启线程池，准备处理任务.
	}

	//IOManager析构函数
	IOManager::~IOManager()
	{
		stop();//Scheduler关闭线程池
		close(m_epfd);//关闭epoll句柄
		close(m_tickleFds[0]); //关闭管道读写端 
		close(m_tickleFds[1]);

		//将fdcontext的文件描述符一个个关闭
		for (size_t i = 0; i < m_fdContexts.size(); i++)
		{
			if (m_fdContexts[i])
			{
				delete m_fdContexts[i];
			}
		}
	}
	void IOManager::contextResize(size_t size)
	{
		m_fdContexts.resize(size);
		//便利m_fdcontext向量，初始化未初始化的fdcontext对象
		for (size_t i = 0; i < m_fdContexts.size(); i++)
		{
			if (m_fdContexts[i] == nullptr)
			{
				m_fdContexts[i] = new FdContext();
				m_fdContexts[i]->fd = i;//文件描述符编号赋值
			}
		}
	}
	bool IOManager::delEvent(int fd, Event event)
	{
		FdContext* fd_ctx = nullptr;

		std::shared_lock<std::shared_mutex>read_lock(m_mutex);//读锁
		if ((int)m_fdContexts.size() > fd)//查找fdcontext如果没查找到代表数组中没这个文件描述符，返回false;
		{
			fd_ctx = m_fdContexts[fd];
			read_lock.unlock();
		}
		else
		{
			read_lock.unlock();
			return false;
		}

		//加互斥锁
		std::lock_guard<std::mutex>lock(fd_ctx->mutex);

		if (!(fd_ctx->events & event))//如果事件不相同返回false
		{
			return false;
		}

		//delete the event
		//对原有的事件状态取反就是删除原有的状态
		// 比如说传入参数是读事件，我们取反就是删除了这个读事件,但可能还要写事件
		Event new_events = (Event)(fd_ctx->events & ~event);
		int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
		epoll_event epevent;
		epevent.events = EPOLLET | new_events;
		epevent.data.ptr = fd_ctx;//这是为了epoll触发时能快速找到与该事件相关联的FdContext对象
        
		int rt = epoll_ctl(m_epfd, op, fd, &epevent);
		if (rt)
		{
			std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
			return -1;
		}

		--m_pendingEventCount;//减少待处理事件

		fd_ctx->events = new_events;//因为要先将fd_ctx的状态放入epevent.data.ptr所以就没先去更新，这也是为什么需要单独写Event new_events
	
		FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
		fd_ctx->resetEventContext(event_ctx);
		return true;
	}
	bool IOManager::cancelEvent(int fd, Event event) {
		FdContext* fd_ctx = nullptr;
		std::shared_lock<std::shared_mutex>read_lock(m_mutex);
		if ((int)m_fdContexts.size() > fd)
		{
			fd_ctx = m_fdContexts[fd];
			read_lock.unlock();
		}
		else
		{
			read_lock.unlock();
			return false;
		}

		std::lock_guard<std::mutex>lock(fd_ctx->mutex);

		if (!(fd_ctx->events & event))
		{
			return false;
		}
		Event new_events = (Event)(fd_ctx->events & ~event);
		int op =new_events?EPOLL_CTL_MOD:EPOLL_CTL_DEL;
		epoll_event epevent;
        epevent.events = EPOLLET | new_events;
		epevent.data.ptr = fd_ctx;

		int rt = epoll_ctl(m_epfd, op, fd, &epevent);
		if (rt)
		{
			std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
		    return -1;
		}
		--m_pendingEventCount;

		fd_ctx->triggerEvent(event);
		return true;

	}

	bool IOManager::cancelAll(int fd)
	{
		FdContext* fd_ctx = nullptr;
		std::shared_lock<std::shared_mutex>read_lock(m_mutex);
		if ((int)m_fdContexts.size() > fd)
		{
			fd_ctx = m_fdContexts[fd];
			read_lock.unlock();
		}
		else
		{
			read_lock.unlock();
			return false;
		}

		std::lock_guard<std::mutex>lock(fd_ctx->mutex);

		if (!fd_ctx->events)
		{
			return false;
		}

		int op = EPOLL_CTL_DEL;
		epoll_event epevent;
		epevent.events = 0;
		epevent.data.ptr = fd_ctx;

		int rt = epoll_ctl(m_epfd, op, fd, &epevent);
		if (rt)
		{
			std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl;
			return -1;
		}

		if (fd_ctx->events & READ)
		{
			fd_ctx->triggerEvent(READ);
			--m_pendingEventCount;
		}
		if (fd_ctx->events & WRITE)
		{
			fd_ctx->triggerEvent(WRITE);
			--m_pendingEventCount;
		}
		assert(fd_ctx->events == 0);
		return true;

	}

	void IOManager::tickle()
	{
		if (!hasIdleThreads())//这个函数在scheduler检查是否有空闲线程，没有直接返回
		{
			return;
		}
		//如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符 "T"。
		// 这个写操作的目的是向等待在 m_tickleFds[0]（管道的另一端）的线程发送一个信号，通知它有新任务可以处理了。
		int rt = write(m_tickleFds[1], "T", 1);
		assert(rt == 1);
	}

	bool IOManager::stopping()
	{
		uint64_t timeout = getNextTimer();

		return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
	}
	void IOManager::idle()
	{
		//定义epoll_wait能同时处理的最大事件数
		static const uint64_t MAX_EVENTS = 256;
		//使用std::unique_ptr动态分配一个大小为MAX_EVENTS的epoll_event数组，用于存储epoll_wait返回的事件。
		std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

		while (true)
		{
			if(debug)std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl;
			if (stopping())
			{
				if(debug)std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
                break;
			}

			int rt = 0;
			while (true)
			{
				static const uint64_t MAX_TIMEOUT = 5000;//定义最大超时时间5000ms
				uint64_t next_timeout = getNextTimer();
				next_timeout = std::min(next_timeout, MAX_TIMEOUT);
				rt = epoll_wait(m_epfd, events.get(), MAX_EVENTS, next_timeout);

				if (rt < 0 && errno == EINTR)//rt小于0代表无限阻塞，errno是EINTR(表示信号中断)
				{
					continue;
				}
				else
				{
					break;
				}

			};//end epoll_wait

			std::vector<std::function<void()>>cbs;//存储超时的回调函数
			listExpiredCb(cbs);//获取所有超时的定时器回调，添加到cbs中
			if (!cbs.empty())
			{
				for (const auto& cb : cbs)
				{
					ScheduleLock(cb);
				}
				cbs.clear();
			}

			//遍历所有的rt，代表多少个事件准备了
			for (int i = 0; i < rt; i++)
			{
				epoll_event &event = events[i];//获取第i个epoll_event,用于处理该事件

				//检查当前时间是否为tickle(唤醒空闲线程)
				if (event.data.fd == m_tickleFds[0])
				{
					uint8_t dummy[256];
					//循环读取管道内容来消耗所有的唤醒信号，防止后续重复触发
					while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
					continue;
				}

				//其他事件
				FdContext *fd_ctx =(FdContext* )event.data.ptr;//通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fd_ctx，该指针包含了与文件描述符相关的上下文信息。
				std::lock_guard<std::mutex> lock(fd_ctx->mutex);

				//如果当前事件是错误或挂起（EPOLLERR 或 EPOLLHUP），则将其转换为可读或可写事件（EPOLLIN 或 EPOLLOUT），以便后续处理。
				if (event.events & (EPOLLERR | EPOLLHUP))
				{
					event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
				}
				//如果当前事件是关闭事件（EPOLLRDHUP），则将其转换为可读事件（EPOLLIN），以便后续处理。
				int real_events = NONE;
				if (event.events & EPOLLIN)
				{
					real_events != READ;
				}
				if (event.events & EPOLLOUT)
				{
                    real_events |= WRITE;

				}
				if ((fd_ctx->events & real_events) == NONE)
				{
					continue;
				}

				//计算剩余未发送事件
				int left_events= (fd_ctx->events & ~real_events);

				int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
				event.events = EPOLLET | left_events;//如果left_event没有事件了那么就只剩下边缘触发了events设置了
				//根据之前计算的操作（op），调用 epoll_ctl 更新或删除 epoll 监听，如果失败，打印错误并继续处理下一个事件。
				int rt2= epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
				if (rt2)
				{
					std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl;
					continue;
				}

				if (real_events & READ)
				{
					fd_ctx->triggerEvent(READ);
					--m_pendingEventCount;
				}
				if (real_events & WRITE)
				{
                    fd_ctx->triggerEvent(WRITE);
                    --m_pendingEventCount;

				}
			}
			//当前线程的协程主动让出控制权，调度器可以选择执行其他任务或再次进入 idle 状态。
			Fiber::GetThis()->yield();
		}
	}

	void IOManager::onTimerInsertedAtFront()
	{
		tickle();//唤醒可能被阻塞的epoll_wait
	}

}