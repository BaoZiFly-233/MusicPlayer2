#pragma once

#include <string>
#include <vector>
#include <mutex>
#include "OnlineSource.h"
#include "nlohmann/json.hpp"

// B源音源。
//
// B源是上游某条产品线的前端，接口自成一套，与K源不通用。
// 匿名浏览与新版账号/播放接口使用不同客户端参数；新版接口需要查询签名。
//
// 使用上的几个要点：
//   * 播放地址有时效，只保留完整音频缓存，不持久化地址
//   * 匿名状态下付费曲会返回 20018「没有解锁付费歌曲」，要如实告诉用户，
//     不要当成播放失败
//   * 绝不要用 anti.s 接口：它对付费曲会静默返回同一个 11 秒试听文件，
//     会造成「能播但只响 11 秒」的诡异现象
namespace bodian
{

// 把 LRCX（B源/上游的逐字歌词）转成播放器认的扩展 LRC：
//   [行绝对时间]<字绝对时间>字<字结束时间>…    原文行
//   [行绝对时间]译文                紧跟其后、同时间戳的译文
// 播放器把「时间戳相同的两行」配成原文加译文，所以译文要挂到上一句原文上；
// 整首第一个标签行要是没有原文可挂的译文槽位，直接丢掉（实测有歌就是这样）。
// 字的位置和时长由本文件 [kuwo:] 的权重从 <a,b> 的和差编码解出，字与
// 字之间的空隙补成空段，填色停在字尾；行时间就是行标签本身。整首都拿不到可用的逐字时间时返回空串，由调用方回退。
std::wstring LrcxToExtendedLyric(const std::string& lrcx_utf8);

class CBodianSource : public online::IOnlineSource
{
public:
    CBodianSource();
    CBodianSource(const CBodianSource& source);
    ~CBodianSource() override;

    // ---- IOnlineSource ----
    std::wstring GetScheme() const override { return L"bodian"; }
    std::wstring GetDisplayName() const override { return L"B源"; }
    std::wstring GetShortName() const override { return L"B源"; }
    bool Search(const std::wstring& keyword, int page, std::vector<online::Track>& result) override;
    bool Browse(const online::BrowseRequest& request, online::BrowseResult& result) override;
    std::wstring ResolvePlayUrl(const std::wstring& virtual_path) override;
    bool GetLyric(const std::wstring& virtual_path, online::Lyric& result) override;
    // 逐字歌词走独立的 mlyric 域名，q 参数是 base64；拿不到则返回 false 由调用方回退
    bool FetchWordLyric(const std::wstring& rid, online::Lyric& result);
    struct Account
    {
        std::string uid, token;
        bool IsLoggedIn() const;
    };
    Account GetAccount() const;
    void SetAccount(const Account& account);
    bool IsLoggedIn() const { return GetAccount().IsLoggedIn(); }
    bool GetQrCode(std::wstring& content);
    online::QrStatus CheckQrCode();
    bool ValidateAccount();
    bool GetProfile(online::AccountProfile& profile) override;
    std::wstring GetCoverUrl(const online::Track& track) override;
    std::wstring GetQualityNote() const override { return m_quality_note; }
    void LoadIdentity(const std::wstring& config_dir);
    bool SaveIdentity(const std::wstring& config_dir) const;
    void Logout() { SetAccount({}); }
    static std::string QuerySignature(const std::wstring& path, const std::string& query, const std::string& body = "");

    // 看广告领免费畅听（B源官方的激励视频权益）。
    //
    // 一次上报换 30 分钟畅听，服务端每天限量（allDayConfig.needWatch），
    // 领取后付费曲的 audioUrl 会从 20018 直接变成 200，无损档同样解开。
    // 这个接口只认匿名安卓客户端的头组合，和登录状态无关，所以未登录也能领。
    bool ClaimAdFreeTime(int& seconds, std::wstring& detail);
    bool QueryAdFreeInfo(int& watched, int& need_watch, int& remain_seconds);
    // 解析播放地址前调用：权益过期就自动观看一次广告，返回当前是否在有效期内。
    bool EnsureAdFreeTime(int& remain_seconds);
    // 走匿名安卓客户端 + 新版播放接口取地址。实测只有这条链路吃畅听权益：
    // 付费曲直接给 2000 kbps 无损，而同一首在账号态请求反而被判成 20018。
    // 取不到时静默失败，由调用方回退到原有链路。
    bool ResolveAdFreePlayUrl(const std::wstring& rid, std::wstring& url);
    bool EarningRequest(const std::wstring& path, const std::wstring& extra_query,
        const std::string& body, nlohmann::json& response, const wchar_t* method);

    // 听歌赚金币。B源按听歌时长分了九级奖励（5 秒到 3 小时，金币 36 到 188），
    // 实测服务端并不核对真实听歌时长：按 id 顺序逐个上报就能领满当天全部金币，
    // 上报后服务端还会把累计听歌时间回写成该级要求的值。
    struct EarningTask
    {
        int id{};
        int stage{};    // 该级要求的听歌秒数
        int gold{};     // 该级金币
        int status{};   // 3 表示已完成，其余可领
    };
    // 搜索结果里补上专辑条目：B源的歌曲搜索不返回专辑实体，
    // 而综合搜索的 albumPage 有。条目里存 albumId，双击后按 AlbumTracks 取曲目。
    void AddAlbums(online::BrowseResult& result, const std::wstring& keyword);
    // 只搜专辑：综合搜索的 albumPage 就是专辑列表
    bool BrowseAlbums(const std::wstring& keyword, int page, online::BrowseResult& result);
    // 一张专辑的曲目，按平台给的曲序返回
    bool BrowseAlbumTracks(const std::wstring& album_id, int page, online::BrowseResult& result);
    bool GetEarningTasks(std::vector<EarningTask>& tasks);
    // 领取一个任务的金币。task_type 取 listen 或 sign。
    // 服务端用同一个接口发放，区别只在 taskType；已领过或尚未轮到的返回 code 1。
    bool ClaimEarningTask(const std::wstring& task_type, int id, bool& awarded);
    // 一次完整流程：领听歌金币 + 领签到金币，回填余额与本次收益。
    // 注意：不自动兑换。实测账号态取流最高只有 320k（无损天卡也一样），
    // 真正出无损的是 ResolveAdFreePlayUrl 那条匿名链路，所以金币留着更划算。
    bool RunEarningCycle(int& balance, int& claimed_count, int& claimed_gold, std::wstring& detail);

    // 上一次失败的原因，可直接显示给用户
    std::wstring GetLastError() const override { return m_last_error; }

protected:
    // 发一个 GET 请求。path 形如 L"/api/search/music/list?..."
    bool Get(const std::wstring& path, nlohmann::json& out_json);
    bool SignedRequest(const std::wstring& path, std::vector<std::pair<std::string, std::string>> params,
        nlohmann::json& response, const std::string& body = "", const wchar_t* method = L"GET");
    bool ApplyAccount(const nlohmann::json& data);
    std::wstring AuthQuery() const;
    // 上报「已观看广告」用的设备号，取值与浏览接口的 devid 无关
    std::string m_ad_devid;
    std::wstring m_settings_path;   // bodian.ini，保存自动观看开关
    std::string m_devid;
    Account m_account;
    std::wstring m_quality_note;   // 取播放地址时的音质说明（降级原因）
    mutable std::mutex m_account_mutex;
    std::wstring m_qr_key;
    ULONGLONG m_qr_created{};

    std::wstring m_last_error;
    std::wstring m_last_response;   // 最近一次接口的原始返回，排查问题时用
};

// 自动观看广告的开关与剩余时长放在进程级，界面层不必持有音源实例就能读写。
// settings_path 留空表示只改内存状态，不写配置文件。
bool AdRewardEnabled();
void SetAdRewardEnabled(bool enabled, const std::wstring& settings_path = std::wstring());
int AdFreeRemainSeconds();

} // namespace bodian
