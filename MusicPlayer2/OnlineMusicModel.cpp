#include "stdafx.h"
#include "MusicPlayer2.h"
#include "OnlineMusicModel.h"
#include "OnlineJson.h"
#include "OnlineMediaCache.h"
#include "OnlineSettingsUI.h"
#include "OnlineDailyRewards.h"
#include "KugouSource.h"
#include "BodianSource.h"
#include "Playlist.h"
#include "FilePathHelper.h"
#include "qrcodegen/qrcodegen.hpp"
#include <thread>
#include <fstream>
#include <filesystem>

using namespace std;
using namespace online;

COnlineMusicModel::COnlineMusicModel() { Publish(); }
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
    COnlineDailyRewards::Instance().Configure(theApp.m_config_dir);
    CPlaylistFile file;
    file.LoadFromFile(LocalPath());
    m_local = file.GetPlaylist();
    owner->PostMessage(WM_PLAY_ONLINE_SONG);
}
void COnlineMusicModel::Publish(bool rows_changed, bool reset_selection)
{
    m_state.busy = m_task != nullptr;
    if (rows_changed)
    {
        if (reset_selection) ++m_state.revision;
        m_state.songs.clear();
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
            m_state.unplayable.push_back(!song.file_path.empty() && m_unplayable.count(song.file_path) != 0);
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
void COnlineMusicModel::SetStatus(const wstring& status) { m_state.status = status; Publish(); }
void COnlineMusicModel::SetPlaybackError(const wstring& error)
{
    if (m_state.playback_error == error) return;
    m_state.playback_error = error; Publish();
}
void COnlineMusicModel::MarkUnplayable(const wstring& path)
{
    // 只记在线曲目，且只记一次；重画列表但保留用户当前选中行
    if (path.empty() || !CSourceRegistry::IsVirtualPath(path)) return;
    if (!m_unplayable.insert(path).second) return;
    Publish(true, false);
}
void COnlineMusicModel::CancelRequest() { if (m_task) m_task->cancelled = true; m_task.reset(); }
void COnlineMusicModel::ResetLogin()
{
    m_login_poll_at = 0; m_login_source.reset(); m_state.qr_pixels.clear(); m_state.qr_size = 0;
}
void COnlineMusicModel::Suspend()
{
    const bool browsing = m_task && m_task->login_action == 0 && m_task->lyric_path.empty() && !m_task->import_all;
    m_interrupted = m_interrupted || browsing;
    if (browsing) m_state.request.page = 1;
    CancelRequest(); ResetLogin(); Publish();
}
void COnlineMusicModel::Shutdown()
{
    COnlineMediaCache::Instance().Shutdown();
    COnlineDailyRewards::Instance().Shutdown();
    Suspend();
    if (m_worker)
    {
        { lock_guard<mutex> lock(m_worker->mutex); m_worker->stopping = true; m_worker->pending.reset(); }
        m_worker->ready.notify_one(); m_worker.reset();
    }
    m_owner = nullptr;
    m_message_window = nullptr;
}
void COnlineMusicModel::SelectPage()
{
    m_cache_view = false;
    m_interrupted = false;
    CancelRequest(); ResetLogin();
    m_state.items.clear(); m_state.detail.clear(); m_state.detail_visible = false; m_state.has_more = false; m_state.request = {};
    if (m_state.page == Page::Local) { ShowLocal(); return; }
    if (m_state.page == Page::Account) { ShowAccount(true); return; }
    if (m_state.page == Page::Search)
    {
        m_state.request.kind = BrowseKind::Search;
        m_state.status = L"输入歌名或歌手，按 Enter 搜索；双击歌曲播放。";
        Publish(true); return;
    }
    m_state.request.kind = m_state.page == Page::Discover ? BrowseKind::Hot
        : m_state.page == Page::Recommend ? BrowseKind::Recommend
        : m_state.page == Page::Charts ? BrowseKind::Charts : BrowseKind::Playlists;
    StartRequest();
}
void COnlineMusicModel::StartRequest(bool append, bool import_all, const wstring& lyric_path, int login_action, bool profile_request)
{
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
    m_task = task;
    bool clear_rows = !append && !import_all && lyric_path.empty() && login_action == 0 && !profile_request;
    if (clear_rows) { m_state.items.clear(); m_state.has_more = false; m_state.detail_visible = false; }
    if (login_action != 2) m_state.status = import_all ? L"正在读取完整歌单…" : L"正在加载…";
    Publish(clear_rows);
    try
    {
        if (!m_worker)
        {
            m_worker = make_shared<Worker>();
            auto worker = m_worker;
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
                        else if (task->import_all)
                        {
                            for (int page = 1; page <= 500 && !task->cancelled; ++page)
                            {
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
        { lock_guard<mutex> lock(m_worker->mutex); m_worker->pending = task; }
        m_worker->ready.notify_one();
    }
    catch (const exception&) { m_task.reset(); SetStatus(L"无法启动网络任务，请稍后重试。"); }
}
void COnlineMusicModel::Poll(bool visible)
{
    COnlineDailyRewards::Instance().Tick();
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
    if (!m_task || !m_task->done) return;
    auto task = std::move(m_task);
    if (task->profile_request)
    {
        if (task->success) m_profiles[m_state.source] = task->profile;
        ShowAccount();
        SetStatus(task->success ? L"" : L"账号信息读取失败"); return;
    }
    if (!task->success) { SetStatus(task->error.empty() ? L"没有可用结果，请刷新重试。" : task->error); return; }
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
                m_state.status = task->source->GetScheme() == L"bodian" ? L"使用波点音乐 App 扫码，并在手机上确认登录。" : L"使用酷狗 App 扫码，并在手机上确认登录。";
                m_login_poll_at = GetTickCount64() + 2000;
            }
            catch (const exception&) { ResetLogin(); m_state.status = L"二维码生成失败，请重试。"; }
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
                ResetLogin(); ShowAccount(true);
                if (!persisted) m_state.status = L"已登录，但本机登录信息保存失败，重启后需要重新登录。";
            }
            else if (status == kugou::CKugouSource::QrStatus::Expired)
            { ResetLogin(); m_state.status = L"二维码已过期，请重新获取。"; }
            else
            {
                m_login_poll_at = GetTickCount64() + 2000;
                if (status == kugou::CKugouSource::QrStatus::Scanned) m_state.status = L"已扫码，请在手机上确认。";
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
        SaveLocal(songs); return;
    }
    if (task->append) m_state.items.insert(m_state.items.end(), task->result.items.begin(), task->result.items.end());
    else m_state.items = std::move(task->result.items);
    m_state.request = task->request; m_state.has_more = task->result.has_more;
    m_state.detail_visible = false;
    m_state.status = m_state.items.empty() ? L"暂无结果，可以换个关键词。"
        : L"已加载 " + to_wstring(m_state.items.size()) + L" 项";
    Publish(true, !task->append);
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
    Publish(true);
}
void COnlineMusicModel::ShowAccount(bool refresh)
{
    m_cache_view = false;
    m_state.detail_visible = true;
    if (auto* bd = dynamic_cast<bodian::CBodianSource*>(CurrentSource()))
        m_state.detail = wstring(L"波点音乐 · ") + (bd->IsLoggedIn() ? L"已登录" : L"未登录");
    else
    {
        auto* kg = dynamic_cast<kugou::CKugouSource*>(CurrentSource());
        m_state.detail = kg && kg->IsLoggedIn()
            ? L"酷狗概念版 · 已登录"
            : L"酷狗概念版 · 未登录";
        m_state.detail += L"\n\n" + COnlineDailyRewards::Instance().Describe();
    }
    m_state.status.clear(); Publish(true);
    auto* source = CurrentSource();
    auto* kg = dynamic_cast<kugou::CKugouSource*>(source);
    auto* bd = dynamic_cast<bodian::CBodianSource*>(source);
    if ((kg && kg->IsLoggedIn()) || (bd && bd->IsLoggedIn()))
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
void COnlineMusicModel::SaveLocal(const vector<SongInfo>& songs)
{
    if (songs.empty()) { SetStatus(L"请先选中歌曲。"); return; }
    CPlaylistFile file; file.AddSongsToPlaylist(m_local);
    int added = file.AddSongsToPlaylist(songs);
    if (!file.SaveToFile(LocalPath())) { SetStatus(L"保存失败，原歌单已保留。请检查目录写入权限。"); return; }
    m_local = file.GetPlaylist();
    if (m_state.page == Page::Local) ShowLocal();
    SetStatus(L"已保存 " + to_wstring(added) + L" 首，跳过重复 " + to_wstring(songs.size() - added) + L" 首。");
}
void COnlineMusicModel::ImportFile()
{
    CFileDialog dialog(TRUE, nullptr, nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
        L"歌单文件|*.m3u8;*.m3u;*.playlist;*.btplaylist;*.json;*.wpl;*.ttpl||", m_owner);
    if (dialog.DoModal() != IDOK) return;
    CFileStatus status;
    if (!CFile::GetStatus(dialog.GetPathName(), status) || status.m_size > 16 * 1024 * 1024)
    { SetStatus(L"无法读取文件，或文件超过 16 MB。"); return; }
    CPlaylistFile file;
    if (!file.LoadFromFile(dialog.GetPathName().GetString()) || file.GetPlaylist().empty())
    { SetStatus(L"没有有效歌曲。可导入 M3U8、原生播放列表或 BoTapMusic JSON 歌单。"); return; }
    m_state.page = Page::Local; SelectPage(); SaveLocal(file.GetPlaylist());
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
    SetStatus(CPlaylistFile::SavePlaylistToFile(songs, path, type)
        ? L"已导出 " + to_wstring(songs.size()) + L" 首歌曲。"
        : L"导出失败，原文件未被替换。请检查磁盘空间和写入权限。");
}
void COnlineMusicModel::Execute(const Command& command)
{
    bool row_action = command.action == Action::Open || command.action == Action::Play || command.action == Action::Queue
        || command.action == Action::Save || command.action == Action::Lyrics || command.action == Action::Export || command.action == Action::Download;
    if (row_action && command.revision != m_state.revision) { SetStatus(L"列表已更新，请重新选择歌曲。"); return; }
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
    case Action::Search:
        if (m_state.query.empty()) { SetStatus(L"请先输入歌名或歌手。"); break; }
        m_state.page = Page::Search; SelectPage(); m_state.request = { BrowseKind::Search, m_state.query, 1 }; StartRequest(); break;
    case Action::SearchPlaylists:
        if (!dynamic_cast<bodian::CBodianSource*>(CurrentSource())) { SetStatus(L"请切换到波点音乐搜索公开歌单。"); break; }
        if (m_state.query.empty()) { SetStatus(L"请在搜索框输入歌单关键词。"); break; }
        m_state.page = Page::Cloud; m_state.request = {BrowseKind::PlaylistSearch, m_state.query, 1}; StartRequest(); break;
    case Action::Open:
        if (m_task || command.rows.empty()) break;
        if (command.rows.front() >= 0 && command.rows.front() < static_cast<int>(m_state.items.size()))
        {
            auto item = m_state.items[command.rows.front()];
            if (item.type == BrowseItem::Type::Song)
            { auto songs = SelectedSongs(command.rows); if (!songs.empty()) m_playback.push_back({std::move(songs), false}); break; }
            m_state.request = { item.type == BrowseItem::Type::Keyword ? BrowseKind::Search
                : item.type == BrowseItem::Type::Chart ? BrowseKind::ChartTracks : BrowseKind::PlaylistTracks, item.id, 1 };
            if (item.type == BrowseItem::Type::Keyword)
            { m_state.page = Page::Search; m_state.query = item.title; ++m_state.query_revision; }
            StartRequest();
        } break;
    case Action::Play: case Action::Queue:
    {
        auto songs = SelectedSongs(command.rows);
        if (songs.empty()) SetStatus(L"请先选中歌曲，可使用 Ctrl / Shift 多选。");
        else m_playback.push_back({std::move(songs), command.action == Action::Queue});
        break;
    }
    case Action::Save: SaveLocal(SelectedSongs(command.rows)); break;
    case Action::Download:
    {
        // 选中了就下载选中的；没选中就整批下载当前列表（歌单页即批量下载整个歌单）
        auto songs = command.rows.empty() ? SelectedSongs({}, true) : SelectedSongs(command.rows);
        if (songs.empty()) { SetStatus(L"当前列表没有可下载的歌曲。"); break; }
        std::wstring directory;
        if (!GetOnlineDownloadDirectory(m_owner, directory)) break;
        size_t count{};
        for (const auto& song : songs)
        {
            if (!CSourceRegistry::IsVirtualPath(song.file_path)) continue;
            Track track; track.virtual_path = song.file_path; track.title = song.GetTitle(); track.artist = song.GetArtist(); track.album = song.GetAlbum();
            COnlineMediaCache::Instance().SaveAudio(track, directory); ++count;
        }
        SetStatus(L"已加入下载队列：" + to_wstring(count) + L" 首"); break;
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
        { SetStatus(L"在搜索框输入歌单编号；波点歌单使用 编号_来源（4、5 或 13）。"); break; }
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
        if (!dynamic_cast<kugou::CKugouSource*>(CurrentSource())) { SetStatus(L"波点暂无签到"); break; }
        m_cache_view = false; COnlineDailyRewards::Instance().Request(); m_state.page = Page::Account; ShowAccount(); break;
    case Action::ToggleSignIn:
        if (!dynamic_cast<kugou::CKugouSource*>(CurrentSource())) { SetStatus(L"自动签到当前支持酷狗概念版。"); break; }
        m_cache_view = false;
        COnlineDailyRewards::Instance().SetEnabled(!COnlineDailyRewards::Instance().Enabled());
        m_state.page = Page::Account; ShowAccount(); break;
    case Action::ImportAccount:
    {
        if (!dynamic_cast<bodian::CBodianSource*>(CurrentSource())) { SetStatus(L"此入口用于导入波点音乐登录文件。"); break; }
        CFileDialog dialog(TRUE, L"json", nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, L"波点登录文件|*.json||", m_owner);
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
