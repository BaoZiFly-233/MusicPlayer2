#include "stdafx.h"
#include "OnlineMusic.h"
#include "UserUi.h"
#include "Player.h"
#include "UiSearchBox.h"
#include "OnlineMediaCache.h"
#include "OnlineDailyRewards.h"

using Model = COnlineMusicModel;
using namespace UiElement;

void OnlineMusicList::SetSnapshot(std::shared_ptr<const Model::State> state)
{
    if (!m_state || m_state->revision != state->revision)
    {
        SelectNone();
        scroll_offset = 0;
    }
    m_state = std::move(state);
    // 列表里混了本地文件和在线曲目（或两个平台混在一起）时，标题列要标出来源；
    // 只含单一来源时不标，免得满屏都是重复标签。判断一次就够，不必每行都算。
    int origin{ -1 };
    m_mixed_origins = false;
    for (const auto& song : m_state->songs)
    {
        if (song.file_path.empty()) continue;
        const int current = online::CSourceRegistry::OriginIndex(song.file_path);
        if (origin < 0) origin = current;
        else if (origin != current) { m_mixed_origins = true; break; }
    }
}
// 一行歌曲的来源短名（本地 / K源 / B源）。来源只看地址本身。
std::wstring OnlineMusicList::RowOrigin(int row) const
{
    // 专辑、歌单、榜单这类行没有音频地址，不属于任何来源，不能给它们贴上「本地」
    if (!m_state || row < 0 || row >= static_cast<int>(m_state->songs.size())
        || m_state->songs[row].file_path.empty()) return {};
    return online::CSourceRegistry::OriginLabel(m_state->songs[row].file_path);
}
IconMgr::IconType OnlineMusicList::GetIcon(int row)
{
    // 本地文件用音符、在线曲目用地球：来源最粗的一档始终可见，窄屏也挤不掉
    if (!m_state || row < 0 || row >= static_cast<int>(m_state->songs.size())
        || m_state->songs[row].file_path.empty()) return IconMgr::IT_NO_ICON;
    return online::CSourceRegistry::IsVirtualPath(m_state->songs[row].file_path)
        ? IconMgr::IT_Online : IconMgr::IT_Music;
}
bool OnlineMusicList::IsSavedRow(int row) const
{
    return m_state && row >= 0 && row < static_cast<int>(m_state->saved_local.size()) && m_state->saved_local[row];
}
// 悬停按钮只给歌曲行；专辑、歌单、榜单行点开就是了，没有「播放这一行」可言
int OnlineMusicList::GetHoverButtonCount(int row)
{
    return IsSongRow(row) && row < static_cast<int>(m_state->songs.size()) && !m_state->songs[row].file_path.empty() ? HB_MAX : 0;
}
IconMgr::IconType OnlineMusicList::GetHoverButtonIcon(int index, int row)
{
    switch (index)
    {
    case HB_PLAY: return IconMgr::IT_Play;
    case HB_PLAY_NEXT: return IconMgr::IT_Play_As_Next;
    case HB_QUEUE: return IconMgr::IT_Add;
    // 已收藏显示实心心（点击是取消），未收藏显示空心心（点击是收藏），与原生媒体库一致
    case HB_SAVE: return IsSavedRow(row) ? IconMgr::IT_Favorite_Off : IconMgr::IT_Favorite_On;
    }
    return IconMgr::IT_NO_ICON;
}
std::wstring OnlineMusicList::GetHoverButtonTooltip(int index, int row)
{
    switch (index)
    {
    // 失败过的行按钮写成「重试播放」，动作也必须跟着走重试，别让按钮和菜单两种说法
    case HB_PLAY: return IsItemEnabled(row) ? L"播放" : L"重试播放（上次播放失败）";
    case HB_PLAY_NEXT: return L"下一首播放";
    case HB_QUEUE: return L"加入播放队列";
    case HB_SAVE: return IsSavedRow(row) ? L"从在线本地歌单移除" : L"保存到在线本地歌单";
    }
    return {};
}
void OnlineMusicList::OnHoverButtonClicked(int btn_index, int row)
{
    switch (btn_index)
    {
    case HB_PLAY: DispatchRow(IsItemEnabled(row) ? Model::Action::Play : Model::Action::RetryPlayback, row); break;
    case HB_PLAY_NEXT: DispatchRow(Model::Action::PlayNext, row); break;
    case HB_QUEUE: DispatchRow(Model::Action::Queue, row); break;
    case HB_SAVE: DispatchRow(IsSavedRow(row) ? Model::Action::RemoveLocal : Model::Action::Save, row); break;
    }
}
// 本地歌单页每一行都在歌单里，红心就没有信息量了，只在其它页面标出已收藏的行
int OnlineMusicList::GetUnHoverIconCount(int row)
{
    return IsSavedRow(row) && m_state->page != Model::Page::Local ? 1 : 0;
}
IconMgr::IconType OnlineMusicList::GetUnHoverIcon(int index, int row)
{
    return index == 0 && IsSavedRow(row) ? IconMgr::IT_Favorite_Off : IconMgr::IT_NO_ICON;
}
std::wstring OnlineMusicList::GetToolTipText(int row)
{
    if (!m_state || row < 0 || row >= static_cast<int>(m_state->items.size())) return {};
    const auto& item = m_state->items[row];
    std::wstring tip = item.title;
    if (item.type != online::BrowseItem::Type::Song)
    {
        if (!item.subtitle.empty()) tip += L"\n" + item.subtitle;
        tip += item.type == online::BrowseItem::Type::Keyword ? L"\n双击搜索这个关键词"
            : item.type == online::BrowseItem::Type::Album ? L"\n双击列出这张专辑的曲目" : L"\n双击打开";
        return tip;
    }
    if (!item.track.artist.empty()) tip += L"\n歌手：" + item.track.artist;
    if (!item.track.album.empty()) tip += L"\n专辑：" + item.track.album;
    // 状态列在中窄屏会收掉，提示里始终带上来源和标记
    const std::wstring origin = RowOrigin(row);
    if (!origin.empty() || !item.badge.empty())
        tip += L"\n来源：" + origin + (item.badge.empty() ? std::wstring() : L" · " + item.badge);
    if (IsSavedRow(row) && m_state->page != Model::Page::Local) tip += L"\n已在在线本地歌单";
    if (row < static_cast<int>(m_state->unplayable.size()) && m_state->unplayable[row])
    {
        // 失败原因按地址从注册表取，命中失败备忘的曲目也能说清是为什么
        const std::wstring reason = online::CSourceRegistry::Instance().PlayError(m_state->songs[row].file_path);
        tip += L"\n上次播放失败" + (reason.empty() ? std::wstring(L"，双击重试") : L"：" + reason + L"\n双击重试");
    }
    return tip;
}
int OnlineMusicList::GetRowCount() { return m_state ? static_cast<int>(m_state->items.size()) : 0; }
// 列数随窗口宽度变化：
//   6 列（宽）序号 | 标题 | 艺术家 | 专辑 | 状态 | 时长
//   5 列（中）序号 | 标题 | 艺术家 | 专辑 | 时长
//   4 列（窄）序号 | 标题 | 艺术家 · 专辑 | 时长
int OnlineMusicList::GetColumnCount()
{
    if (rect.Width() >= ui->DPI(620)) return 6;
    return rect.Width() >= ui->DPI(480) ? 5 : 4;
}
int OnlineMusicList::GetColumnWidth(int col, int width)
{
    const int count = GetColumnCount();
    const int number = ui->DPI(32), duration = ui->DPI(48), badge = ui->DPI(58);
    const int rest = (std::max)(0, width - number - duration - (count >= 6 ? badge : 0));
    if (col == 0) return number;
    if (col == count - 1) return duration;
    if (count >= 6)
    {
        if (col == 4) return badge;
        if (col == 1) return rest * 44 / 100;
        if (col == 2) return rest * 28 / 100;
        return rest * 28 / 100;
    }
    if (count == 5) return col == 1 ? rest * 45 / 100 : col == 2 ? rest * 28 / 100 : rest * 27 / 100;
    return col == 1 ? rest * 65 / 100 : rest * 35 / 100;
}
std::wstring OnlineMusicList::GetItemText(int row, int col)
{
    if (!m_state || row < 0 || row >= GetRowCount()) return {};
    const auto& item = m_state->items[row];
    const int count = GetColumnCount();
    if (col == 0) return std::to_wstring(row + 1);
    if (col == 1)
    {
        // 混着多个来源时标题带上来源前缀，一眼看清哪首是本地、哪首是哪个平台
        if (!m_mixed_origins) return item.title;
        const std::wstring origin = RowOrigin(row);
        return origin.empty() ? item.title : L"[" + origin + L"] " + item.title;
    }
    // 窄屏放不下专辑列，把艺术家与专辑并到同一列
    if (col == 2)
    {
        if (count > 4 || item.track.album.empty()) return item.subtitle;
        return item.subtitle.empty() ? item.track.album : item.subtitle + L" · " + item.track.album;
    }
    if (col == 3 && count >= 5) return item.track.album;
    if (col == 4 && count >= 6)
    {
        // 状态列 = 来源 · 权限/音质标记。来源由界面从地址得出，标记仍由音源自己填。
        const std::wstring origin = RowOrigin(row);
        if (item.badge.empty()) return origin;
        return origin.empty() ? item.badge : origin + L" · " + item.badge;
    }
    if (col != count - 1) return {};
    if (item.type != online::BrowseItem::Type::Song) return L"›";
    if (item.track.duration_ms <= 0) return {};
    int seconds = item.track.duration_ms / 1000;
    CString text; text.Format(L"%d:%02d", seconds / 60, seconds % 60); return text.GetString();
}
std::wstring OnlineMusicList::GetEmptyString()
{
    return m_state ? (m_state->busy ? L"正在加载…" : m_state->status) : L"在线音乐";
}
bool OnlineMusicList::IsHighlightRow(int row)
{
    return m_state && row >= 0 && row < static_cast<int>(m_state->songs.size())
        && !m_state->songs[row].file_path.empty()
        && m_state->songs[row].file_path == CPlayer::GetInstance().GetSafeCurrentSongInfo().file_path;
}

bool OnlineMusicList::IsItemEnabled(int row)
{
    // 播放失败过的曲目用灰色显示，让用户一眼看出哪些听不了
    if (!m_state || row < 0 || row >= static_cast<int>(m_state->unplayable.size())) return true;
    return !m_state->unplayable[row];
}

void OnlineMusicList::Dispatch(Model::Action action, int value)
{
    if (!m_state) return;
    Model::Command command{action}; command.value = value;
    command.revision = m_state->revision; GetItemsSelected(command.rows);
    Model::Instance().Post(std::move(command));
}
void OnlineMusicList::DispatchRow(Model::Action action, int row)
{
    if (!m_state || row < 0) return;
    Model::Command command{action};
    command.revision = m_state->revision; command.rows = {row};
    Model::Instance().Post(std::move(command));
}
bool OnlineMusicList::MoveSelection(int delta, bool page, bool to_edge)
{
    const int count = GetRowCount();
    if (count <= 0) return false;
    int current = GetItemSelected();
    // 多选时以最后点的那一行为基准；没有选中就从头（或尾）开始
    std::vector<int> rows; GetItemsSelected(rows);
    if (!rows.empty()) current = delta > 0 ? rows.back() : rows.front();
    int target;
    if (to_edge) target = delta > 0 ? count - 1 : 0;
    else
    {
        int step = delta;
        if (page)
        {
            const int visible = (std::max)(1, rect.Height() / (std::max)(1, ItemHeight()) - 1);
            step = delta > 0 ? visible : -visible;
        }
        target = current < 0 ? (delta > 0 ? 0 : count - 1) : current + step;
    }
    target = (std::max)(0, (std::min)(count - 1, target));
    SetItemSelected(target);
    return true;
}
bool OnlineMusicList::LocateToCurrent()
{
    for (int row = 0; row < GetRowCount(); ++row)
        if (IsHighlightRow(row)) { SetItemSelected(row); return true; }
    return false;
}
void OnlineMusicList::OnDoubleClicked() { Dispatch(Model::Action::Open); }
bool OnlineMusicList::HasSongSelected() const
{
    if (!m_state) return false;
    std::vector<int> rows; GetItemsSelected(rows);
    for (int row : rows)
        if (row >= 0 && row < static_cast<int>(m_state->songs.size()) && !m_state->songs[row].file_path.empty()) return true;
    return false;
}
bool OnlineMusicList::HasLocalFileSelected() const
{
    if (!m_state) return false;
    std::vector<int> rows; GetItemsSelected(rows);
    for (int row : rows)
        if (row >= 0 && row < static_cast<int>(m_state->songs.size())
            && CCommon::FileExist(m_state->songs[row].file_path)) return true;
    return false;
}
bool OnlineMusicList::IsSongRow(int row) const
{
    return m_state && row >= 0 && row < static_cast<int>(m_state->items.size())
        && m_state->items[row].type == online::BrowseItem::Type::Song;
}

// 右键和「更多」用的是同一份菜单：同一件事只有一个入口名，可用条件也只有一处判断，
// 不会再出现「更多」里有的操作右键找不到、或者菜单项点了没反应的情况。
void OnlineMusicList::ShowStandardMenu(bool full, bool row_is_song)
{
    const auto state = m_state;
    if (!state) return;
    const bool songs = HasSongSelected();
    const bool local_file = HasLocalFileSelected();
    const bool local_page = state->page == Model::Page::Local;
    const bool online_song = songs && !local_page;
    // 选中的行里有没有播放失败过的、有没有已经收藏过的：决定「重试播放」和收藏项的叫法
    std::vector<int> selected_rows; GetItemsSelected(selected_rows);
    bool failed_selected{}, saved_selected{}, unsaved_selected{};
    for (int row : selected_rows)
    {
        if (row < 0 || row >= static_cast<int>(state->songs.size()) || state->songs[row].file_path.empty()) continue;
        if (row < static_cast<int>(state->unplayable.size()) && state->unplayable[row]) failed_selected = true;
        if (IsSavedRow(row)) saved_selected = true; else unsaved_selected = true;
    }
    // 行操作只看「有没有选中歌曲」：正在加载更多时选中行仍然有效（追加分页不改列表版本），
    // 真换了一批结果时模型会回「列表已更新，请重新选择」，不必在这里一律置灰。
    const UINT song_flag = !songs ? MF_GRAYED : MF_STRING;
    const UINT online_flag = !online_song ? MF_GRAYED : MF_STRING;

    // 音源与时长之外的信息都从这里取，菜单只负责把动作摆出来
    enum : UINT
    {
        M_PLAY = 1, M_PLAY_NEXT, M_QUEUE, M_ADD_PLAYLIST, M_ADD_FAVOURITE, M_SAVE_LOCAL, M_REMOVE_LOCAL, M_SAVE_NATIVE,
        M_DOWNLOAD, M_EXPORT, M_OPEN_FILE, M_LYRICS, M_COPY, M_OPEN_ROW, M_RETRY,
        M_OPEN_PLAYLIST, M_IMPORT_ALL, M_SEARCH_PLAYLISTS, M_IMPORT_ACCOUNT,
        M_CACHE_INFO, M_TOGGLE_CACHE, M_CLEAR_CACHE,
        M_SIGN_IN, M_TOGGLE_SIGN_IN, M_AD_REWARD, M_TOGGLE_AD_REWARD,
        M_IMPORT_FILE, M_IMPORT_LINK, M_AUTO_SWITCH,
        M_PLAY_ALL, M_SAVE_LIST,
        // 换源的子菜单按音源下标编号，两个基址之间要留够间距：
        // 原来 M_SWITCH_PLAYER_BASE=120 落在 [100, 100+音源数) 区间里，
        // 音源一多就会被当成「就地换源」派发出去。
        M_SWITCH_BASE = 100, M_SWITCH_PLAYER_BASE = 200, M_SWITCH_PLAYER_NOOP_UNAVAILABLE = 199
    };
    const std::pair<UINT, Model::Action> commands[] = {
        {M_PLAY, Model::Action::Play}, {M_PLAY_NEXT, Model::Action::PlayNext}, {M_QUEUE, Model::Action::Queue},
        {M_ADD_PLAYLIST, Model::Action::AddToPlaylist}, {M_ADD_FAVOURITE, Model::Action::AddToFavourite},
        {M_SAVE_LOCAL, Model::Action::Save},
        {M_REMOVE_LOCAL, Model::Action::RemoveLocal}, {M_SAVE_NATIVE, Model::Action::SaveAsNativePlaylist},
        {M_DOWNLOAD, Model::Action::Download}, {M_EXPORT, Model::Action::Export},
        {M_OPEN_FILE, Model::Action::OpenFileLocation}, {M_LYRICS, Model::Action::Lyrics},
        {M_COPY, Model::Action::CopyInfo}, {M_OPEN_ROW, Model::Action::Open}, {M_RETRY, Model::Action::RetryPlayback},
        {M_OPEN_PLAYLIST, Model::Action::OpenPlaylist}, {M_IMPORT_ALL, Model::Action::ImportAll},
        {M_SEARCH_PLAYLISTS, Model::Action::SearchPlaylists}, {M_IMPORT_ACCOUNT, Model::Action::ImportAccount},
        {M_CACHE_INFO, Model::Action::CacheInfo}, {M_TOGGLE_CACHE, Model::Action::ToggleCache},
        {M_CLEAR_CACHE, Model::Action::ClearCache},
        {M_AUTO_SWITCH, Model::Action::ToggleAutoSwitch},
        {M_SIGN_IN, Model::Action::SignIn}, {M_TOGGLE_SIGN_IN, Model::Action::ToggleSignIn},
        {M_AD_REWARD, Model::Action::AdReward}, {M_TOGGLE_AD_REWARD, Model::Action::ToggleAdReward},
        {M_IMPORT_FILE, Model::Action::Import}, {M_IMPORT_LINK, Model::Action::ImportExternal}
    };

    CMenu menu; menu.CreatePopupMenu();
    if (row_is_song || songs)
    {
        // 播放：和原生播放列表里的叫法保持一致
        menu.AppendMenuW(song_flag, M_PLAY, L"播放所选");
        if (failed_selected) menu.AppendMenuW(MF_STRING, M_RETRY, L"重试播放（丢掉上次的失败结论）");
        menu.AppendMenuW(song_flag, M_PLAY_NEXT, L"下一首播放");
        menu.AppendMenuW(song_flag, M_QUEUE, L"加入播放队列");
        menu.AppendMenuW(song_flag, M_ADD_PLAYLIST, L"加入播放列表…");
        menu.AppendMenuW(song_flag, M_ADD_FAVOURITE, L"加入「我喜欢」");
        menu.AppendMenuW(MF_SEPARATOR);
        // 收藏与歌单：按选中的行里有没有还没收藏的、有没有已收藏的来决定哪一项可用
        menu.AppendMenuW(local_page || !unsaved_selected ? MF_GRAYED : song_flag, M_SAVE_LOCAL, L"保存到在线本地歌单");
        menu.AppendMenuW(saved_selected ? song_flag : MF_GRAYED, M_REMOVE_LOCAL, local_page ? L"从本地歌单移除" : L"取消收藏（从本地歌单移除）");
        menu.AppendMenuW(song_flag, M_SAVE_NATIVE, L"保存为原生播放列表…");
        // 换源：把在线曲目重新匹配到另一个平台。本地文件不参与匹配，会原样保留。
        {
            auto& sources = online::CSourceRegistry::Instance().GetAll();
            CMenu switch_menu; switch_menu.CreatePopupMenu();
            for (size_t i = 0; i < sources.size(); ++i)
                switch_menu.AppendMenuW(MF_STRING | (static_cast<int>(i) == state->source ? MF_CHECKED : 0),
                    M_SWITCH_BASE + static_cast<UINT>(i), sources[i]->GetDisplayName().c_str());
            menu.AppendMenuW(song_flag | MF_POPUP, reinterpret_cast<UINT_PTR>(switch_menu.Detach()),
                local_page ? L"换源到…（更新本地歌单）" : L"换源到…（另存新歌单）");
        }
        menu.AppendMenuW(MF_SEPARATOR);
        // 文件
        menu.AppendMenuW(song_flag, M_DOWNLOAD, L"下载所选歌曲到文件夹…");
        menu.AppendMenuW(song_flag, M_EXPORT, L"导出所选歌曲…");
        menu.AppendMenuW(local_file ? MF_STRING : MF_GRAYED, M_OPEN_FILE, L"打开文件所在位置");
        menu.AppendMenuW(MF_SEPARATOR);
        // 信息
        menu.AppendMenuW(online_flag, M_LYRICS, L"查看歌词");
        menu.AppendMenuW(song_flag, M_COPY, L"复制歌曲信息");
    }
    else
    {
        // 专辑、歌单、榜单这些行只能打开。按行类型给准确的叫法，别一律叫「打开」
        const wchar_t* label = L"打开";
        const int first = selected_rows.empty() ? -1 : selected_rows.front();
        if (first >= 0 && first < static_cast<int>(state->items.size()))
        {
            switch (state->items[first].type)
            {
            case online::BrowseItem::Type::Album: label = L"打开专辑（列出曲目）"; break;
            case online::BrowseItem::Type::Chart: label = L"打开榜单（列出曲目）"; break;
            case online::BrowseItem::Type::Playlist: label = L"打开歌单（列出曲目）"; break;
            case online::BrowseItem::Type::Keyword: label = L"按这个关键词搜索"; break;
            default: break;
            }
        }
        menu.AppendMenuW(state->busy ? MF_GRAYED : MF_STRING, M_OPEN_ROW, label);
    }
    if (full)
    {
        menu.AppendMenuW(MF_SEPARATOR);
        // 整份列表的操作：不用先选中，直接作用于当前这一屏的全部曲目
        const bool list_songs = !state->songs.empty();
        const UINT list_flag = list_songs ? MF_STRING : MF_GRAYED;
        menu.AppendMenuW(list_flag, M_PLAY_ALL, L"播放全部");
        const std::wstring save_label = state->list_title.empty()
            ? L"把当前列表存为歌单…" : L"把整张「" + state->list_title + L"」存为歌单…";
        menu.AppendMenuW(list_flag, M_SAVE_LIST, save_label.c_str());
        menu.AppendMenuW(MF_SEPARATOR);
        // 导入与换源
        menu.AppendMenuW(MF_STRING, M_IMPORT_FILE, L"从本地歌单文件导入…");
        menu.AppendMenuW(MF_STRING, M_IMPORT_LINK, L"从外部平台 歌单链接导入…");
        // 当前播放列表换源：从媒体库打开的导入歌单走这里，结果另存为新歌单
        {
            auto& sources = online::CSourceRegistry::Instance().GetAll();
            CMenu player_menu; player_menu.CreatePopupMenu();
            for (size_t i = 0; i < sources.size(); ++i)
                player_menu.AppendMenuW(MF_STRING, M_SWITCH_PLAYER_BASE + static_cast<UINT>(i), sources[i]->GetDisplayName().c_str());
            player_menu.AppendMenuW(MF_SEPARATOR);
            player_menu.AppendMenuW(MF_STRING, M_SWITCH_PLAYER_NOOP_UNAVAILABLE, L"（作用于当前播放列表，非播放列表模式时不可用）");
            player_menu.EnableMenuItem(M_SWITCH_PLAYER_NOOP_UNAVAILABLE, MF_BYCOMMAND | MF_GRAYED);
            menu.AppendMenuW(MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(player_menu.Detach()), L"把当前播放列表换源到…");
        }
        menu.AppendMenuW(MF_STRING, M_OPEN_PLAYLIST, L"打开歌单编号…（先在搜索框输入编号）");
        menu.AppendMenuW(MF_STRING | (state->request.kind == online::BrowseKind::PlaylistTracks ? 0 : MF_GRAYED),
            M_IMPORT_ALL, L"将完整云歌单导入本地");
        menu.AppendMenuW(MF_SEPARATOR);
        // 下载与缓存
        menu.AppendMenuW(MF_STRING, M_CACHE_INFO, L"下载与缓存状态");
        menu.AppendMenuW(MF_STRING | (online::COnlineMediaCache::Instance().Enabled() ? MF_CHECKED : 0),
            M_TOGGLE_CACHE, L"预缓存下一首");
        menu.AppendMenuW(MF_STRING, M_CLEAR_CACHE, L"清理缓存并取消未完成下载");
        menu.AppendMenuW(MF_STRING | (state->auto_switch_source ? MF_CHECKED : 0),
            M_AUTO_SWITCH, L"无法播放时自动换源");
        menu.AppendMenuW(MF_SEPARATOR);
        // 账号：只摆当前音源相关的项。另一个音源的项在这里是灰的、点了没反应，
        // 摆出来只是噪音，所以干脆不显示。
        if (state->source == 0)
        {
            menu.AppendMenuW(MF_STRING, M_SIGN_IN, L"领取今天的K源会员 / 检查到账");
            menu.AppendMenuW(MF_STRING | (COnlineDailyRewards::Instance().Enabled() ? MF_CHECKED : 0),
                M_TOGGLE_SIGN_IN, L"每天自动领取K源会员");
        }
        else
        {
            menu.AppendMenuW(MF_STRING, M_AD_REWARD, L"看广告领 30 分钟B源会员");
            menu.AppendMenuW(MF_STRING | (CBodianAdRewards::Instance().Enabled() ? MF_CHECKED : 0),
                M_TOGGLE_AD_REWARD, L"自动看广告领B源会员");
            menu.AppendMenuW(MF_STRING, M_SEARCH_PLAYLISTS, L"按关键词搜索B源公开歌单");
            menu.AppendMenuW(MF_STRING, M_IMPORT_ACCOUNT, L"导入B源登录文件…");
        }
    }

    CPoint point; GetCursorPos(&point);
    // 自定义菜单必须先成为前台窗口，否则点菜单外面它不会收起；
    // TPM_NONOTIFY 保证不把菜单 ID 当命令发给主窗口（那些 ID 很小，会撞上真实命令）。
    CWnd* owner = ui ? ui->GetOwner() : nullptr;
    if (owner == nullptr) owner = AfxGetMainWnd();
    if (owner != nullptr) owner->SetForegroundWindow();
    const UINT value = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, owner);
    auto dispatch = [&](Model::Action action, int target = 0) {
        Model::Command command{action};
        command.value = target;
        command.revision = state->revision;
        command.rows = selected_rows;
        Model::Instance().Post(std::move(command));
    };
    for (const auto& command : commands)
        if (command.first == value) { dispatch(command.second); return; }
    // 整份列表的操作要看整屏，不读选中行：不带行号投递，模型按「全部」处理
    if (value == M_PLAY_ALL || value == M_SAVE_LIST)
    {
        Model::Command command{value == M_PLAY_ALL ? Model::Action::PlayAll : Model::Action::SaveAsNativePlaylist};
        command.revision = state->revision;
        Model::Instance().Post(std::move(command));
        return;
    }
    // 换源子菜单：编号减去基址就是目标音源在注册表里的下标
    const size_t source_count = online::CSourceRegistry::Instance().GetAll().size();
    if (value >= M_SWITCH_BASE && value < M_SWITCH_BASE + source_count)
    { dispatch(Model::Action::SwitchInPlace, static_cast<int>(value - M_SWITCH_BASE)); return; }
    if (value >= M_SWITCH_PLAYER_BASE && value < M_SWITCH_PLAYER_BASE + source_count)
    { Dispatch(Model::Action::SwitchPlayerPlaylist, static_cast<int>(value - M_SWITCH_PLAYER_BASE)); return; }
}
void OnlineMusicList::ShowMenu(bool full)
{
    if (!m_state) return;
    // 只有选中歌曲行时才给歌曲那一组。选中的是专辑/歌单/榜单行时按歌曲行给菜单，
    // 用户只会看到一片灰掉的歌曲动作、连「打开专辑」都出不来。
    // 没有选中（工具栏直接点开）时维持原样，菜单里至少还有一组动作。
    std::vector<int> rows; GetItemsSelected(rows);
    ShowStandardMenu(full, rows.empty() ? true : IsSongRow(rows.front()));
}
bool OnlineMusicList::RButtonUp(CPoint point)
{
    if (!rect.PtInRect(point)) return false;
    if (!m_state) return true;
    if (m_state->items.empty() && !m_state->busy) return true;   // 空列表不弹菜单
    // 右键先落在行上：基类 RButtonDown 已经选中了光标所在的行，
    // 这里按那一行是不是歌曲决定给哪些操作，避免对专辑/歌单行给出「下载所选」。
    const int clicked = GetListIndexByPoint(point);
    const bool song_row = clicked >= 0 ? IsSongRow(clicked) : HasSongSelected();
    ShowStandardMenu(false, song_row);
    return true;
}

void OnlineMusicSearch::OnKeyWordsChanged() { Model::Instance().Post({Model::Action::Query, 0, key_word}); }
void OnlineMusicSearch::OnSubmit() { Model::Instance().Post({Model::Action::Search}); }
void OnlineMusicSearch::SyncQuery(const Model::State& state)
{
    if (m_query_revision != state.query_revision)
    {
        key_word = state.query; m_query_revision = state.query_revision;
    }
}
void OnlineMusicDetail::SetSnapshot(std::shared_ptr<const Model::State> state)
{
    if (!m_state || m_state->detail != state->detail)
    {
        m_lines.clear();
        CCommon::StringSplit(state->detail, L'\n', m_lines, false, false);
        scroll_offset = 0;
    }
    m_state = std::move(state);
}
int OnlineMusicDetail::GetScrollAreaHeight()
{
    int text_height = static_cast<int>(m_lines.size()) * ui->DPI(24) + ui->DPI(16);
    if (m_state && m_state->qr_size > 0)
        return rect.Width() >= ui->DPI(520) ? (std::max)(text_height, ui->DPI(240)) : text_height + ui->DPI(240);
    return text_height;
}
void OnlineMusicDetail::DrawScrollArea()
{
    if (!m_state) return;
    DrawAreaGuard clip(&ui->GetDrawer(), rect);
    CRect text_rect = m_scroll_area_rect; text_rect.DeflateRect(ui->DPI(8), ui->DPI(8));
    bool beside = rect.Width() >= ui->DPI(520);
    if (beside && m_state->qr_size) text_rect.right -= ui->DPI(236);
    for (const auto& line : m_lines)
    {
        CRect line_rect = text_rect; line_rect.bottom = line_rect.top + ui->DPI(24);
        ui->GetDrawer().DrawWindowText(line_rect, line.c_str(), ui->GetUIColors().color_text, Alignment::LEFT, false);
        text_rect.top += ui->DPI(24);
    }
    if (m_state->qr_size > 0)
    {
        int cells = m_state->qr_size + 8;
        int scale = (std::max)(1, ui->DPI(220) / cells), size = cells * scale;
        int x = beside ? rect.right - size - ui->DPI(8) : rect.left + ui->DPI(8);
        int y = beside ? m_scroll_area_rect.top + ui->DPI(8) : text_rect.top + ui->DPI(8);
        ui->GetDrawer().FillRect(CRect(x, y, x + size, y + size), RGB(255, 255, 255), false);
        for (int row = 0; row < m_state->qr_size; ++row)
            for (int col = 0; col < m_state->qr_size; ++col)
                if (m_state->qr_pixels[row * m_state->qr_size + col])
                    ui->GetDrawer().FillRect(CRect(x + (col + 4) * scale, y + (row + 4) * scale,
                        x + (col + 5) * scale, y + (row + 5) * scale), RGB(0, 0, 0), false);
    }
}
void OnlineMusic::InitComplete()
{
    // 复用皮肤引擎中的导航、搜索框、按钮和滚动列表，继承当前主题与透明度。
    static const char8_t* layout = u8R"xml(<verticalLayout>
      <horizontalLayout height="32" margin-bottom="6">
        <text id="online_title" type="userDefine" text="在线音乐" font_size="11"/>
        <text id="online_source_label" type="userDefine" text="浏览音源" font_size="9" width="64" margin-left="4"/>
        <comboBox id="online_source" width="128" margin-left="4"/>
        <placeHolder/>
        <comboBox id="online_page" width="104"/>
      </horizontalLayout>
      <horizontalLayout id="online_notice_row" height="24" margin-bottom="4">
        <text id="online_notice" type="userDefine" text="" font_size="9" color_style="emphasis1"/>
      </horizontalLayout>
      <navigationBar id="online_nav" height="30" margin-bottom="6" icon_type="text_only" item_space="10" font_size="9">
        <navigationItem text="发现"/><navigationItem text="搜索"/><navigationItem text="推荐"/>
        <navigationItem text="榜单"/><navigationItem text="云歌单"/><navigationItem text="本地歌单"/><navigationItem text="账号"/>
      </navigationBar>
      <horizontalLayout id="online_search_row" height="28" margin-bottom="6">
        <comboBox id="online_search_type" width="88" margin-right="6"/>
        <onlineMusicSearch id="online_search"/>
        <button id="online_search_submit" icon="find" text="搜索" width="28" margin-left="4"/>
      </horizontalLayout>
      <horizontalLayout id="online_actions" height="28" margin-bottom="6">
        <button id="online_play" icon="play" text="播放" show_text="true" width="66"/>
        <button id="online_queue" icon="add" text="加入队列" show_text="true" width="92" margin-left="4"/>
        <button id="online_save" icon="favoriteOff" text="收藏" show_text="true" width="66" margin-left="4"/>
        <button id="online_clear_local" icon="delete" text="清空" show_text="true" width="56" margin-left="4"/>
        <placeHolder/>
        <button id="online_import" icon="folder" text="导入歌单" width="28"/>
        <button id="online_export" icon="saveAs" text="导出歌单" width="28" margin-left="4"/>
        <button id="online_more_actions" icon="more" text="更多操作" width="28" margin-left="4"/>
      </horizontalLayout>
      <horizontalLayout id="online_account_actions" height="28" margin-bottom="6">
        <button id="online_login" icon="online" text="扫码登录" show_text="true" width="104"/>
        <button id="online_logout" icon="exit" text="退出账号" show_text="true" width="104" margin-left="8"/>
        <placeHolder/>
        <button id="online_account_more" icon="more" text="签到与账号操作" width="28" margin-left="4"/>
      </horizontalLayout>
      <onlineMusicList id="online_results" item_height="32"/>
      <onlineMusicDetail id="online_detail"/>
      <horizontalLayout height="26" margin-top="6">
        <button id="online_back" icon="arrowLeft" text="返回列表" width="26"/>
        <text id="online_status" type="userDefine" text="在线音乐" font_size="8" color_style="emphasis1" margin-left="4"/>
        <text id="online_quality" type="userDefine" text="" font_size="8" width_follow_text="true" max-width="45%" margin-left="8" alignment="right"/>
        <button id="online_locate" icon="locate" text="定位到正在播放（Ctrl+G）" width="26" margin-left="4"/>
        <button id="online_refresh" icon="refresh" text="刷新（F5）" width="26" margin-left="4"/>
        <button id="online_load_more" icon="next" text="加载更多" width="26" margin-left="4"/>
      </horizontalLayout>
    </verticalLayout>)xml";
    tinyxml2::XMLDocument document; document.Parse(reinterpret_cast<const char*>(layout));
    AddChild(CUserUi::BuildUiElementFromXmlNode(document.RootElement(), ui));
    m_list = FindElement<OnlineMusicList>("online_results");
    m_detail = FindElement<OnlineMusicDetail>("online_detail");
    auto* source = FindElement<ComboBox>("online_source");
    for (auto* provider : online::CSourceRegistry::Instance().GetAll()) source->AddString(provider->GetDisplayName(), IconMgr::IT_Online);
    source->SetSelectionChangedTrigger([](ComboBox* combo) { Model::Instance().Post({Model::Action::Source, combo->GetCurSel()}); });
    auto* pages = FindElement<ComboBox>("online_page");
    for (int page = 0; page < 7; ++page) pages->AddString(Model::PageName(static_cast<Model::Page>(page)));
    pages->SetSelectionChangedTrigger([](ComboBox* combo) { Model::Instance().Post({Model::Action::Page, combo->GetCurSel()}); });
    // 搜索对象：让用户一眼看出能搜什么，而不是只能搜歌
    auto* search_type = FindElement<ComboBox>("online_search_type");
    search_type->AddString(L"搜音乐");
    search_type->AddString(L"搜专辑");
    search_type->AddString(L"搜歌单");
    search_type->SetSelectionChangedTrigger([](ComboBox* combo) { Model::Instance().Post({Model::Action::SearchType, combo->GetCurSel()}); });
    FindElement<NavigationBar>("online_nav")->SetSelectionChangedTrigger([](int page) { Model::Instance().Post({Model::Action::Page, page}); });
    const std::pair<const char*, Model::Action> actions[] = {
        {"online_search_submit", Model::Action::Search}, {"online_play", Model::Action::Play},
        {"online_queue", Model::Action::Queue}, {"online_save", Model::Action::Save},
        {"online_export", Model::Action::Export}, {"online_clear_local", Model::Action::ClearLocal},
        {"online_back", Model::Action::Back}, {"online_refresh", Model::Action::Refresh},
        {"online_load_more", Model::Action::More}, {"online_login", Model::Action::Login}, {"online_logout", Model::Action::Logout}
    };
    for (const auto& action : actions)
        FindElement<Button>(action.first)->SetClickedTrigger([this, value = action.second](Button*) { m_list->Dispatch(value); });
    FindElement<Button>("online_locate")->SetClickedTrigger([this](Button*) {
        if (m_list && !m_list->LocateToCurrent()) Model::Instance().SetStatus(L"正在播放的歌曲不在当前列表里。");
    });
    FindElement<Button>("online_import")->SetClickedTrigger([this](Button*) { ShowImportMenu(); });
    FindElement<Button>("online_more_actions")->SetClickedTrigger([this](Button*) { ShowMoreMenu(); });
    FindElement<Button>("online_account_more")->SetClickedTrigger([this](Button*) { ShowMoreMenu(); });
}
void OnlineMusic::ShowMoreMenu()
{
    // 和右键菜单是同一份定义，这里只是把整页操作也带出来
    if (m_list) m_list->ShowMenu(true);
}
void OnlineMusic::ShowImportMenu()
{
    CMenu menu; menu.CreatePopupMenu();
    menu.AppendMenuW(MF_STRING, 1, L"从本地歌单文件导入…");
    menu.AppendMenuW(MF_STRING, 2, L"从外部平台 歌单链接导入…");
    CPoint point; GetCursorPos(&point);
    // 和 ShowStandardMenu 一样先把自己设为前台：否则点菜单外面它不会收起。
    CWnd* owner = ui != nullptr ? ui->GetOwner() : nullptr;
    if (owner == nullptr) owner = AfxGetMainWnd();
    if (owner != nullptr) owner->SetForegroundWindow();
    const UINT value = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y, owner);
    if (value == 1) m_list->Dispatch(Model::Action::Import);
    else if (value == 2) Model::Instance().Post({Model::Action::ImportExternal});
}
void OnlineMusic::SyncLayout(const Model::State& state)
{
    const bool account = state.page == Model::Page::Account;
    const bool search_page = state.page == Model::Page::Search;
    const bool compact = rect.Width() < ui->DPI(500);
    FindElement("online_nav")->SetVisible(!compact);
    FindElement("online_page")->SetVisible(compact);
    FindElement("online_source_label")->SetVisible(!compact);
    auto* queue = FindElement<Button>("online_queue");
    queue->SetShowText(!compact);
    queue->SetWidth(compact ? "32" : "92");
    // 搜索框只在搜索页出现；切到其它页时布局会自动收起这一行，不占高度。
    FindElement("online_search_row")->SetVisible(search_page);
    FindElement("online_actions")->SetVisible(!account);
    FindElement("online_account_actions")->SetVisible(account);
    FindElement("online_save")->SetVisible(state.page != Model::Page::Local);
    FindElement("online_clear_local")->SetVisible(state.page == Model::Page::Local);
    FindElement("online_notice_row")->SetVisible(!state.notice.empty());
    FindElement<Text>("online_notice")->SetText(state.notice);
    m_list->SetVisible(!state.detail_visible); m_detail->SetVisible(state.detail_visible);

    FindElement<ComboBox>("online_page")->SetCurSel(static_cast<int>(state.page));
    FindElement<ComboBox>("online_source")->SetCurSel(state.source);
    FindElement<NavigationBar>("online_nav")->SetSelectedIndex(static_cast<int>(state.page));
    FindElement<OnlineMusicSearch>("online_search")->SyncQuery(state);
    if (search_page) FindElement<ComboBox>("online_search_type")->SetCurSel(state.search_type);
    const wstring title = L"在线音乐";
    FindElement<Text>("online_title")->SetText(title);
    // 状态栏：播放错误 > 操作/列表状态；音质说明单独放右侧，不再挤掉状态文字
    FindElement<Text>("online_status")->SetText(state.playback_error.empty() ? state.status : state.playback_error);
    FindElement<Text>("online_quality")->SetText(state.playback_error.empty() ? state.quality_note : L"");
    FindElement("online_quality")->SetVisible(state.playback_error.empty() && !state.quality_note.empty());

    const bool songs = std::any_of(state.songs.begin(), state.songs.end(), [](const SongInfo& song) { return !song.file_path.empty(); });
    for (const char* id : {"online_play", "online_queue", "online_save", "online_export"})
        FindElement<Button>(id)->SetEnable(songs && !state.busy);
    FindElement<Button>("online_clear_local")->SetEnable(state.page == Model::Page::Local && songs && !state.busy);
    FindElement("online_login")->SetVisible(!state.logged_in);
    FindElement("online_logout")->SetVisible(state.logged_in);
    FindElement<Button>("online_login")->SetEnable(!state.busy);
    FindElement<Button>("online_logout")->SetEnable(!state.busy);
    FindElement("online_load_more")->SetVisible(state.has_more);
    FindElement<Button>("online_load_more")->SetEnable(state.has_more && !state.busy);
    // 「定位到正在播放」只在列表里真有那一行时亮起；正在播放的曲目变化在 Draw 里另行刷新
    const std::wstring playing = CPlayer::GetInstance().GetSafeCurrentSongInfo().file_path;
    const bool playing_here = !state.detail_visible && !playing.empty()
        && std::any_of(state.songs.begin(), state.songs.end(), [&](const SongInfo& song) { return song.file_path == playing; });
    FindElement("online_locate")->SetVisible(!state.detail_visible && songs);
    FindElement<Button>("online_locate")->SetEnable(playing_here);
    m_last_playing = playing;
    const bool back_available = state.detail_visible || state.request.kind == online::BrowseKind::ChartTracks
        || state.request.kind == online::BrowseKind::PlaylistTracks
        || state.request.kind == online::BrowseKind::AlbumTracks;
    FindElement("online_back")->SetVisible(back_available);
    FindElement<Button>("online_back")->SetEnable(back_available && !state.busy);
}

void OnlineMusic::Draw()
{
    std::lock_guard<std::recursive_mutex> lock(m_view_mutex);
    CalculateRect();
    if (!m_activated) { Model::Instance().Post({Model::Action::Activate}); m_activated = true; }
    auto state = Model::Instance().Snapshot();
    const bool state_changed = m_last_state != state;
    if (state_changed)
    {
        m_list->SetSnapshot(state); m_detail->SetSnapshot(state);
        m_last_state = state;
    }
    // 宽度变化会影响导航、页码下拉框和搜索行的显示；状态变化时也要刷新按钮可用性；
    // 切歌也要刷新一次（「定位到正在播放」的可用性跟着当前曲目走）。
    if (state_changed || m_last_width != rect.Width() || m_last_playing != CPlayer::GetInstance().GetSafeCurrentSongInfo().file_path)
    {
        SyncLayout(*state);
        m_last_width = rect.Width();
    }
    Element::Draw();
}
void OnlineMusic::DrawTopMost() { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); Element::DrawTopMost(); }
bool OnlineMusic::HandleKey(UINT key, bool control)
{
    std::lock_guard<std::recursive_mutex> lock(m_view_mutex);
    if (!m_list) return false;
    const auto state = Model::Instance().Snapshot();
    // 详情页（歌词、账号、缓存状态）：Esc 返回列表，其它键不拦
    if (state->detail_visible)
    {
        if (key == VK_ESCAPE && state->page != Model::Page::Account) { m_list->Dispatch(Model::Action::Back); return true; }
        return false;
    }
    if (control && key == 'A') { m_list->SelectAll(); return true; }
    if (control && key == 'G') { if (!m_list->LocateToCurrent()) Model::Instance().SetStatus(L"正在播放的歌曲不在当前列表里。"); return true; }
    if (!control && key == VK_F5) { m_list->Dispatch(Model::Action::Refresh); return true; }
    if (!control && key == VK_RETURN) { m_list->Dispatch(Model::Action::Open); return true; }
    // 列表为空时上下键还给音量调节；有内容时它们是列表导航
    if (m_list->GetRowCount() > 0 && !control)
    {
        switch (key)
        {
        case VK_UP: return m_list->MoveSelection(-1, false, false);
        case VK_DOWN: return m_list->MoveSelection(1, false, false);
        case VK_PRIOR: return m_list->MoveSelection(-1, true, false);
        case VK_NEXT: return m_list->MoveSelection(1, true, false);
        case VK_HOME: return m_list->MoveSelection(-1, false, true);
        case VK_END: return m_list->MoveSelection(1, false, true);
        case VK_DELETE:
            // 本地歌单页 Delete 直接移除；别的页面上 Delete 没有对应动作，不拦
            if (state->page == Model::Page::Local && m_list->HasSongSelected()) { m_list->Dispatch(Model::Action::RemoveLocal); return true; }
            return false;
        }
    }
    return false;
}
bool OnlineMusic::LButtonUp(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::LButtonUp(p); }
bool OnlineMusic::LButtonDown(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::LButtonDown(p); }
bool OnlineMusic::RButtonUp(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::RButtonUp(p); }
bool OnlineMusic::RButtonDown(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::RButtonDown(p); }
bool OnlineMusic::DoubleClick(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::DoubleClick(p); }
bool OnlineMusic::MouseMove(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::MouseMove(p); }
bool OnlineMusic::MouseLeave() { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::MouseLeave(); }
bool OnlineMusic::MouseWheel(int d, CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::MouseWheel(d, p); }
bool OnlineMusic::GlobalLButtonUp(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::GlobalLButtonUp(p); }
bool OnlineMusic::GlobalLButtonDown(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::GlobalLButtonDown(p); }
bool OnlineMusic::GlobalMouseMove(CPoint p) { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::GlobalMouseMove(p); }
bool OnlineMusic::SetCursor() { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); return Element::SetCursor(); }
