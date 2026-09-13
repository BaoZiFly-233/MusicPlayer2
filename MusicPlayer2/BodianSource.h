#pragma once

#include <string>
#include <vector>
#include <mutex>
#include "OnlineSource.h"
#include "nlohmann/json.hpp"

// 波点音乐音源。
//
// 波点音乐是酷我（腾讯音乐）旗下的产品，接口在 bd-api.kuwo.cn，与酷狗不是同一套。
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

class CBodianSource : public online::IOnlineSource
{
public:
    CBodianSource();
    CBodianSource(const CBodianSource& source);
    ~CBodianSource() override;

    // ---- IOnlineSource ----
    std::wstring GetScheme() const override { return L"bodian"; }
    std::wstring GetDisplayName() const override { return L"波点音乐"; }
    bool Search(const std::wstring& keyword, int page, std::vector<online::Track>& result) override;
    bool Browse(const online::BrowseRequest& request, online::BrowseResult& result) override;
    std::wstring ResolvePlayUrl(const std::wstring& virtual_path) override;
    bool GetLyric(const std::wstring& virtual_path, online::Lyric& result) override;
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

    // 上一次失败的原因，可直接显示给用户
    std::wstring GetLastError() const override { return m_last_error; }

protected:
    // 发一个 GET 请求。path 形如 L"/api/search/music/list?..."
    bool Get(const std::wstring& path, nlohmann::json& out_json);
    bool SignedRequest(const std::wstring& path, std::vector<std::pair<std::string, std::string>> params,
        nlohmann::json& response, const std::string& body = "", const wchar_t* method = L"GET");
    bool ApplyAccount(const nlohmann::json& data);
    std::wstring AuthQuery() const;
    std::string m_devid;
    Account m_account;
    std::wstring m_quality_note;   // 取播放地址时的音质说明（降级原因）
    mutable std::mutex m_account_mutex;
    std::wstring m_qr_key;
    ULONGLONG m_qr_created{};

    std::wstring m_last_error;
    std::wstring m_last_response;   // 最近一次接口的原始返回，排查问题时用
};

} // namespace bodian
