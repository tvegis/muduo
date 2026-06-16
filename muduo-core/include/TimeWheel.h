#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <list>

#include "noncopyable.h"
#include "TimerId.h"

class EventLoop;

/**
 * TimeWheel —— 简单时间轮，O(1) 插删，用于海量空闲连接超时
 *
 * 设计要点：
 *  - 固定 N 个槽（默认 60），每个槽为 1 秒 tick
 *  - 每 tick 推进一个槽，槽内所有 entry 到期执行回调
 *  - Entry 使用 shared_ptr/weak_ptr 生命周期管理
 *  - 用户通过 WeakEntryPtr 可以 refresh（续期）或 cancel（取消）
 *
 * 与 TimerQueue 的关系：
 *  - TimerQueue：高精度（timerfd），少量定时任务
 *  - TimeWheel：低精度（1秒 tick），海量空闲超时
 *
 * 使用场景：
 *  检测空闲连接超时：每次收到数据 refresh(entryId) 续期，
 *  到期后 TimeWheel 回调关闭连接。
 */
class TimeWheel : noncopyable
{
public:
    using TimeoutCallback = std::function<void()>;

    // Entry 是内部数据结构，外部通过 WeakEntryPtr 访问
    struct Entry
    {
        TimeoutCallback callback;   // 到期回调（设为 null 表示已取消）
        int rotation;               // 还要转几圈才到期
        int slot;                   // 当前所在槽索引
    };

    using EntryPtr = std::shared_ptr<Entry>;
    using WeakEntryPtr = std::weak_ptr<Entry>;

    TimeWheel(EventLoop *loop,
              int numSlots = 60,
              double tickInterval = 1.0);
    ~TimeWheel();

    // 添加一个超时 entry，timeout 为秒数
    // 返回 WeakEntryPtr，外部持有它后可续期或取消
    WeakEntryPtr addEntry(TimeoutCallback cb, int timeout);

    // 续期：将 entry 移到 timeout 秒后的新槽（线程安全）
    void refreshEntry(WeakEntryPtr weakEntry, int timeout);

    // 取消：清空 entry 的回调（线程安全，不需要 TimeWheel 实例）
    static void cancelEntry(WeakEntryPtr weakEntry);

    // 启动时间轮（通过 EventLoop::runEvery 注册 tick 定时器）
    void start();

private:
    using Bucket = std::list<EntryPtr>;

    void addEntryInLoop(EntryPtr entry, int timeout);
    void refreshEntryInLoop(const WeakEntryPtr &weakEntry, int timeout);

    // tick 触发：推进一个槽，执行到期回调
    void onTick();

    EventLoop *loop_;
    const int numSlots_;
    const double tickInterval_;
    int currentSlot_;
    bool started_;

    std::vector<Bucket> wheel_;
    TimerId tickTimerId_;
};
