#include"Scheduler.h"
static bool debug = false;//默认为false

namespace sylar {
	//用于保存当前线程的调度器对象。
	static thread_local Scheduler* t_scheduler = nullptr;

	Scheduler* Scheduler::GetThis()
	{
		return t_scheduler;
	}
	void Scheduler::SetThis()
	{
		t_scheduler= this;
	}
	Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name) :
		m_useCaller(use_caller), m_name(name)
	{
		//判断线程数量是否大于0，并且调度器的对象是否是空指针，
		// 是就调用setThis()进行设置
		assert(threads > 0&&Scheduler::GetThis() == nullptr);
		SetThis();//设置当前调度器对象

		Thread::SetName(m_name);//设置当前线程名为调度器名称 m_name

		//使用主线程作为工作线程，为了实现更高效的任务调度和管理
		if (use_caller)//如果user_caller为true,表示当前线程也要作为一个工作线程使用
		{
			threads--;//因为此时作为了工作线程，所以线程数量-1

			//创建主协程
			Fiber::GetThis();

			//创建调度协程
			m_schedulerFiber.reset(new Fiber(std::bind(& Scheduler::run, this), 0, false));//false ->该调度协程推出后返回主协程
			Fiber::SetSchedulerFiber(m_schedulerFiber.get());//设置调度协程
			m_rootThread = Thread::GetThreadId();//获取主线程ID
			m_threadIds.push_back(m_rootThread);//将主线程ID添加到线程ID列表中
		}
		m_threadCount = threads;//剩余协程数量
		if (debug) std::cout << "Scheduler::Scheduler() success\n";
	}
	Scheduler::~Scheduler()
	{
		assert(stopping() == true);//判断调度器是否已经停止
		if (GetThis() == this)//获取调度器对象
		{
			t_scheduler = nullptr;//防止悬空指针
		}
		if (debug)std::cout<<"Scheduler::~Scheduler() success\n";
	}
	bool Scheduler::stopping()
	{
        //判断调度器是否已经停止
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stopping &&m_tasks.empty()&&m_activeThreadCount==0;
	}
	void Scheduler::start()
	{
		std::lock_guard<std::mutex> lock(m_mutex);//互斥锁防止共享资源的竞争
		if (m_stopping)//如果调度器退出直接报错打印cerr后面的话
		{
			std::cerr << "Scheduler is stopped" << std::endl;
			return;
		}
		assert(m_threads.empty());//判断线程池是否为空
		m_threads.resize(m_threadCount);//设置线程池大小
		for (size_t i = 0; i < m_threadCount; i++)
		{
            m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));//创建
			m_threadIds.push_back(m_threads[i]->getId());//将线程ID添加到线程ID列表中
		}
		if(debug)std::cout << "Scheduler::start() success\n";
	}
	void Scheduler::run()
	{
		int thread_id= Thread::GetThreadId();//获取当前线程ID
		if(debug)std::cout<<"Scheduler::run() thread_id="<<thread_id<<"\n";

		//set_hook_enable(true);

		SetThis();//设置当前调度器对象
		//运行在新创建的线程->需要创建主协程
		if (thread_id != m_rootThread)//如果不是主线程，创建主协程
		{
			Fiber::GetThis();//分配了线程的主协程和调度协程
		}

		//创建空闲协程，std::make_shared时c++引入的一个函数，
		// 用于创建 std::shared_ptr 对象。相比于直接使用 std::shared_ptr 构造函数，std::make_shared 更高效且更安全，
		// 因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作。
		std::shared_ptr<Fiber> idle_fiber= std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));//子协程
		ScheduleTask task;

		while (true)
		{
			task.reset();
			bool tickle_me = false;//是否唤醒了其他线程进行任务调度
			
			{
				std::lock_guard<std::mutex> lock(m_mutex);//互斥锁防止共享资源的竞争
				auto it = m_tasks.begin();
				//遍历任务队列
				while (it != m_tasks.end())
				{
					if (it->thread != -1 && it->thread != thread_id)//不能等于当前thread_id,其目的是让人任何的线程都可以执行。
					{
                        ++it;
						tickle_me = true;
						continue;
					}

		            //取出任务
					assert(it->fiber || it->cb);
					task = *it;
					m_tasks.erase(it);
					m_activeThreadCount++;
					break;
				}
				tickle_me = tickle_me || (it != m_tasks.end());//如果任务队列不为空，则唤醒其他线程进行任务调度
			}
			if (tickle_me)//这里虽然写了唤醒但并没有具体的逻辑代码，具体的在io+scheduler
			{
				tickle();
			}

			//执行任务
			if (task.fiber)
			{//resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程-1；
				{
					std::lock_guard<std::mutex>lock (task.fiber->m_mutex);
					if (task.fiber->get_state() != Fiber::TERM)
					{
						task.fiber->resume();
					}
				}
				m_activeThreadCount--;//线程完成后就不再处于活跃状态，而是进入空闲
				task.reset();
			}
			else if (task.cb)
			{//函数被调度
				std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
				{
					std::lock_guard<std::mutex>lock(cb_fiber->m_mutex);
					cb_fiber->resume();
				}
				m_activeThreadCount--;
				task.reset();
			}
			//无任务 -> 执行空闲协程
			else
			{
				//系统关闭 ->idle协程将从死循环中跳出并结束,此时idle协程状态为TERM
				if (idle_fiber->get_state() == Fiber::TERM)
				{
					//如果调度器没有调度任务，那么idle协程回不断的resume/yield,不会结束进入一个忙等待，如果idle协程结束了
					//一定是调度器停止了，直到有任务才执行上面的if/else，在这里idle_fiber就是不断的和主协程进行交互的子协程
					if (debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
					break;
				}
				m_idleThreadCount++;
				idle_fiber->resume();
				m_idleThreadCount--;
			}
		}
    }
	void Scheduler::idle()
	{
		while (!stopping())
		{
			if (debug)std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;
			sleep(1);//降低空闲协程在无任务时对cpu占用率，避免空转浪费资源
			Fiber::GetThis()->yield();
		}
	}
	void Scheduler::stop()
	{
		if (debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;
		if (stopping())
		{
			return;
		}
		m_stopping = true;

		if (m_useCaller)
		{
			assert(GetThis() == this);
		}
		else
		{
			assert(GetThis() != this);
		}
		////调用tickle()的目的唤醒空闲线程或协程，防止m_scheduler或其他线程处于永久阻塞在等待任务的状态中
		for (size_t i = 0; i < m_idleThreadCount; i++)
		{
			tickle();
		}

		if (m_schedulerFiber)
		{
			tickle();//唤醒可能处于挂起状态，等待下一个任务的调度的协程
		}
		//当只有主线程或调度线程作为工作线程的情况，只能从stop()方法开始任务调度
		if (m_schedulerFiber)
		{
			m_schedulerFiber->resume();//开始任务调度
			if(debug)std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
		}
		//获取此时的线程通过swap不会增加引用计数的方式加入到thrs，方便下面的join保持线程正常退出
		std::vector<std::shared_ptr<Thread>>thrs;
		{
			std::lock_guard<std::mutex>lock(m_mutex);
			thrs.swap(m_threads);
		}
		for (auto& i : thrs)
		{
			i->join();
		}
		if (debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
	}
	void Scheduler::tickle()
	{

	}
}
