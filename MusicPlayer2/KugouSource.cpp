#include "stdafx.h"
#include "KugouSource.h"
#include "OnlineJson.h"
#include "OnlineHttp.h"
#include "KugouCrypto.h"
#include "KugouKrc.h"
#include "InternetCommon.h"
#include "IniHelper.h"
#include <ctime>
#include <algorithm>
#include <sstream>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace std;
using json = nlohmann::json;

namespace kugou
{

// 接口地址
static const wchar_t* API_BASE = L"https://gateway.kugou.com";
static const wchar_t* ROUTER_SEARCH = L"complexsearch.kugou.com";
// 专辑搜索不在 complexsearch 上，走 msearch；请求过去会 404
static const wchar_t* ROUTER_MSEARCH = L"msearch.kugou.com";
static const wchar_t* ROUTER_TRACKER = L"trackercdn.kugou.com";
static const wchar_t* ROUTER_LYRICS = L"lyrics.kugou.com";

// 配置文件中的小节名
static const wchar_t* SEC_DEVICE = L"kugou_device";
static const wchar_t* SEC_ACCOUNT = L"kugou_account";

// 概念版客户端的固定标识（用了不少年，官方客户端里也是写死的）
static const char* KG_THASH = "5d816a0";
static const char* KG_RF = "B9EDA08A64250DEFFBCADDEE00F8F25F";

// 专辑曲目一次取多少。专辑一般十几首，取 50 基本都能一页读完，
// 之后「把整张专辑存为歌单」才不会只存到第一页。
// 注意这个接口上限就是 50，传 100 会被判 invalid param（20010）。
static const int ALBUM_PAGE_SIZE = 50;

CKugouSource::CKugouSource()
{
}

CKugouSource::CKugouSource(const CKugouSource& source)
{
    std::lock_guard<std::mutex> lock(source.m_state_mutex);
    m_device = source.m_device;
    m_account = source.m_account;
}

CKugouSource::~CKugouSource()
{
}

bool CKugouSource::Browse(const online::BrowseRequest& request, online::BrowseResult& result)
{
    using namespace online;
    result = {};
    m_last_error.clear();
    if (request.kind == BrowseKind::AlbumSearch) return BrowseAlbums(request.id, request.page, result);
    if (request.kind == BrowseKind::Search) return IOnlineSource::Browse(request, result);
    if ((request.kind == BrowseKind::Playlists || request.kind == BrowseKind::Recommend) && !IsLoggedIn())
    {
        m_last_error = L"请在账号页登录K源后查看个人推荐和云歌单。";
        return false;
    }
    vector<pair<string, string>> params = {
        {"dfid", m_device.dfid}, {"mid", m_device.mid}, {"uuid", "-"},
        {"appid", LITE_APPID}, {"clientver", LITE_CLIENTVER},
        {"clienttime", to_string(time(nullptr))}
    };
    wstring path, router;
    string body;
    int page = (std::max)(1, request.page);
    switch (request.kind)
    {
    case BrowseKind::Hot:
        path = L"/api/v3/search/hot_tab"; router = L"msearch.kugou.com";
        params.push_back({"navid", "1"}); params.push_back({"plat", "2"}); break;
    case BrowseKind::Charts:
        path = L"/ocean/v6/rank/list";
        params.push_back({"plat", "2"}); params.push_back({"withsong", "0"}); params.push_back({"parentid", "0"}); break;
    case BrowseKind::ChartTracks:
        if (!IsServiceId(request.id)) { m_last_error = L"榜单编号无效"; return false; }
        path = L"/openapi/kmr/v2/rank/audio";
        body = json{{"show_portrait_mv", 1}, {"show_type_total", 1}, {"filter_original_remarks", 1},
            {"area_code", 1}, {"pagesize", 30}, {"rank_cid", 0}, {"type", 1}, {"page", page}, {"rank_id", ToUtf8(request.id)}}.dump(); break;
    case BrowseKind::AlbumTracks:
    {
        // 专辑曲目：直接按专辑编号取，返回的就是专辑里的曲序，
        // 不要再退回用「歌手 + 专辑名」搜关键词 —— 那样会混进同歌手的其它专辑。
        if (request.id.empty() || request.id.size() > 18
            || request.id.find_first_not_of(L"0123456789") != std::wstring::npos)
        { m_last_error = L"专辑编号无效"; return false; }
        path = L"/v1/album_audio/lite"; router = L"openapi.kugou.com";
        // album_id 必须发数字：发字符串服务端判 invalid param（20010），
        // 而且它参与签名，改类型就等于换了一份请求体。
        const long long album_id = _wtoi64(request.id.c_str());
        // 一次多取一些，整张专辑基本都能在一页里读完
        body = json{{"album_id", album_id}, {"is_buy", ""}, {"page", page}, {"pagesize", ALBUM_PAGE_SIZE}}.dump();
        break;
    }
    case BrowseKind::Recommend:
        path = L"/everyday_song_recommend"; router = L"everydayrec.service.kugou.com";
        body = json{{"platform", "android"}, {"userid", GetAccount().userid}}.dump(); break;
    case BrowseKind::Playlists:
        path = L"/v7/get_all_list"; router = L"cloudlist.service.kugou.com";
        params.push_back({"plat", "1"});
        body = json{{"userid", GetAccount().userid}, {"token", GetAccount().token}, {"total_ver", 979},
            {"type", 2}, {"page", page}, {"pagesize", 30}}.dump(); break;
    case BrowseKind::PlaylistTracks:
        if (!IsServiceId(request.id)) { m_last_error = L"歌单编号无效，请输入K源 global_collection_id"; return false; }
        path = L"/pubsongs/v2/get_other_list_file_nofilt";
        params.insert(params.end(), {{"area_code", "1"}, {"begin_idx", to_string((page - 1) * 30)},
            {"plat", "1"}, {"type", "1"}, {"mode", "1"}, {"personal_switch", "1"},
            {"pagesize", "30"}, {"global_collection_id", ToUtf8(request.id)}}); break;
    default: return false;
    }
    json response;
    if (!Request(path, router, params, body, response)) return false;
    if (JsonNumber(response, "status") != 1 || !response.contains("data"))
    {
        m_last_error = L"K源服务未返回列表（错误码 " + FromUtf8(JsonText(response, "error_code")) + L"），请检查登录状态或稍后重试。";
        return false;
    }
    const auto& data = response["data"];
    if (request.kind == BrowseKind::Hot)
    {
        if (!data.contains("list") || !data["list"].is_array()) { m_last_error = L"热搜数据格式已变化"; return false; }
        for (const auto& group : data["list"])
        {
            if (!group.contains("keywords") || !group["keywords"].is_array()) continue;
            for (const auto& value : group["keywords"])
            {
                BrowseItem item;
                item.type = BrowseItem::Type::Keyword;
                item.id = item.title = FromUtf8(JsonText(value, "keyword"));
                item.subtitle = FromUtf8(JsonText(group, "name"));
                if (!item.title.empty()) result.items.push_back(item);
            }
        }
        return true;
    }
    // 这些字段分别对应榜单、云歌单、歌单歌曲和每日推荐的已知响应契约。
    const json* list = data.is_array() ? &data : nullptr;
    for (const char* key : {"info", "list", "songs", "song_list", "songlist"})
        if (list == nullptr && data.contains(key) && data[key].is_array()) list = &data[key];
    if (list == nullptr) { m_last_error = L"K源列表数据格式已变化"; return false; }
    for (const auto& value : *list)
    {
        if (request.kind == BrowseKind::Charts || request.kind == BrowseKind::Playlists)
        {
            BrowseItem item;
            bool chart = request.kind == BrowseKind::Charts;
            item.type = chart ? BrowseItem::Type::Chart : BrowseItem::Type::Playlist;
            item.id = FromUtf8(JsonText(value, chart ? "rankid" : "global_collection_id"));
            item.title = FromUtf8(JsonText(value, chart ? "rankname" : "name"));
            item.subtitle = chart ? L"双击查看榜单" : L"双击查看云歌单";
            if (IsServiceId(item.id)) result.items.push_back(item);
        }
        else AddTrack(result, KugouTrack(value));
    }
    result.has_more = request.kind != BrowseKind::Charts && request.kind != BrowseKind::Recommend
        && (request.kind == BrowseKind::AlbumTracks ? list->size() >= ALBUM_PAGE_SIZE : list->size() >= 30);
    if (!list->empty() && result.items.empty()) { m_last_error = L"接口有返回数据，但曲目标识不完整"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 设备身份
// ---------------------------------------------------------------------------

void CKugouSource::LoadIdentity(const wstring& config_dir)
{
    wstring ini_path = config_dir + L"kugou.ini";
    CIniHelper ini(ini_path);

    m_device.guid = ToUtf8(ini.GetString(SEC_DEVICE, L"guid", L""));
    m_device.mid = ToUtf8(ini.GetString(SEC_DEVICE, L"mid", L""));
    m_device.mac = ToUtf8(ini.GetString(SEC_DEVICE, L"mac", L""));
    m_device.dfid = ToUtf8(ini.GetString(SEC_DEVICE, L"dfid", L""));
    m_device.server_dev = ToUtf8(ini.GetString(SEC_DEVICE, L"server_dev", L""));

    // 首次运行：生成一份设备身份并立刻保存，之后一直复用。
    // 这一步很关键，每次换新身份会被服务端当成新设备。
    if (m_device.guid.empty())
    {
        m_device.guid = Md5Hex(GenerateUuid());
        m_device.mid = CalculateMid(m_device.guid);
        m_device.mac = GetLocalMac();
        // server_dev 相当于一个客户端标识，同样固定下来
        m_device.server_dev = Md5Hex(GenerateUuid()).substr(0, 10);
        std::transform(m_device.server_dev.begin(), m_device.server_dev.end(),
            m_device.server_dev.begin(), ::toupper);
        SaveIdentity(config_dir);
    }
    else if (m_device.mid.empty())
    {
        m_device.mid = CalculateMid(m_device.guid);
        SaveIdentity(config_dir);
    }

    if (m_device.dfid.empty())
        m_device.dfid = "-";        // 还没注册设备时先用占位符

    Account loaded;
    loaded.token = ToUtf8(ini.GetString(SEC_ACCOUNT, L"token", L""));
    loaded.userid = ToUtf8(ini.GetString(SEC_ACCOUNT, L"userid", L""));
    loaded.t1 = ToUtf8(ini.GetString(SEC_ACCOUNT, L"t1", L""));
    loaded.vip_type = ToUtf8(ini.GetString(SEC_ACCOUNT, L"vip_type", L""));
    loaded.vip_token = ToUtf8(ini.GetString(SEC_ACCOUNT, L"vip_token", L""));
    SetAccount(loaded);
}

void CKugouSource::SaveIdentity(const wstring& config_dir) const
{
    wstring ini_path = config_dir + L"kugou.ini";
    CIniHelper ini(ini_path);

    ini.WriteString(SEC_DEVICE, L"guid", FromUtf8(m_device.guid));
    ini.WriteString(SEC_DEVICE, L"mid", FromUtf8(m_device.mid));
    ini.WriteString(SEC_DEVICE, L"mac", FromUtf8(m_device.mac));
    ini.WriteString(SEC_DEVICE, L"dfid", FromUtf8(m_device.dfid));
    ini.WriteString(SEC_DEVICE, L"server_dev", FromUtf8(m_device.server_dev));

    ini.WriteString(SEC_ACCOUNT, L"token", FromUtf8(GetAccount().token));
    ini.WriteString(SEC_ACCOUNT, L"userid", FromUtf8(GetAccount().userid));
    ini.WriteString(SEC_ACCOUNT, L"t1", FromUtf8(GetAccount().t1));
    ini.WriteString(SEC_ACCOUNT, L"vip_type", FromUtf8(GetAccount().vip_type));
    ini.WriteString(SEC_ACCOUNT, L"vip_token", FromUtf8(GetAccount().vip_token));

    ini.Save();
}

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

bool CKugouSource::Request(const wstring& url_path, const wstring& router,
    const vector<pair<string, string>>& extra_params,
    const string& body, json& out_json, bool need_sign, const wchar_t* method, const wchar_t* base_url)
{
    // 组装业务参数
    vector<SignParam> params;
    for (const auto& kv : extra_params)
        params.push_back({ kv.first, kv.second });

    // userid 是必需参数：未登录时也要带 userid=0，
    // 否则接口会返回 152 Parameter Error。这里统一补上，各接口不必各自记得。
    bool has_userid = false;
    for (const auto& p : params)
    {
        if (p.key == "userid")
        {
            has_userid = true;
            break;
        }
    }
    if (!has_userid)
        params.push_back({ "userid", GetAccount().IsLoggedIn() ? GetAccount().userid : "0" });

    // 登录后带上 token
    if (GetAccount().IsLoggedIn())
    {
        bool has_token = false;
        for (const auto& p : params)
        {
            if (p.key == "token")
            {
                has_token = true;
                break;
            }
        }
        if (!has_token)
            params.push_back({ "token", GetAccount().token });
    }

    // 组装查询串。签名要用到全部参数，所以先收集再拼接。
    string query;
    for (const auto& p : params)
    {
        if (!query.empty())
            query += '&';
        query += p.key;
        query += '=';
        query += UrlEncode(p.value);
    }

    // 签名参数里不包含 signature 自身，用全部业务参数参与计算
    if (need_sign)
    {
        vector<SignParam> sign_params = params;
        // signature 在参数列表里的位置由服务端约定，这里按官方实现放在末尾
        string signature = SignatureAndroid(sign_params, body);
        if (!query.empty())
            query += '&';
        query += "signature=";
        query += signature;
    }

    wstring url = wstring(base_url ? base_url : API_BASE) + url_path + L"?" + FromUtf8(query);

    // 请求头。dfid/clienttime 这些在官方实现里是放在头里的。
    {
        char clienttime[32]{};
        sprintf_s(clienttime, "%lld", static_cast<long long>(time(nullptr)));
        string headers_str;
        headers_str += "dfid: " + m_device.dfid + "\r\n";
        headers_str += "mid: " + m_device.mid + "\r\n";
        headers_str += "clienttime: " + string(clienttime) + "\r\n";
        headers_str += "kg-rc: 1\r\n";
        headers_str += "kg-thash: " + string(KG_THASH) + "\r\n";
        headers_str += "kg-rec: 1\r\n";
        headers_str += "kg-rf: " + string(KG_RF) + "\r\n";
        if (!router.empty())
            headers_str += "x-router: " + ToUtf8(router) + "\r\n";

        wstring result;
        if (!body.empty()) headers_str += "Content-Type: application/json\r\n";
        else if (method && wcscmp(method, L"POST") == 0) headers_str += "Content-Type: application/x-www-form-urlencoded\r\n";
        if (url_path == L"/openapi/kmr/v2/rank/audio") headers_str += "kg-tid: 369\r\n";
        if (url_path == L"/v1/album_audio/lite") headers_str += "kg-tid: 255\r\n";
        if (!online::HttpRequest(url, body, FromUtf8(headers_str), result, m_last_error, method))
        {
            return false;
        }

        try
        {
            out_json = json::parse(ToUtf8(result));
        }
        catch (const json::exception&)
        {
            return false;       // 返回的不是 JSON，多半是被拦截或接口变了
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

bool CKugouSource::Search(const wstring& keyword, int page, vector<online::Track>& result)
{
    result.clear();
    if (keyword.empty())
        return false;

    char buf[32]{};
    sprintf_s(buf, "%lld", static_cast<long long>(time(nullptr)));

    vector<pair<string, string>> params = {
        { "dfid",        m_device.dfid },
        { "mid",         m_device.mid },
        { "uuid",        "-" },
        { "appid",       LITE_APPID },
        { "clientver",   LITE_CLIENTVER },
        { "clienttime",  buf },
        { "keyword",     ToUtf8(keyword) },
        { "page",        to_string(page < 1 ? 1 : page) },
        { "pagesize",    "30" },
        { "platform",    "AndroidFilter" },
        { "iscorrection","1" },
    };
    // userid 与 token 由 Request() 统一补上

    json response;
    if (!Request(L"/v3/search/song", ROUTER_SEARCH, params, "", response))
        return false;

    // 接口正常时 status 为 1
    if (online::JsonNumber(response, "status") != 1)
        return false;

    if (!response.contains("data") || !response["data"].contains("lists"))
        return false;

    // 注意：搜索接口返回的字段名是首字母大写的（FileHash、SingerName 这类），
    // 与其它接口的小写命名不同，这里按实际响应取值。
    for (const auto& item : response["data"]["lists"])
    {
        online::Track track;

        string hash = online::JsonText(item, "FileHash");
        if (hash.empty())
            continue;

        string mixsongid = online::JsonText(item, "MixSongID");
        // 虚拟路径里带上 MixSongID，取地址时要用
        track.virtual_path = FromUtf8("kugou://" + hash +
            (mixsongid.empty() ? "" : "?aaid=" + mixsongid));

        track.title = FromUtf8(online::JsonText(item, "OriSongName"));
        if (track.title.empty())
        {
            // 退而用「歌手 - 歌名」形式的完整文件名
            string file_name = online::JsonText(item, "FileName");
            size_t sep = file_name.find(" - ");
            track.title = FromUtf8(sep == string::npos ? file_name : file_name.substr(sep + 3));
        }

        track.artist = FromUtf8(online::JsonText(item, "SingerName"));
        track.album = FromUtf8(online::JsonText(item, "AlbumName"));
        // 接口返回的是秒。取不到或不是数字时留 0，别让类型不符把整页搜索打断。
        const int duration_seconds = online::JsonNumber(item, "Duration");
        if (duration_seconds > 0 && duration_seconds < 2147483) track.duration_ms = duration_seconds * 1000;
        track.extra = FromUtf8(mixsongid);
        track.cover_url = online::CoverUrl(item);

        result.push_back(track);
    }

    return !result.empty();
}


// 专辑搜索：单独一个路由（msearch），返回结构也和歌曲搜索不同 ——
// 列表字段是 info 而不是 lists，命名是小写的 albumname/singername。
// 条目里存平台给的专辑编号（albumid），点进去按 AlbumTracks 取这张专辑的曲目。
bool CKugouSource::BrowseAlbums(const wstring& keyword, int page, online::BrowseResult& result)
{
    using namespace online;
    result = {};
    m_last_error.clear();
    if (keyword.empty()) { m_last_error = L"请输入专辑名或歌手"; return false; }

    char buf[32]{};
    sprintf_s(buf, "%lld", static_cast<long long>(time(nullptr)));
    vector<pair<string, string>> params = {
        { "dfid",       m_device.dfid },
        { "mid",        m_device.mid },
        { "uuid",       "-" },
        { "appid",      LITE_APPID },
        { "clientver",  LITE_CLIENTVER },
        { "clienttime", buf },
        { "keyword",    ToUtf8(keyword) },
        { "page",       to_string(page < 1 ? 1 : page) },
        { "pagesize",   "30" },
        { "showtype",   "1" },
    };

    json response;
    if (!Request(L"/api/v3/search/album", ROUTER_MSEARCH, params, "", response)) return false;
    if (online::JsonNumber(response, "status") != 1) { m_last_error = L"专辑搜索未返回数据"; return false; }
    if (!response.contains("data") || !response["data"].contains("info")) return false;

    for (const auto& item : response["data"]["info"])
    {
        const wstring name = FromUtf8(online::JsonText(item, "albumname"));
        if (name.empty()) continue;
        // 有的条目没带 albumid，没有编号就打不开专辑，直接跳过
        const wstring id = FromUtf8(online::JsonText(item, "albumid"));
        if (!IsServiceId(id)) continue;
        const wstring artist = FromUtf8(online::JsonText(item, "singername"));

        BrowseItem album;
        album.type = BrowseItem::Type::Album;
        album.title = name;
        album.id = id;
        // 专辑重名很常见，把歌手、发行年份和曲目数一起放进副标题，方便挑对那张
        album.subtitle = artist.empty() ? L"专辑" : L"专辑 · " + artist;
        const wstring publish = FromUtf8(online::JsonText(item, "publishtime"));
        if (publish.size() >= 4) album.subtitle += L" · " + publish.substr(0, 4);
        const int count = item.contains("songcount") && item["songcount"].is_number()
            ? item["songcount"].get<int>() : 0;
        if (count > 0) album.subtitle += L" · " + to_wstring(count) + L" 首";
        result.items.push_back(std::move(album));
    }
    if (result.items.empty()) m_last_error = L"没有找到相关专辑";
    result.has_more = response["data"]["info"].size() >= 30;
    return !result.items.empty();
}

// ---------------------------------------------------------------------------
// 取播放地址
// ---------------------------------------------------------------------------

wstring CKugouSource::ResolvePlayUrl(const wstring& virtual_path)
{
    wstring path = virtual_path;
    const wstring prefix = L"kugou://";
    if (path.compare(0, prefix.size(), prefix) != 0)
        return wstring();

    wstring rest = path.substr(prefix.size());
    wstring hash = rest;
    wstring aaid;

    size_t q = rest.find(L'?');
    if (q != wstring::npos)
    {
        hash = rest.substr(0, q);
        wstring query = rest.substr(q + 1);
        size_t p = query.find(L"aaid=");
        if (p != wstring::npos)
        {
            aaid = query.substr(p + 5);
            size_t amp = aaid.find(L'&');
            if (amp != wstring::npos)
                aaid = aaid.substr(0, amp);
        }
    }

    return FetchPlayUrl(hash, aaid);
}

wstring CKugouSource::FetchPlayUrl(const wstring& hash, const wstring& album_audio_id)
{
    m_last_error.clear();
    if (hash.empty())
        return wstring();

    const Account account = GetAccount();

    // 音质从高到低尝试。拿不到高音质时自动降级，
    // 这样 VIP 过期或某档位无版权时仍能播。把上面几档为什么没成记下来，
    // 最后告诉用户实际用的是哪一档，而不是默默降级。
    static const char* qualities[] = { "flac", "320", "128" };
    static const wchar_t* quality_names[] = { L"无损", L"320k", L"128k" };
    m_quality_note.clear();
    wstring first_failure;

    for (size_t qi = 0; qi < sizeof(qualities) / sizeof(qualities[0]); ++qi)
    {
        const char* quality = qualities[qi];
        char clienttime[32]{};
        sprintf_s(clienttime, "%lld", static_cast<long long>(time(nullptr)));

        // 播放接口同时校验 key 与完整请求签名，账号参数使用同一份快照。
        string userid = account.IsLoggedIn() ? account.userid : "0";
        string key = CalcV5Key(ToUtf8(hash), m_device.mid, userid);

        vector<pair<string, string>> params = {
            { "dfid",           m_device.dfid },
            { "mid",            m_device.mid },
            { "uuid",           "-" },
            { "appid",          LITE_APPID },
            { "clientver",      LITE_CLIENTVER },
            { "clienttime",     clienttime },
            { "hash",           ToUtf8(hash) },
            { "album_audio_id", ToUtf8(album_audio_id) },
            { "quality",        quality },
            { "area_code",      "1" },
            { "behavior",       "play" },
            { "pid",            "411" },        // 概念版固定值
            { "cmd",            "26" },
            { "pidversion",     "3001" },
            { "cdnBackup",      "1" },
            { "module",         "" },
            { "key",            key },
        };
        if (account.IsLoggedIn())
        {
            params.push_back({ "token",  account.token });
            params.push_back({ "userid", account.userid });
        }
        else
        {
            // 未登录也必须带 userid=0，与搜索接口的规则一致
            params.push_back({ "userid", "0" });
        }

        json response;
        if (!Request(L"/v5/url", ROUTER_TRACKER, params, "", response)) return {};
        auto playback = online::ParseKugouPlayback(response, account.IsLoggedIn());
        if (!playback.url.empty())
        {
            m_last_error.clear();
            m_quality_note = qi == 0 ? wstring(L"当前播放：无损")
                : wstring(L"当前播放：") + quality_names[qi] + (first_failure.empty() ? wstring() : L"（无损未获取到：" + first_failure + L"）");
            return playback.url;
        }
        m_last_error = playback.error;
        if (first_failure.empty()) first_failure = playback.error;
        if (!playback.retry_quality) return {};
    }

    if (m_last_error.empty())
        m_last_error = L"暂时拿不到播放地址，可能这首歌已下架或接口有变动";

    return wstring();
}

// ---------------------------------------------------------------------------
// 歌词
// ---------------------------------------------------------------------------

bool CKugouSource::GetLyric(const wstring& virtual_path, online::Lyric& result)
{
    result = online::Lyric();
    m_last_error.clear();

    wstring prefix = L"kugou://";
    if (virtual_path.compare(0, prefix.size(), prefix) != 0)
        return false;

    wstring rest = virtual_path.substr(prefix.size());
    wstring hash = rest;
    size_t q = rest.find(L'?');
    if (q != wstring::npos)
        hash = rest.substr(0, q);

    if (hash.empty())
        return false;

    // 歌词服务使用独立 /v1/search，并签名其业务参数，不注入网关的账号参数。
    vector<SignParam> params = {
        {"album_audio_id", "0"}, {"appid", LITE_APPID}, {"clientver", LITE_CLIENTVER},
        {"duration", "0"}, {"hash", ToUtf8(hash)}, {"keyword", ""}, {"lrctxt", "1"}, {"man", "no"}
    };
    string query;
    for (const auto& param : params)
    {
        if (!query.empty()) query += '&';
        query += param.key + "=" + UrlEncode(param.value);
    }
    query += "&signature=" + SignatureAndroid(params, "");
    const wstring headers = L"User-Agent: " + FromUtf8(DEFAULT_USER_AGENT) + L"\r\n";
    wstring search_text;
    if (!online::HttpRequest(L"https://lyrics.kugou.com/v1/search?" + FromUtf8(query), "", headers, search_text, m_last_error))
        return false;
    json search_result;
    try { search_result = json::parse(ToUtf8(search_text)); }
    catch (const json::exception&) { m_last_error = L"K源歌词搜索响应无法解析"; return false; }
    if (!search_result.contains("candidates") || !search_result["candidates"].is_array() || search_result["candidates"].empty())
    { m_last_error = L"这首歌曲暂时没有匹配的歌词"; return false; }

    const auto& candidate = search_result["candidates"][0];
    string id = online::JsonText(candidate, "id");
    string accesskey = online::JsonText(candidate, "accesskey");
    if (id.empty() || accesskey.empty())
        return false;

    // 第二步：下载歌词。
    // 优先要 krc —— 它是带逐字时间轴的版本，能做逐字填色，而且有些歌只有 krc 没有 lrc
    // （这正是之前「有时候拿不到歌词」的一个原因）。krc 是加密的，解密后再转成
    // 播放器能识别的逐字歌词格式。
    wstring response_text;
    {
        const wstring krc_url = wstring(L"https://lyrics.kugou.com/download?ver=1&client=pc&id=") +
            FromUtf8(UrlEncode(id)) + L"&accesskey=" + FromUtf8(UrlEncode(accesskey)) + L"&fmt=krc&charset=utf8";
        if (online::HttpRequest(krc_url, "", headers, response_text, m_last_error))
        {
            try
            {
                const json obj = json::parse(ToUtf8(response_text));
                const string content = obj.value("content", "");
                if (!content.empty())
                {
                    const wstring lyric_text = KrcToExtendedLyric(DecryptKrc(content));
                    if (!lyric_text.empty())
                    {
                        result.content = lyric_text;
                        m_last_error.clear();
                        return result.HasContent();
                    }
                }
            }
            catch (const json::exception&) { }
        }
    }

    // krc 没拿到就退回普通 lrc
    const wstring download_url = wstring(L"https://lyrics.kugou.com/download?id=") +
        FromUtf8(UrlEncode(id)) + L"&accesskey=" + FromUtf8(UrlEncode(accesskey)) +
        L"&fmt=lrc&charset=utf8&client=android&ver=1";

    if (!online::HttpRequest(download_url, "", headers, response_text, m_last_error))
        return false;

    try
    {
        json obj = json::parse(ToUtf8(response_text));
        string content = obj.value("content", "");
        if (content.empty())
            return false;

        // content 是 base64 编码的歌词文本
        result.content = FromUtf8(DecodeBase64(content));
        return result.HasContent();
    }
    catch (const json::exception&)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------
// 扫码登录
// ---------------------------------------------------------------------------

// 登录接口在 login-user.kugou.com。
// 注意两点（都是实测出来的）：
//   1. 除了平台自己的参数，还要带上客户端的公共参数（dfid/mid/uuid/appid/
//      clientver/clienttime/userid），少了会返回 20010 签名校验失败；
//      clienttime 尤其不能省，否则返回 20006。
//   2. 签名用参数的「原始值」，拼进 URL 时才做 URL 编码，顺序不能反。
bool CKugouSource::RequestLoginApi(const wstring& url_path,
    vector<pair<string, string>>& params, json& out_json)
{
    char clienttime[32]{};
    sprintf_s(clienttime, "%lld", static_cast<long long>(time(nullptr)));

    // 补上公共参数（调用方只传平台自己的参数）
    params.push_back({ "dfid",       m_device.dfid });
    params.push_back({ "mid",        m_device.mid });
    params.push_back({ "uuid",       "-" });
    params.push_back({ "appid",      LITE_APPID });
    params.push_back({ "clientver",  LITE_CLIENTVER });
    params.push_back({ "clienttime", clienttime });
    params.push_back({ "userid",     GetAccount().IsLoggedIn() ? GetAccount().userid : "0" });
    if (GetAccount().IsLoggedIn())
        params.push_back({ "token", GetAccount().token });

    vector<SignParam> sign_params;
    for (const auto& kv : params)
        sign_params.push_back({ kv.first, kv.second });
    string signature = SignatureWeb(sign_params);

    string query;
    for (const auto& kv : params)
    {
        if (!query.empty())
            query += '&';
        query += kv.first;
        query += '=';
        query += UrlEncode(kv.second);
    }
    query += "&signature=";
    query += signature;

    wstring url = L"https://login-user.kugou.com" + url_path + L"?" + FromUtf8(query);

    string headers;
    headers += "User-Agent: " + string(DEFAULT_USER_AGENT) + "\r\n";

    wstring result;
    if (!online::HttpRequest(url, "", FromUtf8(headers), result, m_last_error))
    {
        m_last_error = L"网络请求失败";
        return false;
    }

    try
    {
        out_json = json::parse(ToUtf8(result));
        return true;
    }
    catch (const json::exception&)
    {
        m_last_error = L"登录接口返回的内容无法识别";
        return false;
    }
}

// ---------------------------------------------------------------------------
// 设备注册
// ---------------------------------------------------------------------------

// 概念版客户端的 RSA 公钥（从官方客户端里提取的公开常量）
static const char* LITE_RSA_PUBLIC_KEY =
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDECi0Np2UR87scwrvTr72L6oO01rBbbBPriSDFPxr3Z5syug0O24QyQO8bg27+0+4kBzTBTBOZ/WWU0WryL1JSXRTXLgFVxtzIY41Pe7lPOgsfTCn5kZcvKhYKJesKnnJDNr5/abvTGf+rHG3YRwsCHcQ08/q6ifSioBszvb3QiwIDAQAB";

// 发一个 POST 请求并把响应当二进制读回来。
// 设备注册的响应是 AES 密文，用项目原有的文本式读取会被破坏，所以这里单独实现。
static bool HttpPostBinary(const wstring& url, const string& headers,
    const string& body, vector<BYTE>& out_bytes)
{
    out_bytes.clear();

    wstring rest = url;
    if (rest.compare(0, 8, L"https://") == 0)
        rest = rest.substr(8);
    else if (rest.compare(0, 7, L"http://") == 0)
        rest = rest.substr(7);
    else
        return false;

    const bool https = (url.compare(0, 8, L"https://") == 0);

    size_t slash = rest.find(L'/');
    wstring host = (slash == wstring::npos) ? rest : rest.substr(0, slash);
    wstring path = (slash == wstring::npos) ? L"/" : rest.substr(slash);

    HINTERNET session = WinHttpOpen(L"MusicPlayer2",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return false;
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

    bool ok = false;
    const INTERNET_PORT port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (connect != nullptr)
    {
        HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            https ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr)
        {
            wstring wh = FromUtf8(headers);
            BOOL sent = WinHttpSendRequest(request,
                wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(),
                static_cast<DWORD>(wh.size()),
                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);

            if (sent && WinHttpReceiveResponse(request, nullptr))
            {
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0)
                {
                    size_t offset = out_bytes.size();
                    out_bytes.resize(offset + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, out_bytes.data() + offset, available, &read) || read == 0)
                    {
                        out_bytes.resize(offset);
                        break;
                    }
                    out_bytes.resize(offset + read);
                }
                ok = !out_bytes.empty();
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return ok;
}
bool CKugouSource::RegisterDevice()
{
    m_last_error.clear();

    // 伪造一份安卓设备档案。字段和取值参考官方客户端，不必改。
    json info;
    info["availableRamSize"] = 4983533568LL;
    info["availableRomSize"] = 48114719;
    info["availableSDSize"] = 48114717;
    info["basebandVer"] = "";
    info["batteryLevel"] = 100;
    info["batteryStatus"] = 3;
    info["brand"] = "Redmi";
    info["buildSerial"] = "unknown";
    info["device"] = "marble";
    info["imei"] = m_device.guid;
    info["imsi"] = "";
    info["manufacturer"] = "Xiaomi";
    info["uuid"] = m_device.guid;
    info["accelerometer"] = false;
    info["accelerometerValue"] = "";
    info["gravity"] = false;
    info["gravityValue"] = "";
    info["gyroscope"] = false;
    info["gyroscopeValue"] = "";
    info["light"] = false;
    info["lightValue"] = "";
    info["magnetic"] = false;
    info["magneticValue"] = "";
    info["orientation"] = false;
    info["orientationValue"] = "";
    info["pressure"] = false;
    info["pressureValue"] = "";
    info["step_counter"] = false;
    info["step_counterValue"] = "";
    info["temperature"] = false;
    info["temperatureValue"] = "";

    // 请求体是设备信息的 AES 密文（Base64），服务端响应用同一把 key 加密
    string aes_key = RandomKey6();
    string body = EncodeBase64(AesEncryptForRegister(info.dump(), aes_key));

    // p 参数：把 AES key 和账号信息用 RSA 加密后交给服务端。
    // 注意 uid 未登录时必须给「数字 0」而不是字符串 "0"，否则服务端会报 rsa failure。
    json key_info;
    key_info["aes"] = aes_key;
    if (GetAccount().IsLoggedIn())
        key_info["uid"] = GetAccount().userid;
    else
        key_info["uid"] = 0;
    key_info["token"] = GetAccount().token;
    string p = RsaEncryptPkcs1(key_info.dump(), LITE_RSA_PUBLIC_KEY);
    if (p.empty())
    {
        m_last_error = L"设备注册失败：RSA 加密出错";
        return false;
    }

    char clienttime[32]{};
    sprintf_s(clienttime, "%lld", static_cast<long long>(time(nullptr)));
    string userid = GetAccount().IsLoggedIn() ? GetAccount().userid : "0";

    // 这个接口要 Android 签名，而且签名里要带上 AES 密文作为请求体
    vector<SignParam> sign_params = {
        { "dfid",       "-" },
        { "mid",        m_device.mid },
        { "uuid",       "-" },
        { "appid",      LITE_APPID },
        { "clientver",  LITE_CLIENTVER },
        { "clienttime", clienttime },
        { "userid",     userid },
        { "part",       "1" },
        { "platid",     "1" },
        { "p",          p },
    };
    string signature = SignatureAndroid(sign_params, body);

    string query;
    for (const SignParam& sp : sign_params)
    {
        if (!query.empty())
            query += '&';
        query += sp.key;
        query += '=';
        query += UrlEncode(sp.value);
    }
    query += "&signature=";
    query += signature;

    wstring url = L"https://userservice.kugou.com/risk/v2/r_register_dev?" + FromUtf8(query);

    string headers;
    headers += "User-Agent: " + string(DEFAULT_USER_AGENT) + "\r\n";
    headers += "Content-Type: application/octet-stream\r\n";
    headers += "dfid: -\r\n";
    headers += "mid: " + m_device.mid + "\r\n";
    headers += "clienttime: " + string(clienttime) + "\r\n";
    headers += "kg-rc: 1\r\n";
    headers += "kg-thash: " + string(KG_THASH) + "\r\n";
    headers += "kg-rec: 1\r\n";
    headers += "kg-rf: " + string(KG_RF) + "\r\n";

    vector<BYTE> raw;
    if (!HttpPostBinary(url, headers, body, raw))
    {
        m_last_error = L"设备注册失败：网络请求出错";
        return false;
    }

    // 正常情况下响应体是 AES 密文；但服务端偶尔会直接返回明文报错，
    // 所以这里先看是不是 JSON，是的话直接用它，不是才解密。
    string raw_text(raw.begin(), raw.end());
    string plain;
    if (!raw_text.empty() && raw_text[0] == '{')
    {
        plain = raw_text;
    }
    else
    {
        string cipher_b64 = EncodeBase64(raw_text);
        plain = AesDecryptForRegister(cipher_b64, aes_key);
        if (plain.empty())
        {
            m_last_error = L"设备注册失败：响应既不是 JSON 也解不开密";
            return false;
        }
    }

    try
    {
        json result = json::parse(plain);
        if (online::JsonNumber(result, "status") != 1 || !result.contains("data"))
        {
            m_last_error = L"设备注册被拒绝（" + FromUtf8(plain.substr(0, 120)) + L"）";
            return false;
        }

        string dfid = online::JsonText(result["data"], "dfid");
        if (dfid.empty())
        {
            m_last_error = L"设备注册成功但没返回 dfid";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_device.dfid = dfid;
        }
        return true;
    }
    catch (const json::exception&)
    {
        // 把原始内容带上，便于判断服务端返回了什么
        string shown = plain;
        if (shown.size() > 150)
            shown = shown.substr(0, 150);
        m_last_error = L"设备注册返回内容无法识别（原文：" + FromUtf8(shown) + L"）";
        return false;
    }
}
bool CKugouSource::GetQrCode(wstring& qr_content)
{
    qr_content.clear();
    m_last_error.clear();

    // type/plat 固定；qrcode_txt 里带的 appid=1001 是扫码页要用的，与公共参数的 appid 不是一回事
    vector<pair<string, string>> params = {
        { "type",       "1" },
        { "plat",       "4" },
        { "srcappid",   "2919" },
        { "qrcode_txt", "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1001&" },
    };

    json response;
    if (!RequestLoginApi(L"/v2/qrcode", params, response))
        return false;

    if (online::JsonNumber(response, "status") != 1 || !response.contains("data"))
    {
        string dumped = response.dump();
        if (dumped.size() > 160)
            dumped = dumped.substr(0, 160);
        m_last_error = L"获取登录二维码失败（接口返回：" + FromUtf8(dumped) + L"）";
        return false;
    }

    // 二维码是纯数字长串，服务端换成数字类型也要能取到
    string key = online::JsonText(response["data"], "qrcode");
    if (key.empty())
    {
        m_last_error = L"登录接口没有返回二维码";
        return false;
    }

    m_qr_key = FromUtf8(key);
    qr_content = L"https://h5.kugou.com/apps/loginQRCode/html/index.html?qrcode=" + m_qr_key;
    return true;
}

CKugouSource::QrStatus CKugouSource::CheckQrCode()
{
    if (m_qr_key.empty())
    {
        m_last_error = L"还没有获取二维码";
        return QrStatus::Failed;
    }

    vector<pair<string, string>> params = {
        { "plat",     "4" },
        { "srcappid", "2919" },
        { "qrcode",   ToUtf8(m_qr_key) },
        { "dev",      m_device.server_dev },
    };

    json response;
    if (!RequestLoginApi(L"/v2/get_userinfo_qrcode", params, response))
        return QrStatus::Failed;

    if (!response.contains("data"))
        return QrStatus::Failed;

    const auto& data = response["data"];
    // status 缺失或类型不符时按「状态异常」处理，不能退回 0 —— 0 的含义是二维码已过期
    const int status = data.contains("status") ? online::JsonNumber(data, "status") : -1;

    // 0 过期 / 1 等待扫码 / 2 待确认 / 4 授权成功
    switch (status)
    {
    case 0:
        return QrStatus::Expired;
    case 1:
        return QrStatus::Waiting;
    case 2:
        return QrStatus::Scanned;
    case 4:
    {
        Account authorized;
        authorized.token = online::JsonText(data, "token");
        authorized.userid = online::JsonText(data, "userid");
        if (authorized.token.empty() || authorized.userid.empty())
        {
            m_last_error = L"登录成功但没拿到账号信息";
            return QrStatus::Failed;
        }
        SetAccount(authorized);
        m_last_error.clear();
        return QrStatus::Authorized;
    }
    default:
        m_last_error = L"登录状态异常（返回 " + to_wstring(status) + L"）";
        return QrStatus::Failed;
    }
}
bool CKugouSource::GetProfile(online::AccountProfile& profile)
{
    profile = {}; m_last_error.clear();
    const auto account = GetAccount();
    if (!account.IsLoggedIn()) { m_last_error = L"请先登录"; return false; }
    const auto now = time(nullptr);
    auto encrypted = RsaEncryptRaw(nlohmann::ordered_json{{"token", account.token}, {"clienttime", now}}.dump());
    transform(encrypted.begin(), encrypted.end(), encrypted.begin(), ::toupper);
    vector<pair<string, string>> params{{"appid", LITE_APPID}, {"clientver", LITE_CLIENTVER},
        {"mid", m_device.mid}, {"dfid", m_device.dfid}, {"clienttime", to_string(now)}, {"plat", "1"}};
    json response;
    // userid 只做过非空校验，格式不对时 stoll 会抛异常（那会让整个账号页崩掉），解析失败按 0 提交
    long long userid = 0;
    try { userid = stoll(account.userid); }
    catch (...) { userid = 0; }
    const string body = json{{"visit_time", now}, {"usertype", 1}, {"p", encrypted}, {"userid", userid}}.dump();
    if (!encrypted.empty() && Request(L"/v3/get_my_info", L"usercenter.kugou.com", params, body, response)
        && online::JsonNumber(response, "status") == 1 && response.contains("data"))
        profile.name = FromUtf8(online::JsonText(response["data"], "nickname"));
    params.pop_back(); params.push_back({"busi_type", "concept"});
    if (Request(L"/v1/get_union_vip", L"", params, "", response, true, L"GET", L"https://kugouvip.kugou.com")
        && online::JsonNumber(response, "status") == 1 && response.contains("data"))
        online::ReadKugouMembership(response["data"], profile);
    if (profile.name.empty() && profile.membership.empty()) { m_last_error = L"账号信息读取失败"; return false; }
    return true;
}

wstring CKugouSource::GetCoverUrl(const online::Track& track)
{
    if (!track.cover_url.empty()) return track.cover_url;
    if (track.title.empty()) return {};
    vector<online::Track> tracks;
    auto hash = [](const wstring& path) { auto value = path.substr(0, path.find(L'?')); transform(value.begin(), value.end(), value.begin(), towlower); return value; };
    for (const auto& query : {track.artist + L" " + track.title, track.title})
        if (Search(query, 1, tracks))
            for (const auto& item : tracks)
                if (hash(item.virtual_path) == hash(track.virtual_path) && !item.cover_url.empty()) return item.cover_url;
    return {};
}

void CKugouSource::Logout()
{
    SetAccount(Account());
    m_qr_key.clear();
}

} // namespace kugou
