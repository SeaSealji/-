#include "fiber.h"

static bool debug = false;

namespace sylar {

	//当前线程上的协程控制信息

	//正在运行的协程
	static thread_local Fiber* t_fiber = nullptr;
	//主协程
	static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
	//调度协程
	static thread_local Fiber* t_scheduler_fiber = nullptr;

	//全局协程ID计数器
	static std::atomic<uint64_t> s_fiber_id{0};
	//活跃协程数量计数器
	static std::atomic<uint64_t> s_fiber_count{0};

	void Fiber::SetThis(Fiber* f)
	{
		t_fiber = f;
	}
	
	//首先运行创建主协程
	std::shared_ptr<Fiber> Fiber::GetThis()
	{
		if (t_fiber)
		{
			return t_fiber->shared_from_this();
		}
		std::shared_ptr<Fiber>main_fiber(new Fiber());
		t_thread_fiber = main_fiber;
		t_scheduler_fiber = main_fiber.get();//主协程默认为调度协程

		assert(t_fiber == main_fiber.get());//t_fiber是否等于main_fiber，是就继续执行否则程序终止
		return t_fiber->shared_from_this();

	}

	void Fiber::SetSchedulerFiber(Fiber* f)//设置当前调度协程
	{
		t_scheduler_fiber = f;
	}

	uint64_t Fiber::GetFiberId()//获取当前正在运行的协程ID
	{
		if (t_fiber)
		{
			return t_fiber->get_Id();
		}
		return (uint64_t)-1;//返回-1，
		                    //(Uint64_t)-1那就会转换成UINT64_max，所以用来表示错误的情况
	}
	//创建主协程，设置窗台，初始化上下文，分配ID
	Fiber::Fiber()
	{
		SetThis(this);
		m_state = RUNNING;
		if (getcontext(&m_ctx))
		{
			std::cerr << "Fiber() failed\n";
			pthread_exit(NULL);
		}

		m_id = s_fiber_id++;//分配id，从0开始，用完+1
		s_fiber_count++;//活跃协程数量+1
		if(debug)std::cout << "Fiber::Fiber() id=" << m_id << " total=" << s_fiber_count << std::endl;

	}
	/*
	作用：创建一个新协程，初始化回调函数，栈的大小和状态。分配栈空间，
	并通过make修改上下文当set或swap激活ucontext_t m_ctx上下文时候
	会执行make第二个参数的函数。
	*/
	Fiber::Fiber(std::function<void()> cb, size_t stack_size, bool run_in_scheduler) :
		m_cb(cb), m_run_in_scheduler(run_in_scheduler)
	{
		m_state= READY;

		//分配协程栈空间
		m_stacksize= stack_size ? stack_size : 128000;
		m_stack= malloc(m_stacksize);

		if (getcontext(&m_ctx))
		{
			std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
			pthread_exit(NULL);
		}

		m_ctx.uc_link = nullptr;//这里因为没有设置了后继所以在运行完mainfunc后协程退出，会调用一次yield返回主协程。
	    m_ctx.uc_stack.ss_sp = m_stack ;//设置栈顶
		m_ctx.uc_stack.ss_size = m_stacksize;//设置栈大小
		makecontext(&m_ctx, &Fiber::MainFunc, 0);
		m_id = s_fiber_id++;
		s_fiber_count++;
		if (debug)std::cout << "Fiber():child id=" << m_id << std::endl;
	}
	Fiber::~Fiber()
	{
		s_fiber_count--;//活跃协程数量-1
		if (m_stack)
		{
			free(m_stack);
		}
		if (debug)std::cout << "~Fiber(): id=" << m_id << std::endl;
	}
	//重置协程回调函数，设置上下文，使用与将协程从TERM状态重置为READY状态
	void Fiber::reset(std::function<void()> cb)
	{
		assert(m_stack != nullptr && m_state == TERM);

		m_state= READY;
		m_cb = cb;

		if (getcontext(&m_ctx))
		{
            std::cerr << "Fiber::reset(std::function<void()> cb) failed\n";
            pthread_exit(NULL);
		}
		m_ctx.uc_link = nullptr;
		m_ctx.uc_stack.ss_sp = m_stack;
		m_ctx.uc_stack.ss_size = m_stacksize;
		makecontext(&m_ctx, &Fiber::MainFunc, 0);
	}
	//将协程状态设置为running，恢复协程执行，
	// m_runInScheduler 为 true，则将上下文切换到调度协程；
	// 否则，切换到主线程的协程。
	void Fiber::resume()
	{
        assert(m_state == READY);
        m_state = RUNNING;

		if (m_run_in_scheduler)//类似于非对称协程函数协程切换
		{
			SetThis(this);//目前工作的协程
			if (swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))//切换到m_ctx上下文
			{
				std::cerr<<"resume() to t_scheduler_fibler failed\n";
                pthread_exit(NULL);
			}
		}
		else
		{
			SetThis(this);
			if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
			{
              std::cerr << "resume() to t_thread_fiber failed\n";
              pthread_exit(NULL);
			}

		}
	}

	void Fiber::yield()
	{
		assert(m_state == RUNNING|| m_state == TERM);

		if (m_state != TERM)
		{
			m_state = READY;
		}

		if (m_run_in_scheduler)
		{
            SetThis(t_scheduler_fiber);
			if (swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
			{
                std::cerr << "yield() to t_scheduler_fiber failed\n";
                pthread_exit(NULL);
			}
		}
		else
		{
            SetThis(t_thread_fiber.get());
			if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
			{
              std::cerr << "yield() to t_thread_fiber failed\n";
              pthread_exit(NULL);
			}
		}
	}
	void Fiber::MainFunc()
	{
		std::shared_ptr<Fiber> curr = GetThis();//引用计数+1
		assert(curr!=nullptr);

		curr->m_cb();//执行回调函数
        curr->m_cb = nullptr;//回调函数执行完毕，置空
	    curr->m_state = TERM;//状态设置为TERM

		//运行完毕比->让出执行权
		auto raw_ptr = curr.get();
        curr.reset();
        raw_ptr->yield();	
	}
}