#pragma once
#include <string>
#include <memory>

// Uses the normal daily reward endpoint while the player is running.
class COnlineDailyRewards
{
public:
    static COnlineDailyRewards& Instance();
    void Configure(const std::wstring& config_dir);
    void Tick();
    void Request(bool automatic = false);
    void SetEnabled(bool enabled);
    bool Enabled() const;
    std::wstring Describe() const;
    void CancelPending();
    void Shutdown();
private:
    struct State;
    std::shared_ptr<State> m_state;
    ULONGLONG m_next_check{};
};
