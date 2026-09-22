#include "stdafx.h"
#include "OnlineDailyRewards.h"
#include "OnlineProgress.h"
#include "KugouSource.h"
#include "BodianSource.h"
#include "KugouCrypto.h"
#include "OnlineJson.h"
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>

using online::OnlineProgress;
using online::ProgressResult;

using namespace std;
bool kugou::CKugouSource::GetDailyRewardRecord(wstring& day, bool& received)
{
    m_last_error.clear(); received = false; day.clear();
    if (!IsLoggedIn()) { m_last_error = L"请先登录K源"; return false; }
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

// ---------------------------------------------------------------------------
// 广告奖励上报
// ---------------------------------------------------------------------------

// K源的广告奖励：每天最多 8 次，每次上报一次「广告看完了」。
// 服务端只按 play_start / play_end 的差值判断，这两个值由客户端提供，
// 所以不需要真的播放广告。
static constexpr long long AD_REWARD_ID = 12307537187;   // 广告位编号
static constexpr long long AD_REWARD_PLAY_MS = 30000;    // 上报的播放时长，30 秒
static constexpr int AD_REWARD_EXHAUSTED = 30002;        // 当天次数已用完
static constexpr int AD_REWARD_ALREADY = 130012;         // 当天已经领过
static constexpr int AD_REWARD_MAX_TIMES = 8;            // 每天最多领 8 次
static constexpr int AD_REWARD_INTERVAL_SECONDS = 15;    // 两次上报之间的间隔

bool kugou::CKugouSource::ClaimAdReward(int& remain, int& award_hours, bool& exhausted)
{
    remain = -1; award_hours = 0; exhausted = false;
    m_last_error.clear();
    if (!IsLoggedIn()) { m_last_error = L"请先登录K源"; return false; }

    // 时间戳用毫秒；play_start 往前推 30 秒，与服务端期望的观看时长对齐
    const long long now = static_cast<long long>(time(nullptr)) * 1000;
    nlohmann::json payload;
    payload["ad_id"] = AD_REWARD_ID;
    payload["play_start"] = now - AD_REWARD_PLAY_MS;
    payload["play_end"] = now;

    nlohmann::json response;
    if (!Request(L"/youth/v1/ad/play_report", L"",
        {{"appid", "3116"}, {"clientver", "11440"}, {"mid", m_device.mid}, {"dfid", m_device.dfid},
        {"clienttime", to_string(time(nullptr))}}, payload.dump(), response, true, L"POST"))
        return false;

    int code = static_cast<int>(online::JsonNumber(response, "error_code"));
    if (code == 0) code = static_cast<int>(online::JsonNumber(response, "code"));
    if (code == AD_REWARD_EXHAUSTED)
    {
        exhausted = true; m_last_error = L"今天的广告奖励次数已经用完";
        return false;
    }
    if (code == AD_REWARD_ALREADY) { m_last_error = L"今天的广告奖励已经领过"; return false; }
    if (online::JsonNumber(response, "status") != 1 && code != 0)
    {
        const wstring message = FromUtf8(online::JsonText(response, "error_msg"));
        m_last_error = message.empty() ? L"广告奖励上报未通过（返回 " + to_wstring(code) + L"）" : L"广告奖励：" + message;
        return false;
    }

    // 服务端会把当天进度一起回传，直接用它驱动循环，不必靠固定次数试探。
    if (response.contains("data") && response["data"].is_object())
    {
        const auto& data = response["data"];
        remain = static_cast<int>(online::JsonNumber(data, "remain"));
        award_hours = static_cast<int>(online::JsonNumber(data, "award_vip_hour"));
    }
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
    if (!kg || !kg->IsLoggedIn())
    {
        state->status = L"请先登录K源";
        if (!automatic) OnlineProgress::Start(L"K源会员与签到")->Finish(ProgressResult::Failed, state->status);
        return;
    }
    auto source = make_shared<kugou::CKugouSource>(*kg);
    const unsigned generation = state->generation;
    state->busy = true; state->status = L"正在检查当天会员领取记录…";
    auto progress = OnlineProgress::Start(L"K源会员与签到", state->status, automatic);
    try
    {
        thread([state, source, automatic, generation, progress] {
            wstring status;
            bool confirmed_reward = false;
            try
            {
                wstring day; bool received{};
                if (!source->GetDailyRewardRecord(day, received)) status = source->GetLastError();
                else if (received) { status = day + L" · 当天会员已领取"; confirmed_reward = true; }
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
                            progress->Update(L"正在提交领取并核对到账记录");
                            bool accepted = source->ClaimDailyReward(day);
                            const auto claim_error = source->GetLastError();
                            wstring checked_day; bool confirmed{};
                            if (source->GetDailyRewardRecord(checked_day, confirmed) && checked_day == day && confirmed)
                            { status = day + L" · 会员领取已到账"; confirmed_reward = true; }
                            else status = accepted ? day + L" · 平台已受理，尚未确认到账；请稍后刷新" : claim_error + L"；当天不会自动重复提交";
                        }
                    }
                }

                // 广告奖励：每天最多 8 次，每次上报一次「广告看完了」。放在每日奖励之后，
                // 失败了也不影响上面的结果。两次之间留间隔，避免短时间集中打接口。
                int ad_done = 0, ad_hours = 0;
                wstring ad_note;
                for (int i = 0; i < AD_REWARD_MAX_TIMES; ++i)
                {
                    { lock_guard<mutex> task_guard(state->lock); if (state->stopping) break; }
                    int remain = -1, hours = 0; bool exhausted = false;
                    progress->Update(L"正在处理会员奖励", ad_done);
                    if (!source->ClaimAdReward(remain, hours, exhausted))
                    {
                        if (ad_done == 0 && !exhausted) ad_note = source->GetLastError();
                        break;
                    }
                    ++ad_done; ad_hours += hours;
                    progress->Update(L"会员奖励已到账，等待下一次处理", ad_done,
                        remain >= 0 ? ad_done + remain : 0);
                    {
                        lock_guard<mutex> task_guard(state->lock);
                        state->status = L"广告奖励已完成 " + to_wstring(ad_done) + L" 次（+"
                            + to_wstring(ad_hours) + L" 小时）";
                    }
                    if (remain == 0) break;
                    if (i + 1 < AD_REWARD_MAX_TIMES)
                        this_thread::sleep_for(chrono::seconds(AD_REWARD_INTERVAL_SECONDS));
                }
                status += L"\n广告奖励：" + (ad_done > 0
                    ? L"本次 " + to_wstring(ad_done) + L" 次，共 +" + to_wstring(ad_hours) + L" 小时会员"
                    : (ad_note.empty() ? wstring(L"今天没有可领的次数") : ad_note));
                // 广告这一段失败不推翻上面的结论：每日会员已经确认到账的话，
                // 整体还按成功收尾，广告的问题写在 status 里，不然用户会以为会员没领到。
                // （上面的注释「失败了也不影响上面的结果」说的就是这个意思。）
            }
            catch (const exception&) { confirmed_reward = false; status = L"领取状态无法确认，请稍后查看记录；未自动重试提交"; }
            progress->Finish(confirmed_reward ? ProgressResult::Succeeded : ProgressResult::Failed, status);
            lock_guard<mutex> task_guard(state->lock); state->status = status; state->busy = false;
        }).detach();
    }
    catch (const exception&) { state->busy = false; state->status = L"无法启动签到任务"; progress->Finish(ProgressResult::Failed, state->status); }
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

// ---------------------------------------------------------------------------
// B源：看广告领会员
// ---------------------------------------------------------------------------

struct CBodianAdRewards::State
{
    mutable mutex lock;
    wstring status{ L"尚未检查" };
    bool busy{}, stopping{};
};

CBodianAdRewards& CBodianAdRewards::Instance() { static CBodianAdRewards rewards; return rewards; }

void CBodianAdRewards::Configure(const wstring& config_dir)
{
    if (m_state) return;
    m_state = make_shared<State>();
    m_settings_path = (filesystem::path(config_dir) / L"bodian.ini").wstring();
}

bool CBodianAdRewards::Enabled() const { return bodian::AdRewardEnabled(); }

void CBodianAdRewards::SetEnabled(bool enabled)
{
    bodian::SetAdRewardEnabled(enabled, m_settings_path);
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock);
    m_next_check = 0;
    m_state->status = enabled ? L"已开启自动观看广告领会员" : L"已关闭自动观看广告领会员";
}

void CBodianAdRewards::Tick()
{
    if (!m_state || !Enabled() || GetTickCount64() < m_next_check) return;
    // 权益按 30 分钟一段发放，五分钟检查一次足够，不必每播一首就打接口。
    m_next_check = GetTickCount64() + 5 * 60 * 1000;
    Request(true);
}

void CBodianAdRewards::Request(bool automatic)
{
    if (!m_state) return;
    auto state = m_state;
    lock_guard<mutex> guard(state->lock);
    if (state->busy || state->stopping) return;
    state->busy = true;
    state->status = automatic ? L"正在自动观看广告领取会员畅听…" : L"正在观看广告领取会员畅听…";
    auto progress = OnlineProgress::Start(L"B源会员与签到", L"正在检查会员权益", automatic);
    try
    {
        thread([state, automatic, settings = m_settings_path, progress] {
            wstring status;
            bool confirmed_reward = false;
            try
            {
                auto* registered = dynamic_cast<bodian::CBodianSource*>(online::CSourceRegistry::Instance().FindByScheme(L"bodian"));
                auto source = registered ? make_shared<bodian::CBodianSource>(*registered) : make_shared<bodian::CBodianSource>();
                if (automatic)
                {
                    const int already = bodian::AdFreeRemainSeconds();
                    if (already > 0) { status = L"会员畅听有效，剩余 " + to_wstring(already / 60) + L" 分钟"; confirmed_reward = true; }
                    else
                    {
                        int remain = 0;
                        progress->Update(L"正在领取并核对会员权益");
                        confirmed_reward = source->EnsureAdFreeTime(remain) && remain > 0;
                        status = confirmed_reward
                            ? L"已自动观看广告，获得 " + to_wstring(remain / 60) + L" 分钟会员畅听"
                            : (source->GetLastError().empty() ? L"自动观看广告未成功，稍后会重试" : source->GetLastError());
                    }
                }
                else
                {
                    int seconds = 0, watched = 0, need = 0, remain = 0; wstring detail;
                    if (source->ClaimAdFreeTime(seconds, detail))
                    {
                        confirmed_reward = true;
                        status = detail;
                        if (source->QueryAdFreeInfo(watched, need, remain))
                            status += L"\n今日看广告 " + to_wstring(watched) + L"/" + to_wstring(need) + L" 次";
                    }
                    else status = source->GetLastError();
                }

                // 听歌金币每天领一次，用配置里的日期去重，避免每次定时检查都打接口。
                // 实测服务端不核对真实听歌时长，按 id 顺序上报即可领满当天全部金币。
                wstring today;
                {
                    const time_t now = time(nullptr);
                    tm local{};
                    if (localtime_s(&local, &now) == 0)
                    {
                        wchar_t buffer[16]{};
                        if (wcsftime(buffer, _countof(buffer), L"%Y-%m-%d", &local) > 0) today = buffer;
                    }
                }
                wchar_t claimed[16]{};
                GetPrivateProfileStringW(L"earning", L"day", L"", claimed, _countof(claimed), settings.c_str());
                if (!today.empty() && today != claimed)
                {
                    progress->Update(L"正在处理每日签到与金币");
                    int balance = 0, count = 0, gold = 0; wstring detail;
                    if (source->RunEarningCycle(balance, count, gold, detail))
                        WritePrivateProfileStringW(L"earning", L"day", today.c_str(), settings.c_str());
                    else confirmed_reward = false;
                    status += L"\n金币：" + detail;
                }
            }
            catch (const exception&) { confirmed_reward = false; status = L"领取状态无法确认，请稍后再试"; }
            progress->Finish(confirmed_reward ? ProgressResult::Succeeded : ProgressResult::Failed, status);
            lock_guard<mutex> task_guard(state->lock);
            state->status = status; state->busy = false;
        }).detach();
    }
    catch (const exception&) { state->busy = false; state->status = L"无法启动领取任务"; progress->Finish(ProgressResult::Failed, state->status); }
}

wstring CBodianAdRewards::Describe() const
{
    if (!m_state) return L"B源看广告领会员尚未初始化";
    lock_guard<mutex> guard(m_state->lock);
    wstring text = wstring(L"B源自动观看广告领会员：") + (bodian::AdRewardEnabled() ? L"已开启" : L"已关闭");
    const int remain = bodian::AdFreeRemainSeconds();
    text += remain > 0 ? L"\n会员畅听剩余 " + to_wstring(remain / 60 + 1) + L" 分钟" : L"\n当前没有有效的会员畅听";
    return text + L"\n" + m_state->status;
}

void CBodianAdRewards::Shutdown()
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock); m_state->stopping = true;
}
