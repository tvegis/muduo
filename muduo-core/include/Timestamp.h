#pragma once

#include <string>
#include <cstdint>

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