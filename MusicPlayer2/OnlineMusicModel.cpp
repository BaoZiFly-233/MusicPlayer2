#include "stdafx.h"
#include "MusicPlayer2.h"
#include "OnlineMusicModel.h"
#include "OnlineJson.h"
#include "OnlineMediaCache.h"
#include "OnlineSettingsUI.h"
#include "OnlineDailyRewards.h"
#include "OnlinePlaylistImport.h"
#include "InputDlg.h"
#include "KugouSource.h"
#include "BodianSource.h"
#include "Playlist.h"
#include "CRecentList.h"
#include "FilePathHelper.h"
#include "OnlineHttp.h"
#include "MusicPlayerCmdHelper.h"
#include "Player.h"

#include "AddToPlaylistDlg.h"
#include "qrcodegen/qrcodegen.hpp"
#include <shellapi.h>
#include <thread>
#include <fstream>
#include <filesystem>

using namespace std;
using namespace online;

// 扫码轮询连续失败多少次才判定登录流程彻底失败（每次间隔 2 秒）
static constexpr int LOGIN_POLL_FAILURE_LIMIT = 5;

namespace
{
wstring BrowseTitle(BrowseKind kind)
{
    switch (kind)
    {
    case BrowseKind::Search: return L"搜索歌曲";
    case BrowseKind::AlbumSearch: return L"搜索专辑";
    case BrowseKind::AlbumTracks: return L"加载专辑歌曲";
    case BrowseKind::PlaylistSearch: return L"搜索歌单";
    case BrowseKind::Hot: return L"加载发现";
    case BrowseKind::Recommend: return L"加载推荐";
    case BrowseKind::Charts: return L"加载榜单";
    case BrowseKind::ChartTracks: return L"加载榜单歌曲";
    case BrowseKind::Playlists: return L"加载云歌单";
    default: return L"加载歌单歌曲";
    }
}
// 歌单里已经存着标题、歌手、专辑和时长，换源只靠这些字段重新找一遍，
// 不需要再往歌单里写一份「这首来自哪个平台」的字段：来源始终由虚拟地址决定。
ImportTrack ToImportTrack(const SongInfo& song)
{
    ImportTrack track;
    track.title = song.IsTitleEmpty() ? L"" : song.title;
    track.artist = song.IsArtistEmpty() ? L"" : song.artist;
    track.album = song.IsAlbumEmpty() ? L"" : song.album;
    track.duration_ms = song.length().toInt();
    return track;
}

// 按「歌手 标题」搜一次，搜不到就只用标题再搜一次。外部歌单导入和换源共用这一份，
// 免得同一个匹配规则在两处各写一遍、迟早走样。
bool SearchForTrack(IOnlineSource& source, const ImportTrack& track, vector<Track>& candidates)
{
    candidates.clear();
    wstring query = track.artist;
    // 多歌手只取第一个：整串搜出来常常不是同一首歌
    const size_t separator = query.find_first_of(L"/&、;；,，|");
    if (separator != wstring::npos) query = query.substr(0, separator);
    const size_t first = query.find_first_not_of(L" \t\r\n");
    const size_t last = query.find_last_not_of(L" \t\r\n");
    if (first == wstring::npos) query.clear();
    else query = query.substr(first, last - first + 1);
    if (query.size() > 15) query.resize(15);
    query = query.empty() ? track.title : query + L" " + track.title;
    if (query.empty()) return false;

    source.Search(query, 1, candidates);
    if (PickBest(track, candidates).level == MatchLevel::Auto || query == track.title) return !candidates.empty();
    // 有搜索结果也可能全是别的版本；匹配不足时继续用完整歌名检索。
    vector<Track> fallback;
    if (!track.title.empty() && source.Search(track.title, 1, fallback))
        candidates.insert(candidates.end(), fallback.begin(), fallback.end());
    return !candidates.empty();
}
}

COnlineMusicModel::COnlineMusicModel() { Publish(); }
bool COnlineMusicModel::IsSwitchSourceCommand(unsigned int command)
{
    return command >= ID_ONLINE_SWITCH_SOURCE_START
        && command - ID_ONLINE_SWITCH_SOURCE_START < CSourceRegistry::Instance().GetAll().size();
}
COnlineMusicModel& COnlineMusicModel::Instance() { static COnlineMusicModel model; return model; }
shared_ptr<const COnlineMusicModel::State> COnlineMusicModel::Snapshot() const { return m_snapshot.load(); }
const wchar_t* COnlineMusicModel::PageName(Page page)
{
    static const wchar_t* names[] = { L"发现", L"搜索", L"推荐", L"榜单", L"云歌单", L"本地歌单", L"账号" };
    int index = static_cast<int>(page);
    return index >= 0 && index < 7 ? names[index] : L"在线音乐";
}
void COnlineMusicModel::Initialize(CWnd* owner)
{
    m_owner = owner;
    m_message_window = owner->GetSafeHwnd();
    m_state.auto_switch_source = COnlineSettings::Instance().Get().auto_switch_source;
    COnlineDailyRewards::Instance().Configure(theApp.m_config_dir);
    CBodianAdRewards::Instance().Configure(theApp.m_config_dir);
    CPlaylistFile file;
    file.LoadFromFile(LocalPath());
    m_local = file.GetPlaylist();
    Publish();
    owner->PostMessage(WM_PLAY_ONLINE_SONG);
}
void COnlineMusicModel::Publish(bool rows_changed, bool reset_selection)
{
    // 浏览任务和歌单任务（导入/换源）都算忙：原来只算前者，歌单任务跑着的时候
    // 界面上「清空本地歌单」「保存为原生播放列表」这些还是可点的。
    m_state.busy = m_task != nullptr || m_library_task != nullptr;
    if (rows_changed)
    {
        if (reset_selection) ++m_state.revision;
        m_state.songs.clear();
        m_state.unplayable.clear();
        m_state.saved_local.clear();
        // 每发布一次都要复制整份状态，先按行数留好位置，别在绘制线程读快照时反复扩容
        m_state.songs.reserve(m_state.items.size());
        m_state.unplayable.reserve(m_state.items.size());
        m_state.saved_local.reserve(m_state.items.size());
        // 本地歌单的地址集合只建一次，不为每一行扫一遍 m_local
        std::set<std::wstring> local_paths;
        for (const auto& local : m_local) local_paths.insert(local.file_path);
        for (size_t i = 0; i < m_state.items.size(); ++i)
        {
            SongInfo song;
            const auto& item = m_state.items[i];
            if (m_state.page == Page::Local && i < m_local.size()) song = m_local[i];
            else if (item.track.IsValid())
            {
                song.file_path = item.track.virtual_path; song.title = item.track.title;
                song.artist = item.track.artist; song.album = item.track.album;
                song.end_pos.fromInt(item.track.duration_ms);
            }
            m_state.songs.push_back(std::move(song));
            const auto& path = m_state.songs.back().file_path;
            m_state.unplayable.push_back(!path.empty() && m_unplayable.count(path) != 0);
            m_state.saved_local.push_back(!path.empty() && local_paths.count(path) != 0);
        }
    }
    m_snapshot.store(make_shared<State>(m_state));
}
void COnlineMusicModel::Post(Command command)
{
    {
        lock_guard<mutex> lock(m_commands_mutex);
        m_commands.push_back(std::move(command));
    }
    if (HWND window = m_message_window.load()) ::PostMessage(window, WM_PLAY_ONLINE_SONG, 0, 0);
}
void COnlineMusicModel::ProcessCommands()
{
    deque<Command> commands;
    { lock_guard<mutex> lock(m_commands_mutex); commands.swap(m_commands); }
    for (const auto& command : commands) Execute(command);
}
wstring COnlineMusicModel::LocalPath() const { return theApp.m_config_dir + L"online_library.playlist"; }
IOnlineSource* COnlineMusicModel::CurrentSource() const
{
    const auto& sources = CSourceRegistry::Instance().GetAll();
    return m_state.source >= 0 && m_state.source < static_cast<int>(sources.size()) ? sources[m_state.source] : nullptr;
}
void COnlineMusicModel::SetStatus(const wstring& status)
{
    // 每次操作结果都刷新时间戳，几秒后回落到列表摘要；内容没变就不重建快照
    m_state.status_stamp = GetTickCount64();
    if (m_state.status == status) return;
    m_state.status = status;
    Publish();
}
void COnlineMusicModel::SetPlaybackError(const wstring& error)
{
    if (m_state.playback_error == error) return;
    m_state.playback_error = error; Publish();
}
void COnlineMusicModel::SetQualityNote(const wstring& note)
{
    if (m_state.quality_note == note) return;
    m_state.quality_note = note; Publish();
}
bool COnlineMusicModel::IsSavedLocally(const wstring& path) const
{
    if (path.empty()) return false;
    for (const auto& local : m_local)
        if (local.file_path == path) return true;
    return false;
}
wstring COnlineMusicModel::ListSummary() const
{
    if (m_state.page == Page::Local) return L"本地歌单 · " + to_wstring(m_local.size()) + L" 首";
    if (m_state.page == Page::Account) return L"";
    if (m_state.items.empty()) return L"";
    size_t songs{};
    for (const auto& song : m_state.songs) if (!song.file_path.empty()) ++songs;
    wstring summary = songs == m_state.items.size() ? to_wstring(songs) + L" 首歌曲"
        : L"已加载 " + to_wstring(m_state.items.size()) + L" 项";
    if (m_state.has_more) summary += L" · 还有更多";
    if (auto* source = CurrentSource()) summary += L" · " + source->GetShortName();
    return summary;
}
void COnlineMusicModel::SetNotice(const wstring& notice)
{
    if (m_state.notice == notice && GetTickCount64() < m_notice_until) return;
    m_state.notice = notice;
    m_notice_until = notice.empty() ? 0 : GetTickCount64() + 5000;
    Publish();
}
void COnlineMusicModel::MarkUnplayable(const wstring& path)
{
    if (path.empty() || !CSourceRegistry::IsVirtualPath(path)) return;
    // 只记一次。这个函数由 100 毫秒的定时器在播放出错期间反复调用，
    // 已经标记过就直接返回，别每次都去清备忘（那是一次加锁加查表）。
    if (!m_unplayable.insert(path).second) return;
    // 播放失败常常是地址过期或权限变了，先把备忘丢掉，用户再点一次会重新解析
    CSourceRegistry::Instance().ForgetPlayUrl(path);
    Publish(true, false);
}

void COnlineMusicModel::MarkPlayable(const wstring& path)
{
    // 之前失败过的曲目现在能放了（例如刚领到会员），把灰色标记撤掉
    if (path.empty() || m_unplayable.erase(path) == 0) return;
    Publish(true, false);
}

void COnlineMusicModel::FinishAutoSwitch(ProgressResult result, const wstring& detail)
{
    if (m_auto_switch_task) m_auto_switch_task->cancelled = true;
    m_auto_switch_task.reset();
    if (m_auto_switch_progress) m_auto_switch_progress->Finish(result, detail);
    m_auto_switch_progress.reset();
    m_auto_switch_started = 0;
    m_auto_switch_stopped = result != ProgressResult::Succeeded;
}

bool COnlineMusicModel::RecoverPlayback(CPlayer& player)
{
    unique_lock<timed_mutex> lock(player.GetPlayStatusMutex(), try_to_lock);
    if (!lock.owns_lock()) return m_auto_switch_task != nullptr;
    const auto generation = player.PlaybackGeneration();
    if (generation != m_auto_switch_generation)
    {
        FinishAutoSwitch(ProgressResult::Cancelled, L"播放操作已改变，取消自动换源");
        m_auto_switch_generation = generation;
        m_auto_switch_tried.clear();
        m_auto_switch_error.clear();
        m_auto_switch_stopped = false;
        m_auto_switch_started = 0;
    }
    const auto original = player.GetSafeCurrentSongInfo();
    if (!m_state.auto_switch_source || !player.PlaybackRequested()
        || !CSourceRegistry::IsVirtualPath(original.file_path))
    {
        if (m_auto_switch_task) FinishAutoSwitch(ProgressResult::Cancelled, L"播放已停止或自动换源已关闭");
        return false;
    }
    if (player.m_loading) return m_auto_switch_task != nullptr;
    if (m_auto_switch_progress && m_auto_switch_progress->Cancelled())
    {
        FinishAutoSwitch(ProgressResult::Cancelled, L"自动换源已取消");
        return false;
    }
    if (m_auto_switch_stopped) return false;
    const bool failed = player.IsError() || (player.IsFileOpened() && player.GetSongLength() <= 0);
    if (!failed)
    {
        if (m_auto_switch_task) FinishAutoSwitch(ProgressResult::Cancelled, L"当前音源已恢复播放");
        return false;
    }
    if (m_auto_switch_started && GetTickCount64() - m_auto_switch_started >= 60000)
    {
        FinishAutoSwitch(ProgressResult::Failed, L"自动换源等待超时，可手动重试或播放下一首");
        return false;
    }
    if (m_auto_switch_task)
    {
        if (!m_auto_switch_task->done) return true;
        auto task = std::move(m_auto_switch_task);
        if (task->success && !task->cancelled && !task->switched.empty())
        {
            m_auto_switch_progress->Update(L"正在切换到" + task->source->GetShortName() + L"并恢复播放");
            const bool opened = player.OpenOnlineAlternative(task->switched.front().file_path,
                generation, m_auto_switch_position);
            // 本次恢复产生的 OPEN/PLAY 属于同一次尝试，不能清空已经试过的音源。
            m_auto_switch_generation = player.PlaybackGeneration();
            if (opened)
            {
                const auto message = L"已自动切换到" + task->source->GetShortName() + L"继续播放";
                FinishAutoSwitch(ProgressResult::Succeeded, message);
                MarkPlayable(original.file_path);
                SetNotice(message);
                return false;
            }
            m_auto_switch_error = player.GetErrorInfo();
        }
        else m_auto_switch_error = task->error;
    }
    // 原音源及已尝试的音源在这次播放内各试一次，手动重播才重置。
    m_auto_switch_tried.insert(CSourceRegistry::GetScheme(original.file_path));
    m_auto_switch_tried.insert(CSourceRegistry::GetScheme(player.GetOnlinePlaybackPath()));
    for (auto* source : CSourceRegistry::Instance().GetAll())
    {
        if (!m_auto_switch_tried.insert(source->GetScheme()).second) continue;
        auto task = make_shared<Task>();
        if (auto* kg = dynamic_cast<kugou::CKugouSource*>(source)) task->source = make_shared<kugou::CKugouSource>(*kg);
        else if (auto* bd = dynamic_cast<bodian::CBodianSource*>(source)) task->source = make_shared<bodian::CBodianSource>(*bd);
        if (!task->source) continue;
        if (!m_auto_switch_progress)
        {
            m_auto_switch_progress = OnlineProgress::Start(L"自动换源 · " + original.GetTitle(), L"当前音源无法播放", false, true);
            m_auto_switch_started = GetTickCount64();
            m_auto_switch_position = player.GetCurrentPosition();
        }
        task->automatic_switch = true;
        task->rematch_songs = {original};
        task->progress = m_auto_switch_progress;
        task->progress->Update(L"正在到" + source->GetShortName() + L"查找同一首歌曲");
        m_auto_switch_task = task;
        EnqueueTask(task);
        return true;
    }
    FinishAutoSwitch(ProgressResult::Failed, L"自动换源未成功：" + (m_auto_switch_error.empty()
        ? L"没有其他可用音源或可靠匹配，可手动重试或播放下一首" : m_auto_switch_error));
    return false;
}
void COnlineMusicModel::CancelRequest()
{
    if (m_state.importing && !m_library_task)
    {
        m_state.importing = false;
        m_state.import_done = 0;
        m_state.import_total = 0;
        m_state.status = L"匹配已取消。";
        m_state.status_stamp = GetTickCount64();
    }
    if (m_task)
    {
        m_task->cancelled = true;
        if (m_task->progress && m_task->progress != m_login_progress)
            m_task->progress->Finish(ProgressResult::Cancelled, L"操作已取消");
    }
    m_task.reset();
}
void COnlineMusicModel::ResetLogin()
{
    if (m_login_progress) m_login_progress->Finish(ProgressResult::Cancelled, L"扫码登录已取消");
    m_login_progress.reset();
    m_login_poll_at = 0; m_login_source.reset(); m_state.qr_pixels.clear(); m_state.qr_size = 0;
    m_login_failures = 0;
}
void COnlineMusicModel::Suspend()
{
    const bool browsing = m_task && m_task->login_action == 0 && m_task->lyric_path.empty()
        && !m_task->import_all && !m_task->external_import && !m_task->rematch;
    m_interrupted = m_interrupted || browsing;
    if (browsing) m_state.request.page = 1;
    CancelRequest(); ResetLogin(); Publish();
}
void COnlineMusicModel::Shutdown()
{
    COnlineMediaCache::Instance().Shutdown();
    COnlineDailyRewards::Instance().Shutdown();
    CBodianAdRewards::Instance().Shutdown();
    Suspend();
    FinishAutoSwitch(ProgressResult::Cancelled, L"播放器已关闭");
    if (m_library_task)
    {
        m_library_task->cancelled = true;
        if (m_library_task->progress) m_library_task->progress->Finish(ProgressResult::Cancelled, L"操作已取消");
        m_library_task.reset();
    }
    for (auto* slot : { &m_worker, &m_library_worker, &m_auto_switch_worker })
    {
        auto worker = std::move(*slot);
        if (!worker) continue;
        { lock_guard<mutex> lock(worker->mutex); worker->stopping = true; worker->pending.reset(); }
        worker->ready.notify_one();
    }
    m_owner = nullptr;
    m_message_window = nullptr;
}
// 搜索对象由界面上的选择器决定：音乐（歌曲加专辑）、专辑、歌单。
online::BrowseKind COnlineMusicModel::SearchKind() const
{
    switch (m_state.search_type)
    {
    case 1: return online::BrowseKind::AlbumSearch;
    case 2: return online::BrowseKind::PlaylistSearch;
    default: return online::BrowseKind::Search;
    }
}
void COnlineMusicModel::SelectPage()
{
    m_cache_view = false;
    m_interrupted = false;
    m_state.list_title.clear();
    CancelRequest(); ResetLogin();
    m_state.items.clear(); m_state.detail.clear(); m_state.detail_visible = false; m_state.has_more = false; m_state.request = {};
    if (m_state.page == Page::Local) { ShowLocal(); return; }
    if (m_state.page == Page::Account) { ShowAccount(true); return; }
    if (m_state.page == Page::Search)
    {
        m_state.request.kind = SearchKind();
        // 搜索框里还有词就把上次的搜索重新发一遍：从专辑/榜单里退回来时，
        // 用户要的是刚才那屏结果，而不是一个空列表。
        if (!m_state.query.empty())
        {
            m_state.request = { SearchKind(), m_state.query, 1 };
            StartRequest();
            return;
        }
        m_state.status = m_state.search_type == 1 ? L"输入专辑名或歌手，按 Enter 搜索专辑。"
            : m_state.search_type == 2 ? L"输入歌单关键词，按 Enter 搜索歌单。"
            : L"输入歌名或歌手，按 Enter 搜索；双击歌曲播放。";
        Publish(true); return;
    }
    m_state.request.kind = m_state.page == Page::Discover ? BrowseKind::Hot
        : m_state.page == Page::Recommend ? BrowseKind::Recommend
        : m_state.page == Page::Charts ? BrowseKind::Charts : BrowseKind::Playlists;
    StartRequest();
}
void COnlineMusicModel::StartRequest(bool append, bool import_all, const wstring& lyric_path, int login_action, bool profile_request)
{
    if (import_all && m_library_task) { ShowTip(L"已有歌单任务正在进行，可在底部查看进度。"); return; }
    m_cache_view = false;
    m_interrupted = false;
    CancelRequest();
    auto* source = CurrentSource();
    if (!source) { SetStatus(L"没有可用的在线音源"); return; }
    auto task = make_shared<Task>();
    if (login_action) task->source = m_login_source;
    else if (auto* kg = dynamic_cast<kugou::CKugouSource*>(source)) task->source = make_shared<kugou::CKugouSource>(*kg);
    else if (auto* bd = dynamic_cast<bodian::CBodianSource*>(source)) task->source = make_shared<bodian::CBodianSource>(*bd);
    if (!task->source) { SetStatus(L"该音源暂不支持浏览"); return; }
    task->request = m_state.request; task->append = append; task->import_all = import_all;
    task->lyric_path = lyric_path; task->login_action = login_action;
    task->profile_request = profile_request;
    if (login_action == 1)
        m_login_progress = OnlineProgress::Start(L"扫码登录 · " + source->GetShortName(), L"正在获取二维码", false, true);
    task->progress = login_action == 1 || login_action == 2 ? m_login_progress
        : OnlineProgress::Start(import_all ? L"导入云歌单" : profile_request ? L"刷新账号信息"
            : login_action == 4 ? L"验证登录信息" : !lyric_path.empty() ? L"获取歌词" : BrowseTitle(task->request.kind),
            source->GetShortName() + L" · 正在连接服务", false, true);
    if (import_all) m_library_task = task;
    else m_task = task;
    bool clear_rows = !append && !import_all && lyric_path.empty() && login_action == 0 && !profile_request;
    if (clear_rows) { m_state.items.clear(); m_state.has_more = false; m_state.detail_visible = false; }
    if (login_action != 2) m_state.status = import_all ? L"正在读取完整歌单…" : L"正在加载…";
    Publish(clear_rows);
    EnqueueTask(task);
}

void COnlineMusicModel::EnqueueTask(shared_ptr<Task> task)
{
    if (!task) return;
    try
    {
        auto& worker_slot = task->automatic_switch ? m_auto_switch_worker
            : task->rematch || task->external_import || task->import_all ? m_library_worker : m_worker;
        if (!worker_slot)
        {
            worker_slot = make_shared<Worker>();
            auto worker = worker_slot;
            thread([worker]() {
                for (;;)
                {
                    shared_ptr<Task> task;
                    {
                        unique_lock<mutex> lock(worker->mutex);
                        worker->ready.wait(lock, [&]() { return worker->stopping || worker->pending != nullptr; });
                        if (worker->stopping) return;
                        task = std::move(worker->pending);
                    }
                    if (task->cancelled) continue;
                    // 让这个任务的网络请求可以在用户换关键词、切页面或关窗口时中途结束，
                    // 而不是让新请求排在旧请求的超时后面。
                    online::RequestCancelScope cancel_scope(&task->cancelled);
                    try
                    {
                        if (task->profile_request) task->success = task->source->GetProfile(task->profile);
                        else if (task->login_action)
                        {
                            auto* kg = dynamic_cast<kugou::CKugouSource*>(task->source.get());
                            auto* bd = dynamic_cast<bodian::CBodianSource*>(task->source.get());
                            if (task->login_action == 4)
                            { task->success = bd && bd->ValidateAccount(); task->login_status = static_cast<int>(QrStatus::Authorized); }
                            else if (task->login_action == 1) task->success = kg ? kg->GetQrCode(task->text) : bd && bd->GetQrCode(task->text);
                            else
                            {
                                auto status = kg ? kg->CheckQrCode() : bd ? bd->CheckQrCode() : QrStatus::Failed;
                                task->login_status = static_cast<int>(status);
                                task->success = status != QrStatus::Failed;
                            }
                        }
                        else if (!task->lyric_path.empty())
                        {
                            Lyric lyric;
                            task->success = task->source->GetLyric(task->lyric_path, lyric) && lyric.HasContent();
                            task->text = lyric.content;
                        }
                        else if (task->external_import)
                        {
                            ImportReference reference = ParseShareText(task->share_text);
                            if (!reference.IsValid())
                                task->error = L"没有识别出支持的歌单链接，目前支持外部平台的分享链接";
                            else
                            {
                                vector<ImportTrack> imported;
                                wstring playlist_name;
                                if (!FetchPlaylist(reference, imported, task->error,
                                    [&]() { return task->cancelled.load(); }, &playlist_name))
                                {
                                    if (task->error.empty()) task->error = L"外部歌单读取失败";
                                }
                                else
                                {
                                    if (!playlist_name.empty()) task->import_default_name = std::move(playlist_name);
                                    task->import_total = static_cast<int>(imported.size());
                                    int auto_count{}, confirm_count{}, missing_count{};
                                    for (const auto& source_track : imported)
                                    {
                                        if (task->cancelled) break;
                                        if (task->progress) task->progress->Update(L"正在匹配：" + source_track.title,
                                            task->import_done, task->import_total);
                                        vector<Track> candidates;
                                        if (!SearchForTrack(*task->source, source_track, candidates))
                                        {
                                            ++missing_count; ++task->import_done; continue;
                                        }
                                        MatchResult best = PickBest(source_track, candidates);
                                        if (best.level == MatchLevel::NotFound)
                                        {
                                            ++missing_count; ++task->import_done; continue;
                                        }
                                        best.candidate.badge = best.level == MatchLevel::Auto ? L"自动匹配" : L"待确认";
                                        AddTrack(task->result, best.candidate);
                                        if (best.level == MatchLevel::Auto) ++auto_count; else ++confirm_count;
                                        ++task->import_done;
                                    }
                                    task->import_auto = auto_count;
                                    task->import_confirm = confirm_count;
                                    task->import_missing = missing_count;
                                    task->success = !task->result.items.empty();
                                    if (!task->success) task->error = L"没有匹配到任何歌曲，可能目标音源暂时不可用。";
                                }
                            }
                        }
                        else if (task->automatic_switch)
                        {
                            const auto input = ToImportTrack(task->rematch_songs.front());
                            vector<Track> candidates;
                            MatchResult match;
                            if (SearchForTrack(*task->source, input, candidates)) match = PickBest(input, candidates);
                            if (!task->cancelled && match.level == MatchLevel::Auto)
                            {
                                task->progress->Update(L"已匹配到歌曲，正在检查" + task->source->GetShortName() + L"的播放地址");
                                const auto url = task->source->ResolvePlayUrl(match.candidate.virtual_path);
                                task->success = !task->cancelled && (url.starts_with(L"https://") || url.starts_with(L"http://"));
                                if (task->success)
                                {
                                    CSourceRegistry::Instance().RememberPlayUrl(match.candidate.virtual_path, url, task->source->GetQualityNote());
                                    SongInfo song = task->rematch_songs.front();
                                    song.file_path = match.candidate.virtual_path;
                                    task->switched = {std::move(song)};
                                }
                                else task->error = task->source->GetLastError().empty() ? L"目标音源未提供可用的播放地址" : task->source->GetLastError();
                            }
                            else task->error = task->source->GetLastError().empty() ? L"没有找到同一首歌曲的可靠匹配" : task->source->GetLastError();
                        }
                        else if (task->rematch)
                        {
                            // 换源：逐首用标题和歌手在目标音源上重新找一遍。
                            // 本地文件不参与匹配，原样带过去；匹配不到的原曲目也保留，
                            // 否则一次换源就会悄悄少掉几首。
                            task->import_total = static_cast<int>(task->rematch_songs.size());
                            for (const auto& song : task->rematch_songs)
                            {
                                if (task->cancelled) break;
                                if (task->progress) task->progress->Update(L"正在匹配：" + song.GetTitle(),
                                    task->import_done, task->import_total);
                                if (!CSourceRegistry::IsVirtualPath(song.file_path)
                                    || CSourceRegistry::GetScheme(song.file_path) == task->source->GetScheme())
                                {
                                    task->switched.push_back(song);
                                    ++task->rematch_kept; ++task->import_done; continue;
                                }

                                const ImportTrack source_track = ToImportTrack(song);
                                vector<Track> candidates;
                                MatchResult best;
                                if (SearchForTrack(*task->source, source_track, candidates))
                                    best = PickBest(source_track, candidates);
                                if (best.level != MatchLevel::Auto)
                                {
                                    task->switched.push_back(song);
                                    ++task->rematch_missing; ++task->import_done; continue;
                                }

                                SongInfo moved = song;
                                moved.file_path = best.candidate.virtual_path;
                                if (!best.candidate.title.empty()) moved.title = best.candidate.title;
                                if (!best.candidate.artist.empty()) moved.artist = best.candidate.artist;
                                if (!best.candidate.album.empty()) moved.album = best.candidate.album;
                                if (best.candidate.duration_ms > 0) moved.end_pos.fromInt(best.candidate.duration_ms);
                                task->switched.push_back(std::move(moved));
                                ++task->rematch_moved;
                                ++task->import_done;
                            }
                            task->success = !task->cancelled;
                            if (task->success && task->rematch_moved == 0 && task->rematch_missing > 0)
                            {
                                task->success = false;
                                task->error = task->source->GetLastError();
                                if (task->error.empty()) task->error = L"目标音源没有匹配到歌曲，原歌单已保留";
                            }
                        }
                        else if (task->import_all)
                        {
                            for (int page = 1; page <= 500 && !task->cancelled; ++page)
                            {
                                if (task->progress) task->progress->Update(L"正在读取第 " + to_wstring(page) + L" 页",
                                    task->result.items.size());
                                BrowseResult part; task->request.page = page;
                                if (!task->source->Browse(task->request, part)) break;
                                task->result.items.insert(task->result.items.end(), part.items.begin(), part.items.end());
                                if (!part.has_more) { task->success = true; break; }
                            }
                            if (!task->success) task->error = L"歌单未完整读取，未保存部分结果。请稍后重试。";
                        }
                        else task->success = task->source->Browse(task->request, task->result);
                        if (!task->success && task->error.empty()) task->error = task->source->GetLastError();
                    }
                    catch (const exception&) { task->error = L"无法解析服务响应，请稍后重试。"; }
                    task->done = true;
                }
            }).detach();
        }
        { lock_guard<mutex> lock(worker_slot->mutex); worker_slot->pending = task; }
        worker_slot->ready.notify_one();
    }
    catch (const exception&)
    {
        task->error = L"无法启动网络任务，请稍后重试。";
        task->done = true;
    }
}

void COnlineMusicModel::Poll(bool visible)
{
    COnlineDailyRewards::Instance().Tick();
    CBodianAdRewards::Instance().Tick();
    if (!m_state.notice.empty() && GetTickCount64() >= m_notice_until)
    {
        m_state.notice.clear();
        m_notice_until = 0;
        Publish();
    }
    // 操作结果（「已加入播放队列：3 首」）只在状态栏停留几秒，之后回落到列表摘要，
    // 免得半小时前那条提示一直挂着，让人以为刚才的操作又发生了一次。
    if (visible && !m_task && !m_state.importing && m_state.status_stamp
        && GetTickCount64() - m_state.status_stamp >= 8000)
    {
        const wstring summary = ListSummary();
        m_state.status_stamp = 0;
        if (!summary.empty() && m_state.status != summary) { m_state.status = summary; Publish(); }
    }
    // 歌单任务独立于页面浏览，取消标记先于结果写入处理。
    for (const auto& task : {m_task, m_library_task})
    {
        if (task && task->progress && task->progress->Cancelled()) task->cancelled = true;
    }
    if (m_login_progress && m_login_progress->Cancelled()) { CancelRequest(); ResetLogin(); Publish(); }
    if (m_task && m_task->cancelled) { CancelRequest(); Publish(); }
    if (m_library_task && m_library_task->cancelled)
    {
        if (m_library_task->progress) m_library_task->progress->Finish(ProgressResult::Cancelled, L"操作已取消，未写入结果");
        m_library_task.reset(); m_state.importing = false;
        m_state.import_done = m_state.import_total = 0;
        SetStatus(L"歌单任务已取消。"); Publish();
    }
    if (m_library_task)
    {
        const int total = m_library_task->import_total.load();
        const int done = m_library_task->import_done.load();
        if (!m_state.importing || m_state.import_total != total || m_state.import_done != done)
        {
            m_state.importing = true;
            m_state.import_total = total;
            m_state.import_done = done;
            Publish();
        }
    }
    if (visible && GetTickCount64() >= m_service_refresh_at)
    {
        m_service_refresh_at = GetTickCount64() + 1000;
        if (m_cache_view && m_state.detail_visible)
        { auto detail = COnlineMediaCache::Instance().Describe(); if (detail != m_state.detail) { m_state.detail = detail; Publish(); } }
        auto rewards = COnlineDailyRewards::Instance().Describe();
        if (rewards != m_reward_status)
        {
            m_reward_status = rewards;
            if (m_state.page == Page::Account && m_state.qr_size == 0 && !m_task && !m_cache_view) ShowAccount();
        }
    }
    if (visible && !m_was_visible)
    {
        if (!m_started) { m_started = true; SelectPage(); }
        else if (m_interrupted) { m_interrupted = false; if (m_state.page == Page::Account) ShowAccount(true); else StartRequest(); }
    }
    if (!visible && m_was_visible) Suspend();
    m_was_visible = visible;
    if (!m_task && visible && m_login_poll_at && GetTickCount64() >= m_login_poll_at)
    {
        m_login_poll_at = 0; StartRequest(false, false, L"", 2);
    }
    if (m_library_task && m_library_task->done)
    {
        auto& task = *m_library_task;
        if (!task.rematch || !task.in_place || !task.success || ApplyRematch(task, task.replaced))
        {
            auto completed = std::move(m_library_task);
            m_state.importing = false; m_state.import_done = m_state.import_total = 0;
            Publish(); CompleteTask(std::move(completed));
        }
    }
    if (m_task && m_task->done) CompleteTask(std::move(m_task));
}
void COnlineMusicModel::CompleteTask(shared_ptr<Task> task)
{
    if (!task || task->cancelled) return;
    struct FinishProgress
    {
        Task& task;
        ~FinishProgress()
        {
            if (task.progress && task.login_action != 1 && task.login_action != 2)
                task.progress->Finish(task.success ? ProgressResult::Succeeded : ProgressResult::Failed,
                    task.success ? L"操作完成" : task.error.empty() ? L"操作未完成，请重试" : task.error);
        }
    } finish{*task};
    Publish();
    if (task->external_import)
    {
        m_state.importing = false;
        m_state.import_done = 0;
        m_state.import_total = 0;
        Publish();
    }
    if (task->rematch)
    {
        m_state.importing = false;
        m_state.import_done = 0;
        m_state.import_total = 0;
        if (!task->success)
        {
            const wstring error = task->error.empty() ? L"换源失败，原歌单未改动。" : task->error;
            SetStatus(error);
            SetNotice(error);
            Publish();
            return;
        }
        const int moved = task->rematch_moved.load(), kept = task->rematch_kept.load(), missing = task->rematch_missing.load();
        const wstring summary = L"换源到" + task->rematch_target_name + L"：已切 " + to_wstring(moved)
            + L" 首，同源或本地文件保留 " + to_wstring(kept) + L" 首，未可靠匹配 " + to_wstring(missing) + L" 首（保留原音源）。";
        if (task->in_place)
        {
            const wstring done = task->replaced > 0
                ? L"已将 " + to_wstring(task->replaced) + L" 首歌曲切换到" + task->rematch_target_name
                : L"没有需要替换的歌曲，原歌单已保留";
            SetStatus(done + L"。" + summary);
            SetNotice(done);
            if (task->progress) task->progress->Finish(ProgressResult::Succeeded, done);
            Publish();
            return;
        }
        if (task->switched.empty())
        {
            SetStatus(summary + L" 没有可保存的曲目。");
            SetNotice(L"换源完成，但没有可保存的曲目。");
            return;
        }
        wstring saved_name, save_error;
        if (task->progress) task->progress->Update(L"匹配完成，等待选择保存名称", task->import_total, task->import_total);
        if (!SaveSongsToNativePlaylist(task->switched, task->rematch_default_name,
            L"换源后另存为播放列表", L"换源只改在线曲目的来源，本地文件原样保留，原歌单不会被修改。",
            saved_name, save_error, task->progress))
        {
            task->success = false; task->error = save_error;
            SetStatus(summary + L" " + save_error);
            SetNotice(save_error);
            return;
        }
        SetStatus(summary + L" 已保存为「" + saved_name + L"」。");
        if (task->progress) task->progress->Finish(ProgressResult::Succeeded, L"已保存「" + saved_name + L"」");
        SetNotice(L"换源结果已保存到原生播放列表「" + saved_name + L"」");
        if (m_owner)
        {
            m_owner->SetForegroundWindow();
            const wstring message = summary + L"\n\n已另存为本地播放列表「" + saved_name
                + L"」。\n\n是否现在打开媒体库播放列表查看？";
            if (MessageBoxW(m_owner->GetSafeHwnd(), message.c_str(), L"换源完成",
                MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
                theApp.m_pMainWnd->PostMessage(WM_COMMAND, ID_MEDIA_LIB, 0);
        }
        return;
    }
    if (task->profile_request)
    {
        if (task->success) m_profiles[m_state.source] = task->profile;
        ShowAccount();
        SetStatus(task->success ? L"" : L"账号信息读取失败");
        return;
    }
    if (!task->success)
    {
        // 扫码轮询里的失败多半只是网络抖动：CheckQrCode 把超时、连不上、响应格式异常
        // 一律算成 Failed，直接 ResetLogin 会把用户刚扫完的会话一起丢掉。
        // 连着错几次才当真的失败，中间继续按 2 秒一次重试。
        if (task->login_action == 2 && ++m_login_failures <= LOGIN_POLL_FAILURE_LIMIT)
        {
            m_login_poll_at = GetTickCount64() + 2000;
            if (task->progress) task->progress->Update(L"暂时联系不上服务端，正在重试");
            Publish();
            return;
        }
        const wstring error = task->error.empty() ? L"没有可用结果，请刷新重试。" : task->error;
        if (task->progress) task->progress->Finish(ProgressResult::Failed, error);
        if (task->login_action) ResetLogin();
        SetStatus(error);
        if (task->external_import)
        {
            SetNotice(error);
            if (m_owner) MessageBoxW(m_owner->GetSafeHwnd(), error.c_str(), L"歌单导入失败",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return;
    }
    if (task->external_import)
    {
        const int auto_count = task->import_auto.load();
        const int confirm_count = task->import_confirm.load();
        const int missing_count = task->import_missing.load();
        const wstring summary = L"匹配完成：自动采用 " + to_wstring(auto_count) + L" 首，待确认 "
            + to_wstring(confirm_count) + L" 首，未找到 " + to_wstring(missing_count) + L" 首。";
        vector<SongInfo> songs;
        for (const auto& item : task->result.items)
        {
            if (!item.track.IsValid()) continue;
            SongInfo song; song.file_path = item.track.virtual_path; song.title = item.track.title;
            song.artist = item.track.artist; song.album = item.track.album;
            song.end_pos.fromInt(item.track.duration_ms);
            songs.push_back(std::move(song));
        }
        if (songs.empty())
        {
            SetStatus(summary + L" 没有可保存的曲目。");
            SetNotice(L"没有可保存的曲目。");
            if (m_owner) MessageBoxW(m_owner->GetSafeHwnd(), (summary + L"\n\n没有可保存的曲目。").c_str(),
                L"歌单导入失败", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return;
        }

        // 外部导入保存成原生 .playlist，支持「分组/歌单名」。
        wstring saved_name, save_error;
        if (task->progress) task->progress->Update(L"匹配完成，等待选择保存名称", task->import_done, task->import_total);
        if (!SaveSongsToNativePlaylist(songs, task->import_default_name,
            L"保存为本地播放列表", L"输入名称，或用「分组/歌单名」按语种分组，例如「日语/热榜」。",
            saved_name, save_error, task->progress))
        {
            task->success = false; task->error = save_error;
            SetStatus(summary + L" " + save_error);
            SetNotice(save_error);
            if (m_owner) MessageBoxW(m_owner->GetSafeHwnd(), (summary + L"\n\n" + save_error).c_str(),
                L"歌单导入失败", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return;
        }
        if (m_owner)
        {
            m_owner->SetForegroundWindow();
            const wstring message = summary + L"\n\n已保存为本地播放列表「" + saved_name
                + L"」。\n\n是否现在打开媒体库播放列表查看？";
            if (MessageBoxW(m_owner->GetSafeHwnd(), message.c_str(), L"歌单导入完成",
                MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
                theApp.m_pMainWnd->PostMessage(WM_COMMAND, ID_MEDIA_LIB, 0);
        }
        SetStatus(summary + L" 已保存为本地播放列表「" + saved_name + L"」。");
        SetNotice(L"已保存到原生播放列表「" + saved_name + L"」，请到媒体库 → 播放列表查看。");
        return;
    }
    if (task->login_action)
    {
        if (task->login_action == 1)
        {
            try
            {
                auto qr = qrcodegen::QrCode::encodeText(kugou::ToUtf8(task->text).c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
                m_state.qr_size = qr.getSize(); m_state.qr_pixels.clear();
                for (int y = 0; y < m_state.qr_size; ++y)
                    for (int x = 0; x < m_state.qr_size; ++x) m_state.qr_pixels.push_back(qr.getModule(x, y));
                m_state.status = task->source->GetScheme() == L"bodian" ? L"使用B源 App 扫码，并在手机上确认登录。" : L"使用K源 App 扫码，并在手机上确认登录。";
                if (task->progress) task->progress->Update(L"二维码已就绪，等待手机扫码确认");
                m_login_poll_at = GetTickCount64() + 2000;
            }
            catch (const exception&)
            {
                if (task->progress) task->progress->Finish(ProgressResult::Failed, L"二维码生成失败");
                ResetLogin(); m_state.status = L"二维码生成失败，请重试。";
            }
        }
        else
        {
            auto status = static_cast<kugou::CKugouSource::QrStatus>(task->login_status);
            if (status == kugou::CKugouSource::QrStatus::Authorized)
            {
                bool persisted = true;
                if (auto* snapshot = dynamic_cast<kugou::CKugouSource*>(task->source.get()))
                {
                    auto* kg = dynamic_cast<kugou::CKugouSource*>(CSourceRegistry::Instance().FindByScheme(L"kugou"));
                    COnlineDailyRewards::Instance().CancelPending();
                    kg->SetAccount(snapshot->GetAccount()); kg->SaveIdentity(theApp.m_config_dir);
                }
                else if (auto* snapshot = dynamic_cast<bodian::CBodianSource*>(task->source.get()))
                {
                    auto* bd = dynamic_cast<bodian::CBodianSource*>(CSourceRegistry::Instance().FindByScheme(L"bodian"));
                    bd->SetAccount(snapshot->GetAccount()); persisted = bd->SaveIdentity(theApp.m_config_dir);
                }
                m_profiles.erase(m_state.source);
                // 登录会改变权限，之前缓存的失败结论统统作废
                CSourceRegistry::Instance().ForgetAllPlayUrls();
                if (task->progress) task->progress->Finish(persisted ? ProgressResult::Succeeded : ProgressResult::Failed,
                    persisted ? L"登录成功" : L"登录信息保存失败");
                ResetLogin(); ShowAccount(true);
                if (!persisted) m_state.status = L"已登录，但本机登录信息保存失败，重启后需要重新登录。";
            }
            else if (status == kugou::CKugouSource::QrStatus::Expired)
            {
                if (task->progress) task->progress->Finish(ProgressResult::Failed, L"二维码已过期，请重新获取");
                ResetLogin(); m_state.status = L"二维码已过期，请重新获取。";
            }
            else
            {
                // 这一次问到了状态，连续失败计数清零
                m_login_failures = 0;
                m_login_poll_at = GetTickCount64() + 2000;
                if (status == kugou::CKugouSource::QrStatus::Scanned) m_state.status = L"已扫码，请在手机上确认。";
                if (task->progress) task->progress->Update(status == kugou::CKugouSource::QrStatus::Scanned
                    ? L"已扫码，等待手机确认" : L"等待手机扫码，连接正常");
            }
        }
        Publish(); return;
    }
    if (!task->lyric_path.empty())
    {
        m_state.detail = task->text; m_state.detail_visible = true;
        SetStatus(L"歌词"); return;
    }
    if (task->import_all)
    {
        vector<SongInfo> songs;
        for (const auto& item : task->result.items)
        {
            if (!item.track.IsValid()) continue;
            SongInfo song; song.file_path = item.track.virtual_path; song.title = item.track.title;
            song.artist = item.track.artist; song.album = item.track.album; song.end_pos.fromInt(item.track.duration_ms);
            songs.push_back(std::move(song));
        }
        task->success = SaveLocal(songs, task->progress); return;
    }
    if (task->append) m_state.items.insert(m_state.items.end(), task->result.items.begin(), task->result.items.end());
    else m_state.items = std::move(task->result.items);
    m_state.request = task->request; m_state.has_more = task->result.has_more;
    m_state.detail_visible = false;
    Publish(true, !task->append);
    // 列表摘要不是「操作结果」，不参与几秒后回落，直接常驻
    m_state.status = m_state.items.empty() ? L"暂无结果，可以换个关键词。" : ListSummary();
    m_state.status_stamp = 0;
    Publish();
}
vector<SongInfo> COnlineMusicModel::SelectedSongs(const vector<int>& rows, bool all) const
{
    vector<SongInfo> songs;
    for (size_t i = 0; i < m_state.songs.size(); ++i)
        if (!m_state.songs[i].file_path.empty() && (all || find(rows.begin(), rows.end(), static_cast<int>(i)) != rows.end()))
            songs.push_back(m_state.songs[i]);
    return songs;
}
bool COnlineMusicModel::TakePlayback(Playback& playback)
{
    if (m_playback.empty()) return false;
    playback = std::move(m_playback.front()); m_playback.pop_front(); return true;
}
void COnlineMusicModel::ShowLocal()
{
    m_state.items.clear(); m_state.detail_visible = false;
    for (const auto& song : m_local)
    {
        BrowseItem item; item.track.virtual_path = song.file_path; item.title = song.GetTitle();
        item.subtitle = song.GetArtist(); item.track.title = song.title; item.track.artist = song.artist;
        item.track.album = song.album; item.track.duration_ms = song.length().toInt(); m_state.items.push_back(std::move(item));
    }
    m_state.status = L"本地歌单 · " + to_wstring(m_local.size()) + L" 首";
    m_state.status_stamp = 0;
    Publish(true);
}
void COnlineMusicModel::ShowAccount(bool refresh)
{
    m_cache_view = false;
    m_state.items.clear();
    m_state.has_more = false;
    m_state.request = {};
    m_state.detail_visible = true;
    if (auto* bd = dynamic_cast<bodian::CBodianSource*>(CurrentSource()))
    {
        m_state.detail = wstring(L"B源 · ") + (bd->IsLoggedIn() ? L"已登录" : L"未登录");
        m_state.detail += L"\n\n" + CBodianAdRewards::Instance().Describe();
    }
    else
    {
        auto* kg = dynamic_cast<kugou::CKugouSource*>(CurrentSource());
        m_state.detail = kg && kg->IsLoggedIn()
            ? L"K源 · 已登录"
            : L"K源 · 未登录";
        m_state.detail += L"\n\n" + COnlineDailyRewards::Instance().Describe();
    }
    if (m_state.detail.empty()) m_state.detail = L"在线音乐账号";
    auto* source = CurrentSource();
    auto* kg = dynamic_cast<kugou::CKugouSource*>(source);
    auto* bd = dynamic_cast<bodian::CBodianSource*>(source);
    m_state.logged_in = (kg && kg->IsLoggedIn()) || (bd && bd->IsLoggedIn());
    m_state.status.clear(); Publish(true);
    if (m_state.logged_in)
    {
        auto profile = m_profiles[m_state.source];
        for (auto& c : profile.name) if (c < 32) c = L' ';
        m_state.detail += L"\n\n用户名：" + (profile.name.empty() ? L"未获取" : profile.name)
            + L"\n会员：" + (profile.membership.empty() ? L"未获取" : profile.membership);
        if (!profile.expires.empty()) m_state.detail += L"\n到期：" + profile.expires;
        Publish();
        if (refresh) StartRequest(false, false, L"", 0, true);
    }
}
bool COnlineMusicModel::SaveLocal(const vector<SongInfo>& songs, shared_ptr<OnlineProgress> progress)
{
    if (!progress) progress = OnlineProgress::Start(L"保存在线本地歌单");
    if (songs.empty()) { SetStatus(L"请先选中歌曲。"); progress->Finish(ProgressResult::Failed, m_state.status); return false; }
    progress->Update(L"正在合并歌曲并保存", 0, songs.size());
    CPlaylistFile file; file.AddSongsToPlaylist(m_local);
    int added = file.AddSongsToPlaylist(songs);
    if (!file.SaveToFile(LocalPath())) { SetStatus(L"保存失败，原歌单已保留。请检查目录写入权限。"); progress->Finish(ProgressResult::Failed, m_state.status); return false; }
    m_local = file.GetPlaylist();
    // 红心标记要跟着变：本地页重建列表，其它页只重算每行的收藏状态、保留选中
    if (m_state.page == Page::Local) ShowLocal(); else Publish(true, false);
    SetStatus(L"已保存 " + to_wstring(added) + L" 首，跳过重复 " + to_wstring(songs.size() - added) + L" 首。");
    SetNotice(L"已保存到在线本地歌单。");
    progress->Finish(ProgressResult::Succeeded, m_state.status);
    return true;
}

void COnlineMusicModel::AddToExistingPlaylist(const vector<SongInfo>& songs)
{
    if (songs.empty()) { SetStatus(L"请先选中歌曲，可使用 Ctrl / Shift 多选。"); return; }
    // 复用媒体库的「加入播放列表」流程：选择框、去重、当前正在播放的列表直接改内核，
    // 都在那一条路上，在线页不再自己写一套。
    CAddToPlaylistDlg dialog(m_owner);
    if (dialog.DoModal() != IDOK) { SetStatus(L"已取消加入播放列表。"); return; }
    const wstring playlist_path = CMusicPlayerCmdHelper::ResolvePlaylistPath(dialog.GetPlaylistSelected());
    if (playlist_path.empty() || !CCommon::FileExist(playlist_path))
    {
        SetStatus(L"没有找到这个播放列表，可能已被移动或删除。");
        return;
    }
    CMusicPlayerCmdHelper helper(m_owner);
    const int added = helper.AddToPlaylist(songs, playlist_path);
    SetStatus(added >= 0 ? L"已加入播放列表「" + ListItem{ LT_PLAYLIST, playlist_path }.GetDisplayName()
        + L"」：" + to_wstring(added) + L" 首。" : L"加入播放列表失败，请检查歌单是否可写或稍后重试。");
}

void COnlineMusicModel::CopySongInfo(const vector<int>& rows)
{
    auto songs = SelectedSongs(rows);
    if (songs.empty()) { SetStatus(L"请先选中歌曲。"); return; }
    wstring text;
    for (const auto& song : songs)
    {
        if (!text.empty()) text += L"\r\n";
        const wstring title = song.GetTitle().empty() ? song.file_path : song.GetTitle();
        text += song.GetArtist().empty() ? title : song.GetArtist() + L" - " + title;
        if (!song.file_path.empty()) text += L"\t" + song.file_path;
    }
    SetStatus(CCommon::CopyStringToClipboard(text)
        ? L"已复制 " + to_wstring(songs.size()) + L" 首歌曲信息。" : L"复制失败：剪贴板被其他程序占用。");
}

void COnlineMusicModel::OpenFileLocation(const vector<int>& rows)
{
    auto songs = SelectedSongs(rows);
    if (songs.empty()) { SetStatus(L"请先选中歌曲。"); return; }
    size_t opened{};
    for (const auto& song : songs)
    {
        if (!CCommon::FileExist(song.file_path)) continue;
        const wstring argument = L"/select,\"" + song.file_path + L"\"";
        if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"explorer.exe",
            argument.c_str(), nullptr, SW_SHOWNORMAL)) > 32) ++opened;
    }
    // 在线曲目的 file_path 是虚拟地址，磁盘上没有这个文件，本地歌单里导入进来的才有
    SetStatus(opened ? L"已在资源管理器中定位 " + to_wstring(opened) + L" 个文件。"
        : L"选中的是纯在线曲目，本地还没有文件，可以先下载。");
}

void COnlineMusicModel::RemoveLocal(const vector<int>& rows)
{
    vector<SongInfo> targets;
    if (m_state.page == Page::Local)
    {
        // 本地页的列表就是 m_local，行号直接对应
        for (int row : rows)
            if (row >= 0 && row < static_cast<int>(m_local.size())) targets.push_back(m_local[row]);
    }
    else
    {
        // 在线页的列表是搜索结果，行号对不上 m_local，按虚拟路径找同一首
        for (const auto& song : SelectedSongs(rows))
            for (const auto& local : m_local)
                if (local.file_path == song.file_path) { targets.push_back(local); break; }
    }

    if (targets.empty())
    {
        SetStatus(m_state.page == Page::Local ? L"请先选中要移除的歌曲。" : L"选中的歌曲不在本地歌单里。");
        return;
    }
    auto progress = OnlineProgress::Start(L"移除在线本地收藏", L"正在更新歌单");
    CPlaylistFile file;
    file.AddSongsToPlaylist(m_local);
    for (const auto& song : targets) file.RemoveSong(song);
    if (!file.SaveToFile(LocalPath()))
    {
        SetStatus(L"移除失败，歌单未改动。请检查目录写入权限。");
        progress->Finish(ProgressResult::Failed, m_state.status);
        return;
    }
    m_local = file.GetPlaylist();
    if (m_state.page == Page::Local) ShowLocal(); else Publish(true, false);
    SetStatus(L"已从本地歌单移除 " + to_wstring(targets.size()) + L" 首。");
    SetNotice(L"已从在线本地歌单移除 " + to_wstring(targets.size()) + L" 首。");
    progress->Finish(ProgressResult::Succeeded, m_state.status);
}

void COnlineMusicModel::ClearLocal()
{
    if (m_local.empty()) { SetStatus(L"在线本地歌单已经是空的。"); return; }
    if (m_owner)
    {
        m_owner->SetForegroundWindow();
        const wstring message = L"确定要清空在线本地歌单吗？共 " + to_wstring(m_local.size())
            + L" 首，此操作不可撤销。";
        if (MessageBoxW(m_owner->GetSafeHwnd(), message.c_str(), L"清空在线本地歌单",
            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) != IDYES)
        {
            SetStatus(L"已取消清空。");
            return;
        }
    }
    const vector<SongInfo> empty;
    auto progress = OnlineProgress::Start(L"清空在线本地歌单", L"正在保存空歌单");
    if (!CPlaylistFile::SavePlaylistToFile(empty, LocalPath()))
    {
        SetStatus(L"清空失败，原歌单未改动。请检查目录写入权限。");
        progress->Finish(ProgressResult::Failed, m_state.status);
        return;
    }
    m_local.clear();
    if (m_state.page == Page::Local) ShowLocal();
    else Publish(true, false);
    SetStatus(L"在线本地歌单已清空。");
    SetNotice(L"在线本地歌单已清空。");
    progress->Finish(ProgressResult::Succeeded, m_state.status);
}
void COnlineMusicModel::ImportFile()
{
    CFileDialog dialog(TRUE, nullptr, nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
        L"歌单文件|*.m3u8;*.m3u;*.playlist;*.btplaylist;*.json;*.wpl;*.ttpl||", m_owner);
    if (dialog.DoModal() != IDOK) return;
    auto progress = OnlineProgress::Start(L"导入歌单文件", L"正在读取歌单");
    CFileStatus status;
    if (!CFile::GetStatus(dialog.GetPathName(), status) || status.m_size > 16 * 1024 * 1024)
    { SetStatus(L"无法读取文件，或文件超过 16 MB。"); progress->Finish(ProgressResult::Failed, m_state.status); return; }
    CPlaylistFile file;
    if (!file.LoadFromFile(dialog.GetPathName().GetString()) || file.GetPlaylist().empty())
    { SetStatus(L"没有有效歌曲。可导入 M3U8、原生播放列表或 BoTapMusic JSON 歌单。"); progress->Finish(ProgressResult::Failed, m_state.status); return; }
    m_state.page = Page::Local; SelectPage(); SaveLocal(file.GetPlaylist(), progress);
}
bool COnlineMusicModel::SaveSongsToNativePlaylist(const vector<SongInfo>& songs, const wstring& default_name,
    const wstring& title, const wstring& info, wstring& saved_name, wstring& error, shared_ptr<OnlineProgress> progress)
{
    saved_name.clear();
    error.clear();
    if (!progress) progress = OnlineProgress::Start(L"保存播放列表", L"等待选择保存名称");
    struct SaveProgress
    {
        shared_ptr<OnlineProgress> progress;
        wstring& error;
        ProgressResult result{ProgressResult::Failed};
        ~SaveProgress() { progress->Finish(result, error.empty() ? L"播放列表已保存" : error); }
    } finish{progress, error};
    if (songs.empty()) { error = L"没有可保存的曲目。"; return false; }

    CInputDlg name_dlg(m_owner);
    name_dlg.SetTitle(title.c_str());
    name_dlg.SetInfoText(info.c_str());
    name_dlg.SetEditText(default_name.c_str());
    if (name_dlg.DoModal() != IDOK || progress->Cancelled())
    { error = L"已取消，未保存。"; finish.result = ProgressResult::Cancelled; return false; }
    progress->Update(L"正在保存播放列表", 0, songs.size());

    wstring playlist_name = name_dlg.GetEditText().GetString();
    while (!playlist_name.empty() && (playlist_name.back() == L' ' || playlist_name.back() == L'\t')) playlist_name.pop_back();
    while (!playlist_name.empty() && (playlist_name.front() == L' ' || playlist_name.front() == L'\t')) playlist_name.erase(playlist_name.begin());
    std::replace(playlist_name.begin(), playlist_name.end(), L'/', L'\\');
    std::replace(playlist_name.begin(), playlist_name.end(), L'／', L'\\');
    if (playlist_name.empty() || playlist_name.back() == L'\\' || playlist_name.find(L':') != wstring::npos)
    {
        error = L"名称或分组格式不正确，未保存。";
        return false;
    }
    size_t start_pos{};
    while (start_pos <= playlist_name.size())
    {
        const size_t separator = playlist_name.find(L'\\', start_pos);
        const wstring part = separator == wstring::npos
            ? playlist_name.substr(start_pos) : playlist_name.substr(start_pos, separator - start_pos);
        if (part.empty() || part == L"." || part == L".." || !CCommon::IsFileNameValid(part))
        {
            error = L"名称或分组包含非法字符，未保存。";
            return false;
        }
        if (separator == wstring::npos) break;
        start_pos = separator + 1;
    }

    wstring playlist_path = theApp.m_playlist_dir + playlist_name + PLAYLIST_EXTENSION;
    std::error_code directory_error;
    std::filesystem::create_directories(CFilePathHelper(playlist_path).GetDir(), directory_error);
    if (directory_error) { error = L"无法创建分组文件夹，未保存。"; return false; }
    if (CCommon::FileExist(playlist_path)) CCommon::FileAutoRename(playlist_path);
    if (!CPlaylistFile::SavePlaylistToFile(songs, playlist_path, CPlaylistFile::PL_PLAYLIST))
    {
        error = L"保存本地播放列表失败，请检查磁盘空间和写入权限。";
        return false;
    }
    CRecentList::Instance().AddNewItem(ListItem{ LT_PLAYLIST, playlist_path });
    CRecentList::Instance().SaveData();

    saved_name = CFilePathHelper(playlist_path).GetFileNameWithoutExtension();
    const wstring group_dir = CFilePathHelper(playlist_path).GetDir();
    if (group_dir.size() > theApp.m_playlist_dir.size()
        && _wcsnicmp(group_dir.c_str(), theApp.m_playlist_dir.c_str(), theApp.m_playlist_dir.size()) == 0)
    {
        wstring group = group_dir.substr(theApp.m_playlist_dir.size());
        while (!group.empty() && (group.back() == L'\\' || group.back() == L'/')) group.pop_back();
        if (!group.empty()) saved_name = group + L" / " + saved_name;
    }
    finish.result = ProgressResult::Succeeded;
    return true;
}
void COnlineMusicModel::SaveLocalAsNativePlaylist(const vector<int>& rows)
{
    auto songs = SelectedSongs(rows, rows.empty());
    if (songs.empty())
    {
        m_state.page == Page::Local ? SetStatus(L"请先选中要迁移的歌曲。") : SetStatus(L"当前列表没有可迁移的在线歌曲。");
        return;
    }
    // 在专辑/歌单/榜单详情里整张保存时，默认名直接用这份列表的名字（例如专辑名），
    // 不然用户每次都要自己敲一遍。迁移在线本地歌单时列表没有名字，仍用日期兜底。
    const bool migrating = m_state.list_title.empty();
    const wstring default_name = migrating
        ? wstring(L"在线本地歌单 ") + CTime::GetCurrentTime().Format(L"%Y-%m-%d").GetString() : m_state.list_title;
    wstring saved_name, error;
    if (!SaveSongsToNativePlaylist(songs, default_name,
        migrating ? L"迁移到原生播放列表" : L"存为原生播放列表",
        L"输入名称，或用「分组/歌单名」按语种分组，例如「日语/歌单名」。", saved_name, error))
    {
        SetStatus(error);
        return;
    }
    if (m_owner)
    {
        m_owner->SetForegroundWindow();
        wstring message = L"已保存 " + to_wstring(songs.size()) + L" 首歌曲到原生播放列表「"
            + saved_name + L"」。";
        // 只有在迁移在线本地歌单时才提这一句：整张存歌单不会动任何已有列表
        if (m_state.page == Page::Local)
            message += L"\n\n原在线本地歌单不会自动清空，可在「本地歌单」页多选后移除。";
        MessageBoxW(m_owner->GetSafeHwnd(), message.c_str(), L"已保存为播放列表",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }
    SetStatus(L"已迁移 " + to_wstring(songs.size()) + L" 首到原生播放列表「" + saved_name + L"」。");
}
void COnlineMusicModel::ImportExternal()
{
    CInputDlg dialog(m_owner);
    dialog.SetTitle(L"从外部平台 导入歌单");
    dialog.SetInfoText(L"粘贴外部平台 歌单分享链接或分享文本。");
    if (dialog.DoModal() != IDOK) return;
    CString text = dialog.GetEditText();
    text.Trim();
    if (text.IsEmpty()) { SetStatus(L"请先粘贴歌单分享链接。"); return; }
    const auto reference = ParseShareText(text.GetString());
    if (!reference.IsValid()) { SetStatus(L"没有识别出支持的歌单链接，目前支持外部平台的分享链接。"); return; }
    if (reference.source == ImportSource::Kuwo) { SetStatus(L"上游歌单导入暂未支持。"); return; }
    StartExternalImport(text.GetString());
}
void COnlineMusicModel::StartExternalImport(const wstring& text)
{
    if (m_library_task) { ShowTip(L"已有歌单任务正在进行，可在底部查看进度。"); return; }
    m_cache_view = false;
    m_interrupted = false;
    CancelRequest();
    ResetLogin();
    auto* source = CurrentSource();
    if (!source) { SetStatus(L"没有可用的在线音源"); return; }
    auto task = make_shared<Task>();
    if (auto* kg = dynamic_cast<kugou::CKugouSource*>(source)) task->source = make_shared<kugou::CKugouSource>(*kg);
    else if (auto* bd = dynamic_cast<bodian::CBodianSource*>(source)) task->source = make_shared<bodian::CBodianSource>(*bd);
    if (!task->source) { SetStatus(L"该音源暂不支持外部歌单匹配"); return; }
    task->external_import = true;
    task->share_text = text;
    {
        const auto reference = ParseShareText(text);
        const wchar_t* prefix = L"外部平台歌单";
        task->import_default_name = wstring(prefix) + L" " + CTime::GetCurrentTime().Format(L"%Y-%m-%d").GetString();
    }
    task->progress = OnlineProgress::Start(L"导入外部歌单", L"正在读取歌单", false, true);
    m_library_task = task;
    m_state.items.clear();
    m_state.has_more = false;
    m_state.detail_visible = false;
    m_state.importing = true;
    m_state.import_done = 0;
    m_state.import_total = 0;
    m_state.status = L"正在读取外部歌单…";
    Publish(true);
    EnqueueTask(task);
}
void COnlineMusicModel::ShowTip(const wstring& text)
{
    SetNotice(text);
    // 从媒体库那边发起换源时，在线页的状态栏用户看不到，所以同时在皮肤上打一条提示，
    // 让「点完之后到底有没有反应」立刻有答案。
    CMusicPlayerCmdHelper helper(m_owner);
    helper.ShowTip(text);
}
void COnlineMusicModel::SwitchSource(int target_source, const vector<SongInfo>& songs, const wstring& name_hint,
    bool in_place, bool in_place_local, const wstring& playlist_path)
{
    const auto& sources = CSourceRegistry::Instance().GetAll();
    if (target_source < 0 || target_source >= static_cast<int>(sources.size()))
    { SetStatus(L"请选择目标音源。"); return; }
    if (songs.empty()) { SetStatus(L"没有可换源的歌曲。"); return; }
    if (m_library_task) { ShowTip(L"已有歌单任务正在进行，可在底部查看进度。"); return; }

    auto* target = sources[target_source];
    auto task = make_shared<Task>();
    if (auto* kg = dynamic_cast<kugou::CKugouSource*>(target)) task->source = make_shared<kugou::CKugouSource>(*kg);
    else if (auto* bd = dynamic_cast<bodian::CBodianSource*>(target)) task->source = make_shared<bodian::CBodianSource>(*bd);
    if (!task->source) { SetStatus(L"该音源暂不支持换源。"); return; }

    task->rematch = true;
    task->rematch_songs = songs;
    task->rematch_target_name = target->GetDisplayName();
    task->in_place = in_place;
    task->in_place_local = in_place_local;
    task->target_playlist = playlist_path;
    if (in_place && !in_place_local && task->target_playlist.empty())
    { ShowTip(L"请从已保存的播放列表选择歌曲换源。"); return; }
    // 整份歌单沿用另存行为；单曲和所选保存回任务捕获的原歌单。
    task->rematch_default_name = name_hint.empty()
        ? L"换源到" + target->GetShortName() + L" " + CTime::GetCurrentTime().Format(L"%Y-%m-%d").GetString()
        : name_hint + L" · " + target->GetShortName();

    m_cache_view = false;
    m_interrupted = false;
    task->import_total = static_cast<int>(songs.size());
    task->progress = OnlineProgress::Start(L"换源到" + target->GetShortName(), L"正在准备匹配", false, true);
    m_library_task = task;
    m_state.importing = true;
    m_state.import_done = 0;
    m_state.import_total = static_cast<int>(songs.size());
    m_state.status = L"正在把 " + to_wstring(songs.size()) + L" 首重新匹配到" + task->rematch_target_name + L"…";
    Publish();
    EnqueueTask(task);
}

bool COnlineMusicModel::ApplyRematch(Task& task, int& replaced)
{
    if (task.progress) task.progress->Update(L"正在保存到原歌单", task.import_total, task.import_total);
    if (task.in_place_local)
    {
        auto updated = m_local;
        replaced = ApplySourceMatches(updated, task.rematch_songs, task.switched);
        if (replaced > 0 && !SaveSourceChanges(updated, LocalPath(), task.error)) task.success = false;
        else if (replaced > 0)
        {
            m_local.swap(updated);
            if (m_state.page == Page::Local) ShowLocal();
            else Publish(true, false);
        }
    }
    else
    {
        replaced = CPlayer::GetInstance().ApplyOnlineSourceChanges(task.target_playlist,
            task.rematch_songs, task.switched, task.error);
        if (replaced == -2)
        {
            if (!task.apply_started) task.apply_started = GetTickCount64();
            if (GetTickCount64() - task.apply_started < 30000)
            {
                if (task.progress) task.progress->Update(L"匹配完成，等待播放队列更新后写入", task.import_total, task.import_total);
                return false;
            }
            task.error = L"播放队列仍在忙，未应用换源结果，请重试";
        }
        if (replaced < 0) task.success = false;
    }
    if (task.success && replaced == 0 && task.rematch_moved > 0)
    { task.success = false; task.error = L"原曲目已移除或来源已变更，未覆盖当前歌单"; }
    return true;
}
void COnlineMusicModel::ExportFile(const vector<int>& rows)
{
    auto songs = SelectedSongs(rows, rows.empty());
    if (songs.empty()) { SetStatus(L"当前没有可以导出的歌曲。"); return; }
    CFileDialog dialog(FALSE, L"m3u8", L"我的歌单.m3u8", OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
        L"M3U8 歌单|*.m3u8|BoTapMusic JSON 歌单|*.btplaylist;*.json|原生播放列表|*.playlist||", m_owner);
    if (dialog.DoModal() != IDOK) return;
    wstring path = dialog.GetPathName().GetString(), extension = CFilePathHelper(path).GetFileExtension();
    transform(extension.begin(), extension.end(), extension.begin(), towlower);
    CPlaylistFile::Type type;
    if (extension == L"m3u8") type = CPlaylistFile::PL_M3U8;
    else if (extension == L"json" || extension == L"btplaylist") type = CPlaylistFile::PL_JSON;
    else if (extension == L"playlist") type = CPlaylistFile::PL_PLAYLIST;
    else { SetStatus(L"请使用 .m3u8、.btplaylist、.json 或 .playlist 扩展名。"); return; }
    auto progress = OnlineProgress::Start(L"导出歌单", L"正在写入歌单文件");
    const bool saved = CPlaylistFile::SavePlaylistToFile(songs, path, type);
    SetStatus(saved
        ? L"已导出 " + to_wstring(songs.size()) + L" 首歌曲。"
        : L"导出失败，原文件未被替换。请检查磁盘空间和写入权限。");
    progress->Finish(saved ? ProgressResult::Succeeded : ProgressResult::Failed, m_state.status);
}
void COnlineMusicModel::Execute(const Command& command)
{
    bool row_action = command.action == Action::Open || command.action == Action::Play || command.action == Action::Queue
        || command.action == Action::Save || command.action == Action::Lyrics || command.action == Action::Export
        || command.action == Action::Download || command.action == Action::SaveAsNativePlaylist
        || command.action == Action::PlayNext || command.action == Action::AddToPlaylist
        || command.action == Action::AddToFavourite || command.action == Action::RetryPlayback
        || command.action == Action::CopyInfo || command.action == Action::OpenFileLocation
        || command.action == Action::RemoveLocal
        || command.action == Action::SwitchInPlace;
    if (row_action && command.revision != m_state.revision) { SetStatus(L"列表已更新，请重新选择歌曲。"); return; }
    // 点播失败过的曲目再点一次就是想重试：丢掉备忘和灰色标记，重新解析。
    // 不然用户看着灰色行双击，播放器还是直接从失败备忘里读到上次的结论。
    auto forgive = [this](const vector<SongInfo>& songs) {
        bool changed{};
        for (const auto& song : songs)
        {
            if (song.file_path.empty() || m_unplayable.erase(song.file_path) == 0) continue;
            CSourceRegistry::Instance().ForgetPlayUrl(song.file_path);
            changed = true;
        }
        if (changed) Publish(true, false);
    };
    switch (command.action)
    {
    case Action::Activate:
        if (!m_started) { m_started = true; SelectPage(); }
        else if (m_interrupted) StartRequest();
        break;
    case Action::Page:
        if (command.value >= 0 && command.value < 7) { m_state.page = static_cast<Page>(command.value); SelectPage(); } break;
    case Action::Source:
        if (command.value >= 0 && command.value < static_cast<int>(CSourceRegistry::Instance().GetAll().size()))
        { m_state.source = command.value; SelectPage(); } break;
    case Action::Query: m_state.query = command.text; Publish(); break;
    case Action::SearchType:
        if (command.value < 0 || command.value > 2) break;
        m_state.search_type = command.value;
        // 停在搜索页时顺带刷新提示语；已经有结果就只更新选中项
        if (m_state.page == Page::Search && m_state.query.empty()) SelectPage();
        else Publish();
        break;
    case Action::Search:
        if (m_state.query.empty())
        {
            SetStatus(m_state.search_type == 1 ? L"请先输入专辑名。"
                : m_state.search_type == 2 ? L"请先输入歌单关键词。" : L"请先输入歌名或歌手。");
            break;
        }
        // 一次到位：切到搜索页、写请求、发出去。不必先走一遍 SelectPage 再覆盖请求。
        m_state.page = Page::Search;
        CancelRequest(); ResetLogin();
        m_state.items.clear(); m_state.detail.clear(); m_state.detail_visible = false; m_state.has_more = false;
        m_state.list_title.clear();
        m_state.request = { SearchKind(), m_state.query, 1 };
        StartRequest(); break;
    case Action::SearchPlaylists:
        if (!dynamic_cast<bodian::CBodianSource*>(CurrentSource())) { SetStatus(L"请切换到B源搜索公开歌单。"); break; }
        if (m_state.query.empty()) { SetStatus(L"请在搜索框输入歌单关键词。"); break; }
        m_state.page = Page::Cloud; m_state.request = {BrowseKind::PlaylistSearch, m_state.query, 1}; StartRequest(); break;
    case Action::Open:
        if (command.rows.empty()) break;
        if (command.rows.front() >= 0 && command.rows.front() < static_cast<int>(m_state.items.size()))
        {
            auto item = m_state.items[command.rows.front()];
            // 双击歌曲直接播，不管有没有浏览任务在跑：播放不经过浏览线程，
            // 「上一次加载还没结束」只对打开歌单／专辑这类要发请求的行成立。
            if (item.type == BrowseItem::Type::Song)
            {
                auto songs = SelectedSongs(command.rows);
                forgive(songs);
                if (!songs.empty()) m_playback.push_back({std::move(songs), false});
                break;
            }
            if (m_task) { SetStatus(L"上一次加载还没结束，请稍候再双击。"); break; }
            // 专辑按 AlbumTracks 取这张专辑的曲目；以前是回搜索页用「歌手 + 专辑名」
            // 再搜一次，结果混进同歌手的其它专辑、顺序也是相关度排的，这里不再那么做。
            const bool album = item.type == BrowseItem::Type::Album;
            const bool keyword = item.type == BrowseItem::Type::Keyword;
            m_state.request = { album ? BrowseKind::AlbumTracks : keyword ? BrowseKind::Search
                : item.type == BrowseItem::Type::Chart ? BrowseKind::ChartTracks : BrowseKind::PlaylistTracks, item.id, 1 };
            if (keyword)
            { m_state.page = Page::Search; m_state.query = item.title; ++m_state.query_revision; }
            // 记下这份专辑/歌单/榜单的名字：整批下载按它建子文件夹，「存为歌单」用它当默认名
            m_state.list_title = (album || item.type == BrowseItem::Type::Chart || item.type == BrowseItem::Type::Playlist)
                ? item.title : std::wstring();
            StartRequest();
        } break;
    case Action::Play: case Action::Queue: case Action::PlayNext: case Action::RetryPlayback:
    {
        auto songs = SelectedSongs(command.rows);
        if (songs.empty()) { SetStatus(L"请先选中歌曲，可使用 Ctrl / Shift 多选。"); break; }
        forgive(songs);
        const bool append = command.action == Action::Queue || command.action == Action::PlayNext;
        m_playback.push_back({std::move(songs), append, command.action == Action::PlayNext});
        break;
    }
    case Action::PlayAll:
    {
        // 「播放全部」不要求先选中：打开专辑/歌单以后可以直接整张播
        auto songs = SelectedSongs({}, true);
        if (songs.empty()) { SetStatus(L"当前列表没有可播放的在线歌曲。"); break; }
        forgive(songs);
        m_playback.push_back({std::move(songs), false});
        break;
    }
    case Action::AddToPlaylist: AddToExistingPlaylist(SelectedSongs(command.rows)); break;
    case Action::SwitchInPlace:
    {
        // 在线页里的单曲/所选就地换源：本地歌单页直接改那一份歌单；搜索结果这类
        // 「还没有归属」的行没法就地改，退回另存模式。
        auto songs = command.rows.empty() ? SelectedSongs({}, true) : SelectedSongs(command.rows);
        const bool local_page = m_state.page == Page::Local;
        SwitchSource(command.value, songs, L"", local_page, local_page);
        break;
    }
    case Action::SwitchTrackInPlace:
    {
        SwitchSource(command.value, command.songs, L"", true, false, command.text);
        break;
    }
    case Action::SwitchPlayerPlaylist:
    {
        // 当前播放列表往往是用户从媒体库打开的导入歌单，换源结果另存为新歌单
        auto& player = CPlayer::GetInstance();
        std::unique_lock<std::timed_mutex> lock(player.GetPlayStatusMutex(), std::try_to_lock);
        if (!lock.owns_lock() || player.m_loading) { ShowTip(L"播放队列正在更新，请稍后换源。"); break; }
        vector<SongInfo> songs = player.GetPlayList();
        const wstring hint = CFilePathHelper(player.GetPlaylistPath()).GetFileNameWithoutExtension();
        lock.unlock();
        SwitchSource(command.value, songs, hint);
        break;
    }
    case Action::SwitchFilePlaylist:
    {
        // 媒体库里右键某份歌单换源：直接读那份文件，不动它，结果另存为新歌单
        if (command.text.empty()) { SetStatus(L"没有指定要换源的歌单。"); break; }
        CPlaylistFile file;
        if (!file.LoadFromFile(command.text) || file.GetPlaylist().empty())
        { SetStatus(L"这份歌单读不出来，无法换源。"); break; }
        SwitchSource(command.value, file.GetPlaylist(), CFilePathHelper(command.text).GetFileNameWithoutExtension());
        break;
    }
    case Action::AddToFavourite:
    {
        // 「我喜欢」是特殊播放列表，不会出现在「加入播放列表」对话框里，所以单独给一个入口；
        // 走的仍然是原生那套流程：写列表、打星标、刷新媒体库。
        auto songs = SelectedSongs(command.rows);
        if (songs.empty()) { SetStatus(L"请先选中歌曲，可使用 Ctrl / Shift 多选。"); break; }
        CMusicPlayerCmdHelper helper(m_owner);
        helper.OnAddToPlaylistCommand([&songs](std::vector<SongInfo>& out) { out = songs; }, ID_ADD_TO_MY_FAVOURITE);
        break;
    }
    case Action::CopyInfo: CopySongInfo(command.rows); break;
    case Action::OpenFileLocation: OpenFileLocation(command.rows); break;
    case Action::Save: SaveLocal(SelectedSongs(command.rows)); break;
    case Action::SaveAsNativePlaylist: SaveLocalAsNativePlaylist(command.rows); break;
    case Action::RemoveLocal: RemoveLocal(command.rows); break;
    case Action::ClearLocal: ClearLocal(); break;
    case Action::Download:
    {
        // 选中了就下载选中的；没选中就整批下载当前列表（歌单页即批量下载整个歌单）
        auto songs = command.rows.empty() ? SelectedSongs({}, true) : SelectedSongs(command.rows);
        if (songs.empty()) { SetStatus(L"当前列表没有可下载的歌曲。"); break; }
        std::wstring directory;
        // 没有选中行时是整批下载当前列表；这时按设置把文件放进以歌单名命名的子文件夹
        if (!GetOnlineDownloadDirectory(m_owner, directory,
            command.rows.empty() ? m_state.list_title : std::wstring())) break;
        size_t count{};
        for (const auto& song : songs)
        {
            if (!CSourceRegistry::IsVirtualPath(song.file_path)) continue;
            Track track; track.virtual_path = song.file_path; track.title = song.GetTitle(); track.artist = song.GetArtist(); track.album = song.GetAlbum();
            COnlineMediaCache::Instance().SaveAudio(track, directory); ++count;
        }
        SetStatus(L"已加入下载队列：" + to_wstring(count) + L" 首"); break;
    }
    case Action::ToggleAutoSwitch:
    {
        auto preferences = COnlineSettings::Instance().Get();
        preferences.auto_switch_source = !m_state.auto_switch_source;
        if (!COnlineSettings::Instance().Save(preferences)) { SetNotice(L"自动换源设置保存失败，请检查磁盘空间"); break; }
        m_state.auto_switch_source = preferences.auto_switch_source;
        if (!m_state.auto_switch_source) FinishAutoSwitch(ProgressResult::Cancelled, L"自动换源已关闭");
        else { m_auto_switch_stopped = false; m_auto_switch_tried.clear(); m_auto_switch_started = 0; }
        SetNotice(m_state.auto_switch_source ? L"无法播放时将自动尝试其他音源" : L"已关闭自动换源");
        Publish();
        break;
    }
    case Action::ToggleCache:
        COnlineMediaCache::Instance().SetEnabled(!COnlineMediaCache::Instance().Enabled());
        [[fallthrough]];
    case Action::CacheInfo:
        m_cache_view = true;
        m_state.detail_visible = true; m_state.detail = COnlineMediaCache::Instance().Describe();
        SetStatus(L"下载与缓存"); break;
    case Action::ClearCache:
        m_cache_view = true;
        COnlineMediaCache::Instance().Clear();
        m_state.detail_visible = true; m_state.detail = COnlineMediaCache::Instance().Describe(); Publish(); break;
    case Action::Import: ImportFile(); break;
    case Action::ImportExternal: ImportExternal(); break;
    case Action::Export: ExportFile(command.rows); break;
    case Action::Lyrics:
    {
        auto songs = SelectedSongs(command.rows);
        if (songs.empty() || !CSourceRegistry::IsVirtualPath(songs.front().file_path))
        { SetStatus(L"请先选择一首在线歌曲。"); break; }
        auto* source = CSourceRegistry::Instance().FindByPath(songs.front().file_path);
        const auto& sources = CSourceRegistry::Instance().GetAll();
        auto it = find(sources.begin(), sources.end(), source);
        if (it != sources.end()) m_state.source = static_cast<int>(it - sources.begin());
        StartRequest(false, false, songs.front().file_path); break;
    }
    case Action::Back:
        m_cache_view = false;
        if (m_state.detail_visible && m_state.page != Page::Account)
        { m_state.detail_visible = false; SetStatus(L"已返回歌曲列表"); }
        else SelectPage(); break;
    case Action::Refresh:
        if (m_cache_view) { m_state.detail = COnlineMediaCache::Instance().Describe(); Publish(); break; }
        if (m_state.page == Page::Local || m_state.page == Page::Account) SelectPage();
        else { m_state.request.page = 1; StartRequest(); } break;
    case Action::More:
        if (!m_task && m_state.has_more) { ++m_state.request.page; StartRequest(true); --m_state.request.page; } break;
    case Action::OpenPlaylist:
        if (!CurrentSource() || !IsServiceId(m_state.query))
        { SetStatus(L"在搜索框输入歌单编号；B源歌单使用 编号_来源（4、5 或 13）。"); break; }
        // 按编号打开时没有歌单名可用，整批下载就不建子文件夹
        m_state.list_title.clear();
        m_state.page = Page::Cloud; m_state.request = { BrowseKind::PlaylistTracks, m_state.query, 1 }; StartRequest(); break;
    case Action::ImportAll:
        if (m_state.request.kind == BrowseKind::PlaylistTracks && !m_task) StartRequest(false, true); break;
    case Action::Login:
    {
        CancelRequest(); ResetLogin();
        if (CurrentSource()) m_login_source = CloneSource(CurrentSource()->GetScheme() + L"://login");
        if (m_login_source) { m_state.page = Page::Account; m_state.detail_visible = true; StartRequest(false, false, L"", 1); }
        break;
    }
    case Action::SignIn:
        if (!dynamic_cast<kugou::CKugouSource*>(CurrentSource())) { SetStatus(L"B源暂无签到"); break; }
        m_cache_view = false; COnlineDailyRewards::Instance().Request();
        CSourceRegistry::Instance().ForgetAllPlayUrls();
        m_state.page = Page::Account; ShowAccount(); break;
    case Action::ToggleSignIn:
        if (!dynamic_cast<kugou::CKugouSource*>(CurrentSource())) { SetStatus(L"自动签到当前支持K源。"); break; }
        m_cache_view = false;
        COnlineDailyRewards::Instance().SetEnabled(!COnlineDailyRewards::Instance().Enabled());
        m_state.page = Page::Account; ShowAccount(); break;
    case Action::AdReward:
        if (!dynamic_cast<bodian::CBodianSource*>(CurrentSource())) { SetStatus(L"看广告领会员目前用于B源。"); break; }
        m_cache_view = false; CBodianAdRewards::Instance().Request();
        // 领取后权益变了，刚刚那些「需要会员」的解析结论要丢掉，否则同一首还是播不了
        CSourceRegistry::Instance().ForgetAllPlayUrls();
        m_state.page = Page::Account; ShowAccount(); break;
    case Action::ToggleAdReward:
        if (!dynamic_cast<bodian::CBodianSource*>(CurrentSource())) { SetStatus(L"自动观看广告领会员目前用于B源。"); break; }
        m_cache_view = false;
        CBodianAdRewards::Instance().SetEnabled(!CBodianAdRewards::Instance().Enabled());
        m_state.page = Page::Account; ShowAccount(); break;
    case Action::ImportAccount:
    {
        if (!dynamic_cast<bodian::CBodianSource*>(CurrentSource())) { SetStatus(L"此入口用于导入B源登录文件。"); break; }
        CFileDialog dialog(TRUE, L"json", nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, L"B源登录文件|*.json||", m_owner);
        if (dialog.DoModal() != IDOK) break;
        CFileStatus info;
        if (!CFile::GetStatus(dialog.GetPathName(), info) || info.m_size > 65536) { SetStatus(L"登录文件无法读取或超过 64 KB。"); break; }
        try
        {
            std::ifstream input(std::filesystem::path(dialog.GetPathName().GetString()), std::ios::binary);
            auto data = nlohmann::json::parse(input);
            bodian::CBodianSource::Account account{JsonText(data, "uid"), JsonText(data, "token")};
            if (!account.IsLoggedIn()) { SetStatus(L"登录文件需要有效的 uid 和 token 字段。"); break; }
            CancelRequest(); ResetLogin();
            auto source = make_shared<bodian::CBodianSource>(*dynamic_cast<bodian::CBodianSource*>(CurrentSource()));
            source->SetAccount(account); m_login_source = source; m_state.page = Page::Account;
            StartRequest(false, false, L"", 4);
        }
        catch (const std::exception&) { SetStatus(L"登录文件格式无效，未更改原账号。"); }
        break;
    }
    case Action::Logout:
    {
        CancelRequest(); ResetLogin();
        m_profiles.erase(m_state.source);
        // 退出账号后权限变了，解析结论不能再用
        CSourceRegistry::Instance().ForgetAllPlayUrls();
        auto* kg = dynamic_cast<kugou::CKugouSource*>(CurrentSource());
        if (kg) { COnlineDailyRewards::Instance().CancelPending(); kg->Logout(); kg->SaveIdentity(theApp.m_config_dir); }
        if (auto* bd = dynamic_cast<bodian::CBodianSource*>(CurrentSource()))
        {
            bd->Logout();
            if (!bd->SaveIdentity(theApp.m_config_dir)) { ShowAccount(); SetStatus(L"已退出当前会话，但本机登录信息删除失败。"); break; }
        }
        ShowAccount(); break;
    }
    }
}
