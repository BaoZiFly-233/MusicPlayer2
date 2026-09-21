#pragma once
#include "OnlineSource.h"
#include "KugouCrypto.h"
#include "nlohmann/json.hpp"
#include <limits>
#include <ctime>

namespace online
{
// 平台字段会在数字和字符串之间切换；只接受这两种标量，禁止隐式解析对象。
inline std::string JsonText(const nlohmann::json& value, const char* key)
{
    auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return it->dump();
    return {};
}

inline int JsonNumber(const nlohmann::json& value, const char* key)
{
    std::string text = JsonText(value, key);
    if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    return std::stoi(text);
}

inline bool IsServiceId(const std::wstring& id)
{
    return !id.empty() && id.size() <= 128 && id.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") == std::wstring::npos;
}

inline std::wstring CoverUrl(const nlohmann::json& value)
{
    std::string url;
    for (const char* key : {"albumPic", "albumPic120", "AlbumSizableCover", "sizable_cover", "union_cover"})
    { url = JsonText(value, key); if (!url.empty()) break; }
    if (url.empty() && value.contains("trans_param")) url = JsonText(value["trans_param"], "union_cover");
    if (url.empty() && value.contains("album_info")) url = JsonText(value["album_info"], "sizable_cover");
    for (size_t pos; (pos = url.find("{size}")) != std::string::npos;) url.replace(pos, 6, "500");
    return (url.starts_with("https://") || url.starts_with("http://")) && url.find_first_of("\r\n") == std::string::npos
        ? kugou::FromUtf8(url) : L"";
}

inline bool JsonFlag(const nlohmann::json& data, const char* key)
{
    const auto value = data.find(key);
    return value != data.end() && (value->is_boolean() ? value->get<bool>() : JsonNumber(data, key) > 0);
}
inline void ReadKugouMembership(const nlohmann::json& data, AccountProfile& profile)
{
    if (!data.is_object()) return;
    const nlohmann::json* membership = &data;
    if (data.contains("busi_vip") && data["busi_vip"].is_array())
        for (const auto& item : data["busi_vip"])
            if (JsonText(item, "busi_type") == "concept") { membership = &item; if (JsonFlag(item, "is_vip")) break; }
    if (!membership->contains("is_vip")) return;
    const bool active = JsonFlag(*membership, "is_vip");
    profile.membership = active ? (JsonText(*membership, "product_type") == "svip" ? L"SVIP" : L"VIP") : L"非 VIP";
    profile.expires = active ? kugou::FromUtf8(JsonText(*membership, "vip_end_time")) : L"";
}
// 毫秒时间戳这类大数值超出 int 范围，需要用 64 位读取，否则会被截断。
inline long long JsonInt64(const nlohmann::json& value, const char* key)
{
    auto it = value.find(key);
    if (it == value.end()) return 0;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_string())
    {
        try { return std::stoll(it->get<std::string>()); }
        catch (...) { return 0; }
    }
    return 0;
}

inline AccountProfile ReadBodianProfile(const nlohmann::json& data)
{
    AccountProfile profile;
    const auto user = data.find("userInfo"), pay = data.find("payInfo");
    if (user != data.end() && user->is_object()) profile.name = kugou::FromUtf8(JsonText(*user, "nickname"));
    const bool known = (user != data.end() && user->contains("isVip")) || (pay != data.end() && pay->contains("isVipBoolean"));
    const bool active = (user != data.end() && JsonFlag(*user, "isVip")) || (pay != data.end() && JsonFlag(*pay, "isVipBoolean"));
    if (known) profile.membership = active ? L"VIP" : L"非 VIP";

    // 到期时间：活动会员在 actExpireDate，付费会员在 payExpireDate。都是毫秒时间戳。
    if (active && pay != data.end() && pay->is_object())
    {
        long long expires_ms = JsonInt64(*pay, "actExpireDate");
        if (expires_ms <= 0) expires_ms = JsonInt64(*pay, "payExpireDate");
        if (expires_ms <= 0) expires_ms = JsonInt64(*pay, "expireDate");
        if (expires_ms > 0)
        {
            const std::time_t seconds = static_cast<std::time_t>(expires_ms / 1000);
            std::tm local{};
            if (localtime_s(&local, &seconds) == 0)
            {
                wchar_t buffer[24]{};
                if (wcsftime(buffer, _countof(buffer), L"%Y-%m-%d", &local) > 0) profile.expires = buffer;
            }
        }
    }
    return profile;
}

// 从权限与音质字段推一个短语标记，界面直接显示，不做平台判断：
//   listen_fragment 为 1 表示只能听到片段，这是最需要提前告知用户的情况；
//   否则若资源里带无损档（level 为 ff，或格式是 flac/mflac），标「无损」。
inline std::wstring BodianBadge(const nlohmann::json& value)
{
    const auto pay = value.find("payInfo");
    if (pay != value.end() && pay->is_object())
    {
        const auto fragment = pay->find("listen_fragment");
        if (fragment != pay->end())
        {
            const std::string flag = fragment->is_string() ? fragment->get<std::string>()
                : fragment->is_number_integer() ? std::to_string(fragment->get<long long>()) : std::string();
            if (flag == "1" || flag == "true") return L"试听";
        }
    }

    const auto audios = value.find("audios");
    if (audios != value.end() && audios->is_array())
    {
        for (const auto& audio : *audios)
        {
            const std::string level = JsonText(audio, "level");
            const std::string format = JsonText(audio, "format");
            if (level == "ff" || format == "flac" || format == "mflac") return L"无损";
        }
    }
    return {};
}

inline Track BodianTrack(const nlohmann::json& value)
{
    Track track;
    std::string id = JsonText(value, "musicRid");
    if (id.compare(0, 6, "MUSIC_") == 0) id.erase(0, 6);
    if (id.empty()) id = JsonText(value, "id");
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return track;
    track.virtual_path = kugou::FromUtf8("bodian://" + id);
    track.title = kugou::FromUtf8(JsonText(value, "name"));
    track.artist = kugou::FromUtf8(JsonText(value, "artist"));
    track.album = kugou::FromUtf8(JsonText(value, "album"));
    track.cover_url = kugou::FromUtf8(JsonText(value, "albumPic"));
    if (track.cover_url.empty()) track.cover_url = kugou::FromUtf8(JsonText(value, "albumPic120"));
    int seconds = JsonNumber(value, "duration");
    if (seconds < 2147483) track.duration_ms = seconds * 1000;
    track.badge = BodianBadge(value);
    return track;
}

inline bool ParseBodianPlaylistId(const std::wstring& value, std::wstring& id, std::wstring& source)
{
    const auto separator = value.rfind(L'_');
    id = value.substr(0, separator);
    source = separator == std::wstring::npos ? L"5" : value.substr(separator + 1);
    return !id.empty() && id.size() <= 32 && id.find_first_not_of(L"0123456789") == std::wstring::npos
        && (source == L"4" || source == L"5" || source == L"13");
}

inline bool KugouRewardReceived(const nlohmann::json& data, const std::string& day)
{
    if (!data.contains("list") || !data["list"].is_array()) return false;
    for (const auto& item : data["list"])
        if (JsonText(item, "day") == day && JsonNumber(item, "receive_vip") == 1) return true;
    return false;
}

inline void AddBodianPlaylist(BrowseResult& result, const nlohmann::json& value, const std::wstring& subtitle = L"")
{
    BrowseItem item; item.type = BrowseItem::Type::Playlist;
    auto id = JsonText(value, "id");
    auto source = JsonText(value, "source");
    if (source.empty()) source = JsonText(value, "sourceType");
    if (source.empty()) source = "5";
    item.id = kugou::FromUtf8(id + "_" + source);
    std::wstring parsed_id, parsed_source;
    if (!ParseBodianPlaylistId(item.id, parsed_id, parsed_source)) return;
    item.title = kugou::FromUtf8(JsonText(value, "name"));
    if (item.title.empty()) item.title = kugou::FromUtf8(JsonText(value, "title"));
    if (item.title.empty()) item.title = subtitle;
    item.subtitle = subtitle.empty() ? kugou::FromUtf8(JsonText(value, "creator_name")) : subtitle;
    const auto count = JsonText(value, "musicnum");
    if (!count.empty()) item.subtitle += L" · " + kugou::FromUtf8(count) + L" 首";
    result.items.push_back(std::move(item));
}


// 上游系的双语歌词有个坑：译文行的时间标签标的是「下一句原文」的时间，
// 而不是它自己那句。播放器按「同时间戳两行、前原文后译文」配对翻译
// （见 CLyrics::CombineSameTimeLyric），遇到这种错位会把两者整个配反 ——
// 译文被当原文、原文被当译文。这里把译文的时间改回它所属原文的时间。
inline std::wstring AlignLyricTranslation(const std::wstring& lyric)
{
    std::vector<std::wstring> lines;
    size_t begin = 0;
    for (size_t i = 0; i <= lyric.size(); ++i)
    {
        if (i == lyric.size() || lyric[i] == L'\n')
        {
            std::wstring line = lyric.substr(begin, i - begin);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(std::move(line));
            begin = i + 1;
        }
    }
    if (lines.size() < 4) return lyric;

    // 解析 [mm:ss.xx] 这类时间标签，返回毫秒；不是时间行就返回 false
    auto parse_time = [](const std::wstring& line, long long& ms, size_t& tag_len) -> bool
    {
        if (line.size() < 8 || line[0] != L'[') return false;
        const size_t close = line.find(L']');
        if (close == std::wstring::npos || close < 7) return false;
        const std::wstring tag = line.substr(1, close - 1);
        if (tag.size() < 7 || tag[2] != L':' || (tag[5] != L'.' && tag[5] != L':')) return false;
        for (size_t i = 0; i < tag.size(); ++i)
        {
            if (i == 2 || i == 5) continue;
            if (tag[i] < L'0' || tag[i] > L'9') return false;
        }
        const int mm = (tag[0] - L'0') * 10 + (tag[1] - L'0');
        const int ss = (tag[3] - L'0') * 10 + (tag[4] - L'0');
        int frac = 0;
        const size_t digits = (std::min)(tag.size(), static_cast<size_t>(9)) - 6;
        for (size_t i = 6; i < 6 + digits; ++i) frac = frac * 10 + (tag[i] - L'0');
        const int scale[] = { 1, 100, 10, 1 };
        ms = ((mm * 60LL) + ss) * 1000 + frac * scale[digits];
        tag_len = close + 1;
        return true;
    };

    std::vector<long long> times(lines.size(), -1);
    std::vector<size_t> tag_len(lines.size(), 0);
    for (size_t i = 0; i < lines.size(); ++i) parse_time(lines[i], times[i], tag_len[i]);

    // 先判断到底是不是错位格式，是的话才动手，避免误伤正常歌词
    int shifted = 0, normal = 0;
    for (size_t i = 0; i + 2 < lines.size(); ++i)
    {
        if (times[i] < 0 || times[i + 1] < 0 || times[i + 2] < 0) continue;
        if (times[i] == times[i + 1]) ++normal;
        else if (times[i + 1] == times[i + 2]) ++shifted;
    }
    if (shifted == 0 || shifted <= normal) return lyric;

    std::vector<long long> fixed = times;
    for (size_t i = 0; i + 2 < lines.size(); ++i)
    {
        if (times[i] < 0 || times[i + 1] < 0 || times[i + 2] < 0) continue;
        if (times[i] != times[i + 1] && times[i + 1] == times[i + 2]) fixed[i + 1] = times[i];
    }

    std::wstring out;
    out.reserve(lyric.size() + 64);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (fixed[i] >= 0 && fixed[i] != times[i])
        {
            wchar_t tag[24]{};
            const long long ms = fixed[i];
            swprintf_s(tag, L"[%02lld:%02lld.%02lld]", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 10);
            out += tag;
            out += lines[i].substr(tag_len[i]);
        }
        else out += lines[i];
        if (i + 1 < lines.size()) out += L'\n';
    }
    return out;
}

// K源的权限字段：AlbumPrivilege 为 8 表示只能试听（最该提前告知用户），
// 为 10 是完整播放；SQ 是个对象，带 filesize，有值说明这首提供无损。
inline std::wstring KugouBadge(const nlohmann::json& value)
{
    if (JsonNumber(value, "AlbumPrivilege") == 8) return L"试听";

    const auto sq = value.find("SQ");
    if (sq != value.end() && sq->is_object() && JsonInt64(*sq, "filesize") > 0) return L"无损";

    // 专辑曲目与榜单走的是另一套返回结构：权限在 copyright 里，音质看 audio_info 有没有高档 hash。
    // 不补这一段的话，专辑曲目在列表里会全部没有状态标记。
    const auto copyright = value.find("copyright");
    if (copyright != value.end() && copyright->is_object())
    {
        if (JsonNumber(*copyright, "privilege") == 8 || JsonNumber(*copyright, "privilege_128") == 8
            || JsonNumber(*copyright, "privilege_320") == 8 || JsonNumber(*copyright, "privilege_flac") == 8)
            return L"试听";
    }
    const auto info = value.find("audio_info");
    if (info != value.end() && info->is_object()
        && (!JsonText(*info, "hash_flac").empty() || !JsonText(*info, "hash_super").empty()))
        return L"无损";
    return {};
}

inline Track KugouTrack(const nlohmann::json& value)
{
    Track track;
    const auto& audio = value.contains("audio_info") && value["audio_info"].is_object() ? value["audio_info"] : value;
    // 专辑曲目接口把标题、作者和编号放在 base 里，其余接口放在外层，这里两边都认。
    const auto& base = value.contains("base") && value["base"].is_object() ? value["base"] : value;
    std::string hash = JsonText(audio, "hash");
    if (hash.empty()) hash = JsonText(audio, "hash_128");
    if (hash.empty()) hash = JsonText(value, "FileHash");
    if (hash.size() != 32 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return track;
    std::string id = JsonText(value, "album_audio_id");
    if (id.empty()) id = JsonText(base, "album_audio_id");
    if (id.empty()) id = JsonText(value, "MixSongID");
    if (id.empty() && value.contains("album_info")) id = JsonText(value["album_info"], "album_audio_id");
    if (id.find_first_not_of("0123456789") != std::string::npos) id.clear();
    track.virtual_path = kugou::FromUtf8("kugou://" + hash + (id.empty() ? "" : "?aaid=" + id));
    std::string title = JsonText(value, "OriSongName");
    if (title.empty()) title = JsonText(value, "songname");
    if (title.empty()) title = JsonText(base, "audio_name");
    if (title.empty()) title = JsonText(audio, "audio_name");
    if (title.empty()) title = JsonText(value, "name");
    if (title.empty()) title = JsonText(value, "filename");
    track.title = kugou::FromUtf8(title);
    std::string artist = JsonText(value, "SingerName");
    if (artist.empty()) artist = JsonText(value, "singername");
    if (artist.empty()) artist = JsonText(base, "author_name");
    if (artist.empty() && value.contains("authors") && value["authors"].is_array())
    {
        for (const auto& author : value["authors"])
        {
            if (!artist.empty()) artist += " / ";
            artist += JsonText(author, "author_name");
        }
    }
    track.artist = kugou::FromUtf8(artist);
    std::string album = JsonText(value, "AlbumName");
    if (album.empty()) album = JsonText(value, "album_name");
    if (album.empty() && value.contains("album_info")) album = JsonText(value["album_info"], "album_name");
    track.album = kugou::FromUtf8(album);
    track.cover_url = CoverUrl(value);
    track.duration_ms = JsonNumber(audio, "timelength");
    if (track.duration_ms == 0) track.duration_ms = JsonNumber(audio, "duration_128");
    if (track.duration_ms == 0)
    {
        int seconds = JsonNumber(value, "Duration");
        if (seconds == 0) seconds = JsonNumber(value, "duration");
        if (seconds < 2147483) track.duration_ms = seconds * 1000;
    }
    track.badge = KugouBadge(value);
    return track;
}

inline void AddTrack(BrowseResult& result, const Track& track)
{
    if (!track.IsValid()) return;
    BrowseItem item;
    item.track = track;
    item.title = track.title;
    // 这里只放艺术家：宽屏会在「专辑」列单独显示专辑，窄屏才把两者合并。
    // 具体怎么拼由显示层按列宽决定，数据和呈现不在这里耦合。
    item.subtitle = track.artist;
    item.badge = track.badge;
    result.items.push_back(std::move(item));
}

struct KugouPlayback
{
    std::wstring url;
    std::wstring error;
    bool retry_quality{true};
};

inline KugouPlayback ParseKugouPlayback(const nlohmann::json& response, bool logged_in)
{
    KugouPlayback result;
    if (!response.is_object())
    {
        result.error = L"K源播放接口返回格式异常";
        result.retry_quality = false;
        return result;
    }
    auto code = JsonText(response, "errcode");
    if (code.empty()) code = JsonText(response, "error_code");
    if ((response.contains("status") && JsonNumber(response, "status") == 0) || (!code.empty() && code != "0"))
    {
        result.error = code == "20006" ? L"K源播放请求校验失败（错误 20006）"
            : L"K源未接受播放请求" + (code.empty() ? std::wstring() : L"（错误 " + kugou::FromUtf8(code) + L"）");
        result.retry_quality = false;
        return result;
    }
    // v5/url 的当前响应在根对象中返回 url/backupUrl 数组；兼容旧 data 包装和单个字符串。
    const auto& data = response.contains("data") && response["data"].is_object() ? response["data"] : response;
    if (data.contains("priv_status") && JsonNumber(data, "priv_status") == 0)
    {
        result.error = logged_in ? L"当前账号没有此歌曲或音质的播放权限，请检查平台会员或购买状态"
            : L"请登录K源账号后重试";
        return result;
    }
    auto playable = [](const nlohmann::json& value) -> std::wstring {
        if (!value.is_string()) return {};
        const auto& url = value.get_ref<const std::string&>();
        if ((url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
            || url.find_first_of("\r\n") != std::string::npos) return {};
        return kugou::FromUtf8(url);
    };
    for (const char* key : {"url", "backupUrl", "backup_url"})
    {
        auto urls = data.find(key);
        if (urls == data.end()) continue;
        if (urls->is_array())
        {
            for (const auto& candidate : *urls)
            {
                result.url = playable(candidate);
                if (!result.url.empty()) return result;
            }
        }
        else result.url = playable(*urls);
        if (!result.url.empty()) return result;
    }
    result.error = L"K源未返回可播放地址，请稍后重试或检查曲目权限";
    return result;
}
}
