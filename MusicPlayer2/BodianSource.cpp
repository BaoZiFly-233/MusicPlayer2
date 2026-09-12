#include "stdafx.h"
#include "BodianSource.h"
#include "KugouCrypto.h"        // 复用里面的 UTF-8 转换和 Base64 解码
#include "InternetCommon.h"
#include <random>
#include <ctime>
#include <algorithm>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// nlohmann/json 从 3.8 起把 parse(string) 标为废弃，而本项目把警告当错误处理。
// 这里单独关掉这一条，与 KugouSource 里的调用方式保持一致。
#pragma warning(disable: 4996)

using namespace std;
using json = nlohmann::json;

// 编码转换函数在 kugou 命名空间下，这里复用
using kugou::ToUtf8;
using kugou::FromUtf8;

namespace bodian
{

static const wchar_t* API_HOST = L"bd-api.kuwo.cn";

// 必须伪装成波点的安卓客户端，服务端据此判断请求来源。
// 换成别的 UA 会被当成未知客户端，接口返回「歌曲已下线」。
static const char* BODIAN_USER_AGENT = "Dart/2.10 (dart:io)";

// devid 每个设备一个即可，这里在构造时生成一次并固定下来。
static string g_devid;

static string MakeDevid()
{
    // 11 位数字，形如 123849114429 去掉首位后的样子
    static std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<long long> dist(10000000000LL, 99999999999LL);
    return to_string(dist(rng));
}

// 用 WinHTTP 发一个 GET 请求。
// 波点接口对请求头很敏感，用 WinHTTP 可以完整控制发出去的每一行，
// 不受上层封装的默认行为影响。
static bool HttpGetWinHttp(const wstring& url, const string& headers, wstring& result)
{
    // 拆出主机、路径、端口。地址都是 https，这里只处理这一种。
    wstring rest = url;
    if (rest.compare(0, 8, L"https://") == 0)
        rest = rest.substr(8);
    else
        return false;

    size_t slash = rest.find(L'/');
    wstring host = (slash == wstring::npos) ? rest : rest.substr(0, slash);
    wstring path = (slash == wstring::npos) ? L"/" : rest.substr(slash);

    HINTERNET session = WinHttpOpen(L"MusicPlayer2",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return false;

    // 整体超时：连接 10 秒，收发 20 秒
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect != nullptr)
    {
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
            wstring wh = FromUtf8(headers);
            if (WinHttpSendRequest(request,
                wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(),
                static_cast<DWORD>(wh.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(request, nullptr))
            {
                result.clear();
                string buffer;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0)
                {
                    buffer.assign(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, &buffer[0], available, &read) || read == 0)
                        break;
                    buffer.resize(read);
                    result += FromUtf8(buffer);
                }
                ok = !result.empty();
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return ok;
}

CBodianSource::CBodianSource()
{
    if (g_devid.empty())
        g_devid = MakeDevid();
}

CBodianSource::~CBodianSource()
{
}

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

bool CBodianSource::Get(const wstring& path, json& out_json)
{
    wstring url = wstring(L"https://") + API_HOST + path;

    // 这一组请求头是实测出来的，服务端靠它们识别客户端。
    // 缺 plat / channel / ver / brand 会返回 402，缺 user-agent 会返回「歌曲已下线」。
    // 这里直接用 WinHTTP 发送，以便完整控制请求头。
    string headers;
    headers += "plat: ar\r\n";                      // 安卓端
    headers += "channel: huawei\r\n";
    headers += "brand: huawei mate40\r\n";
    headers += "devid: " + g_devid + "\r\n";
    headers += "ver: 1.1.9\r\n";
    headers += "appuid: 123849114429\r\n";
    headers += "api-ver: application/json\r\n";
    headers += "net: mobile\r\n";
    headers += "user-agent: " + string(BODIAN_USER_AGENT) + "\r\n";

    wstring result;
    if (!HttpGetWinHttp(url, headers, result) || result.empty())
    {
        m_last_error = L"网络请求失败，请检查网络连接";
        return false;
    }
    m_last_response = result;

    try
    {
        out_json = json::parse(ToUtf8(result));
    }
    catch (const json::exception&)
    {
        m_last_error = L"接口返回的内容无法识别，可能接口有变动";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

bool CBodianSource::Search(const wstring& keyword, int page, vector<online::Track>& result)
{
    result.clear();
    if (keyword.empty())
        return false;

    int pn = (page < 1 ? 1 : page) - 1;     // 接口的页码从 0 开始

    wstring path = wstring(L"/api/search/music/list?pn=") + to_wstring(pn) +
        L"&rn=30&keyword=" + FromUtf8(kugou::UrlEncode(ToUtf8(keyword))) +
        L"&uid=-1&token=";

    json response;
    if (!Get(path, response))
        return false;

    int code = response.value("code", 0);
    if (code != 200)
    {
        // 把服务端的说明一并带上，便于判断是接口变动还是别的原因
        wstring msg = FromUtf8(response.value("msg", ""));
        m_last_error = L"搜索失败（返回 " + to_wstring(code) + L" " + msg + L"）";
        return false;
    }

    if (!response.contains("data") || !response["data"].contains("resultList"))
    {
        m_last_error = L"接口返回的结构与预期不符";
        return false;
    }

    for (const auto& item : response["data"]["resultList"])
    {
        online::Track track;

        // 搜索接口给的标识字段是 musicRid，形如 "MUSIC_228908"；
        // 取播放地址的接口只要纯数字那段，带上前缀会报「参数错误」。
        string rid = item.value("musicRid", "");
        const string rid_prefix = "MUSIC_";
        if (rid.compare(0, rid_prefix.size(), rid_prefix) == 0)
            rid = rid.substr(rid_prefix.size());

        if (rid.empty())
            continue;

        track.virtual_path = FromUtf8("bodian://" + rid);
        track.title = FromUtf8(item.value("name", ""));
        track.artist = FromUtf8(item.value("artist", ""));
        track.album = FromUtf8(item.value("album", ""));
        track.duration_ms = item.value("duration", 0) * 1000;   // 接口返回秒

        result.push_back(track);
    }

    if (result.empty())
    {
        // 接口返回 200 但一首都没解析出来，把原始返回带上便于定位
        m_last_error = L"接口返回成功但没解析出歌曲。原始返回: " +
            m_last_response.substr(0, 200);
        return false;
    }

    m_last_error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// 取播放地址
// ---------------------------------------------------------------------------

wstring CBodianSource::ResolvePlayUrl(const wstring& virtual_path)
{
    m_last_error.clear();

    const wstring prefix = L"bodian://";
    if (virtual_path.compare(0, prefix.size(), prefix) != 0)
        return wstring();

    wstring rid = virtual_path.substr(prefix.size());
    if (rid.empty())
        return wstring();

    wstring path = L"/api/service/music/audioUrl/" + rid + L"?uid=-1&token=";

    json response;
    if (!Get(path, response))
        return wstring();

    int code = response.value("code", 0);

    // 这两个错误码含义不同，要分开处理，否则用户会以为程序坏了
    if (code == 20018)
    {
        m_last_error = L"这首歌需要波点音乐会员才能播放";
        return wstring();
    }
    if (code == 20012)
    {
        m_last_error = L"这首歌在波点音乐已下线或无版权";
        return wstring();
    }
    if (code != 200)
    {
        m_last_error = L"取播放地址失败（接口返回 " + to_wstring(code) + L"）";
        return wstring();
    }

    if (!response.contains("data"))
        return wstring();

    const auto& data = response["data"];

    // 优先用 https 地址，避免明文 http 在部分网络下被干扰
    string url = data.value("audioHttpsUrl", "");
    if (url.empty())
        url = data.value("audioUrl", "");

    if (url.empty())
    {
        m_last_error = L"接口没有返回可用的播放地址";
        return wstring();
    }

    m_last_error.clear();
    return FromUtf8(url);
}

// ---------------------------------------------------------------------------
// 歌词
// ---------------------------------------------------------------------------

bool CBodianSource::GetLyric(const wstring& virtual_path, online::Lyric& result)
{
    result = online::Lyric();

    const wstring prefix = L"bodian://";
    if (virtual_path.compare(0, prefix.size(), prefix) != 0)
        return false;

    wstring rid = virtual_path.substr(prefix.size());
    if (rid.empty())
        return false;

    wstring path = L"/api/service/music/lyric/" + rid + L"?uid=-1&token=";

    json response;
    if (!Get(path, response))
        return false;

    if (response.value("code", 0) != 200 || !response.contains("data"))
        return false;

    string content = response["data"].value("content", "");
    if (content.empty())
        return false;

    // 歌词内容是 Base64 编码的 LRC 文本
    result.content = FromUtf8(kugou::DecodeBase64(content));
    return result.HasContent();
}

} // namespace bodian
