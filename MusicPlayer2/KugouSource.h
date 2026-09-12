#pragma once

#include <string>
#include <vector>
#include "OnlineSource.h"
#include "nlohmann/json.hpp"

// 酷狗概念版音源。
//
// 说明几件重要的事：
//   * 概念版的 appid/clientver/salt 与标准版完全不同，token 也不通用
//   * 设备身份（guid/mid/dfid）必须固定并持久化。每次都换会被当成新设备，
//     登录设备列表会越来越长，也更容易触发风控
//   * 播放地址是明文 http 链接，但有有效期，所以不缓存，每次播放前重新取
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

    bool IsLoggedIn() const { return !token.empty() && userid != "0"; }
};

class CKugouSource : public online::IOnlineSource
{
public:
    CKugouSource();
    ~CKugouSource() override;

    // ---- IOnlineSource ----
    std::wstring GetScheme() const override { return L"kugou"; }
    std::wstring GetDisplayName() const override { return L"酷狗概念版"; }
    bool Search(const std::wstring& keyword, int page, std::vector<online::Track>& result) override;
    std::wstring ResolvePlayUrl(const std::wstring& virtual_path) override;
    bool GetLyric(const std::wstring& virtual_path, online::Lyric& result) override;

    // ---- 设备与账号 ----
    // 载入持久化的设备身份与账号；首次调用时生成并保存设备身份
    void LoadIdentity(const std::wstring& config_dir);
    void SaveIdentity(const std::wstring& config_dir) const;

    const DeviceIdentity& GetIdentity() const { return m_device; }
    const Account& GetAccount() const { return m_account; }
    bool IsLoggedIn() const { return m_account.IsLoggedIn(); }

    // 上一次解析失败的原因，可直接显示给用户
    const std::wstring& GetLastError() const { return m_last_error; }

protected:
    // 向接口发一个带概念版公共参数和签名的请求。
    // url_path 形如 L"/v3/search/song"；router 用于设置 x-router 头。
    bool Request(const std::wstring& url_path, const std::wstring& router,
        const std::vector<std::pair<std::string, std::string>>& extra_params,
        const std::string& body, nlohmann::json& out_json, bool need_sign = true);

    // 取歌播放地址。返回空字符串表示失败。
    std::wstring FetchPlayUrl(const std::wstring& hash, const std::wstring& album_audio_id);

    DeviceIdentity m_device;
    Account m_account;
    std::wstring m_last_error;      // 最近一次失败原因
};

} // namespace kugou
