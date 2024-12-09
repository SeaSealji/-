#include "Timer.h"

namespace sylar {
    bool Timer::cancel()
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager -> m_mutex);//写锁互斥锁

        if (m_cb == nullptr)//这里就是将回调函数如果存在设置为nullptr
        {
            return false;
        }
        else
        {
            m_cb= nullptr;
        }

        auto it = m_manager->m_timers.find(shared_from_this());//从定时管理器中找到需要删除的定时器
        if (it != m_manager->m_timers.end())
        {
            m_manager->m_timers.erase(it);//删除定时器
        }
        return true;
    }

    //refresh 只会向后调整
    bool Timer::refresh()
    {
        std::unique_lock<std::shared_mutex>write_lock(m_manager->m_mutex);

        if (!m_cb)
        {
            return false;
        }

        auto it = m_manager->m_timers.find(shared_from_this());;//从定时管理器中找到当前定时器
        if (it == m_manager->m_timers.end())//检查定时器是否存在
        {
            return false;
        }

        //删除定时器更新超时时间
        m_manager->m_timers.erase(it);
        m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
        m_manager->m_timers.insert(shared_from_this());//将新的定时器插入到定时管理器中
        return true;
    }
    bool Timer::reset(uint64_t ms, bool from_now)
    {
        if (ms == m_ms && !from_now)
        {
            return true;//无需重置
        }
        //不满足则需要重置，删除定时器重新计算超时时间插入定时器
        {
            std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

            if (!m_cb)//为空说明定时器已被取消或未初始化，无法重置
            {
                return false;
            }
            
            auto it = m_manager->m_timers.find(shared_from_this());
            if (it == m_manager->m_timers.end())
            {
                return false;;
            }
            m_manager->m_timers.erase(it);//删除定时器
        }

        auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);//如果为true则重新计算超时时间，为false就需要上一次的起点开始
        m_ms = ms;
        m_next = start + std::chrono::milliseconds(m_ms);
        m_manager->addTimer(shared_from_this());
        return true;
    }
    Timer::Timer(uint64_t ms, std::function<void()>cb, bool recurring, TimerManager* manager)
        :m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager)
    {
        auto now = std::chrono::system_clock::now();
        m_next = now + std::chrono::milliseconds(m_ms);//下一次超时
    }

    bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs)const
    {
        assert(lhs != nullptr && rhs != nullptr);
        return lhs->m_next < rhs->m_next;
    }
    TimerManager::TimerManager()
    {
        m_previousTime = std::chrono::system_clock::now();//初始化系统时间
    }
    TimerManager::~TimerManager()
    {

    }
    std::shared_ptr<Timer>TimerManager::addTimer(uint64_t ms, std::function<void()>cb, bool recurring)
    {
        std::shared_ptr<Timer>timer(new Timer(ms, cb, recurring, this));
        addTimer(timer);
        return timer;
    }

    void TimerManager::addTimer(std::shared_ptr<Timer>timer)
    {
        bool at_front = false;//表示插入的是最早超时的定时器
        {
            std::unique_lock<std::shared_mutex>write_lock(m_mutex);
            auto it = m_timers.insert(timer).first;
            at_front = (it == m_timers.begin()) && !m_tickled;;//判断插入的定时器是否是集合超时时间中最早的定时器
            if (at_front)//有一个新的最早定时器被插入
            {
                m_tickled = true;
            }
        }
        if (at_front)
        {
            onTimerInsertedAtFront();//虚函数具体执行在io
        }
    }

    //如果条件存在->执行cb()
    static void onTimer(std::weak_ptr<void>weak_cond, std::function<void()>cb)
    {
        std::shared_ptr<void>tmp = weak_cond.lock();//确保对象依然存在
        if (tmp)
        {
            cb();
        }
    }
    std::shared_ptr<Timer>TimerManager::addConditionTimer(uint64_t ms, std::function<void()>cb, std::weak_ptr<void>weak_cond, bool recurring)
    {
        return addTimer(ms, std::bind(&onTimer, weak_cond, cb), recurring);//将OnTImer的真正指向交给了第一个addtimer
                                                                             //然后创建timer对象。
    }
    uint64_t TimerManager::getNextTimer()
    {
        std::shared_lock<std::shared_mutex>read_lock(m_mutex);//读锁

        m_tickled = false;
        if (m_timers.empty())
        {
            return ~0ull;//最大值
        }

        auto now = std::chrono::system_clock::now();
        auto time = (*m_timers.begin())->m_next;

        if (now > time)
        {
            return 0;
        }
        else
        {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
            return static_cast<uint64_t>(duration.count());
        }
    }
    void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs)
    {
        auto now = std::chrono::system_clock::now();
        std::unique_lock<std::shared_mutex>write_lock(m_mutex);

        bool rollover = detectClockRollover();//判断系统时间是否出错
        //回退 ->清理所有timer ||超时 清洗超时timer 如果 rolover false就没发生系统时间回退
        //如果时间回滚发生或者定时器的超时时间早于或等于当前时间，则需要处理这些定时器。
        // 为什么说早于或等于都要处理，因为超时时间都是基于now后的
        while (!m_timers.empty() && (rollover || (*m_timers.begin())->m_next <= now))
        {
            std::shared_ptr<Timer>temp = *m_timers.begin();
            m_timers.erase(m_timers.begin());

            cbs.push_back(temp->m_cb);
            //如果定时器循环，m_next设置为当前时间加上定时器间隔
            if (temp->m_recurring)
            {
                temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
                m_timers.insert(temp);
            }
            else
            {
                temp->m_cb = nullptr;
            }
        }
    }
    bool TimerManager::detectClockRollover()
    {
        bool rollover = false;

        //比较当前now与上次m_oreviousTime减去一个小时时间量(60*60*1000)
        //如果小于则说明系统时间回退了
        auto now = std::chrono::system_clock::now();
        if (now < m_previousTime - std::chrono::milliseconds(60 * 60 * 1000))
        {
            rollover = true;
        }
        m_previousTime = now;
        return rollover;

    }

    bool TimerManager::hasTimer()
    {
        std::shared_lock<std::shared_mutex>read_lock(m_mutex);
        return !m_timers.empty();
    }
}