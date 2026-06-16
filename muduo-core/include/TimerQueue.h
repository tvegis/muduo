#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <set>
#include <utility>

#include "noncopyable.h"
#include "Timestamp.h"
#include "TimerId.h"

class EventLoop;
class Timer;

/**
 * TimerQueue —— 基于 timerfd 的精确定时器队列
 *
 * 设计要点：
 *  - 每个 EventLoop 持有一个 TimerQueue
 *  - 使用 timerfd_create/timerfd_settime 将定时器 fd 注册到 Poller
 *  - 内部按过期时间排序（std::set），最早到期的定时器决定 timerfd 的触发时间
 *  - 支持一次性定时器和周期性定时器
 *  - 取消定时器采用惰性删除（标记 expired_ 位，到期时跳过）
 */
class TimerQueue : noncopyable
{
public:
    using TimerCallback = std::function<void()>;

    explicit TimerQueue(EventLoop *loop);
    ~TimerQueue();

    // 添加定时器
    // 线程安全：可以从其它线程调用
    TimerId addTimer(TimerCallback cb, Timestamp expiration, double interval);

    // 取消定时器
    void cancel(TimerId timerId);

private:
    using Entry = std::pair<Timestamp, Timer *>;

    // 添加定时器（在 EventLoop 线程中执行）
    void addTimerInLoop(Timer *timer);
    // 取消定时器（在 EventLoop 线程中执行）
    void cancelInLoop(TimerId timerId);

    // timerfd 读事件回调
    void handleRead();

    // 获取所有已到期的定时器
    std::vector<Entry> getExpired(Timestamp now);

    // 重置到期定时器（将重复定时器重新加入队列）
    void reset(const std::vector<Entry> &expired, Timestamp now);

    // 插入定时器到 set 中
    bool insert(Timer *timer);

    EventLoop *loop_;
    const int timerfd_;
    int64_t timerfdReadCount_;   // timerfd 触发次数统计

    // 按过期时间排序的定时器集合
    // 使用 pair<Timestamp, Timer*> 确保时间相同的定时器可以共存
    std::set<Entry> timers_;

    // 当前正在被调用的到期定时器
    // 用于 reset() 判断定时器是否来自本轮到期
    bool callingExpiredTimers_;
    std::set<Entry> canceledTimers_;  // 在回调中被取消的定时器
};
