#pragma once

#include "BaseDialog.h"
#include "ListCtrlEx.h"
#include "OnlineSource.h"
#include "SongInfo.h"
#include <vector>
#include <string>

// 在线音乐搜索对话框。
//
// 用来搜索酷狗概念版和波点音乐上的歌曲，双击结果即可加入播放列表并播放。
// 搜索放在后台线程里做，避免网络慢的时候界面卡住。
class COnlineMusicDlg : public CBaseDialog
{
    DECLARE_DYNAMIC(COnlineMusicDlg)

public:
    COnlineMusicDlg(CWnd* pParent = nullptr);
    virtual ~COnlineMusicDlg();

    enum { IDD = IDD_ONLINE_MUSIC_DIALOG };

    // 搜索完成的通知消息
    static const UINT WM_ONLINE_SEARCH_DONE = WM_USER + 201;
    // 用户双击选好了要播的歌。用消息投递把关闭动作推迟到消息循环里做，
    // 避免在通知消息处理过程中销毁窗口（那会让通用控件访问已释放对象）。
    static const UINT WM_ONLINE_PLAY_SELECTED = WM_USER + 202;

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual CString GetDialogName() const override;
    virtual bool InitializeControls() override;

    // 用户双击选中的曲目。对话框以 IDOK 关闭后，由调用方取走并播放。
    bool HasSelection() const { return m_has_selection; }
    const SongInfo& GetSelectedSong() const { return m_selected; }

protected:
    // 供后台线程回传结果
    struct SearchResult
    {
        std::vector<online::Track> tracks;
        std::wstring error;
        std::wstring source_name;
        bool success{ false };
    };

    // 后台线程只处理一个任务，用指针传递，主线程负责释放。
    // 音源指针由主线程取好后放进来，线程里不再访问界面。
    struct SearchTask
    {
        std::wstring keyword;
        online::IOnlineSource* source{ nullptr };
        std::wstring source_name;
        HWND hwnd{ nullptr };
        SearchResult result;
    };

    static UINT SearchThreadFunc(LPVOID param);

    virtual BOOL OnInitDialog();
    virtual void OnOK();
    virtual void OnCancel();

    afx_msg void OnBnClickedSearch();
    afx_msg void OnNMDblclkList(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg LRESULT OnSearchDone(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnPlaySelected(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    void InitListCtrl();
    void ShowResult(const SearchResult& result);
    void PlaySelected();
    online::IOnlineSource* GetCurrentSource() const;

    CListCtrlEx m_result_list;
    CComboBox m_source_combo;
    CEdit m_keyword_edit;
    CButton m_search_btn;

    // 当前列表对应的曲目，与列表行一一对应
    std::vector<online::Track> m_tracks;
    bool m_searching{ false };

    // 双击选中的曲目，对话框关闭后交给主窗口播放
    SongInfo m_selected;
    bool m_has_selection{ false };
};
