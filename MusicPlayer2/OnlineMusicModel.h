#pragma once
#include "OnlineSource.h"
#include "OnlineProgress.h"
#include "SongInfo.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <map>
#include <set>

// 在线浏览的数据与任务。窗口线程更新状态，皮肤绘制线程只读取不可变快照。
class CPlayer;
class COnlineMusicModel
{
    friend class COnlineMusicPreview;
    friend bool RunOnlineMusicTests(const std::wstring& log_path, bool network);
public:
    enum class Page { Discover, Search, Recommend, Charts, Cloud, Local, Account };
    enum class Action { Activate, Page, Source, Query, Search, Open, Play, Queue, Save,
        Import, ImportExternal, Export, Lyrics, Back, Refresh, More, OpenPlaylist, ImportAll, Login, Logout,
        Download, CacheInfo, ToggleCache, ClearCache, SearchPlaylists, SignIn, ToggleSignIn, ImportAccount,
        AdReward, ToggleAdReward, SearchType, RemoveLocal, SaveAsNativePlaylist, ClearLocal,
        // 与原生播放器接上的那几个：插到当前曲目之后、加入已有的播放列表、
        // 加入「我喜欢」、复制歌曲信息、在资源管理器中定位。
        PlayNext, AddToPlaylist, AddToFavourite, CopyInfo, OpenFileLocation,
        // 播放失败过的曲目再试一次：丢掉备忘和灰色标记，重新解析后播放。
        RetryPlayback,
        // 把一组在线歌曲重新匹配到另一个音源，导入进来的歌单就是靠它换源。
        // command.value 是目标音源在注册表里的下标。
        SwitchSource,
        // 同上，但作用于当前播放列表（用户从媒体库打开的导入歌单就在这里）。
        SwitchPlayerPlaylist,
        // 同上，作用于指定路径的原生播放列表（媒体库里右键某份歌单时用）。路径走 command.text。
        SwitchFilePlaylist,
        // 单曲（或所选）就地换源：在线页给的是列表里的行，播放列表给的是当前列表里的曲目，
        // 匹配成功后直接改地址，不另存歌单。
        SwitchInPlace, SwitchTrackInPlace, ToggleAutoSwitch };
    struct State
    {
        Page page{ Page::Discover };
        int source{};
        // 搜索对象：0 音乐、1 专辑、2 歌单。界面上的选择器直接映射到它。
        int search_type{};
        bool busy{}, has_more{}, detail_visible{};
        bool auto_switch_source{ true };
        // 外部歌单导入进度；total 为 0 时表示还在读取歌单元数据。
        bool importing{};
        int import_done{}, import_total{};
        bool logged_in{};
        unsigned long long revision{}, query_revision{};
        online::BrowseRequest request;
        std::vector<online::BrowseItem> items;
        std::vector<SongInfo> songs; // 与 items 下标一致，非歌曲行保持空值
        std::vector<bool> unplayable; // 与 items 下标一致，true 表示播放失败过（列表里显示灰色）
        // 与 items 下标一致：这首歌是否在在线本地歌单里（列表里用实心红心标出来）
        std::vector<bool> saved_local;
        std::wstring query, status{ L"在线音乐" }, detail, playback_error, notice;
        // 正在播放的在线曲目的音质说明（「当前播放：无损」）。它不是操作结果，
        // 单独存放，不再覆盖 status：切歌时状态栏不会丢掉「已加载 30 项」这类信息。
        std::wstring quality_note;
        // 状态栏正文对应的时间戳：操作结果只显示一会儿，之后回落到列表摘要
        unsigned long long status_stamp{};
        std::vector<bool> qr_pixels;
        int qr_size{};
    };
    struct Command
    {
        Action action;
        int value{};
        std::wstring text;
        std::vector<int> rows;
        unsigned long long revision{};
        std::vector<SongInfo> songs; // 原生菜单在点击时捕获，异步处理不再依赖可变化的行号。
    };
    // append 表示加入队列而不打断当前播放；insert_next 表示同时排到当前曲目之后。
    struct Playback { std::vector<SongInfo> songs; bool append{}; bool insert_next{}; };
    static COnlineMusicModel& Instance();
    std::shared_ptr<const State> Snapshot() const;
    void Initialize(CWnd* owner);
    void Post(Command command);
    void ProcessCommands();
    void Poll(bool visible);
    void Suspend();
    void Shutdown();
    bool TakePlayback(Playback& playback);
    // 返回 true 时暂缓失败跳歌；匹配、取消和开流结果均在窗口线程归并。
    bool RecoverPlayback(CPlayer& player);
    void SetStatus(const std::wstring& status);
    void SetPlaybackError(const std::wstring& error);
    // 正在播放的在线曲目的音质说明；空字符串表示当前没有在线曲目在播。
    void SetQualityNote(const std::wstring& note);
    // 顶部短提示，几秒后自动消失。
    void SetNotice(const std::wstring& notice);
    // 播放失败时调用，把该曲目标记为不可播放（列表里显示灰色）。只在本次运行内有效。
    void MarkUnplayable(const std::wstring& path);
    // 之前失败过的曲目又能播了（例如刚领到会员），撤掉灰色标记。
    void MarkPlayable(const std::wstring& path);
    // 这首歌是否已经在在线本地歌单里。列表的红心图标和右键菜单的叫法都靠它。
    bool IsSavedLocally(const std::wstring& path) const;
    static const wchar_t* PageName(Page page);
    static bool IsSwitchSourceCommand(unsigned int command);

private:
    struct Task
    {
        std::shared_ptr<online::IOnlineSource> source;
        std::shared_ptr<online::OnlineProgress> progress;
        online::BrowseRequest request;
        online::BrowseResult result;
        std::wstring error, lyric_path, text;
        bool success{}, append{}, import_all{};
        bool external_import{};
        std::wstring share_text;
        std::wstring import_default_name;
        std::atomic<int> import_total{ 0 }, import_done{ 0 };
        std::atomic<int> import_auto{ 0 }, import_confirm{ 0 }, import_missing{ 0 };
        bool rematch{};
        bool automatic_switch{};
        // 换源任务：把 rematch_songs 逐首重新匹配到 task->source，结果放进 switched。
        // 本地文件不参与匹配，原样保留并计入 rematch_kept。
        std::wstring rematch_default_name, rematch_target_name;
        // 就地换源：in_place_local 表示改在线「本地歌单」，否则改 target_playlist。
        bool in_place{}, in_place_local{};
        std::wstring target_playlist;
        unsigned long long apply_started{};
        int replaced{};
        std::vector<SongInfo> rematch_songs, switched;
        std::atomic<int> rematch_moved{ 0 }, rematch_kept{ 0 }, rematch_missing{ 0 };
        bool profile_request{};
        online::AccountProfile profile;
        int login_action{}, login_status{};
        std::atomic<bool> done{ false }, cancelled{ false };
    };
    struct Worker
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::shared_ptr<Task> pending;
        bool stopping{};
    };
    COnlineMusicModel();
    void Publish(bool rows_changed = false, bool reset_selection = true);
    void Execute(const Command& command);
    void SelectPage();
    // 当前搜索类型对应的浏览类别，搜索与切页共用
    online::BrowseKind SearchKind() const;
    void StartRequest(bool append = false, bool import_all = false,
        const std::wstring& lyric_path = L"", int login_action = 0, bool profile_request = false);
    void EnqueueTask(std::shared_ptr<Task> task);
    void CompleteTask(std::shared_ptr<Task> task);
    bool ApplyRematch(Task& task, int& replaced);
    void CancelRequest();
    void ResetLogin();
    void FinishAutoSwitch(online::ProgressResult result, const std::wstring& detail);
    void ShowLocal();
    void ShowAccount(bool refresh = false);
    void ImportFile();
    // 从网易云 / QQ 分享链接导入歌单，并匹配到当前音源。
    void ImportExternal();
    void StartExternalImport(const std::wstring& text);
    // 把一组歌曲重新匹配到 target_source 那个音源。只有在线曲目会参与匹配，
    // 本地文件原样保留；完成后保存成新的原生播放列表，不动原来的歌单。
    void SwitchSource(int target_source, const std::vector<SongInfo>& songs, const std::wstring& name_hint,
        bool in_place = false, bool in_place_local = false, const std::wstring& playlist_path = L"");
    // 短暂提示：在线页的状态栏在媒体库那边看不见，所以同时往皮肤上打一条提示。
    void ShowTip(const std::wstring& text);
    void ExportFile(const std::vector<int>& rows);
    bool SaveLocal(const std::vector<SongInfo>& songs, std::shared_ptr<online::OnlineProgress> progress = {});
    // 把歌曲加入一个已有的原生播放列表（弹出选择框，复用媒体库那套流程）。
    void AddToExistingPlaylist(const std::vector<SongInfo>& songs);
    // 复制所选歌曲的标题、歌手和路径到剪贴板。
    void CopySongInfo(const std::vector<int>& rows);
    // 在资源管理器中定位歌曲文件；在线曲目没有本地文件时给出提示。
    void OpenFileLocation(const std::vector<int>& rows);
    // 从本地歌单移除。本地页按行号删；在线页按虚拟路径匹配同名歌曲，
    // 这样在搜索结果里也能直接把收藏撤掉。
    void RemoveLocal(const std::vector<int>& rows);
    // 把一组歌曲保存成原生 .playlist，名称支持「分组/歌单名」。
    bool SaveSongsToNativePlaylist(const std::vector<SongInfo>& songs, const std::wstring& default_name,
        const std::wstring& title, const std::wstring& info, std::wstring& saved_name, std::wstring& error,
        std::shared_ptr<online::OnlineProgress> progress = {});
    // 把在线“本地歌单”里的歌曲迁移到原生播放列表。
    void SaveLocalAsNativePlaylist(const std::vector<int>& rows);
    // 清空在线本地歌单（带确认）。
    void ClearLocal();
    std::vector<SongInfo> SelectedSongs(const std::vector<int>& rows, bool all = false) const;
    std::wstring LocalPath() const;
    online::IOnlineSource* CurrentSource() const;
    // 当前列表的一句话摘要（「已加载 30 项 · 波点」），操作提示消失后状态栏回落到它。
    std::wstring ListSummary() const;

    CWnd* m_owner{};
    std::atomic<HWND> m_message_window{ nullptr };
    bool m_started{};
    bool m_was_visible{}, m_interrupted{};
    State m_state;
    std::atomic<std::shared_ptr<const State>> m_snapshot;
    std::vector<SongInfo> m_local;
    std::shared_ptr<Task> m_task;
    std::shared_ptr<Worker> m_worker;
    std::shared_ptr<Task> m_library_task;
    std::shared_ptr<Worker> m_library_worker;
    std::shared_ptr<Task> m_auto_switch_task;
    std::shared_ptr<Worker> m_auto_switch_worker;
    std::shared_ptr<online::OnlineProgress> m_auto_switch_progress;
    std::uint64_t m_auto_switch_generation{}, m_auto_switch_started{};
    std::set<std::wstring> m_auto_switch_tried;
    std::wstring m_auto_switch_error;
    int m_auto_switch_position{};
    bool m_auto_switch_stopped{};
    std::shared_ptr<online::OnlineProgress> m_login_progress;
    std::shared_ptr<online::IOnlineSource> m_login_source;
    ULONGLONG m_login_poll_at{};
    ULONGLONG m_service_refresh_at{};
    ULONGLONG m_notice_until{};
    bool m_cache_view{};
    std::wstring m_reward_status;
    std::map<int, online::AccountProfile> m_profiles;
    std::set<std::wstring> m_unplayable;   // 播放失败过的虚拟路径
    std::mutex m_commands_mutex;
    std::deque<Command> m_commands;
    std::deque<Playback> m_playback;
};
