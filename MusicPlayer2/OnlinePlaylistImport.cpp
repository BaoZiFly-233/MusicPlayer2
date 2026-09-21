#include "stdafx.h"
#include "OnlinePlaylistImport.h"
#include "Playlist.h"
#include "Common.h"
#include "OnlineHttp.h"
#include "KugouCrypto.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <climits>
#include <cwctype>
#include <set>

using namespace std;

namespace online
{
namespace
{

// 归一化：转小写、去掉所有空白。
// 中英文歌名的空格对匹配没有意义，去掉可以少受排版差异影响。
wstring Normalize(const wstring& text)
{
    wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text)
    {
        if (iswspace(ch)) continue;
        result.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return result;
}

// 去掉括号及其内容。用于比对歌名和歌手 —— 比如「泫雅 (HyunA)」要和「泫雅」匹配得上。
// 注意：查版本词时不能用这个结果，否则「(Live)」里的 live 会一起被删掉，黑名单就失效了。
wstring StripBrackets(const wstring& text)
{
    wstring result;
    result.reserve(text.size());
    int depth = 0;
    for (wchar_t ch : text)
    {
        if (ch == L'(' || ch == L'（' || ch == L'[' || ch == L'【' || ch == L'{')
            ++depth;
        else if (ch == L')' || ch == L'）' || ch == L']' || ch == L'】' || ch == L'}')
        {
            if (depth > 0) --depth;
        }
        else if (depth == 0)
            result.push_back(ch);
    }
    return result;
}

// 把歌手名拆成单个歌手。外部平台用「/」、B源用「&」，还有中文顿号等。
// 必须拆开两两比较：同一歌手的写法可能一个是中文名一个是外文名，整串比会把正确结果误杀。
// 注意间隔号「·」不算分隔符：它只出现在中文音译名里（迈克尔·杰克逊），
// 拆开之后两边都变成半截名字，和外文的 Michael Jackson 一个字都对不上。
vector<wstring> SplitArtists(const wstring& artist)
{
    vector<wstring> result;
    wstring current;
    for (wchar_t ch : artist)
    {
        const bool separator = ch == L'/' || ch == L'&' || ch == L'、' || ch == L';'
            || ch == L'；' || ch == L',' || ch == L'，' || ch == L'|';
        if (separator)
        {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        }
        else
            current.push_back(ch);
    }
    if (!current.empty()) result.push_back(current);
    if (result.empty()) result.push_back(artist);
    return result;
}

// 带这些标记的通常是 Live、翻唱、伴奏之类，不是用户想听的原版
const wchar_t* VERSION_WORDS[] = {
    L"live", L"remix", L"remaster", L"cover", L"acoustic", L"instrumental",
    L"karaoke", L"concert", L"slowed", L"version", L"demo",
    L"现场", L"片段", L"翻唱", L"伴奏", L"演唱会", L"铃声",
    L"音乐银行", L"人气歌谣", L"dj版", L"抖音版", L"加快版", L"慢速版",
};

// 数一个歌名里有多少个版本词。source_words 是源歌名里本来就有的词，这些不扣分
// （否则《Live Forever》这种正常歌名会被误杀）。
// 英文词必须落在词首：归一化把空格都去掉了，只按子串找的话 Olive、Discover、Democracy
// 会被当成 live、cover、demo 命中，正常歌名白白挨一次扣分。
bool ContainsVersionWord(const wstring& text, const wstring& word)
{
    const bool ascii_word = !word.empty() && word.front() < 128;
    size_t pos = text.find(word);
    while (pos != wstring::npos)
    {
        if (!ascii_word || pos == 0 || !iswalnum(text[pos - 1])) return true;
        pos = text.find(word, pos + 1);
    }
    return false;
}

int CountVersionWords(const wstring& normalized, const set<wstring>& source_words)
{
    int count = 0;
    for (const wchar_t* word : VERSION_WORDS)
    {
        if (source_words.count(word) != 0) continue;
        if (ContainsVersionWord(normalized, word)) ++count;
    }
    return count;
}

struct CandidateScore
{
    int score{};
    int version_hits{};
    bool rejected{};
};

// 给单个候选打分。只在双方都有值的字段上加权，
// 避免「接口没返回时长」这种缺失把分数无端压低。
CandidateScore ScoreOne(const ImportTrack& source, const Track& candidate,
    const set<wstring>& source_words)
{
    CandidateScore result;

    const wstring source_title = Normalize(source.title);
    const wstring candidate_title = Normalize(candidate.title);
    if (source_title.empty() || candidate_title.empty()) { result.rejected = true; return result; }

    // 歌名：整串和去括号后各算一次，取高的
    const int title_full = Similarity(source_title, candidate_title);
    const int title_stripped = Similarity(Normalize(StripBrackets(source.title)),
        Normalize(StripBrackets(candidate.title)));
    const int title_score = (max)(title_full, title_stripped);

    // 歌手：拆成单个歌手后两两比，取最高的那一对。
    // 两边都要有值才算这一项，否则跳过 —— 否则「源曲没带歌手」会被比成 0 分，
    // 然后被下面的硬淘汰判掉，整首歌永远匹配不上（实测外部导入和换源都会丢歌）。
    int artist_score = -1;
    const wstring source_artist = StripBrackets(source.artist);
    const wstring candidate_artist = StripBrackets(candidate.artist);
    if (!Normalize(source_artist).empty() && !Normalize(candidate_artist).empty())
    {
        for (const auto& left : SplitArtists(source_artist))
        {
            if (Normalize(left).empty()) continue;
            for (const auto& right : SplitArtists(candidate_artist))
            {
                if (Normalize(right).empty()) continue;
                artist_score = (max)(artist_score, Similarity(Normalize(left), Normalize(right)));
            }
        }
    }

    // 时长：3 秒内不罚，30 秒以上不要。双方缺任一时长时这一项不参与
    int duration_score = -1;
    int duration_diff = 0;
    if (source.duration_ms > 0 && candidate.duration_ms > 0)
    {
        duration_diff = abs(source.duration_ms - candidate.duration_ms);
        if (duration_diff <= 3000) duration_score = 100;
        else if (duration_diff >= 30000) duration_score = 0;
        else duration_score = 100 - (duration_diff - 3000) * 100 / 27000;
    }

    const double TITLE_WEIGHT = 3.0, ARTIST_WEIGHT = 2.0, DURATION_WEIGHT = 2.0;
    double weighted = title_score * TITLE_WEIGHT;
    double weights = TITLE_WEIGHT;
    if (artist_score >= 0) { weighted += artist_score * ARTIST_WEIGHT; weights += ARTIST_WEIGHT; }
    if (duration_score >= 0) { weighted += duration_score * DURATION_WEIGHT; weights += DURATION_WEIGHT; }

    // 版本词：用「未去括号」的归一化串来查
    result.version_hits = CountVersionWords(candidate_title, source_words);
    int score = static_cast<int>(weighted / weights + 0.5) - result.version_hits * 20;

    // 硬淘汰
    if (title_score < 60) result.rejected = true;
    if (artist_score >= 0 && artist_score < 70) result.rejected = true;
    if (duration_diff > 30000) result.rejected = true;

    result.score = (max)(0, score);
    return result;
}

// 从文本里把所有链接挑出来。用户分享出来的往往是一整段话，链接夹在中间。
vector<wstring> ExtractUrls(const wstring& text)
{
    vector<wstring> urls;
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t start = text.find(L"http", pos);
        if (start == wstring::npos) break;
        size_t end = start;
        while (end < text.size() && !iswspace(text[end]) && text[end] != L'"'
            && text[end] != L'>' && text[end] != L'<' && text[end] != L'\r' && text[end] != L'\n')
            ++end;
        wstring url = text.substr(start, end - start);
        while (!url.empty() && (url.back() == L'.' || url.back() == L',' || url.back() == L')'
            || url.back() == L'。' || url.back() == L'，'))
            url.pop_back();
        if (!url.empty()) urls.push_back(url);
        pos = end;
    }
    return urls;
}

// 在文本里取「key=数字」里的数字
wstring NumberAfter(const wstring& text, const wstring& key)
{
    const size_t at = text.find(key);
    if (at == wstring::npos) return wstring();
    size_t start = at + key.size();
    size_t end = start;
    while (end < text.size() && text[end] >= L'0' && text[end] <= L'9') ++end;
    return text.substr(start, end - start);
}

// 取「/xxx/数字」这种路径里的数字
wstring NumberInPath(const wstring& text, const wstring& segment)
{
    const size_t at = text.find(segment);
    if (at == wstring::npos) return wstring();
    size_t start = at + segment.size();
    if (start < text.size() && text[start] == L'/') ++start;
    size_t end = start;
    while (end < text.size() && text[end] >= L'0' && text[end] <= L'9') ++end;
    return text.substr(start, end - start);
}

wstring ToLower(const wstring& text)
{
    wstring result = text;
    transform(result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return result;
}

bool IsShortLink(const wstring& host)
{
    return host.find(L"163cn.tv") != wstring::npos
        || host.find(L"url.cn") != wstring::npos
        || host.find(L"t.cn") != wstring::npos
        || host.find(L"c6.y.qq.com") != wstring::npos;
}

} // namespace

int Similarity(const wstring& a, const wstring& b)
{
    if (a.empty() && b.empty()) return 100;
    if (a.empty() || b.empty()) return 0;
    if (a == b) return 100;

    const size_t n = a.size(), m = b.size();
    vector<size_t> previous(m + 1), current(m + 1);
    for (size_t j = 0; j <= m; ++j) previous[j] = j;
    for (size_t i = 1; i <= n; ++i)
    {
        current[0] = i;
        for (size_t j = 1; j <= m; ++j)
        {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            current[j] = (min)({ previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost });
        }
        previous.swap(current);
    }

    const size_t longest = (max)(n, m);
    const size_t distance = previous[m];
    if (distance >= longest) return 0;
    return static_cast<int>((longest - distance) * 100 / longest);
}

MatchResult PickBest(const ImportTrack& source, const vector<Track>& candidates)
{
    MatchResult result;

    // 源歌名里本来就有的版本词不参与扣分
    set<wstring> source_words;
    const wstring source_normalized = Normalize(source.title);
    for (const wchar_t* word : VERSION_WORDS)
        if (ContainsVersionWord(source_normalized, word)) source_words.insert(word);

    int best_version_hits = INT_MAX;
    for (const auto& candidate : candidates)
    {
        const auto scored = ScoreOne(source, candidate, source_words);
        if (scored.rejected) continue;

        // 双键排序：先看版本词命中数（干净版优先），再看分数。
        // 只看分数不够 —— 带 (Live) 的版本歌名歌手都接近满分，扣分后仍可能压过原版。
        if (scored.version_hits < best_version_hits
            || (scored.version_hits == best_version_hits && scored.score > result.score))
        {
            best_version_hits = scored.version_hits;
            result.score = scored.score;
            result.candidate = candidate;
        }
    }

    if (result.candidate.virtual_path.empty())
        result.level = MatchLevel::NotFound;
    else if (result.score >= 80 && best_version_hits == 0)
        result.level = MatchLevel::Auto;
    else if (result.score >= 65)
        result.level = MatchLevel::Confirm;
    else
        result.level = MatchLevel::NotFound;

    return result;
}

ImportReference ParseShareText(const wstring& text)
{
    ImportReference reference;
    if (text.empty()) return reference;

    // 先看有没有链接；没有就把整段文本当作可能含编号的输入
    vector<wstring> pieces = ExtractUrls(text);
    if (pieces.empty()) pieces.push_back(text);

    for (const auto& piece : pieces)
    {
        const wstring lower = ToLower(piece);

        if (lower.find(L"music.163.com") != wstring::npos || lower.find(L"163cn.tv") != wstring::npos)
        {
            reference.source = ImportSource::Netease;
            // 短链本身不含编号，先把完整地址存下来，取歌单时再跟随跳转
            if (IsShortLink(lower)) { reference.id = piece; return reference; }
            wstring id = NumberAfter(lower, L"id=");
            if (id.empty()) id = NumberInPath(lower, L"playlist");
            if (!id.empty()) { reference.id = id; return reference; }
            reference = ImportReference();
        }

        if (lower.find(L"y.qq.com") != wstring::npos || lower.find(L"i.y.qq.com") != wstring::npos
            || lower.find(L"c6.y.qq.com") != wstring::npos)
        {
            reference.source = ImportSource::QQ;
            if (IsShortLink(lower)) { reference.id = piece; return reference; }
            wstring id = NumberAfter(lower, L"id=");
            if (id.empty()) id = NumberInPath(lower, L"playlist");
            if (id.empty()) id = NumberAfter(lower, L"disstid=");
            if (!id.empty()) { reference.id = id; return reference; }
        }

        if (lower.find(L"kuwo.cn") != wstring::npos)
        {
            reference.source = ImportSource::Kuwo;
            wstring id = NumberInPath(lower, L"playlist_detail");
            if (id.empty()) id = NumberAfter(lower, L"pid=");
            if (!id.empty()) { reference.id = id; return reference; }
        }
    }

    reference = ImportReference();
    return reference;
}

namespace
{

// 跟随 302 取最终地址。短链只能这样拿到歌单编号。
wstring ResolveRedirect(const wstring& url, wstring& error)
{
    URL_COMPONENTS parts{ sizeof(URL_COMPONENTS) };
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    const bool secure = url.compare(0, 8, L"https://") == 0;
    const wstring rest = secure ? url.substr(8) : (url.compare(0, 7, L"http://") == 0 ? url.substr(7) : wstring());
    if (rest.empty() || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        error = L"分享链接格式不对";
        return wstring();
    }

    wstring host(parts.lpszHostName, parts.dwHostNameLength);
    wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    auto close = [](void* handle) { if (handle) WinHttpCloseHandle(handle); };
    using Handle = unique_ptr<void, decltype(close)>;
    Handle session(WinHttpOpen(L"MusicPlayer2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), close);
    if (!session) { error = L"无法初始化网络连接"; return wstring(); }
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 15000);

    const INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    Handle connection(WinHttpConnect(session.get(), host.c_str(), port, 0), close);
    Handle request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr, close);
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        error = L"打开分享链接失败";
        return wstring();
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status < 300 || status >= 400) { error = L"分享链接没有跳转"; return wstring(); }

    wchar_t location[2048]{};
    size = sizeof(location);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
        location, &size, WINHTTP_NO_HEADER_INDEX))
    {
        error = L"分享链接没有给出跳转地址";
        return wstring();
    }
    return wstring(location);
}

// 外部平台：先取歌单详情拿到完整 id 列表，再分批补全歌曲信息。
// 注意普通歌单的 tracks 字段只给 10 首（是固定上限，不是版权过滤），只能信 trackIds。
bool FetchNetease(const wstring& id, vector<ImportTrack>& tracks, wstring& error,
    const function<bool()>& cancelled, wstring* playlist_name)
{
    const wstring detail_url = L"https://music.163.com/api/v6/playlist/detail?id=" + id + L"&n=1000";
    wstring detail_text;
    if (!HttpRequest(detail_url, string(), wstring(), detail_text, error)) return false;
    if (cancelled()) return false;

    vector<wstring> ids;
    try
    {
        const auto root = nlohmann::json::parse(kugou::ToUtf8(detail_text));
        if (!root.contains("playlist")) { error = L"这个歌单不存在或没有公开"; return false; }
        const auto& playlist = root["playlist"];
        if (playlist_name) *playlist_name = kugou::FromUtf8(playlist.value("name", ""));
        const auto& track_ids = playlist.value("trackIds", nlohmann::json::array());
        for (const auto& item : track_ids)
        {
            if (item.is_object() && item.contains("id"))
                ids.push_back(kugou::FromUtf8(to_string(item["id"].get<long long>())));
        }
    }
    catch (const nlohmann::json::exception&)
    {
        error = L"歌单数据解析失败";
        return false;
    }

    if (ids.empty()) { error = L"这个歌单里没有歌曲"; return false; }

    // 分批补全，一次 100 个（实测 200 也可以，100 更稳、URL 更短）
    const size_t BATCH = 100;
    for (size_t start = 0; start < ids.size(); start += BATCH)
    {
        if (cancelled()) return false;
        wstring joined;
        for (size_t i = start; i < ids.size() && i < start + BATCH; ++i)
        {
            if (!joined.empty()) joined += L",";
            joined += ids[i];
        }
        const wstring url = L"https://music.163.com/api/song/detail?ids=[" + joined + L"]";
        wstring text;
        if (!HttpRequest(url, string(), wstring(), text, error)) return false;

        try
        {
            const auto root = nlohmann::json::parse(kugou::ToUtf8(text));
            for (const auto& song : root.value("songs", nlohmann::json::array()))
            {
                ImportTrack track;
                track.title = kugou::FromUtf8(song.value("name", ""));
                wstring artist;
                for (const auto& one : song.value("artists", nlohmann::json::array()))
                {
                    const wstring name = kugou::FromUtf8(one.value("name", ""));
                    if (name.empty()) continue;
                    if (!artist.empty()) artist += L"/";
                    artist += name;
                }
                track.artist = artist;
                if (song.contains("album") && song["album"].is_object())
                    track.album = kugou::FromUtf8(song["album"].value("name", ""));
                track.duration_ms = song.value("duration", 0);
                if (track.IsValid()) tracks.push_back(std::move(track));
            }
        }
        catch (const nlohmann::json::exception&)
        {
            error = L"歌曲数据解析失败";
            return false;
        }
    }

    if (tracks.empty()) { error = L"没有取到任何歌曲"; return false; }
    return true;
}

// QQ 音乐：一次请求就能拿到全部曲目（实测 2000 多首也是全量返回）。
// 唯一门槛是必须带 Referer，否则接口直接拒绝。
bool FetchQQ(const wstring& id, vector<ImportTrack>& tracks, wstring& error,
    const function<bool()>& cancelled, wstring* playlist_name)
{
    const wstring url = L"https://i.y.qq.com/qzone-music/fcg-bin/fcg_ucc_getcdinfo_byids_cp.fcg"
        L"?type=1&json=1&utf8=1&onlysong=0&nosign=1&disstid=" + id +
        L"&g_tk=5381&loginUin=0&hostUin=0&format=json"
        L"&inCharset=GB2312&outCharset=utf-8&notice=0&platform=yqq&needNewCode=0";
    const wstring headers = L"Referer: https://y.qq.com/\r\n";

    wstring text;
    if (!HttpRequest(url, string(), headers, text, error)) return false;
    if (cancelled()) return false;

    try
    {
        const auto root = nlohmann::json::parse(kugou::ToUtf8(text));
        if (root.value("code", -1) != 0)
        {
            error = L"这个歌单不存在、未公开，或者是私密歌单（需要登录）";
            return false;
        }
        const auto list = root.value("cdlist", nlohmann::json::array());
        if (list.empty()) { error = L"歌单里没有歌曲"; return false; }
        if (playlist_name) *playlist_name = kugou::FromUtf8(list[0].value("dissname", ""));

        for (const auto& song : list[0].value("songlist", nlohmann::json::array()))
        {
            ImportTrack track;
            track.title = kugou::FromUtf8(song.value("songname", ""));
            wstring artist;
            for (const auto& one : song.value("singer", nlohmann::json::array()))
            {
                const wstring name = kugou::FromUtf8(one.value("name", ""));
                if (name.empty()) continue;
                if (!artist.empty()) artist += L"/";
                artist += name;
            }
            track.artist = artist;
            track.album = kugou::FromUtf8(song.value("albumname", ""));
            track.duration_ms = song.value("interval", 0) * 1000;   // 接口给的是秒
            if (track.IsValid()) tracks.push_back(std::move(track));
        }
    }
    catch (const nlohmann::json::exception&)
    {
        error = L"歌单数据解析失败";
        return false;
    }

    if (tracks.empty()) { error = L"没有取到任何歌曲"; return false; }
    return true;
}

} // namespace

bool FetchPlaylist(const ImportReference& reference, vector<ImportTrack>& tracks,
    wstring& error, const function<bool()>& cancelled, wstring* playlist_name)
{
    tracks.clear();
    error.clear();
    if (reference.source == ImportSource::Unknown || reference.id.empty())
    {
        error = L"没认出这是哪个平台的歌单链接，请确认链接完整";
        return false;
    }

    // 短链要先跟随跳转，拿到真正带编号的地址
    ImportReference resolved = reference;
    if (IsShortLink(ToLower(reference.id)))
    {
        wstring redirect_error;
        const wstring location = ResolveRedirect(reference.id, redirect_error);
        if (location.empty()) { error = redirect_error; return false; }
        resolved = ParseShareText(location);
        if (!resolved.IsValid()) { error = L"分享链接跳转后没找到歌单编号"; return false; }
    }

    if (cancelled()) return false;

    switch (resolved.source)
    {
    case ImportSource::Netease:
        return FetchNetease(resolved.id, tracks, error, cancelled, playlist_name);
    case ImportSource::QQ:
        return FetchQQ(resolved.id, tracks, error, cancelled, playlist_name);
    default:
        error = L"暂不支持这个平台的歌单";
        return false;
    }
}

int ApplySourceMatches(vector<SongInfo>& playlist, const vector<SongInfo>& original, const vector<SongInfo>& matched)
{
    int replaced = 0;
    for (auto& song : playlist)
    {
        for (size_t i = 0; i < original.size() && i < matched.size(); ++i)
        {
            if (!(song == original[i]) || matched[i].file_path == original[i].file_path
                || !CSourceRegistry::IsVirtualPath(original[i].file_path)
                || !CSourceRegistry::IsVirtualPath(matched[i].file_path)) continue;
            song.file_path = matched[i].file_path;
            song.title = matched[i].title; song.artist = matched[i].artist; song.album = matched[i].album;
            song.start_pos.fromInt(0); song.end_pos = matched[i].end_pos;
            song.is_cue = false; song.cue_file_path.clear(); song.lyric_file.clear();
            song.bitrate = 0; song.freq = 0; song.bits = 0; song.channels = 0;
            song.SetChannelInfoAcquired(false);
            ++replaced;
            break;
        }
    }
    return replaced;
}
bool SaveSourceChanges(const vector<SongInfo>& playlist, const wstring& path, wstring& error)
{
    if (path.empty()) { error = L"没有可写入的原歌单"; return false; }
    if (CCommon::FileExist(path) && !CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE))
    { error = L"无法备份原歌单，未应用换源结果"; return false; }
    if (!CPlaylistFile::SavePlaylistToFile(playlist, path))
    { error = L"歌单保存失败，原内容已保留"; return false; }
    return true;
}
} // namespace online
