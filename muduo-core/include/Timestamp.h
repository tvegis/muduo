#pragma once

#include <string>
#include <cstdint>

// 时间偏移工具：返回 timestamp + seconds（秒可为小数，如 0.5 表示 500ms）
inline Timestamp addTime(Timestamp timestamp, double seconds)
{
    int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
    return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

class Timestamp
{
public:
    static const int kMicroSecondsPerSecond = 1000 * 1000;

    Timestamp();
    explicit Timestamp(int64_t microSecondsSinceEpoch);
    static Timestamp now();
    static Timestamp invalid() { return Timestamp(); }

    std::string toString() const;

    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
    bool valid() const { return microSecondsSinceEpoch_ > 0; }

    bool operator<(const Timestamp& rhs) const { return microSecondsSinceEpoch_ < rhs.microSecondsSinceEpoch_; }
    bool operator==(const Timestamp& rhs) const { return microSecondsSinceEpoch_ == rhs.microSecondsSinceEpoch_; }

private:
    int64_t microSecondsSinceEpoch_;
};