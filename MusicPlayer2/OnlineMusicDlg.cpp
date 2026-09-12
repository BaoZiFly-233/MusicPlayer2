#include "stdafx.h"
#include "MusicPlayer2.h"
#include "OnlineMusicDlg.h"
#include "Player.h"
#include "SongInfo.h"

using namespace std;

IMPLEMENT_DYNAMIC(COnlineMusicDlg, CBaseDialog)

const UINT COnlineMusicDlg::WM_ONLINE_SEARCH_DONE;

COnlineMusicDlg::COnlineMusicDlg(CWnd* pParent /*=nullptr*/)
    : CBaseDialog(IDD_ONLINE_MUSIC_DIALOG, pParent)
{
}

COnlineMusicDlg::~COnlineMusicDlg()
{
}

void COnlineMusicDlg::DoDataExchange(CDataExchange* pDX)
{
    CBaseDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_ONLINE_RESULT_LIST, m_result_list);
    DDX_Control(pDX, IDC_ONLINE_SOURCE_COMBO, m_source_combo);
    DDX_Control(pDX, IDC_ONLINE_KEYWORD_EDIT, m_keyword_edit);
    DDX_Control(pDX, IDC_ONLINE_SEARCH_BUTTON, m_search_btn);
}

BEGIN_MESSAGE_MAP(COnlineMusicDlg, CBaseDialog)
    ON_BN_CLICKED(IDC_ONLINE_SEARCH_BUTTON, &COnlineMusicDlg::OnBnClickedSearch)
    ON_NOTIFY(NM_DBLCLK, IDC_ONLINE_RESULT_LIST, &COnlineMusicDlg::OnNMDblclkList)
    ON_MESSAGE(WM_ONLINE_SEARCH_DONE, &COnlineMusicDlg::OnSearchDone)
END_MESSAGE_MAP()

CString COnlineMusicDlg::GetDialogName() const
{
    // 返回非空名称后，对话框会记住窗口大小，并且同一时间只允许开一个
    return _T("OnlineMusicDlg");
}

bool COnlineMusicDlg::InitializeControls()
{
    // 音源下拉框：把已注册的音源都列出来。
    // 放在这里而不是 OnInitDialog，是为了让下拉框宽度能按内容自适应。
    const auto& sources = online::CSourceRegistry::Instance().GetAll();
    for (online::IOnlineSource* src : sources)
        m_source_combo.AddString(src->GetDisplayName().c_str());
    if (!sources.empty())
        m_source_combo.SetCurSel(0);
    else
    {
        // 一个音源都没有时禁用搜索
        m_search_btn.EnableWindow(FALSE);
        m_keyword_edit.EnableWindow(FALSE);
    }
    return false;
}

BOOL COnlineMusicDlg::OnInitDialog()
{
    CBaseDialog::OnInitDialog();

    SetWindowText(theApp.m_str_table.LoadText(L"TXT_ONLINE_MUSIC_TITLE").c_str());

    // 列表列宽依赖窗口实际大小，所以放在这里初始化
    InitListCtrl();

    m_keyword_edit.SetFocus();
    return FALSE;       // 已经自己设置焦点，不需要框架再设
}

void COnlineMusicDlg::InitListCtrl()
{
    m_result_list.SetExtendedStyle(m_result_list.GetExtendedStyle()
        | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);

    CRect rect;
    m_result_list.GetWindowRect(rect);
    const int total = rect.Width() - theApp.DPI(20) - 1;
    const int w_index = theApp.DPI(30);
    const int w_title = total * 32 / 100;
    const int w_artist = total * 24 / 100;
    const int w_album = total * 24 / 100;
    const int w_source = total - w_index - w_title - w_artist - w_album;

    int col = 0;
    m_result_list.InsertColumn(col++, theApp.m_str_table.LoadText(L"TXT_SERIAL_NUMBER").c_str(), LVCFMT_LEFT, w_index);
    m_result_list.InsertColumn(col++, theApp.m_str_table.LoadText(L"TXT_TITLE").c_str(), LVCFMT_LEFT, w_title);
    m_result_list.InsertColumn(col++, theApp.m_str_table.LoadText(L"TXT_ARTIST").c_str(), LVCFMT_LEFT, w_artist);
    m_result_list.InsertColumn(col++, theApp.m_str_table.LoadText(L"TXT_ALBUM").c_str(), LVCFMT_LEFT, w_album);
    m_result_list.InsertColumn(col++, theApp.m_str_table.LoadText(L"TXT_MUSIC_SOURCE").c_str(), LVCFMT_LEFT, w_source);
}

online::IOnlineSource* COnlineMusicDlg::GetCurrentSource() const
{
    int sel = m_source_combo.GetCurSel();
    const auto& sources = online::CSourceRegistry::Instance().GetAll();
    if (sel < 0 || sel >= static_cast<int>(sources.size()))
        return nullptr;
    return sources[sel];
}

void COnlineMusicDlg::OnBnClickedSearch()
{
    if (m_searching)
        return;

    online::IOnlineSource* source = GetCurrentSource();
    if (source == nullptr)
        return;

    CString keyword;
    m_keyword_edit.GetWindowText(keyword);
    if (keyword.IsEmpty())
        return;

    // 清空上次结果
    m_result_list.DeleteAllItems();
    m_tracks.clear();
    m_searching = true;
    m_search_btn.EnableWindow(FALSE);
    m_result_list.InsertItem(0, theApp.m_str_table.LoadText(L"TXT_SEARCHING").c_str());

    // 网络请求放到后台线程，避免界面卡住。
    // 音源指针在这里（主线程）取好放进任务，线程里不再碰界面。
    SearchTask* task = new SearchTask();
    task->keyword = keyword.GetString();
    task->hwnd = GetSafeHwnd();
    task->source = source;
    task->source_name = source->GetDisplayName();
    AfxBeginThread(SearchThreadFunc, task);
}

UINT COnlineMusicDlg::SearchThreadFunc(LPVOID param)
{
    SearchTask* task = static_cast<SearchTask*>(param);
    if (task == nullptr)
        return 0;

    // 线程里只调音源接口，不碰任何界面控件
    if (task->source != nullptr)
    {
        bool ok = task->source->Search(task->keyword, 1, task->result.tracks);
        task->result.success = ok;
        if (!ok)
            task->result.error = task->source->GetLastError();
    }
    else
    {
        task->result.success = false;
        task->result.error = L"没有可用的音乐来源";
    }

    // 结果交回主线程处理，任务对象也由主线程释放
    ::PostMessage(task->hwnd, COnlineMusicDlg::WM_ONLINE_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(task));
    return 0;
}

LRESULT COnlineMusicDlg::OnSearchDone(WPARAM wParam, LPARAM lParam)
{
    SearchTask* task = reinterpret_cast<SearchTask*>(lParam);
    if (task == nullptr)
        return 0;

    m_searching = false;
    m_search_btn.EnableWindow(TRUE);
    ShowResult(task->result);
    delete task;
    return 0;
}

void COnlineMusicDlg::ShowResult(const SearchResult& result)
{
    m_result_list.DeleteAllItems();
    m_tracks = result.tracks;

    if (!result.success)
    {
        CString msg;
        msg.Format(L"%s", result.error.empty() ? L"搜索失败" : result.error.c_str());
        m_result_list.InsertItem(0, msg);
        return;
    }

    for (size_t i = 0; i < m_tracks.size(); ++i)
    {
        const online::Track& t = m_tracks[i];
        CString index;
        index.Format(L"%d", static_cast<int>(i + 1));
        int row = m_result_list.InsertItem(static_cast<int>(i), index);
        m_result_list.SetItemText(row, 1, t.title.c_str());
        m_result_list.SetItemText(row, 2, t.artist.c_str());
        m_result_list.SetItemText(row, 3, t.album.c_str());
        m_result_list.SetItemText(row, 4, result.source_name.c_str());
    }
}

void COnlineMusicDlg::OnNMDblclkList(NMHDR* pNMHDR, LRESULT* pResult)
{
    PlaySelected();
    if (pResult != nullptr)
        *pResult = 0;
}

void COnlineMusicDlg::PlaySelected()
{
    POSITION pos = m_result_list.GetFirstSelectedItemPosition();
    if (pos == nullptr)
        return;

    int row = m_result_list.GetNextSelectedItem(pos);
    if (row < 0 || row >= static_cast<int>(m_tracks.size()))
        return;

    const online::Track& t = m_tracks[row];

    // 把搜索结果变成播放器认识的曲目。
    // 这里存的是虚拟地址（如 kugou://xxx），播放时由音源层换成真实网络地址。
    SongInfo song;
    song.file_path = t.virtual_path;
    song.title = t.title;
    song.artist = t.artist;
    song.album = t.album;
    // 曲目时长由结束位置表示：SongInfo::length() 等于 end_pos - start_pos
    if (t.duration_ms > 0)
        song.end_pos.fromInt(t.duration_ms);

    vector<SongInfo> songs{ song };
    CPlayer::GetInstance().OpenSongsInTempPlaylist(songs, 0, true);
    CPlayer::GetInstance().PlayTrack(0, false);
}

void COnlineMusicDlg::OnOK()
{
    // 回车时触发搜索而不是关闭对话框
    OnBnClickedSearch();
}

void COnlineMusicDlg::OnCancel()
{
    CBaseDialog::OnCancel();
}
