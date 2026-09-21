#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

// 在线音源的抽象层。
//
// 设计要点：在线歌曲在曲库/播放列表里用一个「虚拟路径」作为唯一标识，
// 例如 kugou://<hash>、bodian://<rid>。这样 SongKey（file_path + cue_track）
// 这套既有机制无需改动，曲库、歌单、收藏都能直接复用。
// 真正播放前，由本层选择完整本地缓存或实际的 http(s) 播放地址，
// 再交给 IPlayerCore::Open()。
//
// 一个音源需要提供的能力，按使用顺序排列：
//   1. GetScheme()           —— 声明自己负责哪个虚拟路径前缀
//   2. Search()              —— 按关键字找歌，产出带虚拟路径的曲目
//   3. ResolvePlayUrl()      —— 播放前把虚拟路径换成真实地址（地址有时效，只在本进程内短时记住）
//   4. GetLyric()            —— 取歌词（可选，失败不影响播放）
//
// 新增音源只需继承本类并在 CSourceRegistry 注册，播放器其余部分不用改。

namespace online
{

// 音源返回的一首曲目。字段够用即可，不追求与各平台一一对应。
struct Track
{
    std::wstring virtual_path;      // 虚拟路径，形如 kugou://<hash>，作为唯一标识
    std::wstring title;             // 标题
    std::wstring artist;            // 艺术家
    std::wstring album;             // 唱片集
    std::wstring cover_url;
    int duration_ms{ 0 };           // 时长（毫秒），未知为 0
    std::wstring extra;             // 音源私有附加信息（如 album_audio_id），播放时回传
    // 一句短语的状态标记，例如「VIP」「试听」。空表示无需提示。
    // 各音源按自己的权限字段填，界面统一呈现，不做平台判断。
    std::wstring badge;

    bool IsValid() const { return !virtual_path.empty(); }
};

// 歌词查询结果
struct Lyric
{
    std::wstring content;           // 歌词文本（已解码成 LRC 文本）
    bool HasContent() const { return !content.empty(); }
};

// AlbumSearch 是「只搜专辑」：Search 会把歌曲和专辑混在一起返回，
// 但用户明确想找某张专辑时，单独一类更直接。音源不支持就返回 false。
// AlbumTracks 取一张专辑的曲目，id 是平台给的专辑编号（不是「歌手 专辑名」那种关键词）。
enum class BrowseKind { Search, AlbumSearch, AlbumTracks, Hot, Recommend, Charts, ChartTracks, Playlists, PlaylistTracks, PlaylistSearch };
enum class QrStatus { Expired, Waiting, Scanned, Authorized, Failed };
struct AccountProfile
{
    std::wstring name, membership, expires;
};

struct BrowseRequest
{
    BrowseKind kind{ BrowseKind::Hot };
    std::wstring id;       // 搜索词、专辑/分类/歌单编号，不接受任意接口地址
    int page{ 1 };
};

struct BrowseItem
{
    // Album 是搜索结果里的专辑条目，id 里存平台给的专辑编号，
    // 双击后按 AlbumTracks 取曲目（不要退回用「歌手 + 专辑名」当关键词搜，
    // 那样拿到的是同歌手的所有歌，顺序也不是专辑顺序）。
    enum class Type { Song, Keyword, Chart, Playlist, Album };
    Type type{ Type::Song };
    Track track;
    std::wstring id;
    std::wstring title;
    std::wstring subtitle;
    std::wstring badge;     // 状态标记，与 Track::badge 同源
};

struct BrowseResult
{
    std::vector<BrowseItem> items;
    bool has_more{ false };
};

// 音源接口
class IOnlineSource
{
public:
    virtual ~IOnlineSource() {}

    // 本音源负责的虚拟路径 scheme，返回小写、不含 "://"，例如 L"kugou"
    virtual std::wstring GetScheme() const = 0;

    // 界面上显示的音源名称，例如 L"K源"
    virtual std::wstring GetDisplayName() const = 0;

    // 列表里给每一行标来源用的短名，例如 L"K源"、L"B源"。默认取全名，
    // 音源自己觉得名字太长就重写它。用来把本地文件和两个平台严格分开显示。
    virtual std::wstring GetShortName() const { return GetDisplayName(); }

    // 按关键字搜索
    virtual bool Search(const std::wstring& keyword, int page, std::vector<Track>& result) = 0;

    virtual bool Browse(const BrowseRequest& request, BrowseResult& result);

    // 把虚拟路径解析成可直接播放的 http(s) 地址。
    // 返回空字符串表示失败（无版权、需会员、接口变更等）。
    virtual std::wstring ResolvePlayUrl(const std::wstring& virtual_path) = 0;

    // 取歌词。失败返回 false 即可，不影响播放。
    virtual bool GetLyric(const std::wstring& virtual_path, Lyric& result) { return false; }
    virtual std::wstring GetCoverUrl(const Track& track) { return {}; }
    // 上一次取播放地址时的音质说明。比如「已降级到 320k：无损需要会员」。
    // 拿不到最好音质时靠它告诉用户原因，而不是默默降级。
    virtual std::wstring GetQualityNote() const { return std::wstring(); }
    virtual bool GetProfile(AccountProfile& profile) { return false; }

    // 上一次操作的失败原因，可直接显示给用户。
    // 播放失败时界面靠它给出「需要会员」这类准确提示，而不是笼统的「播放失败」。
    virtual std::wstring GetLastError() const { return std::wstring(); }
};

// 音源注册表：负责按虚拟路径找到对应音源，并提供统一的解析入口。
class CSourceRegistry
{
public:
    static CSourceRegistry& Instance();

    // 注册一个音源（由初始化代码调用）
    void Register(IOnlineSource* source);

    // 虚拟路径判断：是否属于某个在线音源
    static bool IsVirtualPath(const std::wstring& path);

    // 从虚拟路径中取出 scheme（小写）。非虚拟路径返回空字符串。
    static std::wstring GetScheme(const std::wstring& path);

    // 按虚拟路径找音源。找不到返回 nullptr。
    IOnlineSource* FindByPath(const std::wstring& path);

    // 按 scheme 找音源（不区分大小写）。
    IOnlineSource* FindByScheme(const std::wstring& scheme);

    // 解析成完整缓存文件或可播放地址。失败返回空字符串。
    // 传入本地路径时原样返回，方便调用方无脑调用。
    std::wstring ResolvePlayUrl(const std::wstring& path);

    // 记下一个已经解析好的地址。后台预缓存解析成功后调它，轮到这一首时
    // 就能直接命中，不必在切歌的路径上再发一次网络请求。
    // 只接受 http(s) 地址；本地缓存文件不进这个表。
    // quality_note 是解析时音源给出的音质说明，和地址绑在一起记：
    // 说明属于「这一次解析出的这条流」，不属于音源对象。
    void RememberPlayUrl(const std::wstring& path, const std::wstring& url, const std::wstring& quality_note = std::wstring());

    // 查备忘里还有效的地址。命中就返回，没有就返回空，绝不发请求。
    // 后台任务用它避免重复解析，同时不把解析工作挪到公共音源实例上。
    std::wstring CachedPlayUrl(const std::wstring& path) const;

    // 这首歌最近一次拿到播放来源时的音质说明（「无损」「320k（无损未获取到：…）」「本地缓存 · 无损」）。
    // 以前直接问音源对象的 GetQualityNote()，但解析可能发生在预缓存线程的音源副本上，
    // 界面线程命中备忘时公共实例上的说明还是上一首的。说明跟着地址走就不会串。
    std::wstring QualityNote(const std::wstring& path) const;

    // 这首歌最近一次解析失败的原因。播放失败时界面靠它给出「需要会员」这类准确提示；
    // 同样按地址记，命中失败备忘时音源对象根本没被问过，它身上的错误是别的歌的。
    // 失败原因不随 ForgetPlayUrl 丢掉（那只是让下次重新解析），解析成功或清空时才清。
    std::wstring PlayError(const std::wstring& path) const;

    // 丢掉某个虚拟地址的备忘（播放失败、地址过期、账号切换后调用），
    // 下次播放会重新解析。账号登录或退出会改变权限，那时用下面那个清空全部。
    void ForgetPlayUrl(const std::wstring& path);
    void ForgetAllPlayUrls();

    // 已注册的音源列表（界面用）
    const std::vector<IOnlineSource*>& GetAll() const { return m_sources; }

    // 一行歌曲的来源标签：本地文件返回 L"本地"，在线曲目返回音源的短名。
    // 界面用它把本地文件和两个平台严格分开显示，判断依据只有地址本身，
    // 不引入第二份可能与地址矛盾的来源字段。
    static std::wstring OriginLabel(const std::wstring& path);
    // 同上，但带上内部标记：本地为 0，在线为音源在注册表里的下标 + 1。
    static int OriginIndex(const std::wstring& path);

private:
    // 解析结果的短时备忘。地址有时效，所以只留几分钟；失败也留一小会儿，
    // 免得同一首被反复解析。表按条目数封顶，不会随浏览无限增长。
    struct PlayUrlMemo
    {
        std::wstring value;
        std::uint64_t stamp{};
        bool usable{};
    };
    static std::uint64_t NowMs();
    void StorePlayUrl(const std::wstring& path, const std::wstring& url, bool usable, const std::wstring& error = std::wstring());
    void StoreQualityNote(const std::wstring& path, const std::wstring& note);
    CSourceRegistry() {}
    std::vector<IOnlineSource*> m_sources;
    mutable std::mutex m_url_mutex;
    std::map<std::wstring, PlayUrlMemo> m_urls;
    // 音质说明单独一张表：本地缓存命中时地址备忘里没有条目，但说明照样要有。
    std::map<std::wstring, std::wstring> m_notes;
    // 解析失败原因，按地址记；见 PlayError。
    std::map<std::wstring, std::wstring> m_errors;
};

// 初始化：注册所有内置音源。程序启动时调用一次。
void InitOnlineSources();

} // namespace online
