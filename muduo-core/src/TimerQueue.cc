#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <algorithm>

#include "TimerQueue.h"
#include "Timer.h"
#include "EventLoop.h"
#include "Channel.h"
#include "Logger.h"

// 创建 timerfd，返回文件描述符
static int createTimerFd()
{
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                   TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0)
    {
        LOG_FATAL("timerfd_create error:%d\n", errno);
    }
    return timerfd;
}

// 读取 timerfd 的到期计数（8 字节），不清除计数的话 epoll 会持续触发
static void readTimerFd(int timerfd)
{
    uint64_t howmany;
    ssize_t n = ::read(timerfd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany))
    {
        LOG_ERROR("TimerQueue::handleRead() reads %ld bytes instead of 8\n", n);
    }
}

// 设置 timerfd 的下一次到期时间
static void resetTimerFd(int timerfd, Timestamp expiration)
{
    struct itimerspec newValue;
    struct itimerspec oldValue;
    ::memset(&newValue, 0, sizeof(newValue));
    ::memset(&oldValue, 0, sizeof(oldValue));

    int64_t microSeconds = expiration.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
    if (microSeconds < 100)
    {
        microSeconds = 100; // 最小 100 微秒，避免过于频繁的 epoll 唤醒
    }

    // 转换为 timespec 结构
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(microSeconds / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>((microSeconds % Timestamp::kMicroSecondsPerSecond) * 1000);
    newValue.it_value = ts;

    if (::timerfd_settime(timerfd, 0, &newValue, &oldValue) < 0)
    {
        LOG_ERROR("timerfd_settime error:%d\n", errno);
    }
}

TimerQueue::TimerQueue(EventLoop *loop)
    : loop_(loop)
    , timerfd_(createTimerFd())
    , timerfdReadCount_(0)
    , timers_()
    , callingExpiredTimers_(false)
{
    Channel *channel = new Channel(loop_, timerfd_);
    channel->setReadCallback(std::bind(&TimerQueue::handleRead, this));
    channel->enableReading();
    // Channel 由 loop 接管生命周期
    // 此处不保存 channel 指针，因为 TimerQueue 生命周期与 EventLoop 相同
}

TimerQueue::~TimerQueue()
{
    // 删除所有定时器
    for (const auto &entry : timers_)
    {
        delete entry.second;
    }
    ::close(timerfd_);
}

// 添加定时器 —— 线程安全，可以在其它线程调用
TimerId TimerQueue::addTimer(TimerCallback cb, Timestamp expiration, double interval)
{
    Timer *timer = new Timer(std::move(cb), expiration, interval);
    loop_->runInLoop(std::bind(&TimerQueue::addTimerInLoop, this, timer));
    return TimerId(timer, timer->sequence());
}

// 取消定时器 —— 线程安全
void TimerQueue::cancel(TimerId timerId)
{
    loop_->runInLoop(std::bind(&TimerQueue::cancelInLoop, this, timerId));
}

// 在 EventLoop 线程中执行的添加操作
void TimerQueue::addTimerInLoop(Timer *timer)
{
    bool earliestChanged = insert(timer);

    if (earliestChanged)
    {
        resetTimerFd(timerfd_, timer->expiration());
    }
}

// 在 EventLoop 线程中执行的取消操作
void TimerQueue::cancelInLoop(TimerId timerId)
{
    auto it = timers_.find(Entry(timerId.timer()->expiration(), timerId.timer()));
    if (it != timers_.end() && it->second->sequence() == timerId.sequence())
    {
        timers_.erase(it);
        delete timerId.timer();
    }
}

// timerfd 可读事件回调
void TimerQueue::handleRead()
{
    Timestamp now(Timestamp::now());
    readTimerFd(timerfd_);

    // 获取到期定时器列表
    std::vector<Entry> expired = getExpired(now);

    callingExpiredTimers_ = true;
    canceledTimers_.clear();

    // 逐个执行回调
    for (const Entry &entry : expired)
    {
        entry.second->run();
    }

    callingExpiredTimers_ = false;

    // 重置周期性定时器
    reset(expired, now);
}

// 获取所有到期定时器（expiration <= now）
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
    std::vector<Entry> expired;
    Entry sentry(now, reinterpret_cast<Timer *>(UINTPTR_MAX)); // 哨兵

    auto end = timers_.lower_bound(sentry);
    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);

    return expired;
}

// 重置到期定时器（周期性定时器重新加入，非周期性定时器删除）
void TimerQueue::reset(const std::vector<Entry> &expired, Timestamp now)
{
    Timestamp nextExpire;

    for (const Entry &entry : expired)
    {
        bool repeat = entry.second->repeat();
        // 如果是周期性定时器且不在取消列表中，重新加入
        if (repeat)
        {
            entry.second->restart(now);
            insert(entry.second);
        }
        else
        {
            delete entry.second; // 一次性定时器，释放资源
        }
    }

    // 设置 timerfd 的下一次到期时间
    if (!timers_.empty())
    {
        nextExpire = timers_.begin()->second->expiration();
    }

    if (nextExpire.valid())
    {
        resetTimerFd(timerfd_, nextExpire);
    }
}

// 向 timers_ 中插入定时器。如果该定时器是最早到期的，返回 true
bool TimerQueue::insert(Timer *timer)
{
    bool earliestChanged = false;
    Timestamp expiration = timer->expiration();

    auto it = timers_.begin();
    if (it == timers_.end() || expiration < it->first)
    {
        earliestChanged = true;
    }

    timers_.insert(Entry(expiration, timer));
    return earliestChanged;
}
