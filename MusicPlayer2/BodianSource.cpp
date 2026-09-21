#include "stdafx.h"
#include "BodianSource.h"
#include "OnlineJson.h"
#include "OnlineHttp.h"
#include "KugouCrypto.h"        // 复用里面的 UTF-8 转换和 Base64 解码
#include "InternetCommon.h"
#include <random>
#include <ctime>
#include <algorithm>
#include <winhttp.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <chrono>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

// nlohmann/json 从 3.8 起把 parse(string) 标为废弃，而本项目把警告当错误处理。
// 这里单独关掉这一条，与 KugouSource 里的调用方式保持一致。
#pragma warning(disable: 4996)

using namespace std;
using json = nlohmann::json;

// 编码转换函数在 kugou 命名空间下，这里复用
using kugou::ToUtf8;
using kugou::FromUtf8;


// ---------------------------------------------------------------------------
// 看广告领免费畅听的进程级状态
// ---------------------------------------------------------------------------

// 畅听权益按设备与网络发放，不跟着登录态走，所以缓存必须放在进程级：
// 音源实例会被复制给后台任务，放实例里会导致每次复制都重新领取一次。
namespace
{
    mutex g_ad_state_lock;
    ULONGLONG g_ad_expire_tick{};        // 本地记下的权益到期时刻
    ULONGLONG g_ad_attempt_tick{};       // 上次上报时刻，用于失败后的退避
    bool g_ad_enabled = true;            // 自动观看广告，默认开启
    bool g_ad_enabled_loaded = false;
    bool g_ad_claimed_once = false;      // 本次运行是否成功领过，用于状态展示
}

namespace bodian
{

static const wchar_t* API_HOST = L"bd-api.kuwo.cn";

// 必须伪装成波点的安卓客户端，服务端据此判断请求来源。
// 换成别的 UA 会被当成未知客户端，接口返回「歌曲已下线」。
static const char* BODIAN_USER_AGENT = "Dart/2.10 (dart:io)";

static string MakeDevid()
{
    // 11 位数字，形如 123849114429 去掉首位后的样子
    static std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<long long> dist(10000000000LL, 99999999999LL);
    return to_string(dist(rng));
}

CBodianSource::CBodianSource() : m_ad_devid(MakeDevid()), m_devid(MakeDevid()) {}
CBodianSource::CBodianSource(const CBodianSource& source)
    : m_ad_devid(source.m_ad_devid), m_settings_path(source.m_settings_path),
    m_devid(source.m_devid), m_account(source.GetAccount()) {}

CBodianSource::~CBodianSource()
{
}

bool CBodianSource::Account::IsLoggedIn() const
{
    return !uid.empty() && uid != "0" && uid.size() <= 32 && uid.find_first_not_of("0123456789") == string::npos
        && !token.empty() && token.size() < 8192
        && all_of(token.begin(), token.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}
CBodianSource::Account CBodianSource::GetAccount() const { lock_guard<mutex> guard(m_account_mutex); return m_account; }
void CBodianSource::SetAccount(const Account& account) { lock_guard<mutex> guard(m_account_mutex); m_account = account; }
wstring CBodianSource::AuthQuery() const
{
    auto account = GetAccount();
    return L"uid=" + FromUtf8(account.IsLoggedIn() ? account.uid : "-1") + L"&token=" + FromUtf8(kugou::UrlEncode(account.token));
}
void CBodianSource::LoadIdentity(const wstring& config_dir)
{
    namespace fs = std::filesystem;
    wchar_t device[128]{};
    const auto settings = fs::path(config_dir) / L"bodian.ini";
    m_settings_path = settings.wstring();
    {
        lock_guard<mutex> guard(g_ad_state_lock);
        if (!g_ad_enabled_loaded)
        {
            // 缺省开启：这个开关就是「自动观看广告领会员」，默认关掉等于功能不存在
            g_ad_enabled = GetPrivateProfileIntW(L"ad_reward", L"enabled", 1, m_settings_path.c_str()) != 0;
            g_ad_enabled_loaded = true;
        }
    }
    GetPrivateProfileStringW(L"device", L"id", L"", device, _countof(device), settings.c_str());
    if (device[0]) m_devid = ToUtf8(device);
    else WritePrivateProfileStringW(L"device", L"id", FromUtf8(m_devid).c_str(), settings.c_str());
    const auto path = fs::path(config_dir) / L"bodian_account.dat";
    error_code error; auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 65536) return;
    ifstream input(path, ios::binary); string bytes(static_cast<size_t>(size), '\0');
    if (!input.read(bytes.data(), bytes.size())) return;
    DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())}, plain{};
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return;
    try { ApplyAccount(json::parse(string(reinterpret_cast<char*>(plain.pbData), plain.cbData))); }
    catch (const json::exception&) { m_last_error = L"本机波点登录信息已损坏，请重新登录"; }
    SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
}
bool CBodianSource::SaveIdentity(const wstring& config_dir) const
{
    namespace fs = std::filesystem;
    const auto path = fs::path(config_dir) / L"bodian_account.dat";
    auto account = GetAccount();
    if (!account.IsLoggedIn()) { error_code error; fs::remove(path, error); return !error; }
    string bytes = json{{"uid", account.uid}, {"token", account.token}}.dump();
    DATA_BLOB plain{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())}, encrypted{};
    if (!CryptProtectData(&plain, L"BoTapMusic Bodian", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) return false;
    const auto temporary = fs::path(config_dir) / L"bodian_account.dat.tmp";
    ofstream output(temporary, ios::binary | ios::trunc);
    output.write(reinterpret_cast<char*>(encrypted.pbData), encrypted.cbData); output.close();
    bool saved = output && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    LocalFree(encrypted.pbData); SecureZeroMemory(bytes.data(), bytes.size());
    error_code error; fs::remove(temporary, error);
    return saved;
}
string CBodianSource::QuerySignature(const wstring& path, const string& query, const string& body)
{
    string sorted;
    for (unsigned char c : query) if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) sorted += c;
    sort(sorted.begin(), sorted.end());
    return kugou::Md5Hex("kuwotest" + sorted + (body.empty() ? "" : kugou::Md5Hex(body + "kuwotest")) + ToUtf8(path));
}
bool CBodianSource::SignedRequest(const wstring& path, vector<pair<string, string>> params,
    json& response, const string& body, const wchar_t* method)
{
    auto account = GetAccount();
    params.push_back({"uid", account.IsLoggedIn() ? account.uid : "-1"});
    params.push_back({"token", account.token});
    params.push_back({"timestamp", to_string(chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count())});
    string query;
    for (const auto& [key, value] : params) { if (!query.empty()) query += '&'; query += key + '=' + kugou::UrlEncode(value); }
    query += "&sign=" + QuerySignature(path, query, wcscmp(method, L"POST") == 0 ? body : "");
    string headers = "user-agent: Dart/3.3 (dart:io)\r\nplat: win\r\nchannel: W1\r\nbrand: Windows\r\nnet: wifi\r\nver: 1.1.5\r\nsvrver: 13\r\napi-ver: application/json\r\nContent-Type: application/json\r\ndevid: " + m_devid + "\r\n";
    if (account.IsLoggedIn()) headers += "uid: " + account.uid + "\r\ntoken: " + account.token + "\r\n";
    wstring result;
    if (!online::HttpRequest(L"https://bd-api.kuwo.cn" + path + L"?" + FromUtf8(query), body, FromUtf8(headers), result, m_last_error, method)) return false;
    try { response = json::parse(ToUtf8(result)); }
    catch (const json::exception&) { m_last_error = L"波点响应格式无法识别"; return false; }
    if (online::JsonNumber(response, "code") != 200)
    { m_last_error = L"波点服务：" + FromUtf8(online::JsonText(response, "msg")); return false; }
    return true;
}
bool CBodianSource::ApplyAccount(const json& data)
{
    Account account;
    account.uid = online::JsonText(data, "id");
    if (account.uid.empty()) account.uid = online::JsonText(data, "uid");
    if (account.uid.empty()) account.uid = online::JsonText(data, "userId");
    account.token = online::JsonText(data, "token");
    if (account.token.empty()) account.token = online::JsonText(data, "userToken");
    if (!account.IsLoggedIn()) return false;
    SetAccount(account); return true;
}
bool CBodianSource::GetProfile(online::AccountProfile& profile)
{
    profile = {}; m_last_error.clear();
    if (!IsLoggedIn()) { m_last_error = L"请先登录"; return false; }
    // 用户资料走公开信息接口。注意 /api/ucenter/users/login 是登录用的 POST 接口，
    // 用 GET 调它只会返回 500，这里曾因此一直读不到用户名与会员状态。
    const auto account = GetAccount();
    json response;
    if (!SignedRequest(L"/api/ucenter/users/pub/" + FromUtf8(account.uid),
        {{"fromUid", account.uid}, {"platform", "ios"}}, response) || !response.contains("data"))
    {
        if (m_last_error.empty()) m_last_error = L"账号信息读取失败";
        return false;
    }
    profile = online::ReadBodianProfile(response["data"]);
    if (profile.name.empty() && profile.membership.empty()) { m_last_error = L"账号信息读取失败"; return false; }
    return true;
}

wstring CBodianSource::GetCoverUrl(const online::Track& track)
{
    if (!track.cover_url.empty()) return track.cover_url;
    if (track.title.empty()) return {};
    vector<online::Track> tracks;
    for (const auto& query : {track.artist + L" " + track.title, track.title})
        if (Search(query, 1, tracks))
            for (const auto& item : tracks)
                if (item.virtual_path == track.virtual_path && !item.cover_url.empty()) return item.cover_url;
    return {};
}

bool CBodianSource::ValidateAccount()
{
    if (!IsLoggedIn()) { m_last_error = L"登录文件缺少有效 uid / token"; return false; }
    json response;
    if (!SignedRequest(L"/api/ucenter/users/login", {}, response)) return false;
    if (response.contains("data") && response["data"].is_object())
    {
        // Some successful refreshes return profile data without issuing a new token.
        const auto& data = response["data"];
        if (ApplyAccount(data) || (data.contains("userInfo") && data["userInfo"].is_object() && !data["userInfo"].empty())) return true;
    }
    m_last_error = L"波点未返回有效账号信息"; return false;
}
bool CBodianSource::GetQrCode(wstring& content)
{
    Logout(); m_qr_key.clear();
    json response;
    if (!SignedRequest(L"/api/ucenter/login/qrCode", {}, response)) return false;
    m_qr_key = response.contains("data") ? FromUtf8(online::JsonText(response["data"], "qrCode")) : L"";
    if (m_qr_key.empty()) { m_last_error = L"波点未返回登录二维码"; return false; }
    content = L"https://bodian-oia.kuwo.cn/bodian/download.html?pageName=login_pc&pt=3&id=" + FromUtf8(kugou::UrlEncode(ToUtf8(m_qr_key)));
    m_qr_created = GetTickCount64(); return true;
}
online::QrStatus CBodianSource::CheckQrCode()
{
    using online::QrStatus;
    if (m_qr_key.empty() || GetTickCount64() - m_qr_created > 180000) return QrStatus::Expired;
    json response;
    if (!SignedRequest(L"/api/ucenter/login/qrCodeStatus", {{"qrCode", ToUtf8(m_qr_key)}}, response)) return QrStatus::Failed;
    if (!response.contains("data") || !response["data"].is_object()) { m_last_error = L"波点二维码状态格式已变化"; return QrStatus::Failed; }
    auto data = response["data"];
    if (ApplyAccount(data)) return QrStatus::Authorized;
    int status = online::JsonNumber(data, "status");
    if (status == 3)
    {
        const string body = json{{"authType", 10}, {"qrCode", ToUtf8(m_qr_key)}}.dump();
        if (!SignedRequest(L"/api/ucenter/users/login", {}, response, body, L"POST")) return QrStatus::Failed;
        if (response.contains("data") && ApplyAccount(response["data"])) return QrStatus::Authorized;
        m_last_error = L"扫码已确认，但波点未返回登录凭据。可从更多菜单导入自己的登录文件。"; return QrStatus::Failed;
    }
    if (status == 2) return QrStatus::Scanned;
    if (status > 3) return QrStatus::Expired;
    return QrStatus::Waiting;
}

bool CBodianSource::Browse(const online::BrowseRequest& request, online::BrowseResult& result)
{
    using namespace online;
    result = {};
    m_last_error.clear();
    if (request.kind == BrowseKind::AlbumSearch) return BrowseAlbums(request.id, request.page, result);
    if (request.kind == BrowseKind::Search)
    {
        if (!IOnlineSource::Browse(request, result)) return false;
        AddAlbums(result, request.id);      // 歌曲之后追加同名专辑
        return true;
    }
    if (request.kind == BrowseKind::Playlists && !IsLoggedIn())
    {
        json response;
        if (!Get(L"/api/service/home/module?moduleId=2&uid=-1&token=", response)) return false;
        if (JsonNumber(response, "code") != 200 || !response.contains("data") || !response["data"].contains("songList")
            || !response["data"]["songList"].is_array()) { m_last_error = L"首页精选歌单暂不可用"; return false; }
        for (const auto& playlist : response["data"]["songList"]) AddBodianPlaylist(result, playlist, L"波点精选歌单");
        return true;
    }
    if (request.kind == BrowseKind::Playlists && IsLoggedIn())
    {
        const auto account = GetAccount();
        if (request.page == 1)
        {
            json fond;
            if (!Get(L"/api/service/playlist/fond?userId=" + FromUtf8(account.uid) + L"&uid=-1&token=", fond)) return false;
            if (JsonNumber(fond, "code") != 200) { m_last_error = L"云歌单读取失败，请重新登录波点音乐"; return false; }
            if (fond.contains("data") && fond["data"].is_object()) AddBodianPlaylist(result, fond["data"], L"我喜欢的音乐");
            json created;
            if (!Get(L"/api/service/playlist/userCreate?userId=" + FromUtf8(account.uid) + L"&uid=-1&token=", created)) return false;
            if (JsonNumber(created, "code") != 200) { m_last_error = L"创建的歌单读取失败"; return false; }
            if (created.contains("data") && created["data"].contains("playLists") && created["data"]["playLists"].is_array())
                for (const auto& item : created["data"]["playLists"]) AddBodianPlaylist(result, item, L"我创建的歌单");
        }
        json collected;
        if (!Get(L"/api/service/collect/4/list?userId=" + FromUtf8(account.uid) + L"&fromUid=" + FromUtf8(account.uid)
            + L"&pn=" + to_wstring((std::max)(1, request.page)) + L"&rn=30&uid=-1&token=", collected)) return false;
        if (JsonNumber(collected, "code") != 200) { m_last_error = L"收藏歌单读取失败"; return false; }
        if (collected.contains("data") && collected["data"].contains("playLists") && collected["data"]["playLists"].is_array())
        {
            for (const auto& item : collected["data"]["playLists"]) AddBodianPlaylist(result, item, L"我收藏的歌单");
            result.has_more = collected["data"]["playLists"].size() >= 30;
        }
        return true;
    }
    if (request.kind == BrowseKind::PlaylistSearch || request.kind == BrowseKind::Playlists || request.kind == BrowseKind::PlaylistTracks)
    {
        wstring path;
        const bool tracks = request.kind == BrowseKind::PlaylistTracks;
        if (tracks)
        {
            wstring id, source;
            if (!ParseBodianPlaylistId(request.id, id, source)) { m_last_error = L"波点歌单编号无效，来源支持 4、5、13"; return false; }
            path = L"/api/service/playlist/" + id + L"/musicList?source=" + source + L"&pn=" + to_wstring((std::max)(1, request.page));
        }
        else path = L"/api/search/playlist/list?keyword=" + FromUtf8(kugou::UrlEncode(ToUtf8(request.id.empty() ? L"精选" : request.id)))
            + L"&pn=" + to_wstring((std::max)(1, request.page) - 1);
        json response;
        if (!Get(path + L"&rn=30&uid=-1&token=", response)) return false;
        if (JsonNumber(response, "code") != 200 || !response.contains("data") || !response["data"].is_object())
        { m_last_error = L"歌单读取失败：" + FromUtf8(JsonText(response, "msg")); return false; }
        const auto& data = response["data"];
        const char* list_key = !tracks ? "resultList" : data.contains("list") ? "list" : "musicList";
        if (!data.contains(list_key) || !data[list_key].is_array()) { m_last_error = L"歌单列表格式已变化"; return false; }
        for (const auto& value : data[list_key])
            if (tracks) AddTrack(result, BodianTrack(value)); else AddBodianPlaylist(result, value);
        result.has_more = data[list_key].size() >= 30;
        int total = JsonNumber(data, "total");
        if (total > 0 && request.page * 30 >= total) result.has_more = false;
        return true;
    }
    if (request.kind == BrowseKind::Charts)
    {
        json ranks;
        if (!Get(L"/api/service/home/module?moduleId=5&uid=-1&token=", ranks)) return false;
        if (JsonNumber(ranks, "code") != 200 || !ranks.contains("data") || !ranks["data"].contains("bangList")
            || !ranks["data"]["bangList"].is_array()) { m_last_error = L"波点排行榜暂不可用"; return false; }
        for (const auto& rank : ranks["data"]["bangList"])
        {
            const auto id = JsonText(rank, "id");
            if (id.empty() || id.find_first_not_of("0123456789") != string::npos) continue;
            BrowseItem item; item.type = BrowseItem::Type::Chart; item.id = L"bang_" + FromUtf8(id);
            item.title = FromUtf8(JsonText(rank, "name")); item.subtitle = L"波波排行榜"; result.items.push_back(std::move(item));
        }
    }
    wstring path;
    bool ranking = request.kind == BrowseKind::ChartTracks && request.id.starts_with(L"bang_");
    switch (request.kind)
    {
    case BrowseKind::Hot: path = L"/api/search/hot/list"; break;
    case BrowseKind::Recommend: path = L"/api/service/resource/recommend"; break;
    case BrowseKind::Charts: path = L"/api/service/category/list"; break;
    case BrowseKind::ChartTracks:
        if (!IsServiceId(request.id)) { m_last_error = L"分类编号无效"; return false; }
        path = ranking ? L"/api/service/bang/" + request.id.substr(5) + L"/musics" : L"/api/service/category/" + request.id + L"/musics"; break;
    default: m_last_error = L"波点暂不支持此页面"; return false;
    }
    path += L"?uid=-1&token=&pn=" + to_wstring((std::max)(1, request.page) - (ranking ? 0 : 1)) + L"&rn=30";
    json response;
    if (!Get(path, response)) return false;
    if (JsonNumber(response, "code") != 200 || !response.contains("data") || !response["data"].is_object())
    {
        m_last_error = L"波点服务返回错误：" + FromUtf8(JsonText(response, "msg"));
        return false;
    }
    const auto& data = response["data"];
    if (request.kind == BrowseKind::Charts)
    {
        if (!data.contains("categories") || !data["categories"].is_array()) { m_last_error = L"分类数据格式已变化"; return false; }
        for (const auto& group : data["categories"])
        {
            if (!group.contains("subCategories") || !group["subCategories"].is_array()) continue;
            for (const auto& category : group["subCategories"])
            {
                BrowseItem item;
                item.type = BrowseItem::Type::Chart;
                item.id = FromUtf8(JsonText(category, "id"));
                item.title = FromUtf8(JsonText(category, "name"));
                item.subtitle = FromUtf8(JsonText(group, "name"));
                if (IsServiceId(item.id)) result.items.push_back(item);
            }
        }
        return true;
    }
    const char* list_key = ranking ? "musics" : request.kind == BrowseKind::Recommend ? "resourceList" : request.kind == BrowseKind::ChartTracks ? "musicList" : "resultList";
    if (!data.contains(list_key) || !data[list_key].is_array()) { m_last_error = L"列表数据格式已变化"; return false; }
    for (const auto& value : data[list_key])
    {
        if (request.kind == BrowseKind::Hot)
        {
            BrowseItem item;
            item.type = BrowseItem::Type::Keyword;
            item.id = item.title = FromUtf8(JsonText(value, "key"));
            item.subtitle = L"双击搜索";
            if (!item.title.empty()) result.items.push_back(item);
        }
        else AddTrack(result, BodianTrack(value));
    }
    result.has_more = request.kind == BrowseKind::ChartTracks && data[list_key].size() >= 30;
    if (ranking && JsonNumber(data, "total") > 0 && request.page * 30 >= JsonNumber(data, "total")) result.has_more = false;
    return true;
}

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

bool CBodianSource::Get(const wstring& path, json& out_json)
{
    wstring authenticated = path;
    const auto auth_pos = authenticated.find(L"uid=-1&token=");
    if (auth_pos != wstring::npos) authenticated.replace(auth_pos, wcslen(L"uid=-1&token="), AuthQuery());
    wstring url = wstring(L"https://") + API_HOST + authenticated;

    // 这一组请求头是实测出来的，服务端靠它们识别客户端。
    // 缺 plat / channel / ver / brand 会返回 402，缺 user-agent 会返回「歌曲已下线」。
    // 这里直接用 WinHTTP 发送，以便完整控制请求头。
    string headers;
    headers += "plat: ar\r\n";                      // 安卓端
    headers += "channel: huawei\r\n";
    headers += "brand: huawei mate40\r\n";
    headers += "devid: " + m_devid + "\r\n";
    headers += "ver: 1.1.9\r\n";
    headers += "appuid: 123849114429\r\n";
    headers += "api-ver: application/json\r\n";
    headers += "net: mobile\r\n";
    headers += "user-agent: " + string(BODIAN_USER_AGENT) + "\r\n";

    wstring result;
    if (!online::HttpRequest(url, "", FromUtf8(headers), result, m_last_error))
    {
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

    m_last_error.clear();
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

    if (!response.contains("data") || !response["data"].contains("resultList") || !response["data"]["resultList"].is_array())
    {
        m_last_error = L"接口返回的结构与预期不符";
        return false;
    }

    for (const auto& item : response["data"]["resultList"])
    {
        auto track = online::BodianTrack(item);
        if (track.IsValid()) result.push_back(std::move(track));
    }

    m_last_error.clear();
    return true;
}

// 搜索结果里补上专辑条目。波点的歌曲搜索不返回专辑实体，综合搜索的
// albumPage 才有；条目 id 里存「艺术家 + 专辑名」，点进去会用它再搜一次歌曲。
void CBodianSource::AddAlbums(online::BrowseResult& result, const wstring& keyword)
{
    using namespace online;
    if (keyword.empty()) return;

    json response;
    const wstring path = L"/api/search/comprehensive/list?pn=0&rn=6&keyword=" +
        FromUtf8(kugou::UrlEncode(ToUtf8(keyword))) + L"&uid=-1&token=";
    if (!Get(path, response)) return;
    if (JsonNumber(response, "code") != 200 || !response.contains("data")) return;

    const auto& data = response["data"];
    if (!data.contains("albumPage") || !data["albumPage"].is_array()) return;

    for (const auto& album : data["albumPage"])
    {
        const wstring name = FromUtf8(JsonText(album, "name"));
        if (name.empty()) continue;
        const wstring artist = FromUtf8(JsonText(album, "artist"));
        BrowseItem item;
        item.type = BrowseItem::Type::Album;
        item.title = name;
        item.subtitle = artist.empty() ? L"专辑" : L"专辑 · " + artist;
        item.id = artist.empty() ? name : artist + L" " + name;
        result.items.push_back(std::move(item));
    }
}

// 只搜专辑。和追加逻辑共用同一套字段，但要能如实报告失败，
// 否则用户切到「搜专辑」后只会看到一片空白。
bool CBodianSource::BrowseAlbums(const wstring& keyword, int page, online::BrowseResult& result)
{
    using namespace online;
    result = {};
    m_last_error.clear();
    if (keyword.empty()) { m_last_error = L"请输入专辑名或歌手"; return false; }

    json response;
    const wstring path = L"/api/search/comprehensive/list?pn=" + to_wstring((std::max)(1, page) - 1) +
        L"&rn=30&keyword=" + FromUtf8(kugou::UrlEncode(ToUtf8(keyword))) + L"&uid=-1&token=";
    if (!Get(path, response)) return false;
    if (JsonNumber(response, "code") != 200 || !response.contains("data"))
    { m_last_error = L"专辑搜索失败，请稍后重试"; return false; }

    const auto& data = response["data"];
    if (!data.contains("albumPage") || !data["albumPage"].is_array())
    { m_last_error = L"专辑搜索返回的结构与预期不符"; return false; }

    for (const auto& album : data["albumPage"])
    {
        const wstring name = FromUtf8(JsonText(album, "name"));
        if (name.empty()) continue;
        const wstring artist = FromUtf8(JsonText(album, "artist"));
        BrowseItem item;
        item.type = BrowseItem::Type::Album;
        item.title = name;
        item.subtitle = artist.empty() ? L"专辑" : L"专辑 · " + artist;
        item.id = artist.empty() ? name : artist + L" " + name;
        result.items.push_back(std::move(item));
    }
    if (result.items.empty()) m_last_error = L"没有找到相关专辑";
    result.has_more = data["albumPage"].size() >= 30;
    return !result.items.empty();
}

// ---------------------------------------------------------------------------
// 看广告领免费畅听
// ---------------------------------------------------------------------------

// 这个接口只认匿名安卓客户端的头组合：换成浏览接口那套头、或带上真实账号，
// 都会返回 400 PARAM_ERROR。所以这里整套照搬客户端参数，不掺杂账号信息。
static const char* AD_WATCH_PATH = "/api/service/advert/watch";
static const char* AD_WATCH_BODY = "{\"type\":5,\"subType\":5,\"musicId\":0,\"adToken\":\"\"}";
static const char* AD_QIMEI36 = "1e9970cbcdc20a031dee9f37100017e1840e";
// 客户端长期沿用的固定签名。正常情况下用当场生成的时间戳签名，
// 只有服务端拒绝时才退回这一条。
static const char* AD_FIXED_QUERY = "uid=-1&token=&timestamp=1724306124436&sign=15a676d66285117ad714e8c8371691da";
static constexpr int AD_FREE_SECONDS = 1800;            // 一次上报换 30 分钟
static constexpr ULONGLONG AD_RETRY_INTERVAL = 30 * 1000;

bool AdRewardEnabled()
{
    lock_guard<mutex> guard(g_ad_state_lock);
    return g_ad_enabled;
}

void SetAdRewardEnabled(bool enabled, const wstring& settings_path)
{
    {
        lock_guard<mutex> guard(g_ad_state_lock);
        g_ad_enabled = enabled;
        g_ad_enabled_loaded = true;
        if (!enabled) g_ad_expire_tick = 0;
    }
    if (!settings_path.empty())
        WritePrivateProfileStringW(L"ad_reward", L"enabled", enabled ? L"1" : L"0", settings_path.c_str());
}

int AdFreeRemainSeconds()
{
    lock_guard<mutex> guard(g_ad_state_lock);
    const ULONGLONG now = GetTickCount64();
    return g_ad_expire_tick > now ? static_cast<int>((g_ad_expire_tick - now) / 1000) : 0;
}

bool CBodianSource::ClaimAdFreeTime(int& seconds, wstring& detail)
{
    seconds = 0; detail.clear();
    const wstring path = FromUtf8(AD_WATCH_PATH);
    const string body = AD_WATCH_BODY;
    const string headers = "user-agent: Dart/2.19 (dart:io)\r\nplat: ar\r\nchannel: aliopen\r\nver: 3.9.0\r\n"
        "api-ver: application/json\r\nnet: mobile\r\ncontent-type: application/json; charset=utf-8\r\n"
        "devid: " + m_ad_devid + "\r\nqimei36: " + string(AD_QIMEI36) + "\r\n";

    json response;
    auto post = [&](const wstring& url) {
        wstring raw;
        if (!online::HttpRequest(url, body, FromUtf8(headers), raw, m_last_error, L"POST")) return false;
        m_last_response = raw;
        try { response = json::parse(ToUtf8(raw)); }
        catch (const json::exception&) { m_last_error = L"波点响应格式无法识别"; return false; }
        return true;
    };

    const string stamp = to_string(chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count());
    const string query = "uid=-1&token=&timestamp=" + stamp;
    const wstring fresh_url = wstring(L"https://") + API_HOST + path + L"?" + FromUtf8(query)
        + L"&sign=" + FromUtf8(QuerySignature(path, query, body));

    bool accepted = post(fresh_url) && online::JsonNumber(response, "code") == 200;
    if (!accepted)
    {
        const wstring fixed_url = wstring(L"https://") + API_HOST + path + L"?" + FromUtf8(AD_FIXED_QUERY);
        accepted = post(fixed_url) && online::JsonNumber(response, "code") == 200;
    }
    if (!accepted)
    {
        if (m_last_error.empty())
        {
            wstring msg = FromUtf8(online::JsonText(response, "msg"));
            m_last_error = msg.empty() ? L"看广告领畅听没有成功，请稍后重试" : L"波点服务：" + msg;
        }
        return false;
    }

    if (response.contains("data")) seconds = static_cast<int>(online::JsonNumber(response["data"], "expireTime"));
    if (seconds <= 0) seconds = AD_FREE_SECONDS;
    const int minutes = (std::max)(1, seconds / 60);
    detail = L"已观看广告，获得 " + to_wstring(minutes) + L" 分钟会员畅听";
    {
        lock_guard<mutex> guard(g_ad_state_lock);
        g_ad_expire_tick = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
        g_ad_attempt_tick = GetTickCount64();
        g_ad_claimed_once = true;
    }
    m_last_error.clear();
    return true;
}

bool CBodianSource::QueryAdFreeInfo(int& watched, int& need_watch, int& remain_seconds)
{
    watched = need_watch = remain_seconds = 0;
    json response;
    if (!Get(L"/api/advert/free/config?uid=-1&token=", response)) return false;
    if (online::JsonNumber(response, "code") != 200 || !response.contains("data") || !response["data"].is_object()) return false;
    const auto& data = response["data"];
    if (data.contains("allDayConfig") && data["allDayConfig"].is_object())
    {
        watched = static_cast<int>(online::JsonNumber(data["allDayConfig"], "watched"));
        need_watch = static_cast<int>(online::JsonNumber(data["allDayConfig"], "needWatch"));
    }
    if (data.contains("freeAdInfo") && data["freeAdInfo"].is_object())
        remain_seconds = static_cast<int>(online::JsonNumber(data["freeAdInfo"], "freeAdRemainSeconds"));
    if (data.contains("freeInfo") && data["freeInfo"].is_object())
        remain_seconds = (std::max)(remain_seconds, static_cast<int>(online::JsonNumber(data["freeInfo"], "remainFreeSeconds")));
    return true;
}

bool CBodianSource::EnsureAdFreeTime(int& remain_seconds)
{
    remain_seconds = 0;
    if (!AdRewardEnabled()) return false;
    const ULONGLONG now = GetTickCount64();
    {
        lock_guard<mutex> guard(g_ad_state_lock);
        if (g_ad_expire_tick > now)
        {
            remain_seconds = static_cast<int>((g_ad_expire_tick - now) / 1000);
            return true;
        }
        // 上报失败要退避，否则每次点歌都会打一次接口
        if (g_ad_attempt_tick && now - g_ad_attempt_tick < AD_RETRY_INTERVAL) return false;
        g_ad_attempt_tick = now;
    }
    int seconds = 0; wstring detail;
    if (!ClaimAdFreeTime(seconds, detail)) return false;
    remain_seconds = seconds;
    return true;
}

// 播放接口的匿名客户端参数。和浏览接口那套（plat=ar + channel=huawei + ver=1.1.9）
// 不是一回事：这套 channel=aliopen 的组合才能过付费曲的权限判定。
static const char* AD_PLAY_HEADERS_TAIL = "\r\napi-ver: application/json\r\nnet: mobile\r\ncontent-type: application/json; charset=utf-8\r\n";

bool CBodianSource::ResolveAdFreePlayUrl(const wstring& rid, wstring& url)
{
    url.clear();
    const string id = ToUtf8(rid);
    const wstring path = L"/api/play/music/v2/audioUrl";
    const string headers = "user-agent: Dart/2.19 (dart:io)\r\nplat: ar\r\nchannel: aliopen\r\nver: 3.9.0"
        + string(AD_PLAY_HEADERS_TAIL) + "devid: " + m_ad_devid + "\r\nqimei36: " + string(AD_QIMEI36) + "\r\n";

    struct Quality { const wchar_t* name; const char* br; };
    static const Quality qualities[] = {
        { L"无损", "2000kflac" },
        { L"320k", "320kmp3" },
        { L"128k", "128kmp3" },
    };

    bool claimed = false;   // 单次解析只补领一次权益，避免反复打上报接口
    wstring failure;
    for (const auto& quality : qualities)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const string stamp = to_string(chrono::duration_cast<chrono::milliseconds>(
                chrono::system_clock::now().time_since_epoch()).count());
            const string query = "br=" + string(quality.br) + "&musicId=" + id + "&timestamp=" + stamp;
            const wstring request_url = wstring(L"https://") + API_HOST + path + L"?" + FromUtf8(query)
                + L"&sign=" + FromUtf8(QuerySignature(path, query, ""));

            wstring raw, error;
            json response;
            if (!online::HttpRequest(request_url, "", FromUtf8(headers), raw, error))
            {
                if (failure.empty()) failure = error;
                break;
            }
            try { response = json::parse(ToUtf8(raw)); }
            catch (const json::exception&) { break; }

            const int code = static_cast<int>(online::JsonNumber(response, "code"));
            if (code == 200 && response.contains("data") && response["data"].is_object())
            {
                string audio = online::JsonText(response["data"], "audioHttpsUrl");
                if (audio.empty()) audio = online::JsonText(response["data"], "audioUrl");
                if (audio.starts_with("http://") || audio.starts_with("https://"))
                {
                    const int bitrate = static_cast<int>(online::JsonNumber(response["data"], "bitrate"));
                    m_quality_note = wcscmp(quality.name, L"无损") == 0
                        ? wstring(L"当前播放：无损")
                        : wstring(L"当前播放：") + quality.name + (bitrate > 0 ? L"（" + to_wstring(bitrate) + L" kbps）" : L"");
                    m_last_error.clear();
                    url = FromUtf8(audio);
                    return true;
                }
            }
            // 20018 是付费墙：先补一次畅听权益再重试同一档；其余错误直接换下一档
            if (code == 20018 && !claimed && AdRewardEnabled())
            {
                claimed = true;
                int remain = 0;
                if (EnsureAdFreeTime(remain)) continue;
            }
            if (failure.empty())
            {
                const wstring msg = FromUtf8(online::JsonText(response, "msg"));
                failure = msg.empty() ? L"新版播放接口返回 " + to_wstring(code) : msg;
            }
            break;
        }
    }
    if (!failure.empty()) m_last_response = failure;
    return false;
}


// ---------------------------------------------------------------------------
// 听歌赚金币
// ---------------------------------------------------------------------------

// 这组接口有版本门槛：低于 6.0.3 会返回 low version，所以单独用一套头，
// 不去动浏览与播放接口用的那套（它们依赖 1.1.9）。
static const char* EARNING_USER_AGENT = "Dart/3.3 (dart:io)";
static const char* EARNING_VERSION = "6.0.3";

bool CBodianSource::EarningRequest(const wstring& path, const wstring& extra_query,
    const string& body, json& response, const wchar_t* method)
{
    auto account = GetAccount();
    if (!account.IsLoggedIn()) { m_last_error = L"请先登录波点音乐再领取"; return false; }

    // uid / token / timestamp 与业务参数一起参与签名，顺序不影响结果
    string query = extra_query.empty() ? string() : ToUtf8(extra_query) + "&";
    query += "uid=" + account.uid;
    query += "&token=" + kugou::UrlEncode(account.token);
    query += "&timestamp=" + to_string(chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count());

    const string signed_query = query + "&sign=" + QuerySignature(path, query, body);
    const string headers = "user-agent: " + string(EARNING_USER_AGENT) + "\r\nplat: win\r\nchannel: W1\r\n"
        "brand: Windows\r\nnet: wifi\r\nver: " + string(EARNING_VERSION) + "\r\nsvrver: 13\r\n"
        "api-ver: application/json\r\nContent-Type: application/json\r\n"
        "devid: " + m_devid + "\r\nuid: " + account.uid + "\r\ntoken: " + account.token + "\r\n";

    wstring result;
    const wstring url = wstring(L"https://") + API_HOST + path + L"?" + FromUtf8(signed_query);
    if (!online::HttpRequest(url, body, FromUtf8(headers), result, m_last_error, method)) return false;
    try { response = json::parse(ToUtf8(result)); }
    catch (const json::exception&) { m_last_error = L"波点响应格式无法识别"; return false; }
    return true;
}

bool CBodianSource::GetEarningTasks(vector<EarningTask>& tasks)
{
    tasks.clear();
    m_last_error.clear();
    json response;
    if (!EarningRequest(L"/api/advert/earning/app/task/info", L"taskType=listen", "", response, L"GET")) return false;
    if (!response.contains("data") || !response["data"].is_object()) return false;
    const auto& data = response["data"];
    if (!data.contains("list") || !data["list"].is_array())
    {
        m_last_error = L"波点没有返回听歌任务列表";
        return false;
    }
    for (const auto& item : data["list"])
    {
        EarningTask task;
        task.id = static_cast<int>(online::JsonNumber(item, "id"));
        task.stage = static_cast<int>(online::JsonNumber(item, "stage"));
        task.gold = static_cast<int>(online::JsonNumber(item, "gold"));
        task.status = static_cast<int>(online::JsonNumber(item, "status"));
        if (task.id > 0) tasks.push_back(task);
    }
    return true;
}

bool CBodianSource::ClaimEarningTask(const wstring& task_type, int id, bool& awarded)
{
    awarded = false;
    json payload;
    payload["taskType"] = ToUtf8(task_type);
    payload["id"] = id;

    json response;
    if (!EarningRequest(L"/api/advert/earning/h5/doTask", L"", payload.dump(), response, L"POST")) return false;

    const int code = static_cast<int>(online::JsonNumber(response, "code"));
    if (code == 200) { awarded = true; return true; }
    // code 1 表示这条任务已经领过，或者还没轮到（例如后面几天的签到），不算错误
    if (code == 1) return true;
    m_last_error = L"领取任务 " + to_wstring(id) + L" 失败：" + FromUtf8(online::JsonText(response, "msg"));
    return false;
}

bool CBodianSource::RunEarningCycle(int& balance, int& claimed_count, int& claimed_gold, wstring& detail)
{
    balance = 0; claimed_count = 0; claimed_gold = 0;
    detail.clear();
    m_last_error.clear();

    // 主页接口同时给出余额与签到任务
    json home;
    if (!EarningRequest(L"/api/advert/earning/h5/home", L"", "", home, L"GET"))
    {
        detail = m_last_error.empty() ? L"金币主页读取失败" : m_last_error;
        return false;
    }
    if (!home.contains("data") || !home["data"].is_object())
    {
        detail = L"金币主页返回内容无法识别";
        return false;
    }
    const auto& data = home["data"];
    if (data.contains("userInfo") && data["userInfo"].is_object())
        balance = static_cast<int>(online::JsonNumber(data["userInfo"], "balance"));

    // 听歌任务：九级阶梯，服务端不核对真实时长，逐个上报即可
    vector<EarningTask> tasks;
    if (GetEarningTasks(tasks))
    {
        for (const auto& task : tasks)
        {
            bool awarded = false;
            if (!ClaimEarningTask(L"listen", task.id, awarded)) break;
            if (awarded) { ++claimed_count; claimed_gold += task.gold; }
        }
    }

    // 签到任务：只有轮到的那一天会发放，其余返回 code 1 被跳过
    if (data.contains("popup") && data["popup"].is_object())
    {
        const auto& popup = data["popup"];
        if (popup.contains("list") && popup["list"].is_array())
        {
            for (const auto& item : popup["list"])
            {
                const int id = static_cast<int>(online::JsonNumber(item, "id"));
                const int gold = static_cast<int>(online::JsonNumber(item, "gold"));
                if (id <= 0) continue;
                bool awarded = false;
                if (ClaimEarningTask(L"sign", id, awarded) && awarded) { ++claimed_count; claimed_gold += gold; }
            }
        }
    }

    // 复查余额，供界面展示
    json after;
    if (EarningRequest(L"/api/advert/earning/h5/home", L"", "", after, L"GET"))
    {
        if (after.contains("data") && after["data"].is_object() && after["data"].contains("userInfo"))
            balance = static_cast<int>(online::JsonNumber(after["data"]["userInfo"], "balance"));
    }

    detail = claimed_count > 0
        ? L"领到 " + to_wstring(claimed_count) + L" 项共 " + to_wstring(claimed_gold) + L" 金币，余额 " + to_wstring(balance)
        : L"今天的金币已经领完，余额 " + to_wstring(balance);
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
    if (rid.empty() || rid.size() > 18 || rid.find_first_not_of(L"0123456789") != wstring::npos)
        return wstring();

    // 先走吃畅听权益的匿名新版接口：付费曲能直接拿到无损，而账号态的同类请求
    // 会被判成 20018。取不到就落回下面的原有链路，行为与改动前一致。
    {
        wstring adfree;
        if (ResolveAdFreePlayUrl(rid, adfree)) return adfree;
    }

    if (IsLoggedIn())
    {
        json response;
        const string id = ToUtf8(rid);
        const string rights_body = json{{"musicId", stoll(id)}, {"freeSign", ""}}.dump();
        if (!SignedRequest(L"/api/play/music/v2/checkRight", {{"musicId", id}, {"freeSign", ""}}, response, rights_body)) return {};
        const int right = response.contains("data") ? online::JsonNumber(response["data"], "status") : 0;
        if (right != 1 && right != 4)
        { m_last_error = right == 3 ? L"当前波点账号仅有试听权限，未播放试听片段" : L"波点账号没有这首歌曲的完整播放权限"; return {}; }
        // 按音质从高到低试，失败就往下退。把每次失败的原因记下来，
        // 最后告诉用户实际用的是哪一档、上面几档为什么没成 —— 否则用户只会看到「怎么没有无损」。
        const vector<pair<wstring, pair<string, string>>> qualities = {
            { L"无损", { "flac", "2000kflac" } },
            { L"320k", { "mp3", "320kmp3" } },
            { L"128k", { "mp3", "128kmp3" } },
        };
        wstring first_failure;
        for (size_t qi = 0; qi < qualities.size(); ++qi)
        {
            const wstring& quality_name = qualities[qi].first;
            const auto& [format, br] = qualities[qi].second;
            const string body = json{{"devId", m_devid}, {"musicId", stoll(id)}, {"format", format}, {"br", br}, {"freeSign", ""}}.dump();
            if (!SignedRequest(L"/api/play/music/v2/audioUrl", {{"devId", m_devid}, {"musicId", id}, {"format", format}, {"br", br}, {"freeSign", ""}}, response, body))
            {
                if (first_failure.empty()) first_failure = m_last_error;
                continue;
            }
            if (!response.contains("data"))
            {
                if (first_failure.empty()) first_failure = quality_name + L"档没有返回数据";
                continue;
            }
            auto url = online::JsonText(response["data"], "audioHttpsUrl");
            if (url.empty()) url = online::JsonText(response["data"], "audioUrl");
            if (url.starts_with("https://") || url.starts_with("http://"))
            {
                m_quality_note = qi == 0 ? L"当前播放：无损" : (L"当前播放：" + quality_name + L"（无损未获取到：" + first_failure + L"）");
                m_last_error.clear();
                return FromUtf8(url);
            }
        }
        if (m_last_error.empty()) m_last_error = L"波点没有返回可播放的音频地址";
        return {};
    }

    wstring path = L"/api/service/music/audioUrl/" + rid + L"?uid=-1&token=";

    json response;
    if (!Get(path, response))
        return wstring();

    int code = response.value("code", 0);

    // 付费墙就是「自动观看广告领会员」的落点：先换一次 30 分钟畅听权益，再重试同一首。
    // 权益是按设备与网络发放的，重试时服务端会直接放行。
    if (code == 20018 && AdRewardEnabled())
    {
        int remain = 0;
        if (EnsureAdFreeTime(remain) && Get(path, response) && response.value("code", 0) == 200)
            code = 200;
    }

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


// ---------------------------------------------------------------------------
// 逐字歌词（LRCX）
// ---------------------------------------------------------------------------

namespace
{
// [mm:ss.xxx] 转毫秒。成功时 pos 停在 ']' 之后。
bool ParseLrcxTime(const wstring& line, size_t pos, int& ms)
{
    if (pos + 10 > line.size() || line[pos] != L'[') return false;
    const size_t close = line.find(L']', pos);
    if (close == wstring::npos || close - pos < 10) return false;
    const wstring tag = line.substr(pos + 1, close - pos - 1);
    if (tag[2] != L':' || tag[5] != L'.') return false;
    for (size_t i = 0; i < tag.size(); ++i)
    {
        if (i == 2 || i == 5) continue;
        if (tag[i] < L'0' || tag[i] > L'9') return false;
    }
    const int mm = (tag[0] - L'0') * 10 + (tag[1] - L'0');
    const int ss = (tag[3] - L'0') * 10 + (tag[4] - L'0');
    int frac = 0;
    for (size_t i = 6; i < tag.size() && i < 9; ++i) frac = frac * 10 + (tag[i] - L'0');
    const int digits = static_cast<int>((std::min)(tag.size(), static_cast<size_t>(9)) - 6);
    const int scale[] = { 1, 100, 10, 1 };
    ms = ((mm * 60) + ss) * 1000 + frac * scale[digits];
    return true;
}

// LRCX 每首歌的 [kuwo:NNN] 是本文件的时间权重（按八进制读），NNN 拆成两个系数。
// <a,b> 是「字位置」和「字时长」的和差编码：
//     a = k1 * 位置 + k2 * 时长
//     b = k1 * 位置 - k2 * 时长
// 反解就是 位置 = |a + b| / (2 * k1)、时长 = |a - b| / (2 * k2)，两者都是毫秒。
// 拿不到权重就没法还原时间轴，返回 false 让调用方退回普通歌词。
//
// 实测 23 首：这样解出来的字位置行内 100% 单调、87% 的字和下一个字正好首尾相接、
// 每行首字位置恰好是 0。把第一个字段除以 10 当偏移用则会得到两成倒挂的行 ——
// 句首句尾看着没错，句内填色会乱，所以不能那么解。
bool ParseLrcxWeights(const wstring& text, int& k1, int& k2)
{
    const wstring marker = L"[kuwo:";
    const size_t begin = text.find(marker);
    if (begin == wstring::npos) return false;
    const size_t end = text.find(L']', begin + marker.size());
    if (end == wstring::npos || end == begin + marker.size()) return false;

    int weight = 0;
    for (size_t i = begin + marker.size(); i < end; ++i)
    {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'7') return false;     // 八进制里不会出现 8/9
        weight = weight * 8 + (c - L'0');
        if (weight > 077777) return false;
    }
    k1 = weight / 10;
    k2 = weight % 10;
    return k1 > 0 && k2 > 0;
}

// 和差编码的反解，四舍五入到毫秒
int LrcxMs(int value, int divisor)
{
    if (value < 0) value = -value;
    return (value + divisor / 2) / divisor;
}

// 从 line 的 pos 处解析 <a,b>字 序列，解出每个字相对本行标签的起始和结束毫秒
bool ParseLrcxWords(const wstring& line, size_t pos, int k1, int k2, vector<wstring>& texts,
    vector<int>& starts, vector<int>& ends, bool& word_timed)
{
    texts.clear(); starts.clear(); ends.clear(); word_timed = false;
    while (pos < line.size() && line[pos] == L'<')
    {
        const size_t close = line.find(L'>', pos);
        if (close == wstring::npos) break;
        const wstring tag = line.substr(pos + 1, close - pos - 1);
        const size_t comma = tag.find(L',');
        if (comma == wstring::npos) break;
        const int a = _wtoi(tag.substr(0, comma).c_str());
        const int b = _wtoi(tag.substr(comma + 1).c_str());
        if (a != 0 || b != 0) word_timed = true;

        const size_t next = line.find(L'<', close + 1);
        texts.push_back(line.substr(close + 1, next == wstring::npos ? wstring::npos : next - close - 1));
        starts.push_back(LrcxMs(a + b, 2 * k1));
        ends.push_back(starts.back() + LrcxMs(a - b, 2 * k2));
        pos = (next == wstring::npos) ? line.size() : next;
    }
    return !texts.empty();
}

wstring LrcxStamp(int ms)
{
    if (ms < 0) ms = 0;
    wchar_t buffer[24]{};
    swprintf_s(buffer, L"%02d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return buffer;
}
} // namespace

// 把 LRCX 转成播放器认的扩展 LRC：
//   [行绝对时间]<字绝对时间>字<字结束时间>…
// 每个字的位置和时长都由 [kuwo:] 的权重解出来（见 ParseLrcxWeights），所以字与字
// 之间的空隙能补成空段，播放器的填色会停在字尾，而不是一路填到下一个字开始唱。
// 翻译行紧跟原文、用同一个行时间戳，播放器据此配成原文加译文。
// 整首都拿不到可用的逐字时间时返回空串，由调用方回退到普通歌词。
wstring LrcxToExtendedLyric(const string& lrcx_utf8)
{
    if (lrcx_utf8.empty()) return wstring();

    const wstring text = FromUtf8(lrcx_utf8);
    // 每首歌的时间权重；没有它就没法还原时间轴
    int weight_pos = 0, weight_len = 0;
    if (!ParseLrcxWeights(text, weight_pos, weight_len)) return wstring();

    struct Row { int ms{}; vector<wstring> texts; vector<int> starts; vector<int> ends; bool word_timed{}; };
    vector<Row> rows;
    size_t begin = 0;
    while (begin <= text.size())
    {
        const size_t end = text.find(L'\n', begin);
        wstring line = text.substr(begin, end == wstring::npos ? wstring::npos : end - begin);
        begin = (end == wstring::npos) ? text.size() + 1 : end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;

        int ms = 0;
        if (!ParseLrcxTime(line, 0, ms)) continue;
        const size_t close = line.find(L']');
        Row row; row.ms = ms;
        if (!ParseLrcxWords(line, close + 1, weight_pos, weight_len, row.texts, row.starts, row.ends, row.word_timed))
            continue;
        rows.push_back(std::move(row));
    }

    struct Entry
    {
        int ms{};                   // 行时间标签上的时间
        vector<wstring> texts;      // 逐字文本，空表示这行没有可用的逐字时间
        vector<int> starts;         // 每个字相对行标签的起始毫秒，单调不减
        vector<int> ends;           // 每个字相对行标签的结束毫秒，不早于起始
        wstring text;               // 没有逐字时间时的整行文本
        wstring translation;
    };
    vector<Entry> entries;
    bool has_word_timing = false;
    // 用下标记录待配对的原文行。不能用指针：entries 会随着 push_back 扩容，
    // 之前存下来的指针会失效，翻译就被写到已经释放的内存里去了。
    size_t pending_index = 0;
    bool has_pending = false;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        Row& row = rows[i];
        wstring joined;
        for (const auto& t : row.texts) joined += t;

        if (!row.word_timed)
        {
            // <0,0> 行是酷我放译文的地方：每句原文后面跟一句译文，但译文的时间标签
            // 标的是下一句原文的时间 —— 和下一行共用同一个时间标签就是它的特征。
            // 它不参与自己的定位，而是挂到上一句原文上，播放器把「时间戳相同的
            // 两行」当成原文加译文。
            const bool translation = (i + 1 >= rows.size() || rows[i + 1].ms == row.ms);
            if (translation)
            {
                // 整行空白就当作没有翻译
                if (has_pending && joined.find_first_not_of(L" \t\u3000") != wstring::npos)
                    entries[pending_index].translation = joined;
                has_pending = false;
                // 前面没有原文可挂时（整首第一个标签行就是译文槽位，实测 23 首里
                // 有 3 首这样）必须丢掉。留着会多出一句空歌词，而且它和紧跟的原文
                // 行时间戳接近，容易被当成那句原文的译文，把原文顶掉。
                continue;
            }
            // 不共用时间标签的空标签行是这句本身没有逐字时间：照常输出，
            // 只是不带逐字标签，播放器会按普通歌词行处理。
        }

        // 兜底：位置单调不减、结束不早于开始。按实测数据本来就成立（相邻字 87%
        // 正好首尾相接，没有一处重叠），留这一步是防止个别数据让播放器按相邻差
        // 算时长时出现负值，把整行填色算崩。
        int previous_end = 0;
        for (size_t w = 0; w < row.starts.size(); ++w)
        {
            row.starts[w] = (std::max)(row.starts[w], previous_end);
            row.ends[w] = (std::max)(row.ends[w], row.starts[w]);
            previous_end = row.ends[w];
        }

        Entry entry;
        entry.ms = row.ms;
        if (row.word_timed)
        {
            entry.texts = row.texts;
            entry.starts = row.starts;
            entry.ends = row.ends;
            has_word_timing = true;
        }
        else
        {
            entry.text = joined;
        }
        entries.push_back(std::move(entry));
        pending_index = entries.size() - 1;
        has_pending = true;
    }

    // 一句带时间的都没有，让调用方回退到普通歌词接口
    if (!has_word_timing) return wstring();

    std::wostringstream out;
    for (size_t e = 0; e < entries.size(); ++e)
    {
        Entry& entry = entries[e];
        // 行时间取首字的绝对时间。播放器解析扩展歌词时会把行内第一个尖括号标签
        // 当作行起始时间（Lyric.cpp 里对 ESLyric 格式的处理），而实测首字位置恒为
        // 0，所以这里就是行标签本身；译文行用同一个时间戳才能配成原文加译文。
        const int line_ms = entry.ms + (entry.texts.empty() ? 0 : entry.starts.front());
        // 下一句的行时间：给最后一个字兜住结束时间，也用来补末尾那段留白
        int next_line_ms = 0;
        if (e + 1 < entries.size())
        {
            const Entry& next = entries[e + 1];
            next_line_ms = next.ms + (next.texts.empty() ? 0 : next.starts.front());
        }

        out << L"[" << LrcxStamp(line_ms) << L"]";
        if (entry.texts.empty())
        {
            // 没有逐字时间的行：整行文本，播放器按普通歌词行处理
            out << entry.text;
        }
        else
        {
            if (e + 1 < entries.size())
                entry.ends.back() = (std::min)(entry.ends.back(), (std::max)(0, next_line_ms - entry.ms));
            for (size_t i = 0; i < entry.texts.size(); ++i)
            {
                out << L"<" << LrcxStamp(entry.ms + entry.starts[i]) << L">" << entry.texts[i];
                // 只在字尾和下一个字头之间真有停顿（或这是最后一个字）时补结束
                // 标签：否则会多出零长度的空段，白白多占标签
                const bool gap_after = (i + 1 == entry.texts.size())
                    || (entry.starts[i + 1] > entry.ends[i]);
                if (gap_after)
                    out << L"<" << LrcxStamp(entry.ms + entry.ends[i]) << L">";
            }
            // 最后一个字唱完到下一句之间也是留白，补一个空段让填色停在字尾
            if (e + 1 < entries.size() && next_line_ms > entry.ms + entry.ends.back())
                out << L"<" << LrcxStamp(next_line_ms) << L">";
        }
        out << L"\n";
        if (!entry.translation.empty())
            out << L"[" << LrcxStamp(line_ms) << L"]" << entry.translation << L"\n";
    }
    return out.str();
}

// 逐字歌词走独立的 mlyric 域名：q 是 base64 的参数串，不需要歌名歌手，
// 只用 rid 就能定位。拿不到时回退到普通歌词（中文老歌可能没有逐字版本）。
bool CBodianSource::FetchWordLyric(const wstring& rid, online::Lyric& result)
{
    const string plain = "type=lyric&trans_type=&req=2&lrcx=1&rid=" + ToUtf8(rid) + "&corp=kuwo&fromchannel=bodian";
    const string query = "f=bodian&timestamp=" + to_string(chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count()) +
        "&q=" + kugou::UrlEncode(kugou::EncodeBase64(plain));

    const wstring url = FromUtf8("https://") + L"mlyric.kuwo.cn/mobi.s?" + FromUtf8(query);
    const wstring headers = L"user-agent: Dart/3.3 (dart:io)\r\n";
    wstring raw;
    if (!online::HttpRequest(url, "", headers, raw, m_last_error)) return false;

    json response;
    try { response = json::parse(ToUtf8(raw)); }
    catch (const json::exception&) { return false; }
    if (online::JsonNumber(response, "code") != 200) return false;
    // 客户端的逐字标志：0 表示这首没有逐字版本。真正的兜底在下面 —— 转换不出
    // 逐字时间就返回 false。实测这个字段并不可靠（整句 LRC 也可能回 1），所以
    // 不能只看它。
    if (online::JsonNumber(response, "lrcx") != 1) return false;
    if (!response.contains("data") || !response["data"].is_object()) return false;

    const string content = online::JsonText(response["data"], "content");
    if (content.empty()) return false;

    const wstring converted = LrcxToExtendedLyric(kugou::DecodeBase64(content));
    if (converted.empty()) return false;

    m_last_error.clear();
    result.content = converted;
    return true;
}

bool CBodianSource::GetLyric(const wstring& virtual_path, online::Lyric& result)
{
    result = online::Lyric();

    const wstring prefix = L"bodian://";
    if (virtual_path.compare(0, prefix.size(), prefix) != 0)
        return false;

    wstring rid = virtual_path.substr(prefix.size());
    if (rid.empty())
        return false;

    // 优先取逐字歌词。它同时带翻译，拿不到再回退普通歌词接口。
    if (FetchWordLyric(rid, result)) return true;

    wstring path = L"/api/service/music/lyric/" + rid + L"?uid=-1&token=";

    json response;
    if (!Get(path, response))
        return false;

    if (response.value("code", 0) != 200 || !response.contains("data"))
        return false;

    string content = response["data"].value("content", "");
    if (content.empty())
        return false;

    // 歌词内容是 Base64 编码的 LRC 文本。外文歌会带上中文翻译，但译文行的
    // 时间标签标的是下一句，直接交给播放器会把原文和译文配反，这里先掰正。
    result.content = online::AlignLyricTranslation(FromUtf8(kugou::DecodeBase64(content)));
    return result.HasContent();
}

} // namespace bodian
