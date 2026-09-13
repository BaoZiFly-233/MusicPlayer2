#pragma once
#include "OnlineSource.h"
#include "SongInfo.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <map>

// 在线浏览的数据与任务。窗口线程更新状态，皮肤绘制线程只读取不可变快照。
class COnlineMusicModel
{
    friend class COnlineMusicPreview;
    friend bool RunOnlineMusicTests(const std::wstring& log_path, bool network);
public:
    enum class Page { Discover, Search, Recommend, Charts, Cloud, Local, Account };
    enum class Action { Activate, Page, Source, Query, Search, Open, Play, Queue, Save,
        Import, Export, Lyrics, Back, Refresh, More, OpenPlaylist, ImportAll, Login, Logout,
        Download, CacheInfo, ToggleCache, ClearCache, SearchPlaylists, SignIn, ToggleSignIn, ImportAccount };
    struct State
    {
        Page page{ Page::Discover };
        int source{};
        bool busy{}, has_more{}, detail_visible{};
        unsigned long long revision{}, query_revision{};
        online::BrowseRequest request;
        std::vector<online::BrowseItem> items;
        std::vector<SongInfo> songs; // 与 items 下标一致，非歌曲行保持空值
        std::wstring query, status{ L"在线音乐" }, detail, playback_error;
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
    };
    struct Playback { std::vector<SongInfo> songs; bool append{}; };
    static COnlineMusicModel& Instance();
    std::shared_ptr<const State> Snapshot() const;
    void Initialize(CWnd* owner);
    void Post(Command command);
    void ProcessCommands();
    void Poll(bool visible);
    void Suspend();
    void Shutdown();
    bool TakePlayback(Playback& playback);
    void SetStatus(const std::wstring& status);
    void SetPlaybackError(const std::wstring& error);
    static const wchar_t* PageName(Page page);

private:
    struct Task
    {
        std::shared_ptr<online::IOnlineSource> source;
        online::BrowseRequest request;
        online::BrowseResult result;
        std::wstring error, lyric_path, text;
        bool success{}, append{}, import_all{};
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
    void StartRequest(bool append = false, bool import_all = false,
        const std::wstring& lyric_path = L"", int login_action = 0, bool profile_request = false);
    void CancelRequest();
    void ResetLogin();
    void ShowLocal();
    void ShowAccount(bool refresh = false);
    void ImportFile();
    void ExportFile(const std::vector<int>& rows);
    void SaveLocal(const std::vector<SongInfo>& songs);
    std::vector<SongInfo> SelectedSongs(const std::vector<int>& rows, bool all = false) const;
    std::wstring LocalPath() const;
    online::IOnlineSource* CurrentSource() const;

    CWnd* m_owner{};
    std::atomic<HWND> m_message_window{ nullptr };
    bool m_started{};
    bool m_was_visible{}, m_interrupted{};
    State m_state;
    std::atomic<std::shared_ptr<const State>> m_snapshot;
    std::vector<SongInfo> m_local;
    std::shared_ptr<Task> m_task;
    std::shared_ptr<Worker> m_worker;
    std::shared_ptr<online::IOnlineSource> m_login_source;
    ULONGLONG m_login_poll_at{};
    ULONGLONG m_service_refresh_at{};
    bool m_cache_view{};
    std::wstring m_reward_status;
    std::map<int, online::AccountProfile> m_profiles;
    std::mutex m_commands_mutex;
    std::deque<Command> m_commands;
    std::deque<Playback> m_playback;
};
