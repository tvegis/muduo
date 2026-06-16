#pragma once

#include <cstdint>

class Timer;

/**
 * TimerId —— 定时器标识符，用于取消定时器
 *
 * 持有 Timer* 和 sequence 双重校验：
 *  - Timer* 指向要取消的定时器对象
 *  - sequence 是创建时的序列号，防止 Timer 被重用后误取消
 */
class TimerId
{
public:
    TimerId()
        : timer_(nullptr)
        , sequence_(0)
    {
    }

    TimerId(Timer *timer, int64_t seq)
        : timer_(timer)
        , sequence_(seq)
    {
    }

    Timer *timer() const { return timer_; }
    int64_t sequence() const { return sequence_; }

private:
    Timer *timer_;
    int64_t sequence_;
};
