#include "stdafx.h"
#include "OnlineDailyRewards.h"
#include "KugouSource.h"
#include "KugouCrypto.h"
#include "OnlineJson.h"
#include <filesystem>
#include <thread>
#include <mutex>

using namespace std;
bool kugou::CKugouSource::GetDailyRewardRecord(wstring& day, bool& received)
{
    m_last_error.clear(); received = false; day.clear();
    if (!IsLoggedIn()) { m_last_error = L"请先登录酷狗概念版"; return false; }
    nlohmann::json response;
    if (!Request(L"/youth/v1/activity/get_month_vip_record", L"",
        {{"appid", "3116"}, {"clientver", "11440"}, {"mid", m_device.mid}, {"dfid", m_device.dfid},
        {"clienttime", to_string(time(nullptr))}, {"latest_limit", "100"}}, "", response)) return false;
    if (online::JsonNumber(response, "status") != 1 || !response.contains("data") || !response["data"].is_object()
        || !response["data"].contains("list") || !response["data"]["list"].is_array())
    { m_last_error = L"会员记录读取失败：" + FromUtf8(online::JsonText(response, "error_msg")); return false; }
    auto server_time = online::JsonText(response["data"], "server_time");
    if (server_time.size() != 10 || server_time.find_first_not_of("0123456789") != string::npos)
    { m_last_error = L"会员记录没有有效的服务端日期，未提交领取"; return false; }
    __time64_t seconds = stoll(server_time) + 8 * 3600;
    tm china_time{};
    if (_gmtime64_s(&china_time, &seconds) != 0) { m_last_error = L"会员记录日期无效"; return false; }
    wchar_t date[16]{}; wcsftime(date, _countof(date), L"%Y-%m-%d", &china_time); day = date;
    received = online::KugouRewardReceived(response["data"], ToUtf8(day)); return true;
}
bool kugou::CKugouSource::ClaimDailyReward(const wstring& day)
{
    if (!IsLoggedIn() || day.size() != 10 || day[4] != L'-' || day[7] != L'-'
        || day.find_first_not_of(L"0123456789-") != wstring::npos) { m_last_error = L"领取账号或日期无效"; return false; }
    nlohmann::json response;
    if (!Request(L"/youth/v1/recharge/receive_vip_listen_song", L"",
        {{"appid", "3116"}, {"clientver", "11440"}, {"mid", m_device.mid}, {"dfid", m_device.dfid},
        {"clienttime", to_string(time(nullptr))}, {"source_id", "90139"}, {"receive_day", ToUtf8(day)}}, "", response, true, L"POST")) return false;
    if (online::JsonNumber(response, "status") != 1)
    { m_last_error = L"领取未完成：" + FromUtf8(online::JsonText(response, "error_msg")); return false; }
    return true;
}
struct COnlineDailyRewards::State
{
    mutable mutex lock;
    wstring settings, status{L"尚未检查"};
    bool enabled{}, busy{}, stopping{};
    unsigned generation{};
};
COnlineDailyRewards& COnlineDailyRewards::Instance() { static COnlineDailyRewards rewards; return rewards; }
void COnlineDailyRewards::Configure(const wstring& config_dir)
{
    if (m_state) return;
    m_state = make_shared<State>(); m_state->settings = (filesystem::path(config_dir) / L"online_media.ini").wstring();
    m_state->enabled = GetPrivateProfileIntW(L"daily", L"enabled", 0, m_state->settings.c_str()) != 0;
}
bool COnlineDailyRewards::Enabled() const
{
    if (!m_state) return false;
    lock_guard<mutex> guard(m_state->lock); return m_state->enabled;
}
void COnlineDailyRewards::SetEnabled(bool enabled)
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock);
    if (WritePrivateProfileStringW(L"daily", L"enabled", enabled ? L"1" : L"0", m_state->settings.c_str()))
    { m_state->enabled = enabled; m_next_check = 0; if (!enabled) ++m_state->generation; }
    else m_state->status = L"自动签到设置保存失败";
}
void COnlineDailyRewards::Tick()
{
    if (!Enabled() || GetTickCount64() < m_next_check) return;
    m_next_check = GetTickCount64() + 60 * 60 * 1000;
    Request(true);
}
void COnlineDailyRewards::Request(bool automatic)
{
    if (!m_state) return;
    auto* kg = dynamic_cast<kugou::CKugouSource*>(online::CSourceRegistry::Instance().FindByScheme(L"kugou"));
    auto state = m_state;
    lock_guard<mutex> guard(state->lock);
    if (state->busy || state->stopping) return;
    if (!kg || !kg->IsLoggedIn()) { state->status = L"请先登录酷狗概念版"; return; }
    auto source = make_shared<kugou::CKugouSource>(*kg);
    const unsigned generation = state->generation;
    state->busy = true; state->status = L"正在检查当天会员领取记录…";
    try
    {
        thread([state, source, automatic, generation] {
            wstring status;
            try
            {
                wstring day; bool received{};
                if (!source->GetDailyRewardRecord(day, received)) status = source->GetLastError();
                else if (received) status = day + L" · 当天会员已领取";
                else
                {
                    const auto account_key = kugou::FromUtf8(kugou::Md5Hex(source->GetAccount().userid));
                    wchar_t attempted[32]{};
                    GetPrivateProfileStringW(L"daily_attempt", account_key.c_str(), L"", attempted, _countof(attempted), state->settings.c_str());
                    if (day == attempted) status = day + L" · 已尝试领取，尚未确认到账；不会重复提交";
                    else
                    {
                        bool may_claim;
                        { lock_guard<mutex> task_guard(state->lock); may_claim = !state->stopping && generation == state->generation && (!automatic || state->enabled); }
                        if (!may_claim) status = L"领取已取消";
                        else if (!WritePrivateProfileStringW(L"daily_attempt", account_key.c_str(), day.c_str(), state->settings.c_str()))
                            status = L"无法保存领取记录，未提交请求";
                        else
                        {
                            bool accepted = source->ClaimDailyReward(day);
                            const auto claim_error = source->GetLastError();
                            wstring checked_day; bool confirmed{};
                            if (source->GetDailyRewardRecord(checked_day, confirmed) && checked_day == day && confirmed)
                                status = day + L" · 会员领取已到账";
                            else status = accepted ? day + L" · 平台已受理，尚未确认到账；请稍后刷新" : claim_error + L"；当天不会自动重复提交";
                        }
                    }
                }
            }
            catch (const exception&) { status = L"领取状态无法确认，请稍后查看记录；未自动重试提交"; }
            lock_guard<mutex> task_guard(state->lock); state->status = status; state->busy = false;
        }).detach();
    }
    catch (const exception&) { state->busy = false; state->status = L"无法启动签到任务"; }
}
wstring COnlineDailyRewards::Describe() const
{
    if (!m_state) return L"签到尚未初始化";
    lock_guard<mutex> guard(m_state->lock);
    return wstring(L"自动签到：") + (m_state->enabled ? L"已开启" : L"已关闭") + L"\n" + m_state->status;
}
void COnlineDailyRewards::Shutdown()
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock); m_state->stopping = true;
}
void COnlineDailyRewards::CancelPending()
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock); ++m_state->generation; m_next_check = 0;
}
