#pragma once
#include "OnlineSource.h"
#include "SongInfo.h"
#include <functional>
#include <string>
#include <vector>

// 把其他音乐平台的歌单导入进来。
//
// 流程分两步：
//   1. 解析用户粘贴的分享链接/文本，识别平台并取出歌单编号，然后联网拉取曲目列表
//   2. 把每首歌拿到本平台（K源/B源）搜索，用打分算法挑出最匹配的一首
//
// 第一步各平台接口不同，第二步是纯计算、不依赖网络，所以匹配算法可以单独做离线测试。

namespace online
{

// 从其他平台导入来的一首歌。此时还没有匹配到本平台的曲目。
struct ImportTrack
{
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int duration_ms{ 0 };

    bool IsValid() const { return !title.empty(); }
};

// 支持的导入来源。K源自己的歌单已经能直接打开，所以这里不重复支持。
enum class ImportSource
{
    Unknown,
    Netease,
    QQ,
    Kuwo,
};

// 从分享文本里识别出来的歌单引用
struct ImportReference
{
    ImportSource source{ ImportSource::Unknown };
    std::wstring id;

    bool IsValid() const { return source != ImportSource::Unknown && !id.empty(); }
};

// 从一整段文本里识别平台并取出歌单编号。
// 用户从 App 复制的文本形如「分享歌单《xxx》http://163cn.tv/xxxxx 来自@外部平台」，
// 所以这里是在整段文本里找链接，而不是要求用户只粘贴一条干净的链接。
ImportReference ParseShareText(const std::wstring& text);

// 拉取歌单曲目。cancelled 返回 true 时尽快退出。
bool FetchPlaylist(const ImportReference& reference, std::vector<ImportTrack>& tracks,
    std::wstring& error, const std::function<bool()>& cancelled,
    std::wstring* playlist_name = nullptr);

// ---- 匹配 ----

// 匹配档次。分数高的直接用，中间档让用户确认，太低的不采用。
enum class MatchLevel
{
    Auto,
    Confirm,
    NotFound,
};

struct MatchResult
{
    MatchLevel level{ MatchLevel::NotFound };
    Track candidate;        // 匹配到的曲目，NotFound 时无效
    int score{ 0 };
};

// 归一化相似度，0~100
int Similarity(const std::wstring& a, const std::wstring& b);

// 从搜索返回的候选里挑最合适的一首
MatchResult PickBest(const ImportTrack& source, const std::vector<Track>& candidates);

// 仅替换仍与原歌曲身份相符的条目；保留期间新增的歌曲、排序及收藏属性。
int ApplySourceMatches(std::vector<SongInfo>& playlist, const std::vector<SongInfo>& original,
    const std::vector<SongInfo>& matched);
bool SaveSourceChanges(const std::vector<SongInfo>& playlist, const std::wstring& path, std::wstring& error);

} // namespace online
