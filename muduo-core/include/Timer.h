#pragma once

#include <functional>
#include <atomic>

#include "Timestamp.h"

class Timer
{
public:
    using TimerCallback = std::function<void()>;

    Timer(TimerCallback cb, Timestamp expiration, double interval)
        : callback_(std::move(cb))
        , expiration_(expiration)
        , interval_(interval)
        , sequence_(s_numCreated_++)
        , repeat_(interval > 0.0)
    {
    }

    void run() const
    {
        if (callback_)
        {
            callback_();
        }
    }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    double interval() const { return interval_; }
    int64_t sequence() const { return sequence_; }

    // 重启定时器（用于周期性定时器）：将过期时间设为 now + interval
    void restart(Timestamp now);

private:
    TimerCallback callback_;   // 定时器回调函数
    Timestamp expiration_;     // 过期时间（绝对时间）
    const double interval_;    // 间隔（秒），<= 0 表示一次性
    const int64_t sequence_;   // 序列号，用于排序和取消
    const bool repeat_;        // 是否重复

    static std::atomic_int64_t s_numCreated_;
};
