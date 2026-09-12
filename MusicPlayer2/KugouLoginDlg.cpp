#include "stdafx.h"
#include "MusicPlayer2.h"
#include "KugouLoginDlg.h"
#include "KugouCrypto.h"
#include "qrcodegen/qrcodegen.hpp"
#include <gdiplus.h>
#include <vector>

using namespace std;
using namespace qrcodegen;

IMPLEMENT_DYNAMIC(CKugouLoginDlg, CBaseDialog)

const UINT CKugouLoginDlg::TIMER_POLL;

CKugouLoginDlg::CKugouLoginDlg(CWnd* pParent /*=nullptr*/)
    : CBaseDialog(IDD_KUGOU_LOGIN_DIALOG, pParent)
{
    // 音源从注册表里取，登录成功后写进去的账号信息对全局生效
    online::IOnlineSource* src = online::CSourceRegistry::Instance().FindByScheme(L"kugou");
    m_source = dynamic_cast<kugou::CKugouSource*>(src);
}

CKugouLoginDlg::~CKugouLoginDlg()
{
    if (m_qr_bitmap != nullptr)
    {
        delete static_cast<Gdiplus::Bitmap*>(m_qr_bitmap);
        m_qr_bitmap = nullptr;
    }
}

void CKugouLoginDlg::DoDataExchange(CDataExchange* pDX)
{
    CBaseDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_KUGOU_QR_STATIC, m_qr_static);
    DDX_Control(pDX, IDC_KUGOU_LOGIN_STATUS_STATIC, m_status_static);
    DDX_Control(pDX, IDC_KUGOU_REFRESH_BUTTON, m_refresh_btn);
}

BEGIN_MESSAGE_MAP(CKugouLoginDlg, CBaseDialog)
    ON_BN_CLICKED(IDC_KUGOU_REFRESH_BUTTON, &CKugouLoginDlg::OnBnClickedRefresh)
    ON_WM_TIMER()
END_MESSAGE_MAP()

CString CKugouLoginDlg::GetDialogName() const
{
    return _T("KugouLoginDlg");
}

BOOL CKugouLoginDlg::OnInitDialog()
{
    CBaseDialog::OnInitDialog();

    SetWindowText(theApp.m_str_table.LoadText(L"TXT_KUGOU_LOGIN_TITLE").c_str());

    if (m_source == nullptr)
    {
        UpdateStatusText(L"音源不可用");
        m_refresh_btn.EnableWindow(FALSE);
        return TRUE;
    }

    if (m_source->IsLoggedIn())
    {
        UpdateStatusText(L"已登录");
        m_refresh_btn.EnableWindow(FALSE);
        return TRUE;
    }

    RefreshQrCode();
    return TRUE;
}

void CKugouLoginDlg::RefreshQrCode()
{
    if (m_source == nullptr)
        return;

    UpdateStatusText(L"正在获取二维码…");

    wstring content;
    if (!m_source->GetQrCode(content))
    {
        wstring err = L"获取二维码失败：" + m_source->GetLastError();
        UpdateStatusText(err.c_str());
        return;
    }

    m_qr_content = content;
    if (!RenderQrToCtrl())
    {
        UpdateStatusText(L"二维码绘制失败");
        return;
    }

    UpdateStatusText(L"请用酷狗App扫描二维码");

    // 每 2 秒问一次服务端扫了没有
    m_waiting = true;
    SetTimer(TIMER_POLL, 2000, nullptr);
}

// 把二维码内容渲染成位图贴到静态控件上
bool CKugouLoginDlg::RenderQrToCtrl()
{
    if (m_qr_content.empty())
        return false;

    // 生成二维码矩阵。纠错级别用 Medium，兼顾容错和尺寸。
    const QrCode qr = QrCode::encodeText(kugou::ToUtf8(m_qr_content).c_str(), QrCode::Ecc::MEDIUM);
    const int modules = qr.getSize();

    // 每个模块画多少像素，以及在控件里的留白
    const int ctrl_size = 220;
    const int quiet_zone = 4;                       // 二维码规范要求的静默区
    const int total_modules = modules + quiet_zone * 2;
    const int scale = max(2, ctrl_size / total_modules);
    const int img_size = total_modules * scale;

    // 用 GDI+ 画，先画到内存位图再交给静态控件
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(img_size, img_size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));

        Gdiplus::SolidBrush black(Gdiplus::Color(255, 0, 0, 0));
        for (int y = 0; y < modules; ++y)
        {
            for (int x = 0; x < modules; ++x)
            {
                if (qr.getModule(x, y))
                {
                    g.FillRectangle(&black,
                        (x + quiet_zone) * scale, (y + quiet_zone) * scale,
                        scale, scale);
                }
            }
        }
    }

    // 换掉上一张
    if (m_qr_bitmap != nullptr)
        delete static_cast<Gdiplus::Bitmap*>(m_qr_bitmap);
    m_qr_bitmap = bmp;

    // 贴到静态控件（用 HBITMAP 更简单，这里转一下）
    HBITMAP hbmp = nullptr;
    bmp->GetHBITMAP(Gdiplus::Color(255, 255, 255), &hbmp);
    if (hbmp == nullptr)
        return false;

    // 用静态控件的 SS_BITMAP 方式显示，控件会自己释放旧图
    m_qr_static.SetBitmap(hbmp);
    return true;
}

void CKugouLoginDlg::UpdateStatusText(const wchar_t* text)
{
    if (m_status_static.GetSafeHwnd() != nullptr)
        m_status_static.SetWindowText(text);
}

void CKugouLoginDlg::OnBnClickedRefresh()
{
    KillTimer(TIMER_POLL);
    m_waiting = false;
    RefreshQrCode();
}

void CKugouLoginDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == TIMER_POLL && m_waiting && m_source != nullptr)
    {
        switch (m_source->CheckQrCode())
        {
        case kugou::CKugouSource::QrStatus::Waiting:
            // 还在等，什么都不做
            break;

        case kugou::CKugouSource::QrStatus::Scanned:
            UpdateStatusText(L"已扫描，请在手机上确认");
            break;

        case kugou::CKugouSource::QrStatus::Expired:
            KillTimer(TIMER_POLL);
            m_waiting = false;
            UpdateStatusText(L"二维码已过期，请点「刷新二维码」");
            break;

        case kugou::CKugouSource::QrStatus::Authorized:
            KillTimer(TIMER_POLL);
            m_waiting = false;
            m_login_ok = true;
            UpdateStatusText(L"登录成功");
            // 让用户看到成功提示再关
            SetTimer(TIMER_POLL + 1, 800, nullptr);
            break;

        default:
            KillTimer(TIMER_POLL);
            m_waiting = false;
            {
                wstring err = L"登录出错：" + m_source->GetLastError();
                UpdateStatusText(err.c_str());
            }
            break;
        }
    }
    else if (nIDEvent == TIMER_POLL + 1)
    {
        KillTimer(TIMER_POLL + 1);
        EndDialog(IDOK);
    }

    CBaseDialog::OnTimer(nIDEvent);
}

void CKugouLoginDlg::OnOK()
{
    // 回车不关对话框，避免误触（真正关闭走成功或取消）
}

void CKugouLoginDlg::OnCancel()
{
    KillTimer(TIMER_POLL);
    KillTimer(TIMER_POLL + 1);
    CBaseDialog::OnCancel();
}
