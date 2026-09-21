#pragma once

#include "BaseDialog.h"
#include "KugouSource.h"
#include <string>

// K源扫码登录对话框。
//
// 打开后会自动取一个登录二维码显示出来，用户用K源App扫码确认即可。
// 二维码有有效期，过期后界面会提示，点「刷新二维码」重新获取。
//
// 登录成功后账号信息写在音源对象里，由调用方负责保存到配置文件。
class CKugouLoginDlg : public CBaseDialog
{
    DECLARE_DYNAMIC(CKugouLoginDlg)

public:
    CKugouLoginDlg(CWnd* pParent = nullptr);
    virtual ~CKugouLoginDlg();

    enum { IDD = IDD_KUGOU_LOGIN_DIALOG };

    // 定时器：轮询扫码状态
    static const UINT TIMER_POLL = 1;

    // 登录是否成功（对话框关闭后由调用方查询）
    bool IsLoginSucceeded() const { return m_login_ok; }

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual CString GetDialogName() const override;

protected:
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    virtual void OnCancel();

    afx_msg void OnBnClickedRefresh();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    DECLARE_MESSAGE_MAP()

private:
    void RefreshQrCode();
    void UpdateStatusText(const wchar_t* text);
    void DrawQrCode();
    // 把二维码内容画到静态控件上。失败时返回 false。
    bool RenderQrToCtrl();

    kugou::CKugouSource* m_source{ nullptr };
    std::wstring m_qr_content;
    CStatic m_qr_static;
    CStatic m_status_static;
    CButton m_refresh_btn;

    // 二维码用 GDI+ 画，画出来的位图需要自己管生命周期
    void* m_qr_bitmap{ nullptr };
    bool m_login_ok{ false };
    bool m_waiting{ false };
};
