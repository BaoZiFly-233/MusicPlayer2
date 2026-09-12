#include "stdafx.h"
#include "KugouSource.h"
#include "KugouCrypto.h"
#include "InternetCommon.h"
#include "IniHelper.h"
#include <ctime>
#include <algorithm>
#include <sstream>

using namespace std;
using json = nlohmann::json;

namespace kugou
{

// 接口地址
static const wchar_t* API_BASE = L"https://gateway.kugou.com";
static const wchar_t* ROUTER_SEARCH = L"complexsearch.kugou.com";
static const wchar_t* ROUTER_TRACKER = L"trackercdn.kugou.com";
static const wchar_t* ROUTER_LYRICS = L"lyrics.kugou.com";

// 配置文件中的小节名
static const wchar_t* SEC_DEVICE = L"kugou_device";
static const wchar_t* SEC_ACCOUNT = L"kugou_account";

// 概念版客户端的固定标识（用了不少年，官方客户端里也是写死的）
static const char* KG_THASH = "5d816a0";
static const char* KG_RF = "B9EDA08A64250DEFFBCADDEE00F8F25F";

CKugouSource::CKugouSource()
{
}

CKugouSource::~CKugouSource()
{
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

    m_account.token = ToUtf8(ini.GetString(SEC_ACCOUNT, L"token", L""));
    m_account.userid = ToUtf8(ini.GetString(SEC_ACCOUNT, L"userid", L""));
    m_account.t1 = ToUtf8(ini.GetString(SEC_ACCOUNT, L"t1", L""));
    m_account.vip_type = ToUtf8(ini.GetString(SEC_ACCOUNT, L"vip_type", L""));
    m_account.vip_token = ToUtf8(ini.GetString(SEC_ACCOUNT, L"vip_token", L""));
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

    ini.WriteString(SEC_ACCOUNT, L"token", FromUtf8(m_account.token));
    ini.WriteString(SEC_ACCOUNT, L"userid", FromUtf8(m_account.userid));
    ini.WriteString(SEC_ACCOUNT, L"t1", FromUtf8(m_account.t1));
    ini.WriteString(SEC_ACCOUNT, L"vip_type", FromUtf8(m_account.vip_type));
    ini.WriteString(SEC_ACCOUNT, L"vip_token", FromUtf8(m_account.vip_token));

    ini.Save();
}

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

bool CKugouSource::Request(const wstring& url_path, const wstring& router,
    const vector<pair<string, string>>& extra_params,
    const string& body, json& out_json, bool need_sign)
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
        params.push_back({ "userid", m_account.IsLoggedIn() ? m_account.userid : "0" });

    // 登录后带上 token
    if (m_account.IsLoggedIn())
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
            params.push_back({ "token", m_account.token });
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

    wstring url = wstring(API_BASE) + url_path + L"?" + FromUtf8(query);

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
        int ret = CInternetCommon::HttpGet(url, result, FromUtf8(headers_str), false);
        if (ret != CInternetCommon::SUCCESS || result.empty())
            return false;

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
    if (response.value("status", 0) != 1)
        return false;

    if (!response.contains("data") || !response["data"].contains("lists"))
        return false;

    // 注意：搜索接口返回的字段名是首字母大写的（FileHash、SingerName 这类），
    // 与其它接口的小写命名不同，这里按实际响应取值。
    for (const auto& item : response["data"]["lists"])
    {
        online::Track track;

        string hash = item.value("FileHash", "");
        if (hash.empty())
            continue;

        string mixsongid = item.value("MixSongID", "");
        // 虚拟路径里带上 MixSongID，取地址时要用
        track.virtual_path = FromUtf8("kugou://" + hash +
            (mixsongid.empty() ? "" : "?aaid=" + mixsongid));

        track.title = FromUtf8(item.value("OriSongName", ""));
        if (track.title.empty())
        {
            // 退而用「歌手 - 歌名」形式的完整文件名
            string file_name = item.value("FileName", "");
            size_t sep = file_name.find(" - ");
            track.title = FromUtf8(sep == string::npos ? file_name : file_name.substr(sep + 3));
        }

        track.artist = FromUtf8(item.value("SingerName", ""));
        track.album = FromUtf8(item.value("AlbumName", ""));
        track.duration_ms = item.value("Duration", 0) * 1000;   // 接口返回的是秒
        track.extra = FromUtf8(mixsongid);

        result.push_back(track);
    }

    return !result.empty();
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
    if (hash.empty())
        return wstring();

    // 音质从高到低尝试。拿不到高音质时自动降级，
    // 这样 VIP 过期或某档位无版权时仍能播。
    static const char* qualities[] = { "flac", "320", "128" };

    for (const char* quality : qualities)
    {
        char clienttime[32]{};
        sprintf_s(clienttime, "%lld", static_cast<long long>(time(nullptr)));

        // 取地址接口不签名，但需要 key。
        // userid 未登录时为 0，key 仍要按这个规则算。
        string userid = m_account.IsLoggedIn() ? m_account.userid : "0";
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
        if (m_account.IsLoggedIn())
        {
            params.push_back({ "token",  m_account.token });
            params.push_back({ "userid", m_account.userid });
        }
        else
        {
            // 未登录也必须带 userid=0，与搜索接口的规则一致
            params.push_back({ "userid", "0" });
        }

        json response;
        // 这个接口按官方实现是不签名的（用 key 代替 signature）
        if (!Request(L"/v5/url", ROUTER_TRACKER, params, "", response, false))
            continue;

        if (!response.contains("data"))
            continue;

        const auto& data = response["data"];
        if (!data.is_object())
            continue;

        // 有权限时直接给地址。优先用备用地址，主地址在部分网络下会返回 403。
        string url = data.value("backup_url", "");
        if (url.empty())
            url = data.value("url", "");

        if (!url.empty())
            return FromUtf8(url);

        // 没有地址时区分原因，便于界面给出准确提示而不是笼统的「播放失败」。
        // priv_status 为 0 且 fail_process 含 pkg/buy，说明这首歌需要购买或会员。
        int priv_status = data.value("priv_status", 1);
        bool need_purchase = false;
        if (data.contains("fail_process") && data["fail_process"].is_array())
        {
            for (const auto& f : data["fail_process"])
            {
                if (f.is_string())
                {
                    string reason = f.get<string>();
                    if (reason == "pkg" || reason == "buy")
                        need_purchase = true;
                }
            }
        }

        if (priv_status == 0 && need_purchase)
        {
            m_last_error = m_account.IsLoggedIn()
                ? L"这首歌需要购买或开通会员才能完整播放"
                : L"需要登录酷狗概念版账号才能播放这首歌";
            return wstring();       // 不必再降级尝试其它音质，权限都一样
        }
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

    // 第一步：用 hash 换歌词的 id 和 accesskey
    char clienttime[32]{};
    sprintf_s(clienttime, "%lld", static_cast<long long>(time(nullptr)));

    vector<pair<string, string>> params = {
        { "dfid",       m_device.dfid },
        { "mid",        m_device.mid },
        { "uuid",       "-" },
        { "appid",      LITE_APPID },
        { "clientver",  LITE_CLIENTVER },
        { "clienttime", clienttime },
        { "hash",       ToUtf8(hash) },
        { "man",        "yes" },
    };

    json search_result;
    if (!Request(L"/v1/search/lyric", ROUTER_LYRICS, params, "", search_result))
        return false;

    if (!search_result.contains("candidates") || search_result["candidates"].empty())
        return false;

    const auto& candidate = search_result["candidates"][0];
    string id = candidate.value("id", "");
    string accesskey = candidate.value("accesskey", "");
    if (id.empty() || accesskey.empty())
        return false;

    // 第二步：下载歌词。这里要 lrc 格式，krc 需要额外解密，先用能直接用的。
    wstring download_url = wstring(L"https://lyrics.kugou.com/download?id=") +
        FromUtf8(id) + L"&accesskey=" + FromUtf8(accesskey) +
        L"&fmt=lrc&charset=utf8&client=android&ver=1";

    wstring response_text;
    if (CInternetCommon::HttpGet(download_url, response_text, L"", false) != CInternetCommon::SUCCESS)
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

// 登录接口在 login-user.kugou.com，用 Web 签名，参数不带那套公共参数，
// 所以单独走这里，不复用 Request()。
// 注意：签名要用参数的「原始值」，拼进 URL 时才做 URL 编码，顺序不能反。
bool CKugouSource::RequestLoginApi(const wstring& url_path,
    const vector<pair<string, string>>& params, json& out_json)
{
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
    if (!query.empty())
        query += '&';
    query += "signature=";
    query += signature;

    wstring url = L"https://login-user.kugou.com" + url_path + L"?" + FromUtf8(query);

    string headers;
    headers += "User-Agent: " + string(DEFAULT_USER_AGENT) + "\r\n";

    wstring result;
    if (CInternetCommon::HttpGet(url, result, FromUtf8(headers), false) != CInternetCommon::SUCCESS
        || result.empty())
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

bool CKugouSource::GetQrCode(wstring& qr_content)
{
    qr_content.clear();
    m_last_error.clear();

    // 概念版客户端用 appid=1001，平台固定 4
    vector<pair<string, string>> params = {
        { "appid",      "1001" },
        { "type",       "1" },
        { "plat",       "4" },
        { "srcappid",   "2919" },
        { "qrcode_txt", "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1001&" },
    };

    json response;
    if (!RequestLoginApi(L"/v2/qrcode", params, response))
        return false;

    if (response.value("status", 0) != 1 || !response.contains("data"))
    {
        // 接口目前返回 error_code 20010。带上原话方便以后排查。
        string dumped = response.dump();
        if (dumped.size() > 160)
            dumped = dumped.substr(0, 160);
        m_last_error = L"获取登录二维码失败（接口返回：" + FromUtf8(dumped) + L"）";
        return false;
    }

    string key = response["data"].value("qrcode", "");
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
        { "appid",    "3116" },
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
    int status = data.value("status", -1);

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
        m_account.token = data.value("token", "");
        m_account.userid = data.value("userid", "");
        if (m_account.token.empty() || m_account.userid.empty())
        {
            m_last_error = L"登录成功但没拿到账号信息";
            return QrStatus::Failed;
        }
        m_last_error.clear();
        return QrStatus::Authorized;
    }
    default:
        m_last_error = L"登录状态异常（返回 " + to_wstring(status) + L"）";
        return QrStatus::Failed;
    }
}
void CKugouSource::Logout()
{
    m_account = Account();
    m_qr_key.clear();
}

} // namespace kugou
