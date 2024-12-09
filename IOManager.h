#pragma once 
#include"Scheduler.h"
#include"Timer.h"
#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>
namespace sylar {
	class IOManager :public Scheduler, public TimerManager
	{
	public:
		enum Event//枚举类
		{
			NONE = 0x0,//没有事件
			//READ == EPOLLIN
			READ = 0X1,//表示读事件，对应于epoll的EPOLLIN事件
			//WRITE == EPOLLOUT

			WRITE = 0X4//写事件，对应于epoll的EPOLLOUT事件。

		};
	private:
		struct FdContext//用于描述一个文件描述的事件上下文
		{
			struct EventContext//描述一个具体事件的上下文，如读事件或写事件。
			{
				Scheduler* scheduler = nullptr;//关联调度器
				std::shared_ptr<Fiber> fiber;//关联回调线数（协程）
				std::function<void()>cb;//关联回调函数
			};

			EventContext read;
			EventContext write;

			int fd = 0;

			Event events = NONE;//当前注册的事件目前是没有事件，但可能变成 READ、WRITE 或二者的组合。
			std::mutex mutex;
			EventContext& getEventContext(Event event);//根据时间类型获取相应的事件上下文
			void resetEventContext(EventContext& ctx);//重置事件上下文
			void triggerEvent(Event event);//触发事件

		};
	public:
		//threads线程数量，use_caller是否讲主线程或调度线程包含进行，name调度器的名字
		IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "IOManager");//允许设置线程数，是否使用调用者线程和名称
		~IOManager();
		//时间管理方法
		int addEvent(int fd, Event event, std::function<void()>cb = nullptr);//添加一个事件到文件描述符fd上，关联一个回调函数cb

		bool delEvent(int fd, Event event);//删除文件描述符的某个事件

		bool cancelEvent(int fd, Event event);//取消文件描述符的某个事件，触发回调函数

		bool cancelAll(int fd);

		static IOManager* GetThis();
	protected:
		//通知调度器有任务调度
		//写pipe让idle协程从epoll_wait退出，待idle协程yield后Scheduler：：run就可以调度其他任务
		void tickle() override;
		//判断调度器是否可以停止
		//判断条件是Scheduler::stopping()外加IOManager的m_pendingEventcount为0，表示没有IO事件可调度
		bool stopping() override;
		//实际idle协程只负责收集所有已触发的fd回调函数并加入调度器任务队列
		//真正执行是idle协程推出后，调度器在下一轮调度时执行
		void idle()override;//这里是scheduler的重写，当没有事件处理，线程处于空闲
		void onTimerInsertedAtFront()override;//因为Timer类成员函数重写当有新的定时器插入到前面的处理逻辑
		void contextResize(size_t size);//调整文件描述符上下文数组大小
	private:
		int m_epfd = 0;//用于epoll的文件描述符
		int m_tickleFds[2];//线程间通信的管道文件描述符，0是读，1是写

		//原子计数器，用于记录待处理的事件数量。
		// 使用atomic的好处是这个变量再进行加或-都是不会被多线程影响
		std::atomic<size_t>m_pendingEventCount = { 0 };
		std::shared_mutex m_mutex;//读写锁

		std::vector<FdContext*>m_fdContexts;//文件描述符上下文组数，用于存储描述符的FdContext
	};
}