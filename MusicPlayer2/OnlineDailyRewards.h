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

// 波点的「看广告领会员」：上报一次广告换 30 分钟会员畅听，服务端每天限量。
// 与酷狗「一天领一次」不同，它按时长续期，所以在播放器运行期间按剩余时间自动补领。
class CBodianAdRewards
{
public:
    static CBodianAdRewards& Instance();
    void Configure(const std::wstring& config_dir);
    void Tick();
    void Request(bool automatic = false);
    void SetEnabled(bool enabled);
    bool Enabled() const;
    std::wstring Describe() const;
    void Shutdown();
private:
    struct State;
    std::shared_ptr<State> m_state;
    ULONGLONG m_next_check{};
    std::wstring m_settings_path;
};
