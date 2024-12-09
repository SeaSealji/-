#ifndef _COROUTINE_H_
#define _COROUTINE_H_
#include <iostream>
#include <memory>
#include <atomic>
#include <functional>
#include <cassert>
#include <ucontext.h>
#include <unistd.h>
#include <mutex>

namespace sylar {
	class Fiber : public std::enable_shared_from_this<Fiber>
	{
	public:
		enum State//定义协程的状态，属于协程的上下文切换需要被保存
		{
			READY,
			RUNNING,
			TERM
		};
	private:
		Fiber();//私有函数，只能被GetThis调用，创建主协程
	public:
		//创建指定回调函数，栈大小和run_in_scheduler的本协程是否参与调度器调度
		Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
		~Fiber();
	public:
		void reset(std::function<void()> cb);//重置协程状态和入口函数，复用栈控件==空间，不重新创建栈
		void resume();//恢复协程执行
		void yield();//让出当前协程的执行权
		uint64_t get_Id() const { return m_id; }//获取唯一标识
		State get_state() const { return m_state; }//获取协程状态
	public:

		static void SetThis(Fiber* f);//设置当前协程
		static std::shared_ptr<Fiber> GetThis();//获取当前协程的shared_ptr
		static void SetSchedulerFiber(Fiber* f);//设置调度协程，默认为主
		static uint64_t GetFiberId();//获取当前运行的协程id
		static void MainFunc();//协程主函数，入口点

	private:
		uint64_t m_id = 0;//唯一标识
		uint32_t m_stacksize = 0;//栈大小
		State m_state = READY;//协程状态
		ucontext_t m_ctx;//协程上下文
		void* m_stack = nullptr;//协程栈指针
		std::function<void()> m_cb;//协程入口函数
		bool m_run_in_scheduler;//是否将执行器交给调度函数
	public:
		std::mutex m_mutex;
	};
}

#endif