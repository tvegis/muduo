#include "TimeWheel.h"
#include "EventLoop.h"
#include "Logger.h"

TimeWheel::TimeWheel(EventLoop *loop, int numSlots, double tickInterval)
    : loop_(loop)
    , numSlots_(numSlots)
    , tickInterval_(tickInterval)
    , currentSlot_(0)
    , started_(false)
    , wheel_(numSlots)
{
}

TimeWheel::~TimeWheel()
{
    if (started_)
    {
        loop_->cancel(tickTimerId_);
    }
}

// 添加超时 entry —— 线程安全
TimeWheel::WeakEntryPtr TimeWheel::addEntry(TimeoutCallback cb, int timeout)
{
    EntryPtr entry(new Entry);
    entry->callback = std::move(cb);
    entry->rotation = 0;  // addEntryInLoop 会计算

    WeakEntryPtr weakEntry(entry);
    loop_->runInLoop(
        std::bind(&TimeWheel::addEntryInLoop, this, entry, timeout));
    return weakEntry;
}

// 续期 —— 线程安全
void TimeWheel::refreshEntry(WeakEntryPtr weakEntry, int timeout)
{
    loop_->runInLoop(
        std::bind(&TimeWheel::refreshEntryInLoop, this, weakEntry, timeout));
}

// 取消 —— 静态方法，线程安全
void TimeWheel::cancelEntry(WeakEntryPtr weakEntry)
{
    EntryPtr entry = weakEntry.lock();
    if (entry)
    {
        entry->callback = nullptr;
    }
}

// 启动时间轮
void TimeWheel::start()
{
    if (!started_)
    {
        started_ = true;
        tickTimerId_ = loop_->runEvery(tickInterval_,
            std::bind(&TimeWheel::onTick, this));
    }
}

// ==================== 内部方法 ====================

void TimeWheel::addEntryInLoop(EntryPtr entry, int timeout)
{
    // 计算槽位：timeout 个 tick 后的位置
    int totalSlots = (currentSlot_ + timeout) % numSlots_;
    int slot = totalSlots % numSlots_;
    entry->rotation = timeout / numSlots_;  // 还要转几圈

    // 如果 timeout=0, 放到下一个槽，1 tick 后执行
    if (timeout == 0)
    {
        slot = (currentSlot_ + 1) % numSlots_;
        entry->rotation = 0;
    }

    entry->slot = slot;
    wheel_[slot].push_back(entry);
}

void TimeWheel::refreshEntryInLoop(const WeakEntryPtr &weakEntry, int timeout)
{
    EntryPtr entry = weakEntry.lock();
    if (!entry)
    {
        return;  // entry 已过期
    }

    // 创建新 entry 继承原回调
    EntryPtr newEntry(new Entry);
    newEntry->callback = entry->callback;
    // 原 entry 回调置空（到期后自动跳过）
    entry->callback = nullptr;

    // 将新 entry 加入 timeout 后的槽
    int slot = (currentSlot_ + timeout) % numSlots_;
    newEntry->rotation = timeout / numSlots_;
    newEntry->slot = slot;
    if (timeout == 0)
    {
        slot = (currentSlot_ + 1) % numSlots_;
        newEntry->slot = slot;
        newEntry->rotation = 0;
    }
    wheel_[slot].push_back(newEntry);
}

// tick 触发：推进一个槽，执行到期回调
void TimeWheel::onTick()
{
    // 推进到下一个槽
    currentSlot_ = (currentSlot_ + 1) % numSlots_;

    Bucket &bucket = wheel_[currentSlot_];
    if (bucket.empty())
    {
        return;
    }

    // 遍历当前槽的所有 entry
    for (auto it = bucket.begin(); it != bucket.end(); )
    {
        EntryPtr entry = *it;

        // 回调为空表示已被取消
        if (!entry->callback)
        {
            it = bucket.erase(it);
            continue;
        }

        if (entry->rotation > 0)
        {
            // 未到期，减少旋转次数
            entry->rotation--;
            ++it;
        }
        else
        {
            // 到期，执行回调后移除
            TimeoutCallback cb = std::move(entry->callback);
            it = bucket.erase(it);
            cb();  // 执行回调（回调中可能再次 addEntry）
        }
    }
}
