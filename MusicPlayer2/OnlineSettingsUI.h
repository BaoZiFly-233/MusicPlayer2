#pragma once
#include "resource.h"
#include "TabDlg.h"
#include "OnlineSettings.h"
#include "UIPanel/SettingsPanel/SettingsPanelTab.h"

// Shared download setup is used by the toolbar, bulk download and both settings surfaces.
bool ConfigureOnlineDownload(CWnd* owner, bool first_download = false);
bool GetOnlineDownloadDirectory(CWnd* owner, std::wstring& directory, const std::wstring& playlist = L"");
void OpenOnlineSettings(CWnd* owner);

class COnlineDownloadSetupDlg : public CBaseDialog
{
public:
    COnlineDownloadSetupDlg(CWnd* owner, bool first_download);
protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    CString GetDialogName() const override { return L"OnlineDownloadSetup"; }
    afx_msg void OnBrowse();
    afx_msg void OnNameChanged();
    DECLARE_MESSAGE_MAP()
private:
    online::OnlineSettingsData m_data;
    bool m_first_download;
};

class COnlineSettingsTabDlg : public CTabDlg
{
public:
    COnlineSettingsTabDlg(CWnd* owner) : CTabDlg(IDD_ONLINE_SETTINGS_DIALOG, owner) {}
    BOOL OnInitDialog() override;
    void GetDataFromUi() override;
    void ApplyDataToUi() override;
protected:
    afx_msg void OnBrowse();
    afx_msg void OnClearCache();
    afx_msg void OnOpenDownloads();
    DECLARE_MESSAGE_MAP()
};

class CSettingsPanelOnline : public CSettingsPanelTab
{
public:
    explicit CSettingsPanelOnline(std::shared_ptr<UiElement::Panel> root) : CSettingsPanelTab(root) {}
    void Init() override;
    void UpdateSettingsData() override;
    void SettingDataToUi() override;
    void OnSettingsChanged() override;
private:
    online::OnlineSettingsData m_data;
    bool m_prefetch{}, m_daily{};
};
