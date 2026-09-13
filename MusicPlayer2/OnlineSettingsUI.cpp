#include "stdafx.h"
#include "OnlineSettingsUI.h"
#include "OnlineMediaCache.h"
#include "OnlineDailyRewards.h"
#include "MusicPlayer2.h"
#include "UIElement/CombinedElement/ToggleSettingGroup.h"
#include "UIElement/ComboBox.h"
#include "UIElement/Button.h"
#include "UIElement/Text.h"
#include <filesystem>

using namespace std;
using namespace online;
namespace
{
void FillNameOrder(CWnd* owner, int selection)
{
    auto* combo = static_cast<CComboBox*>(owner->GetDlgItem(IDC_ONLINE_NAME_ORDER));
    combo->ResetContent();
    for (int i = 0; i < 3; ++i) combo->AddString(COnlineSettings::NameOrderText(i));
    combo->SetCurSel(selection);
}
void ChooseDirectory(CWnd* owner)
{
    CString current; owner->GetDlgItemTextW(IDC_ONLINE_DOWNLOAD_DIR, current);
    CFolderPickerDialog dialog(current.IsEmpty() ? nullptr : current.GetString(), OFN_PATHMUSTEXIST, owner);
    if (dialog.DoModal() == IDOK) owner->SetDlgItemTextW(IDC_ONLINE_DOWNLOAD_DIR, dialog.GetPathName());
}
void ClearOnlineCache(CWnd* owner)
{
    if (MessageBoxW(owner->GetSafeHwnd(), L"清理缓存会取消尚未完成的下载。已保存的音乐会保留。", L"清理在线缓存", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
        COnlineMediaCache::Instance().Clear();
}
void OpenDownloadFolder(CWnd* owner)
{
    auto path = COnlineSettings::Instance().Get().download_directory;
    if (path.empty()) { ConfigureOnlineDownload(owner); path = COnlineSettings::Instance().Get().download_directory; }
    if (!path.empty()) ShellExecuteW(owner->GetSafeHwnd(), L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}
BEGIN_MESSAGE_MAP(COnlineDownloadSetupDlg, CBaseDialog)
    ON_BN_CLICKED(IDC_ONLINE_BROWSE, &COnlineDownloadSetupDlg::OnBrowse)
    ON_CBN_SELCHANGE(IDC_ONLINE_NAME_ORDER, &COnlineDownloadSetupDlg::OnNameChanged)
END_MESSAGE_MAP()
COnlineDownloadSetupDlg::COnlineDownloadSetupDlg(CWnd* owner, bool first_download)
    : CBaseDialog(IDD_ONLINE_DOWNLOAD_SETUP, owner), m_data(COnlineSettings::Instance().Get()), m_first_download(first_download) {}
BOOL COnlineDownloadSetupDlg::OnInitDialog()
{
    CBaseDialog::OnInitDialog();
    SetWindowTextW(m_first_download ? L"首次下载设置" : L"下载设置");
    SetDlgItemTextW(IDC_ONLINE_DOWNLOAD_DIR, m_data.download_directory.c_str());
    FillNameOrder(this, static_cast<int>(m_data.name_order));
    CheckDlgButton(IDC_ONLINE_PLAYLIST_FOLDER, m_data.playlist_subfolder);
    OnNameChanged(); return TRUE;
}
void COnlineDownloadSetupDlg::OnBrowse() { ChooseDirectory(this); }
void COnlineDownloadSetupDlg::OnNameChanged()
{
    const int order = static_cast<CComboBox*>(GetDlgItem(IDC_ONLINE_NAME_ORDER))->GetCurSel();
    Track example; example.title = L"歌曲名称"; example.artist = L"歌手名称";
    SetDlgItemTextW(IDC_ONLINE_NAME_EXAMPLE, (L"示例：" + COnlineSettings::DownloadName(example, static_cast<DownloadNameOrder>(order)) + L".flac").c_str());
}
void COnlineDownloadSetupDlg::OnOK()
{
    CString directory; GetDlgItemTextW(IDC_ONLINE_DOWNLOAD_DIR, directory);
    error_code error;
    if (directory.IsEmpty() || !filesystem::is_directory(directory.GetString(), error))
    { MessageBoxW(L"请选择可用的下载文件夹。", L"下载设置", MB_ICONINFORMATION); return; }
    m_data.download_directory = directory.GetString();
    m_data.name_order = static_cast<DownloadNameOrder>(static_cast<CComboBox*>(GetDlgItem(IDC_ONLINE_NAME_ORDER))->GetCurSel());
    m_data.playlist_subfolder = IsDlgButtonChecked(IDC_ONLINE_PLAYLIST_FOLDER) == BST_CHECKED;
    if (!COnlineSettings::Instance().Save(m_data)) { MessageBoxW(L"下载设置保存失败，请检查磁盘空间。", L"下载设置", MB_ICONERROR); return; }
    CBaseDialog::OnOK();
}
bool ConfigureOnlineDownload(CWnd* owner, bool first_download) { COnlineDownloadSetupDlg dialog(owner, first_download); return dialog.DoModal() == IDOK; }
bool GetOnlineDownloadDirectory(CWnd* owner, wstring& directory, const wstring& playlist)
{
    auto data = COnlineSettings::Instance().Get(); error_code error;
    if (data.download_directory.empty() || !filesystem::is_directory(data.download_directory, error))
    {
        if (!ConfigureOnlineDownload(owner, true)) return false;
        data = COnlineSettings::Instance().Get();
    }
    directory = data.download_directory;
    if (data.playlist_subfolder && !playlist.empty())
    {
        directory = (filesystem::path(directory) / COnlineSettings::SafeFileName(playlist)).wstring();
        filesystem::create_directories(directory, error);
        if (error) { MessageBoxW(owner->GetSafeHwnd(), L"无法创建歌单下载文件夹。", L"下载", MB_ICONERROR); return false; }
    }
    return true;
}
void OpenOnlineSettings(CWnd* owner) { theApp.m_pMainWnd->PostMessage(WM_OPTION_SETTINGS, 6, reinterpret_cast<LPARAM>(owner)); }
BEGIN_MESSAGE_MAP(COnlineSettingsTabDlg, CTabDlg)
    ON_BN_CLICKED(IDC_ONLINE_BROWSE, &COnlineSettingsTabDlg::OnBrowse)
    ON_BN_CLICKED(IDC_ONLINE_CLEAR_CACHE, &COnlineSettingsTabDlg::OnClearCache)
    ON_BN_CLICKED(IDC_ONLINE_OPEN_DOWNLOADS, &COnlineSettingsTabDlg::OnOpenDownloads)
END_MESSAGE_MAP()
BOOL COnlineSettingsTabDlg::OnInitDialog() { CTabDlg::OnInitDialog(); ApplyDataToUi(); return TRUE; }
void COnlineSettingsTabDlg::ApplyDataToUi()
{
    const auto data = COnlineSettings::Instance().Get();
    SetDlgItemTextW(IDC_ONLINE_DOWNLOAD_DIR, data.download_directory.empty() ? L"首次下载时选择" : data.download_directory.c_str());
    FillNameOrder(this, static_cast<int>(data.name_order));
    CheckDlgButton(IDC_ONLINE_PLAYLIST_FOLDER, data.playlist_subfolder);
    CheckDlgButton(IDC_ONLINE_AUTO_LYRIC, data.auto_lyrics); CheckDlgButton(IDC_ONLINE_AUTO_COVER, data.auto_cover);
    CheckDlgButton(IDC_ONLINE_PREFETCH, COnlineMediaCache::Instance().Enabled());
    CheckDlgButton(IDC_ONLINE_DAILY, COnlineDailyRewards::Instance().Enabled());
}
void COnlineSettingsTabDlg::GetDataFromUi()
{
    auto data = COnlineSettings::Instance().Get(); CString directory; GetDlgItemTextW(IDC_ONLINE_DOWNLOAD_DIR, directory);
    if (directory != L"首次下载时选择") data.download_directory = directory.GetString();
    data.name_order = static_cast<DownloadNameOrder>(static_cast<CComboBox*>(GetDlgItem(IDC_ONLINE_NAME_ORDER))->GetCurSel());
    data.playlist_subfolder = IsDlgButtonChecked(IDC_ONLINE_PLAYLIST_FOLDER) == BST_CHECKED;
    data.auto_lyrics = IsDlgButtonChecked(IDC_ONLINE_AUTO_LYRIC) == BST_CHECKED;
    data.auto_cover = IsDlgButtonChecked(IDC_ONLINE_AUTO_COVER) == BST_CHECKED;
    if (!COnlineSettings::Instance().Save(data)) { MessageBoxW(L"在线设置保存失败，请检查磁盘空间。", L"在线音乐", MB_ICONERROR); return; }
    COnlineMediaCache::Instance().SetEnabled(IsDlgButtonChecked(IDC_ONLINE_PREFETCH) == BST_CHECKED);
    COnlineDailyRewards::Instance().SetEnabled(IsDlgButtonChecked(IDC_ONLINE_DAILY) == BST_CHECKED);
}
void COnlineSettingsTabDlg::OnBrowse() { ChooseDirectory(this); }
void COnlineSettingsTabDlg::OnClearCache() { ClearOnlineCache(this); }
void COnlineSettingsTabDlg::OnOpenDownloads() { OpenDownloadFolder(this); }
void CSettingsPanelOnline::Init()
{
    using namespace UiElement;
    auto* names = m_root_element->FindElement<ComboBox>("onlineNameOrder");
    for (int i = 0; i < 3; ++i) names->AddString(COnlineSettings::NameOrderText(i));
    names->SetSelectionChangedTrigger([this](ComboBox* sender) { UpdateSettingsData(); m_data.name_order = static_cast<DownloadNameOrder>(sender->GetCurSel()); OnSettingsChanged(); });
    ConnectToggleTrigger(m_root_element->FindElement<ToggleSettingGroup>("onlinePlaylistFolder"), m_data.playlist_subfolder);
    ConnectToggleTrigger(m_root_element->FindElement<ToggleSettingGroup>("onlineAutoLyrics"), m_data.auto_lyrics);
    ConnectToggleTrigger(m_root_element->FindElement<ToggleSettingGroup>("onlineAutoCover"), m_data.auto_cover);
    ConnectToggleTrigger(m_root_element->FindElement<ToggleSettingGroup>("onlinePrefetch"), m_prefetch);
    ConnectToggleTrigger(m_root_element->FindElement<ToggleSettingGroup>("onlineDailyReward"), m_daily);
    m_root_element->FindElement<Button>("onlineDownloadConfig")->SetClickedTrigger([this](Button*) { ConfigureOnlineDownload(theApp.m_pMainWnd); UpdateSettingsData(); SettingDataToUi(); });
    m_root_element->FindElement<Button>("onlineClearCache")->SetClickedTrigger([](Button*) { ClearOnlineCache(theApp.m_pMainWnd); });
    m_root_element->FindElement<Button>("onlineOpenDownloads")->SetClickedTrigger([](Button*) { OpenDownloadFolder(theApp.m_pMainWnd); });
}
void CSettingsPanelOnline::UpdateSettingsData() { m_data = COnlineSettings::Instance().Get(); m_prefetch = COnlineMediaCache::Instance().Enabled(); m_daily = COnlineDailyRewards::Instance().Enabled(); }
void CSettingsPanelOnline::SettingDataToUi()
{
    m_root_element->FindElement<UiElement::Text>("onlineDownloadPath")->SetText(m_data.download_directory.empty() ? L"首次下载时选择" : m_data.download_directory);
    m_root_element->FindElement<UiElement::ComboBox>("onlineNameOrder")->SetCurSel(static_cast<int>(m_data.name_order));
    for (const auto& [id, checked] : vector<pair<const char*, bool>>{{"onlinePlaylistFolder", m_data.playlist_subfolder},
        {"onlineAutoLyrics", m_data.auto_lyrics}, {"onlineAutoCover", m_data.auto_cover}, {"onlinePrefetch", m_prefetch}, {"onlineDailyReward", m_daily}})
        m_root_element->FindElement<UiElement::ToggleSettingGroup>(id)->GetToggleBtn()->SetChecked(checked);
}
void CSettingsPanelOnline::OnSettingsChanged()
{
    if (!COnlineSettings::Instance().Save(m_data)) { MessageBoxW(theApp.m_pMainWnd->GetSafeHwnd(), L"在线设置保存失败，请检查磁盘空间。", L"在线音乐", MB_ICONERROR); UpdateSettingsData(); return; }
    COnlineMediaCache::Instance().SetEnabled(m_prefetch); COnlineDailyRewards::Instance().SetEnabled(m_daily); SettingDataToUi();
}
