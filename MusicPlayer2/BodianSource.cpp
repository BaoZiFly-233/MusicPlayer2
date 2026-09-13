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

CBodianSource::CBodianSource() : m_devid(MakeDevid()) {}
CBodianSource::CBodianSource(const CBodianSource& source)
    : m_devid(source.m_devid), m_account(source.GetAccount()) {}

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
    json response;
    if (!SignedRequest(L"/api/ucenter/users/login", {}, response) || !response.contains("data")) return false;
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
    if (request.kind == BrowseKind::Search) return IOnlineSource::Browse(request, result);
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
