#pragma once
#include "OnlineSource.h"
#include "KugouCrypto.h"
#include "nlohmann/json.hpp"
#include <limits>

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
inline AccountProfile ReadBodianProfile(const nlohmann::json& data)
{
    AccountProfile profile;
    const auto user = data.find("userInfo"), pay = data.find("payInfo");
    if (user != data.end() && user->is_object()) profile.name = kugou::FromUtf8(JsonText(*user, "nickname"));
    const bool known = (user != data.end() && user->contains("isVip")) || (pay != data.end() && pay->contains("isVipBoolean"));
    const bool active = (user != data.end() && JsonFlag(*user, "isVip")) || (pay != data.end() && JsonFlag(*pay, "isVipBoolean"));
    if (known) profile.membership = active ? L"VIP" : L"非 VIP";
    return profile;
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

inline Track KugouTrack(const nlohmann::json& value)
{
    Track track;
    const auto& audio = value.contains("audio_info") && value["audio_info"].is_object() ? value["audio_info"] : value;
    std::string hash = JsonText(audio, "hash");
    if (hash.empty()) hash = JsonText(audio, "hash_128");
    if (hash.empty()) hash = JsonText(value, "FileHash");
    if (hash.size() != 32 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return track;
    std::string id = JsonText(value, "album_audio_id");
    if (id.empty()) id = JsonText(value, "MixSongID");
    if (id.empty() && value.contains("album_info")) id = JsonText(value["album_info"], "album_audio_id");
    if (id.find_first_not_of("0123456789") != std::string::npos) id.clear();
    track.virtual_path = kugou::FromUtf8("kugou://" + hash + (id.empty() ? "" : "?aaid=" + id));
    std::string title = JsonText(value, "OriSongName");
    if (title.empty()) title = JsonText(value, "songname");
    if (title.empty()) title = JsonText(audio, "audio_name");
    if (title.empty()) title = JsonText(value, "name");
    if (title.empty()) title = JsonText(value, "filename");
    track.title = kugou::FromUtf8(title);
    std::string artist = JsonText(value, "SingerName");
    if (artist.empty()) artist = JsonText(value, "singername");
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
    return track;
}

inline void AddTrack(BrowseResult& result, const Track& track)
{
    if (!track.IsValid()) return;
    BrowseItem item;
    item.track = track;
    item.title = track.title;
    item.subtitle = track.artist;
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
        result.error = L"酷狗播放接口返回格式异常";
        result.retry_quality = false;
        return result;
    }
    auto code = JsonText(response, "errcode");
    if (code.empty()) code = JsonText(response, "error_code");
    if ((response.contains("status") && JsonNumber(response, "status") == 0) || (!code.empty() && code != "0"))
    {
        result.error = code == "20006" ? L"酷狗播放请求校验失败（错误 20006）"
            : L"酷狗未接受播放请求" + (code.empty() ? std::wstring() : L"（错误 " + kugou::FromUtf8(code) + L"）");
        result.retry_quality = false;
        return result;
    }
    // v5/url 的当前响应在根对象中返回 url/backupUrl 数组；兼容旧 data 包装和单个字符串。
    const auto& data = response.contains("data") && response["data"].is_object() ? response["data"] : response;
    if (data.contains("priv_status") && JsonNumber(data, "priv_status") == 0)
    {
        result.error = logged_in ? L"当前账号没有此歌曲或音质的播放权限，请检查平台会员或购买状态"
            : L"请登录酷狗概念版账号后重试";
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
    result.error = L"酷狗未返回可播放地址，请稍后重试或检查曲目权限";
    return result;
}
}
