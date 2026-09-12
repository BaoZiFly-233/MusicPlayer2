#pragma once

#include <string>
#include <vector>
#include "OnlineSource.h"
#include "nlohmann/json.hpp"

// 波点音乐音源。
//
// 波点音乐是酷我（腾讯音乐）旗下的产品，接口在 bd-api.kuwo.cn，与酷狗不是同一套。
// 它比酷狗简单得多：不需要签名，但要带一组特定的请求头，否则接口会返回
// 「歌曲已下线」。这些头是实测出来的，见 docs/research/波点音乐接口调研报告.md。
//
// 使用上的几个要点：
//   * 播放地址是明文直链，没有防盗链，可直接交给播放器
//   * 地址里的时间戳会过期，所以不缓存，每次播放前重新取
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
    ~CBodianSource() override;

    // ---- IOnlineSource ----
    std::wstring GetScheme() const override { return L"bodian"; }
    std::wstring GetDisplayName() const override { return L"波点音乐"; }
    bool Search(const std::wstring& keyword, int page, std::vector<online::Track>& result) override;
    std::wstring ResolvePlayUrl(const std::wstring& virtual_path) override;
    bool GetLyric(const std::wstring& virtual_path, online::Lyric& result) override;

    // 上一次失败的原因，可直接显示给用户
    std::wstring GetLastError() const override { return m_last_error; }

protected:
    // 发一个 GET 请求。path 形如 L"/api/search/music/list?..."
    bool Get(const std::wstring& path, nlohmann::json& out_json);

    std::wstring m_last_error;
    std::wstring m_last_response;   // 最近一次接口的原始返回，排查问题时用
};

} // namespace bodian
