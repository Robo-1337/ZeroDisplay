#pragma once
#include "core/Common.h"

#include <mutex>
#include <string>
#include <vector>

namespace cc::net {

// The sharing PIN, checked for both the ZeroDisplay stream (UDP) and the browser viewer (WebSocket). Wrong guesses
// are limited: 5 different wrong PINs per address within a minute lock that address for a minute, and 30 wrong ones
// a minute from anywhere make everyone wait, so trying all 1,000,000 PINs would take weeks. The same wrong PIN sent
// again (a viewer retrying) counts once.
class PinGate {
public:
    enum class Result { Ok, Needed, Wrong, Wait };
    static constexpr size_t kLength = 6;

    void Set(bool required, const std::string& pin);
    bool Required() const;
    std::string Pin() const;
    // `ipv4` in network byte order; `pin` as typed (may be empty or anything).
    Result Check(uint32_t ipv4, std::string_view pin);

    static std::string NewPin();   // 6 random digits (BCryptGenRandom, no modulo bias)
    static bool Valid(std::string_view pin);
    static bool SelfTest();        // the limits above, on a gate with a test clock (--pin-test)

    int64_t testNowMs = -1;        // testing: a fake clock in ms (>= 0)

private:
    struct Tries {
        uint32_t ip = 0;
        int count = 0;
        int64_t windowStartMs = 0, lockUntilMs = 0;
        std::string lastWrong;
    };
    int64_t NowMs() const;

    mutable std::mutex m_mutex;
    bool m_required = true;
    std::string m_pin;
    std::vector<Tries> m_tries;   // at most kMaxTracked addresses (the oldest is forgotten)
    int m_globalWrong = 0;
    int64_t m_globalStartMs = 0;
    static constexpr size_t kMaxTracked = 256;
};

} // namespace cc::net
