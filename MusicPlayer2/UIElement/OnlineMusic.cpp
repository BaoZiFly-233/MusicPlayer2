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
}
int OnlineMusicList::GetRowCount() { return m_state ? static_cast<int>(m_state->items.size()) : 0; }
int OnlineMusicList::GetColumnCount() { return rect.Width() >= ui->DPI(560) ? 5 : 4; }
int OnlineMusicList::GetColumnWidth(int col, int width)
{
    int number = ui->DPI(32), duration = ui->DPI(48), rest = (std::max)(0, width - number - duration);
    if (col == 0) return number;
    if (col == GetColumnCount() - 1) return duration;
    if (GetColumnCount() == 5) return col == 1 ? rest * 45 / 100 : col == 2 ? rest * 28 / 100 : rest * 27 / 100;
    return col == 1 ? rest * 65 / 100 : rest * 35 / 100;
}
std::wstring OnlineMusicList::GetItemText(int row, int col)
{
    if (!m_state || row < 0 || row >= GetRowCount()) return {};
    const auto& item = m_state->items[row];
    if (col == 0) return std::to_wstring(row + 1);
    if (col == 1) return item.title;
    if (col == 2) return item.subtitle;
    if (col == 3 && GetColumnCount() == 5) return item.track.album;
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

void OnlineMusicList::Dispatch(Model::Action action)
{
    if (!m_state) return;
    Model::Command command{action}; command.revision = m_state->revision; GetItemsSelected(command.rows);
    Model::Instance().Post(std::move(command));
}
void OnlineMusicList::OnDoubleClicked() { Dispatch(Model::Action::Open); }
bool OnlineMusicList::RButtonUp(CPoint point)
{
    if (!rect.PtInRect(point)) return false;
    CMenu menu; menu.CreatePopupMenu();
    // 按用途分组：播放、文件、查看
    menu.AppendMenuW(MF_STRING, 1, L"播放所选");
    menu.AppendMenuW(MF_STRING, 2, L"加入播放队列");
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING, 3, L"下载所选歌曲到文件夹…");
    menu.AppendMenuW(MF_STRING, 4, L"保存到本地歌单");
    menu.AppendMenuW(MF_STRING, 5, L"导出所选歌曲…");
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING, 6, L"查看歌词");
    CPoint screen = point; ui->GetOwner()->ClientToScreen(&screen);
    UINT action = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, ui->GetOwner());
    const Model::Action actions[] = { Model::Action::Play, Model::Action::Queue, Model::Action::Download,
        Model::Action::Save, Model::Action::Export, Model::Action::Lyrics };
    if (action >= 1 && action <= _countof(actions)) Dispatch(actions[action - 1]);
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
        <comboBox id="online_source" width="128" margin-left="8"/>
      </horizontalLayout>
      <navigationBar id="online_nav" height="30" margin-bottom="6" icon_type="text_only" item_space="10" font_size="9">
        <navigationItem text="发现"/><navigationItem text="搜索"/><navigationItem text="推荐"/>
        <navigationItem text="榜单"/><navigationItem text="云歌单"/><navigationItem text="本地歌单"/><navigationItem text="账号"/>
      </navigationBar>
      <horizontalLayout height="28" margin-bottom="6">
        <comboBox id="online_page" width="104" margin-right="6"/>
        <onlineMusicSearch id="online_search"/>
        <button id="online_search_submit" icon="find" text="搜索" width="28" margin-left="4"/>
      </horizontalLayout>
      <horizontalLayout id="online_actions" height="28" margin-bottom="6">
        <button id="online_play" icon="play" text="播放" show_text="true" width="66"/>
        <button id="online_queue" icon="add" text="加入队列" show_text="true" width="92" margin-left="4"/>
        <button id="online_save" icon="favoriteOff" text="收藏" show_text="true" width="66" margin-left="4"/>
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
        <button id="online_refresh" icon="refresh" text="刷新" width="26" margin-left="4"/>
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
    FindElement<NavigationBar>("online_nav")->SetSelectionChangedTrigger([](int page) { Model::Instance().Post({Model::Action::Page, page}); });
    const std::pair<const char*, Model::Action> actions[] = {
        {"online_search_submit", Model::Action::Search}, {"online_play", Model::Action::Play},
        {"online_queue", Model::Action::Queue}, {"online_save", Model::Action::Save},
        {"online_import", Model::Action::Import}, {"online_export", Model::Action::Export},
        {"online_back", Model::Action::Back}, {"online_refresh", Model::Action::Refresh},
        {"online_load_more", Model::Action::More}, {"online_login", Model::Action::Login}, {"online_logout", Model::Action::Logout}
    };
    for (const auto& action : actions)
        FindElement<Button>(action.first)->SetClickedTrigger([this, value = action.second](Button*) { m_list->Dispatch(value); });
    FindElement<Button>("online_more_actions")->SetClickedTrigger([this](Button*) { ShowMoreMenu(); });
    FindElement<Button>("online_account_more")->SetClickedTrigger([this](Button*) { ShowMoreMenu(); });
}
void OnlineMusic::ShowMoreMenu()
{
    auto state = Model::Instance().Snapshot();
    CMenu menu; menu.CreatePopupMenu();
    menu.AppendMenuW(MF_STRING, 1, L"查看所选歌曲的歌词");
    menu.AppendMenuW(MF_STRING, 2, L"打开搜索框中的歌单编号");
    menu.AppendMenuW(MF_STRING | (state->request.kind == online::BrowseKind::PlaylistTracks ? 0 : MF_GRAYED), 3, L"将完整云歌单导入本地");
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING, 4, L"保存所选歌曲到文件夹…");
    menu.AppendMenuW(MF_STRING, 5, L"下载与缓存");
    menu.AppendMenuW(MF_STRING | (online::COnlineMediaCache::Instance().Enabled() ? MF_CHECKED : 0), 6, L"预缓存下一首");
    menu.AppendMenuW(MF_STRING, 7, L"清理缓存并取消未完成下载");
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING | (state->source == 1 ? 0 : MF_GRAYED), 8, L"按关键词搜索波点歌单");
    menu.AppendMenuW(MF_STRING | (state->source == 1 ? 0 : MF_GRAYED), 9, L"导入波点登录文件…");
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING | (state->source == 0 ? 0 : MF_GRAYED), 10, L"酷狗今日签到领会员 / 检查到账");
    menu.AppendMenuW(MF_STRING | (state->source == 0 ? 0 : MF_GRAYED) | (COnlineDailyRewards::Instance().Enabled() ? MF_CHECKED : 0), 11, L"酷狗自动签到领会员");
    CPoint point; GetCursorPos(&point);
    UINT value = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, ui->GetOwner());
    const Model::Action actions[] = {Model::Action::Lyrics, Model::Action::OpenPlaylist, Model::Action::ImportAll,
        Model::Action::Download, Model::Action::CacheInfo, Model::Action::ToggleCache, Model::Action::ClearCache,
        Model::Action::SearchPlaylists, Model::Action::ImportAccount, Model::Action::SignIn, Model::Action::ToggleSignIn};
    if (value >= 1 && value <= _countof(actions)) m_list->Dispatch(actions[value - 1]);
}
void OnlineMusic::Draw()
{
    std::lock_guard<std::recursive_mutex> lock(m_view_mutex);
    CalculateRect();
    if (!m_activated) { Model::Instance().Post({Model::Action::Activate}); m_activated = true; }
    auto state = Model::Instance().Snapshot();
    m_list->SetSnapshot(state); m_detail->SetSnapshot(state);
    bool account = state->page == Model::Page::Account;
    bool compact = rect.Width() < ui->DPI(500);
    FindElement("online_nav")->SetVisible(!compact);
    FindElement("online_page")->SetVisible(compact);
    FindElement<ComboBox>("online_page")->SetCurSel(static_cast<int>(state->page));
    FindElement<ComboBox>("online_source")->SetCurSel(state->source);
    FindElement<NavigationBar>("online_nav")->SetSelectedIndex(static_cast<int>(state->page));
    FindElement<OnlineMusicSearch>("online_search")->SyncQuery(*state);
    FindElement<Text>("online_title")->SetText(std::wstring(L"在线音乐 / ") + Model::PageName(state->page));
    FindElement<Text>("online_status")->SetText(state->playback_error.empty() ? state->status : state->playback_error);
    FindElement("online_actions")->SetVisible(!account);
    FindElement("online_account_actions")->SetVisible(account);
    m_list->SetVisible(!state->detail_visible); m_detail->SetVisible(state->detail_visible);
    bool songs = std::any_of(state->songs.begin(), state->songs.end(), [](const SongInfo& song) { return !song.file_path.empty(); });
    for (const char* id : {"online_play", "online_queue", "online_save", "online_export"}) FindElement(id)->SetEnable(songs && !state->busy);
    FindElement("online_login")->SetEnable(!state->busy);
    FindElement("online_load_more")->SetEnable(state->has_more && !state->busy);
    FindElement("online_back")->SetEnable(state->detail_visible || state->request.kind == online::BrowseKind::ChartTracks || state->request.kind == online::BrowseKind::PlaylistTracks);
    Element::Draw();
}
void OnlineMusic::DrawTopMost() { std::lock_guard<std::recursive_mutex> lock(m_view_mutex); Element::DrawTopMost(); }
bool OnlineMusic::HandleKey(UINT key, bool control)
{
    std::lock_guard<std::recursive_mutex> lock(m_view_mutex);
    if (!m_list || Model::Instance().Snapshot()->detail_visible) return false;
    if (control && key == 'A') { m_list->SelectAll(); return true; }
    if (!control && key == VK_RETURN) { m_list->Dispatch(Model::Action::Open); return true; }
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
