#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include<memory>//����ָ��
#include<vector>
#include<set>
#include<shared_mutex>
#include<assert.h>
#include<functional>
#include<mutex>

namespace sylar {
	class TimerManager;//��ʱ��������
	//�̳е�public��������������ָ��timer��thisֵ
	class Timer :public std::enable_shared_from_this<Timer>
	{
		friend class TimerManager;//���ó���Ԫ
	public:
		//��ʱ���ɾ��timer
		bool cancel();
		//ˢ��timer#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include<memory>//智能指针
#include<vector>
#include<set>
#include<shared_mutex>
#include<assert.h>
#include<functional>
#include<mutex>

namespace sylar {
	class TimerManager;//定时器管理类
	//继承的public是用来返回智能指针timer的this值
	class Timer :public std::enable_shared_from_this<Timer>
	{
		friend class TimerManager;//设置成友元
	public:
		//从时间堆删除timer
		bool cancel();
		//刷新timer
		bool refresh();
		//重设timer超时时间
		//1执行间隔ms，2是否从当前时间开始计算
		bool reset(uint64_t ms, bool from_now);
	private:
		Timer(uint64_t ms,std::function<void()>cb, bool recurring,TimerManager* manager);
	    //是否循环
		bool m_recurring = false;
		//超时时间
		uint64_t m_ms = 0;
		//绝对超时时间，即该定时器下次触发时间点
		std::chrono::time_point<std::chrono::system_clock> m_next;
		//超时触发回调函数
		std::function<void()>m_cb;
		//管理此timer管理器
		TimerManager* m_manager = nullptr;
	    //实现最小堆的比较函数，比较两个Timer，依据绝对超时时间
		struct Comparator
		{
			bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
		};
	};
	class TimerManager
	{
		friend class Timer;
	public:
		TimerManager();//构造函数
		virtual ~TimerManager();

		//添加timer
		//ms定时器执行间隔时间
		//cb定时器执行回调函数
		//recurring是否循环
        std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
	    

		//添加条件timer
		//weak_cond
		std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

		//拿到堆最近超时时间
		uint64_t getNextTimer();

		//取出所有超时定时器的回调函数
		void listExpiredCb(std::vector<std::function<void()>>& cbs);

		//堆中是否有定时器timer
		bool hasTimer();

	protected:
		//当一个最早的timer加入堆中，调用它
		virtual void onTimerInsertedAtFront() {};

		//添加timer
		void addTimer(std::shared_ptr<Timer> timer);

	private:
		//当系统时间改变
		bool detectClockRollover();
		std::shared_mutex m_mutex;
		//时间堆,存储所有的 Timer 对象，
		// 并使用 Timer::Comparator 进行排序，确保最早超时的 Timer 在最前面。
		std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

		//在下次获取最近超时时间前检查onTimerInsertedAtFront是否被触发-》在此过程中 onTimerInsertedAtFront()只执行一次。防止重复调用
	  bool m_tickled = false;
	  //上次检查时间是否回退的绝对时间
	  std::chrono::time_point<std::chrono::system_clock> m_previousTime;
	
	};
}
#endif
		bool refresh();
		//����timer��ʱʱ��
		//1ִ�м��ms��2�Ƿ�ӵ�ǰʱ�俪ʼ����
		bool reset(uint64_t ms, bool from_now);
	private:
		Timer(uint64_t ms,std::function<void()>cb, bool recurring,TimerManager* manager);
	    //�Ƿ�ѭ��
		bool m_recurring = false;
		//��ʱʱ��
		uint64_t m_ms = 0;
		//���Գ�ʱʱ�䣬���ö�ʱ���´δ���ʱ���
		std::chrono::time_point<std::chrono::system_clock> m_next;
		//��ʱ�����ص�����
		std::function<void()>m_cb;
		//������timer������
		TimerManager* m_manager = nullptr;
	    //ʵ����С�ѵıȽϺ������Ƚ�����Timer�����ݾ��Գ�ʱʱ��
		struct Comparator
		{
			bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
		};
	};
	class TimerManager
	{
		friend class Timer;
	public:
		TimerManager();//���캯��
		virtual ~TimerManager();

		//����timer
		//ms��ʱ��ִ�м��ʱ��
		//cb��ʱ��ִ�лص�����
		//recurring�Ƿ�ѭ��
        std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
	    

		//��������timer
		//weak_cond
		std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

		//�õ��������ʱʱ��
		uint64_t getNextTimer();

		//ȡ�����г�ʱ��ʱ���Ļص�����
		void listExpiredCb(std::vector<std::function<void()>>& cbs);

		//�����Ƿ��ж�ʱ��timer
		bool hasTimer();

	protected:
		//��һ�������timer������У�������
		virtual void onTimerInsertedAtFront() {};

		//����timer
		void addTimer(std::shared_ptr<Timer> timer);

	private:
		//��ϵͳʱ��ı�
		bool detectClockRollover();
		std::shared_mutex m_mutex;
		//ʱ���,�洢���е� Timer ����
		// ��ʹ�� Timer::Comparator ��������ȷ�����糬ʱ�� Timer ����ǰ�档
		std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

		//���´λ�ȡ�����ʱʱ��ǰ���onTimerInsertedAtFront�Ƿ񱻴���-���ڴ˹����� onTimerInsertedAtFront()ִֻ��һ�Ρ���ֹ�ظ�����
	  bool m_tickled = false;
	  //�ϴμ��ʱ���Ƿ���˵ľ���ʱ��
	  std::chrono::time_point<std::chrono::system_clock> m_previousTime;
	
	};
}
#endif