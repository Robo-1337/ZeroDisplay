#include "net/PinGate.h"
#include "core/Log.h"

#include <bcrypt.h>

#include <algorithm>

namespace cc::net {

namespace {
constexpr int kPerAddress = 5;            // different wrong PINs per address and minute
constexpr int kGlobal = 30;               // wrong PINs per minute from anywhere
constexpr int64_t kWindowMs = 60000;
constexpr int64_t kLockMs = 60000;

bool SameText(std::string_view a, std::string_view b)   // time doesn't depend on where they differ
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= uint8_t(a[i]) ^ uint8_t(b[i]);
    return diff == 0;
}
} // namespace

int64_t PinGate::NowMs() const { return testNowMs >= 0 ? testNowMs : int64_t(QpcToMs(QpcNow())); }

void PinGate::Set(bool required, const std::string& pin)
{
    std::lock_guard lock(m_mutex);
    m_required = required;
    m_pin = pin;
}

bool PinGate::Required() const
{
    std::lock_guard lock(m_mutex);
    return m_required;
}

std::string PinGate::Pin() const
{
    std::lock_guard lock(m_mutex);
    return m_pin;
}

bool PinGate::Valid(std::string_view pin)
{
    return pin.size() == kLength && std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string PinGate::NewPin()
{
    std::string pin;
    while (pin.size() < kLength) {
        uint8_t rnd[16] = {};
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            // Should never happen; still never a fixed PIN.
            uint64_t x = uint64_t(QpcNow()) ^ (uint64_t(GetTickCount64()) << 21) ^ uint64_t(GetCurrentProcessId());
            for (auto& b : rnd) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                b = uint8_t(x);
            }
        }
        for (uint8_t b : rnd)
            if (b < 250 && pin.size() < kLength) pin += char('0' + b % 10);   // 250 = 25 x 10: every digit equally likely
    }
    return pin;
}

PinGate::Result PinGate::Check(uint32_t ipv4, std::string_view pin)
{
    std::lock_guard lock(m_mutex);
    if (!m_required) return Result::Ok;
    const int64_t now = NowMs();
    auto it = std::find_if(m_tries.begin(), m_tries.end(), [&](const Tries& t) { return t.ip == ipv4; });
    if (it != m_tries.end() && now < it->lockUntilMs) return Result::Wait;
    if (pin.empty()) return Result::Needed;
    if (now - m_globalStartMs >= kWindowMs) {
        m_globalStartMs = now;
        m_globalWrong = 0;
    }
    if (m_globalWrong >= kGlobal) return Result::Wait;   // even the right PIN waits: a guesser can't tell
    if (Valid(m_pin) && SameText(pin, m_pin)) {
        if (it != m_tries.end()) m_tries.erase(it);
        return Result::Ok;
    }
    if (it == m_tries.end()) {
        if (m_tries.size() >= kMaxTracked) m_tries.erase(m_tries.begin());
        m_tries.push_back({ipv4, 0, now, 0, {}});
        it = m_tries.end() - 1;
    }
    if (now - it->windowStartMs >= kWindowMs) {
        it->windowStartMs = now;
        it->count = 0;
        it->lastWrong.clear();
    }
    if (pin == it->lastWrong) return Result::Wrong;   // the same wrong PIN again: counted once
    it->lastWrong.assign(pin.substr(0, 16));
    ++m_globalWrong;
    if (++it->count >= kPerAddress) {
        it->lockUntilMs = now + kLockMs;
        it->count = 0;
        it->lastWrong.clear();
        LOGW(L"Sharing: too many wrong PINs from one address, it waits a minute");
    }
    return Result::Wrong;
}

bool PinGate::SelfTest()
{
    bool ok = true;
    auto expect = [&](bool c, const wchar_t* what) {
        if (!c) {
            ok = false;
            LOGE(L"[pin-test] FAIL: %s", what);
        }
    };
    for (int i = 0; i < 200; ++i) expect(Valid(NewPin()), L"new PINs have 6 digits");
    PinGate g;
    g.testNowMs = 1000;
    g.Set(true, "123456");
    const uint32_t a = 0x0100000A, b = 0x0200000A;   // 10.0.0.1, 10.0.0.2
    expect(g.Check(a, "") == Result::Needed, L"no PIN: needed");
    expect(g.Check(a, "123456") == Result::Ok, L"right PIN: ok");
    expect(g.Check(a, "000000") == Result::Wrong, L"wrong PIN: wrong");
    for (int i = 0; i < 10; ++i) expect(g.Check(a, "000000") == Result::Wrong, L"the same wrong PIN again: still only wrong");
    expect(g.Check(a, "123456") == Result::Ok, L"right PIN after a repeated wrong one");
    for (int i = 1; i <= 4; ++i) expect(g.Check(a, std::to_string(100000 + i)) == Result::Wrong, L"4 different wrong PINs");
    expect(g.Check(a, "200000") == Result::Wrong, L"the 5th wrong PIN locks the address");
    expect(g.Check(a, "123456") == Result::Wait, L"locked: even the right PIN waits");
    expect(g.Check(b, "123456") == Result::Ok, L"another address is not locked");
    g.testNowMs += kLockMs + 1;
    expect(g.Check(a, "123456") == Result::Ok, L"after a minute the address may try again");
    // 30 wrong PINs a minute from many addresses: everyone waits until the minute is over.
    for (int i = 0; i < kGlobal; ++i) g.Check(0x0A000000u + uint32_t(i), std::to_string(300000 + i));
    expect(g.Check(b, "123456") == Result::Wait, L"global limit: everyone waits");
    g.testNowMs += kWindowMs + 1;
    expect(g.Check(b, "123456") == Result::Ok, L"global limit over after a minute");
    g.Set(false, "123456");
    expect(g.Check(a, "") == Result::Ok, L"PIN off: anyone");
    g.Set(true, "");
    expect(g.Check(a, "") == Result::Needed && g.Check(a, "x") == Result::Wrong, L"no valid PIN set: nothing matches");
    LOGI(L"[pin-test] %s", ok ? L"PASS" : L"FAIL");
    return ok;
}

} // namespace cc::net
