#pragma once

#include <string>
#include <vector>
#include <mutex>
#include "OnlineSource.h"
#include "nlohmann/json.hpp"

// 酷狗概念版音源。
//
// 说明几件重要的事：
//   * 概念版的 appid/clientver/salt 与标准版完全不同，token 也不通用
//   * 设备身份（guid/mid/dfid）必须固定并持久化。每次都换会被当成新设备，
//     登录设备列表会越来越长，也更容易触发风控
//   * 播放地址有有效期；完整音频可由媒体缓存复用，地址本身不持久化
//   * 匿名状态下接口会返回「需要购买」，这是权限限制而不是签名错误。
//     想正常听歌需要登录概念版账号
namespace kugou
{

// 设备身份。由 guid 派生出 mid，dfid 需要向官方注册接口换取。
struct DeviceIdentity
{
    std::string guid;       // 随机生成一次后固定
    std::string mid;        // CalculateMid(guid)
    std::string mac;        // 本机网卡地址
    std::string dfid;       // 设备指纹，向 /risk/v2/r_register_dev 换取
    std::string server_dev; // 固定的客户端标识

    bool IsComplete() const { return !guid.empty() && !mid.empty(); }
};

// 登录态
struct Account
{
    std::string token;
    std::string userid;
    std::string t1;         // 概念版刷新 token 需要
    std::string vip_type;
    std::string vip_token;

    bool IsLoggedIn() const { return !token.empty() && !userid.empty() && userid != "0"; }
};

class CKugouSource : public online::IOnlineSource
{
public:
    CKugouSource();
    CKugouSource(const CKugouSource& source);
    ~CKugouSource() override;

    // ---- IOnlineSource ----
    std::wstring GetScheme() const override { return L"kugou"; }
    std::wstring GetDisplayName() const override { return L"酷狗概念版"; }
    bool Search(const std::wstring& keyword, int page, std::vector<online::Track>& result) override;
    bool Browse(const online::BrowseRequest& request, online::BrowseResult& result) override;
    std::wstring ResolvePlayUrl(const std::wstring& virtual_path) override;
    bool GetLyric(const std::wstring& virtual_path, online::Lyric& result) override;

    // ---- 设备与账号 ----
    // 载入持久化的设备身份与账号；首次调用时生成并保存设备身份
    void LoadIdentity(const std::wstring& config_dir);
    void SaveIdentity(const std::wstring& config_dir) const;

    // 向服务端注册本机设备，换取设备指纹 dfid。
    // 取播放地址前需要有它，否则接口会返回「本次请求需要验证」。
    // 注册成功后 dfid 写进 m_device，调用方记得 SaveIdentity。
    bool RegisterDevice();

    const DeviceIdentity& GetIdentity() const { return m_device; }
    Account GetAccount() const { std::lock_guard<std::mutex> lock(m_state_mutex); return m_account; }
    void SetAccount(const Account& account) { std::lock_guard<std::mutex> lock(m_state_mutex); m_account = account; }
    bool IsLoggedIn() const { return GetAccount().IsLoggedIn(); }

    // 上一次解析失败的原因，可直接显示给用户
    std::wstring GetLastError() const override { return m_last_error; }

    // ---- 扫码登录 ----
    // 登录流程：GetQrCode 拿二维码内容 -> 用户用酷狗App扫 -> 反复 CheckQrCode
    // 直到返回已授权，此时账号信息会写进 m_account。
    using QrStatus = online::QrStatus;

    // 获取登录二维码，返回给用户去扫的内容（一个网址）
    bool GetQrCode(std::wstring& qr_content);

    // 查询扫码状态。返回 Authorized 时账号已写入，调用方记得 SaveIdentity。
    QrStatus CheckQrCode();

    // 退出登录（清掉内存里的账号信息，调用方负责保存）
    void Logout();
    bool GetDailyRewardRecord(std::wstring& day, bool& received);
    bool ClaimDailyReward(const std::wstring& day);
    bool GetProfile(online::AccountProfile& profile) override;
    std::wstring GetCoverUrl(const online::Track& track) override;

protected:
    // 向接口发一个带概念版公共参数和签名的请求。
    // url_path 形如 L"/v3/search/song"；router 用于设置 x-router 头。
    bool Request(const std::wstring& url_path, const std::wstring& router,
        const std::vector<std::pair<std::string, std::string>>& extra_params,
        const std::string& body, nlohmann::json& out_json, bool need_sign = true, const wchar_t* method = nullptr, const wchar_t* base_url = nullptr);

    // 登录接口在另一个域名上，用 Web 签名且不带公共参数，所以单独一个函数。
    // 会往 params 里补公共参数，所以传入的是可修改的引用
    bool RequestLoginApi(const std::wstring& url_path,
        std::vector<std::pair<std::string, std::string>>& params,
        nlohmann::json& out_json);

    // 取歌播放地址。返回空字符串表示失败。
    std::wstring FetchPlayUrl(const std::wstring& hash, const std::wstring& album_audio_id);

    DeviceIdentity m_device;
    Account m_account;
    mutable std::mutex m_state_mutex;
    std::wstring m_last_error;      // 最近一次失败原因
    std::wstring m_qr_key;          // 当前登录二维码的 key
};

} // namespace kugou
