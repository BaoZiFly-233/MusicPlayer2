// CMediaLibDlg.cpp: 实现文件
//

#include "stdafx.h"
#include "MusicPlayer2.h"
#include "CMediaLibDlg.h"
#include "MediaLibStatisticsDlg.h"

// CMediaLibDlg 对话框

int CMediaLibDlg::m_last_tab{};

IMPLEMENT_DYNAMIC(CMediaLibDlg, CBaseDialog)

CMediaLibDlg::CMediaLibDlg(int cur_tab, CWnd* pParent /*=nullptr*/)
    : CBaseDialog(IDD_MEDIA_LIB_DIALOG, pParent), m_init_tab(cur_tab)
{
}

CMediaLibDlg::~CMediaLibDlg()
{
    // 保存设置更改到OnDestroy，因为CMediaLibDlg不会在关闭时被delete析构而是下次打开/程序退出时析构
}

void CMediaLibDlg::SetCurTab(int tab)
{
    m_tab_ctrl.SetCurTab(tab);
}

bool CMediaLibDlg::NavigateToItem(const wstring& item)
{
    CWnd* pCurTab{ m_tab_ctrl.GetCurrentTab() };
    CMediaClassifyDlg* media_classfi_dlg = dynamic_cast<CMediaClassifyDlg*>(pCurTab);
    if (media_classfi_dlg != nullptr)
    {
        return media_classfi_dlg->SetLeftListSel(item);
    }
    
    CSetPathDlg* folder_dlg = dynamic_cast<CSetPathDlg*>(pCurTab);
    if (folder_dlg != nullptr)
    {
        return folder_dlg->SetCurSel(ListItem{ LT_FOLDER, item });
    }

    CSelectPlaylistDlg* playlist_dlg = dynamic_cast<CSelectPlaylistDlg*>(pCurTab);
    if (playlist_dlg != nullptr)
    {
        return playlist_dlg->SetCurSel(ListItem{ LT_PLAYLIST, item });
    }

    return false;
}

CString CMediaLibDlg::GetDialogName() const
{
    return L"MediaLibDlg";
}

bool CMediaLibDlg::InitializeControls()
{
    wstring temp;
    temp = theApp.m_str_table.LoadText(L"TITLE_LIB");
    SetWindowTextW(temp.c_str());
    temp = theApp.m_str_table.LoadText(L"TXT_LIB_MEDIA_LIB_SETTING");
    SetDlgItemTextW(IDC_MEDIA_LIB_SETTINGS_BTN, temp.c_str());
    temp = theApp.m_str_table.LoadText(L"TXT_LIB_STATISTICS_INFO");
    SetDlgItemTextW(IDC_STATISTICS_INFO_BUTTON, temp.c_str());
    temp = theApp.m_str_table.LoadText(L"TXT_LIB_PLAY_SEL");
    SetDlgItemTextW(IDC_PLAY_SELECTED, temp.c_str());
    temp = theApp.m_str_table.LoadText(L"TXT_CLOSE");
    SetDlgItemTextW(IDCANCEL, temp.c_str());

    RepositionTextBasedControls({
        { CtrlTextInfo::L2, IDC_MEDIA_LIB_SETTINGS_BTN, CtrlTextInfo::W32 },
        { CtrlTextInfo::L1, IDC_STATISTICS_INFO_BUTTON, CtrlTextInfo::W32 },
        { CtrlTextInfo::R1, IDC_PLAY_SELECTED, CtrlTextInfo::W32 },
        { CtrlTextInfo::R2, IDCANCEL, CtrlTextInfo::W32 }
        });
    return true;
}

void CMediaLibDlg::DoDataExchange(CDataExchange* pDX)
{
    CBaseDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TAB1, m_tab_ctrl);
}


BEGIN_MESSAGE_MAP(CMediaLibDlg, CBaseDialog)
    ON_BN_CLICKED(IDC_PLAY_SELECTED, &CMediaLibDlg::OnBnClickedPlaySelected)
    ON_MESSAGE(WM_PLAY_SELECTED_BTN_ENABLE, &CMediaLibDlg::OnPlaySelectedBtnEnable)
    ON_BN_CLICKED(IDC_MEDIA_LIB_SETTINGS_BTN, &CMediaLibDlg::OnBnClickedMediaLibSettingsBtn)
    ON_BN_CLICKED(IDC_STATISTICS_INFO_BUTTON, &CMediaLibDlg::OnBnClickedStatisticsInfoButton)
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_WM_SIZE()
    ON_BN_CLICKED(ONLINE_CANCEL, &CMediaLibDlg::OnCancelOnlineTask)
    ON_BN_CLICKED(ONLINE_NEXT, &CMediaLibDlg::OnNextOnlineTask)
END_MESSAGE_MAP()


// CMediaLibDlg 消息处理程序

BOOL CMediaLibDlg::OnInitDialog()
{
    CBaseDialog::OnInitDialog();

    // TODO:  在此添加额外的初始化
    ModifyStyle(0, WS_CLIPCHILDREN);

    SetIcon(IconMgr::IconType::IT_Media_Lib, FALSE);
    SetIcon(IconMgr::IconType::IT_Media_Lib, TRUE);

    SetButtonIcon(IDC_MEDIA_LIB_SETTINGS_BTN, IconMgr::IconType::IT_Setting);
    SetButtonIcon(IDC_STATISTICS_INFO_BUTTON, IconMgr::IconType::IT_Info);
    SetButtonIcon(IDC_PLAY_SELECTED, IconMgr::IconType::IT_Play);

    // 创建子对话框，因为CMediaLibDlg不会及时析构为避免持续占用大量内存不再使用成员变量存储子窗口窗口类
    m_path_dlg = new CSetPathDlg();
    m_playlist_dlg = new CSelectPlaylistDlg();
    m_artist_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_ARTIST);
    m_album_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_ALBUM);
    m_genre_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_GENRE);
    m_year_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_YEAR);
    m_type_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_TYPE);
    m_bitrate_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_BITRATE);
    m_rating_dlg = new CMediaClassifyDlg(ListItem::ClassificationType::CT_RATING);
    m_all_media_dlg = new CAllMediaDlg(CAllMediaDlg::DT_ALL_MEDIA);
    m_recent_media_dlg = new CAllMediaDlg(CAllMediaDlg::DT_RECENT_MEDIA);
    m_folder_explore_dlg = new CFolderExploreDlg();
    //文件夹
    m_path_dlg->Create(IDD_SET_PATH_DIALOG);
    m_tab_ctrl.AddWindow(m_path_dlg, theApp.m_str_table.LoadText(L"TXT_FOLDER").c_str(), IconMgr::IconType::IT_Folder);

    //播放列表
    m_playlist_dlg->Create(IDD_SELECT_PLAYLIST_DIALOG);
    m_tab_ctrl.AddWindow(m_playlist_dlg, theApp.m_str_table.LoadText(L"TXT_PLAYLIST").c_str(), IconMgr::IconType::IT_Playlist);

    auto isTabShown = [&](MediaLibDisplayItem tab) {
        return (theApp.m_media_lib_setting_data.display_item & tab) || (m_tab_show_force & tab);
    };

    //艺术家
    if (isTabShown(MLDI_ARTIST))
    {
        m_artist_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_artist_dlg, theApp.m_str_table.LoadText(L"TXT_ARTIST").c_str(), IconMgr::IconType::IT_Artist);
    }
    //唱片集
    if (isTabShown(MLDI_ALBUM))
    {
        m_album_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_album_dlg, theApp.m_str_table.LoadText(L"TXT_ALBUM").c_str(), IconMgr::IconType::IT_Album);
    }
    //流派
    if (isTabShown(MLDI_GENRE))
    {
        m_genre_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_genre_dlg, theApp.m_str_table.LoadText(L"TXT_GENRE").c_str(), IconMgr::IconType::IT_Genre);
    }
    //年份
    if (isTabShown(MLDI_YEAR))
    {
        m_year_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_year_dlg, theApp.m_str_table.LoadText(L"TXT_YEAR").c_str(), IconMgr::IconType::IT_Year);
    }
    //类型
    if (isTabShown(MLDI_TYPE))
    {
        m_type_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_type_dlg, theApp.m_str_table.LoadText(L"TXT_FILE_TYPE").c_str(), IconMgr::IconType::IT_File_Relate);
    }
    //比特率
    if (isTabShown(MLDI_BITRATE))
    {
        m_bitrate_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_bitrate_dlg, theApp.m_str_table.LoadText(L"TXT_BITRATE").c_str(), IconMgr::IconType::IT_Bitrate);
    }
    //分级
    if (isTabShown(MLDI_RATING))
    {
        m_rating_dlg->Create(IDD_MEDIA_CLASSIFY_DIALOG);
        m_tab_ctrl.AddWindow(m_rating_dlg, theApp.m_str_table.LoadText(L"TXT_RATING").c_str(), IconMgr::IconType::IT_Star);
    }
    //所有曲目
    if (isTabShown(MLDI_ALL))
    {
        m_all_media_dlg->Create(IDD_ALL_MEDIA_DIALOG);
        m_tab_ctrl.AddWindow(m_all_media_dlg, theApp.m_str_table.LoadText(L"TXT_ALL_TRACKS").c_str(), IconMgr::IconType::IT_Media_Lib);
    }
    //最近播放
    if (isTabShown(MLDI_RECENT))
    {
        m_recent_media_dlg->Create(IDD_ALL_MEDIA_DIALOG);
        m_tab_ctrl.AddWindow(m_recent_media_dlg, theApp.m_str_table.LoadText(L"TXT_RECENT_PLAYED").c_str(), IconMgr::IconType::IT_History);
    }
    //文件夹浏览
    if (isTabShown(MLDI_FOLDER_EXPLORE))
    {
        m_folder_explore_dlg->Create(IDD_FOLDER_EXPLORE_DIALOG);
        m_tab_ctrl.AddWindow(m_folder_explore_dlg, theApp.m_str_table.LoadText(L"TXT_FOLDER_EXPLORE").c_str(), IconMgr::IconType::IT_Folder_Explore);
    }

    m_tab_ctrl.SetItemSize(CSize(theApp.DPI(60), theApp.DPI(24)));
    m_tab_ctrl.AdjustTabWindowSize();

    //如果要显示的标签序号无效，则保持上次的标签
    m_tab_ctrl.SetCurTab(m_init_tab >= 0 ? m_init_tab : m_last_tab);

    m_path_dlg->AdjustColumnWidth();
    m_playlist_dlg->AdjustColumnWidth();

    CenterWindow(GetParent());

    CRect client, tab;
    GetClientRect(client); m_tab_ctrl.GetWindowRect(tab); ScreenToClient(tab);
    m_tab_bottom_gap = client.bottom - tab.bottom;
    const DWORD text_style = WS_CHILD | SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE;
    m_online_heading.Create(L"", text_style, CRect(), this);
    m_online_detail.Create(L"", text_style, CRect(), this);
    m_online_progress.Create(WS_CHILD | PBS_SMOOTH, CRect(), this, 0);
    m_online_progress.SetRange32(0, 100);
    m_online_marquee = false;
    m_online_cancel.Create(L"取消", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, CRect(), this, ONLINE_CANCEL);
    m_online_next.Create(L"下一项", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, CRect(), this, ONLINE_NEXT);
    for (CWnd* control : {static_cast<CWnd*>(&m_online_heading), static_cast<CWnd*>(&m_online_detail),
        static_cast<CWnd*>(&m_online_cancel), static_cast<CWnd*>(&m_online_next)})
        control->SetFont(GetFont());
    m_online_tasks = online::OnlineProgress::Snapshot();
    UpdateOnlineProgress();
    SetTimer(ONLINE_PROGRESS_TIMER, 100, nullptr);

    return TRUE;  // return TRUE unless you set the focus to a control
                  // 异常: OCX 属性页应返回 FALSE
}


void CMediaLibDlg::OnBnClickedPlaySelected()
{
    // 调用子窗口的OnOK方法
    CTabDlg* current_tab = dynamic_cast<CTabDlg*>(m_tab_ctrl.GetCurrentTab());
    if (current_tab != nullptr)
        current_tab->OnOK();    // OnOK是虚方法，此处实际上执行的是CTabDlg的最终派生类的OnOK
}


afx_msg LRESULT CMediaLibDlg::OnPlaySelectedBtnEnable(WPARAM wParam, LPARAM lParam)
{
    bool enable = (wParam != 0);
    CWnd* pBtn = GetDlgItem(IDC_PLAY_SELECTED);
    if (pBtn != nullptr)
        pBtn->EnableWindow(enable);

    return 0;
}


void CMediaLibDlg::OnBnClickedMediaLibSettingsBtn()
{
    // TODO: 在此添加控件通知处理程序代码
    theApp.m_pMainWnd->SendMessage(WM_OPTION_SETTINGS, 4, LPARAM(this));
}


void CMediaLibDlg::OnBnClickedStatisticsInfoButton()
{
    // TODO: 在此添加控件通知处理程序代码
    CMediaLibStatisticsDlg dlg;
    dlg.DoModal();
}


void CMediaLibDlg::OnDestroy()
{
    KillTimer(ONLINE_PROGRESS_TIMER);
    m_tab_bottom_gap = 0;
    CBaseDialog::OnDestroy();

    // TODO: 在此处添加消息处理程序代码

    //窗口关闭时保存选中的标签
    m_last_tab = m_tab_ctrl.GetCurSel();

    // 销毁并释放子窗口内存
    if (m_path_dlg != nullptr) { m_path_dlg->DestroyWindow(); delete m_path_dlg; m_path_dlg = nullptr; }
    if (m_playlist_dlg != nullptr) { m_playlist_dlg->DestroyWindow(); delete m_playlist_dlg; m_playlist_dlg = nullptr; }
    if (m_artist_dlg != nullptr) { m_artist_dlg->DestroyWindow(); delete m_artist_dlg; m_artist_dlg = nullptr; }
    if (m_album_dlg != nullptr) { m_album_dlg->DestroyWindow(); delete m_album_dlg; m_album_dlg = nullptr; }
    if (m_genre_dlg != nullptr) { m_genre_dlg->DestroyWindow(); delete m_genre_dlg; m_genre_dlg = nullptr; }
    if (m_year_dlg != nullptr) { m_year_dlg->DestroyWindow(); delete m_year_dlg; m_year_dlg = nullptr; }
    if (m_type_dlg != nullptr) { m_type_dlg->DestroyWindow(); delete m_type_dlg; m_type_dlg = nullptr; }
    if (m_bitrate_dlg != nullptr) { m_bitrate_dlg->DestroyWindow(); delete m_bitrate_dlg; m_bitrate_dlg = nullptr; }
    if (m_rating_dlg != nullptr) { m_rating_dlg->DestroyWindow(); delete m_rating_dlg; m_rating_dlg = nullptr; }
    if (m_all_media_dlg != nullptr) { m_all_media_dlg->DestroyWindow(); delete m_all_media_dlg; m_all_media_dlg = nullptr; }
    if (m_recent_media_dlg != nullptr) { m_recent_media_dlg->DestroyWindow(); delete m_recent_media_dlg; m_recent_media_dlg = nullptr; }
    if (m_folder_explore_dlg != nullptr) { m_folder_explore_dlg->DestroyWindow(); delete m_folder_explore_dlg; m_folder_explore_dlg = nullptr; }
}


void CMediaLibDlg::OnOK()
{
    // TODO: 在此添加专用代码和/或调用基类
    DestroyWindow();
    // CDialog::OnOK();
}


void CMediaLibDlg::OnCancel()
{
    // TODO: 在此添加专用代码和/或调用基类
    DestroyWindow();
    // CDialog::OnCancel();
}

void CMediaLibDlg::LayoutOnlineProgress()
{
    if (!m_tab_bottom_gap || !m_tab_ctrl.GetSafeHwnd()) return;
    CRect client, tab;
    GetClientRect(client); m_tab_ctrl.GetWindowRect(tab); ScreenToClient(tab);
    const int bottom = client.bottom - m_tab_bottom_gap;
    const int height = m_online_tasks.empty() ? 0 : theApp.DPI(48);
    const int target_bottom = (std::max<LONG>)(tab.top + theApp.DPI(60), bottom - height);
    if (tab.bottom != target_bottom)
    {
        tab.bottom = target_bottom;
        m_tab_ctrl.MoveWindow(tab);
    }
    const CRect progress_rect(tab.left, tab.bottom, tab.right, bottom);
    if (progress_rect != m_online_progress_rect)
    {
        m_online_progress_rect = progress_rect;
        Invalidate();
    }
    if (height == 0 || !m_online_progress.GetSafeHwnd()) return;
    auto move = [this](CWnd& control, const CRect& target) {
        CRect current; control.GetWindowRect(current); ScreenToClient(current);
        if (current != target) control.MoveWindow(target);
    };
    CRect line = m_online_progress_rect;
    line.DeflateRect(theApp.DPI(4), 0);
    line.top += theApp.DPI(4); line.bottom = line.top + theApp.DPI(22);
    CRect cancel = line; cancel.left = cancel.right - theApp.DPI(52);
    move(m_online_cancel, cancel);
    if (!m_online_tasks[m_online_progress_index % m_online_tasks.size()].Running()
        || m_online_tasks[m_online_progress_index % m_online_tasks.size()].cancellable)
        line.right = cancel.left - theApp.DPI(6);
    CRect next = line; next.left = next.right - theApp.DPI(110);
    move(m_online_next, next);
    if (m_online_tasks.size() > 1) line.right = next.left - theApp.DPI(6);
    move(m_online_heading, line);
    CRect detail(m_online_progress_rect.left + theApp.DPI(4), m_online_progress_rect.top + theApp.DPI(27),
        m_online_progress_rect.right - theApp.DPI(4), m_online_progress_rect.bottom - theApp.DPI(3));
    CRect progress = detail;
    progress.left = progress.right - (std::min)(theApp.DPI(180), progress.Width() / 3);
    progress.DeflateRect(0, theApp.DPI(3));
    move(m_online_progress, progress);
    detail.right = progress.left - theApp.DPI(10);
    move(m_online_detail, detail);
}

void CMediaLibDlg::OnSize(UINT type, int cx, int cy)
{
    CBaseDialog::OnSize(type, cx, cy);
    LayoutOnlineProgress();
}

void CMediaLibDlg::OnTimer(UINT_PTR id)
{
    if (id == ONLINE_PROGRESS_TIMER)
    {
        m_online_tasks = online::OnlineProgress::Snapshot();
        UpdateOnlineProgress();
    }
    CBaseDialog::OnTimer(id);
}

void CMediaLibDlg::UpdateOnlineProgress()
{
    LayoutOnlineProgress();
    const bool visible = !m_online_tasks.empty();
    auto show = [](CWnd& control, bool show) {
        if (((control.GetStyle() & WS_VISIBLE) != 0) != show)
            control.ShowWindow(show ? SW_SHOWNA : SW_HIDE);
    };
    for (CWnd* control : {static_cast<CWnd*>(&m_online_heading), static_cast<CWnd*>(&m_online_detail),
        static_cast<CWnd*>(&m_online_progress)})
        show(*control, visible);
    if (!visible)
    {
        show(m_online_cancel, false);
        show(m_online_next, false);
        if (m_online_marquee) m_online_progress.SetMarquee(FALSE, 0);
        m_online_marquee = false;
        m_online_progress.ModifyStyle(PBS_MARQUEE, 0);
        return;
    }
    m_online_progress_index %= m_online_tasks.size();
    const auto& task = m_online_tasks[m_online_progress_index];
    // 原生控件只在内容变化时更新，避免定时轮询导致文字闪烁和按钮焦点丢失。
    auto set_text = [](CWnd& control, const std::wstring& text) {
        CString previous; control.GetWindowText(previous);
        if (previous != text.c_str()) control.SetWindowText(text.c_str());
    };
    set_text(m_online_heading, task.title + L"  ·  " + task.Counter());
    auto phase = task.Timing(online::OnlineProgress::Now()) + L"  ·  " + task.detail;
    std::replace(phase.begin(), phase.end(), L'\n', L' ');
    std::replace(phase.begin(), phase.end(), L'\r', L' ');
    set_text(m_online_detail, phase);
    set_text(m_online_cancel, task.Running() ? L"取消" : L"关闭");
    show(m_online_cancel, !task.Running() || task.cancellable);
    m_online_cancel.EnableWindow(!task.cancelling);
    set_text(m_online_next, L"下一项 " + std::to_wstring(m_online_progress_index + 1) + L"/" + std::to_wstring(m_online_tasks.size()));
    show(m_online_next, m_online_tasks.size() > 1);
    const int percent = task.Percent();
    const bool marquee = percent < 0 && task.Running();
    if (marquee != m_online_marquee)
    {
        m_online_progress.ModifyStyle(marquee ? 0 : PBS_MARQUEE, marquee ? PBS_MARQUEE : 0);
        m_online_progress.SetMarquee(marquee, 40);
        m_online_marquee = marquee;
    }
    if (!marquee) m_online_progress.SetPos((std::max)(0, percent));
    m_online_progress.SendMessage(PBM_SETSTATE, task.result == online::ProgressResult::Failed ? PBST_ERROR
        : task.result == online::ProgressResult::Cancelled ? PBST_PAUSED : PBST_NORMAL);
}

void CMediaLibDlg::OnCancelOnlineTask()
{
    if (m_online_tasks.empty()) return;
    const auto id = m_online_tasks[m_online_progress_index % m_online_tasks.size()].id;
    online::OnlineProgress::RequestCancel(id);
    online::OnlineProgress::Dismiss(id);
    m_online_tasks = online::OnlineProgress::Snapshot();
    UpdateOnlineProgress();
}

void CMediaLibDlg::OnNextOnlineTask()
{
    ++m_online_progress_index;
    UpdateOnlineProgress();
}
