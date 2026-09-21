#pragma once
#include "OnlineMusicModel.h"
#include "OnlineDailyRewards.h"
#include "UserUi.h"
#include "ColorConvert.h"
#include "Player.h"
#include "CMediaLibDlg.h"
#include <fstream>

// 独立命令行预览：使用实际皮肤绘制器和合成曲目，离屏输出，不播放、不保存用户设置。
class COnlineMusicPreview
{
public:
    static bool Run(const std::wstring& directory)
    {
        CCommon::CreateDir(directory);
        theApp.LoadImgResource();
        theApp.m_font_set.Init(theApp.m_str_table.GetDefaultFontName().c_str());
        theApp.m_lyric_setting_data.lyric_font.name = theApp.m_str_table.GetDefaultFontName();
        theApp.m_lyric_setting_data.lyric_font.size = 18;
        theApp.m_lyric_setting_data.lyric_line_space = 8;
        theApp.m_font_set.lyric.SetFont(theApp.m_lyric_setting_data.lyric_font);
        theApp.m_font_set.lyric_translate.SetFont(theApp.m_lyric_setting_data.lyric_font);
        theApp.m_ui_data.show_playlist = false;
        theApp.m_ui_data.narrow_mode = false;
        theApp.m_ui_data.enable_background = true;
        theApp.m_ui_data.default_background.Load((theApp.m_local_dir + L"default_background.jpg").c_str());
        theApp.m_app_setting_data.enable_background = true;
        theApp.m_app_setting_data.show_window_frame = false;
        CColorConvert::ConvertColor(theApp.m_app_setting_data.theme_color);

        CWnd owner;
        if (!owner.CreateEx(0, AfxRegisterWndClass(0), L"BoTapMusic native preview", WS_POPUP, CRect(0, 0, 1020, 740), nullptr, 0)) return false;
        CWnd* old_owner = theApp.m_pMainWnd; theApp.m_pMainWnd = &owner;
        auto& model = COnlineMusicModel::Instance();
        COnlineDailyRewards::Instance().Configure(theApp.m_config_dir);
        model.m_started = true;
        model.m_state = {};
        model.m_state.page = COnlineMusicModel::Page::Search;
        model.m_state.status = L"12 首歌曲";
        model.m_state.query = L"纯音乐"; model.m_state.query_revision = 1;
        static const wchar_t* titles[] = {L"雨后街灯", L"晨间来信", L"山间小径", L"晚风与海", L"经过旧车站", L"午后的咖啡馆", L"远方的微光", L"云上旅行", L"星夜漫步", L"春日序曲", L"沿途风景", L"慢慢听"};
        for (int i = 0; i < 12; ++i)
        {
            online::BrowseItem item;
            item.title = item.track.title = titles[i];
            item.subtitle = item.track.artist = i % 2 ? L"城市室内乐团" : L"林间钢琴";
            item.track.album = L"日常旋律";
            item.track.virtual_path = L"bodian://" + std::to_wstring(i + 1);
            item.track.duration_ms = (180 + i * 11) * 1000;
            model.m_state.items.push_back(std::move(item));
        }
        // 预览里也摆出列表的几种状态：第 3 首已收藏（实心红心）、第 5 首播放失败过（灰色）
        {
            SongInfo saved; saved.file_path = L"bodian://3"; saved.title = titles[2]; saved.artist = L"林间钢琴";
            model.m_local = {saved};
            model.m_unplayable = {L"bodian://5"};
        }
        model.Publish(true);
        SongInfo playing; playing.file_path = L"bodian://preview"; playing.title = L"雨后街灯";
        playing.artist = L"林间钢琴"; playing.album = L"日常旋律";
        CPlayer::GetInstance().GetPlayList() = {playing};
        const auto initial_state = model.m_state;
        const auto initial_local = model.m_local;

        struct Case { UINT resource; int width, height; bool dark; const wchar_t* file; const wchar_t* skin = nullptr; bool narrow = false; int detail = 0; };
        const Case cases[] = {
            {IDR_UI2, 1020, 740, false, L"modern-light.png"},
            {IDR_UI2, 1020, 740, true, L"modern-dark.png"},
            {IDR_UI2, 480, 680, true, L"modern-compact.png"},
            {IDR_UI1, 600, 740, false, L"classic-light.png"},
            {IDR_UI1, 600, 740, true, L"classic-dark.png"},
            {IDR_UI2, 480, 680, false, L"modern-narrow.png", nullptr, true},
            {0, 1020, 740, false, L"groove-light.png", L"02_grooveMusic.xml"},
            {0, 1020, 740, true, L"groove-win11-dark.png", L"03_grooveMusicWin11.xml"},
            {0, 480, 680, true, L"groove-narrow.png", L"02_grooveMusic.xml", true},
            {IDR_UI2, 1020, 740, false, L"cache-light.png", nullptr, false, 1},
            {IDR_UI2, 480, 680, true, L"kugou-account-narrow.png", nullptr, true, 2},
            {IDR_UI2, 480, 680, false, L"bodian-account-narrow.png", nullptr, true, 3},
            {IDR_UI2, 1020, 740, false, L"playing-line-lyrics.png", nullptr, false, 4},
            {IDR_UI2, 1020, 740, false, L"playing-word-lyrics.png", nullptr, false, 5},
            {IDR_UI2, 480, 680, false, L"playing-modern-narrow.png", nullptr, true, 4},
            {0, 480, 680, true, L"playing-groove-narrow.png", L"02_grooveMusic.xml", true, 4},
            {0, 1020, 740, false, L"playing-groove-win11.png", L"03_grooveMusicWin11.xml", false, 4},
            {IDR_UI2, 1020, 740, false, L"online-importing.png", nullptr, false, 6},
            {IDR_UI2, 480, 680, true, L"progress-multiple-narrow.png", nullptr, true, 6},
            {IDR_UI2, 600, 220, true, L"progress-small-player.png", nullptr, false, 4},
            {IDR_UI2, 1020, 740, false, L"online-local-clear.png", nullptr, false, 7},
            {IDR_UI2, 480, 680, true, L"online-account-notice.png", nullptr, true, 8}
        };
        bool success = true;
        std::ofstream interaction_log(directory + L"progress-interactions.log");
        auto check = [&](bool condition, const char* text) {
            interaction_log << (condition ? "PASS " : "FAIL ") << text << '\n';
            success = condition && success;
        };
        std::vector<std::shared_ptr<online::OnlineProgress>> preview_progress;
        auto reset_progress = [&]() {
            for (auto& progress : preview_progress)
                if (progress) progress->Finish(online::ProgressResult::Cancelled, L"预览状态结束");
            preview_progress.clear();
            for (const auto& task : online::OnlineProgress::Snapshot())
                if (!task.Running()) online::OnlineProgress::Dismiss(task.id);
        };
        auto start_progress = [&](const std::wstring& title, const std::wstring& detail,
            bool background = false, bool cancellable = true, bool queued = false) {
            auto progress = online::OnlineProgress::Start(title, detail, background, cancellable, queued);
            preview_progress.push_back(progress);
            return progress;
        };
        for (const auto& test : cases)
        {
            reset_progress();
            model.m_state = initial_state;
            model.m_local = initial_local;
            model.m_state.importing = false;
            model.m_state.notice.clear();
            model.m_state.quality_note = test.detail == 0 ? L"当前播放：无损" : L"";
            model.m_state.page = test.detail >= 2 ? COnlineMusicModel::Page::Account : COnlineMusicModel::Page::Search;
            if (test.detail == 7) model.m_state.page = COnlineMusicModel::Page::Local;
            if (test.detail == 6) model.m_state.page = COnlineMusicModel::Page::Search;
            model.m_state.source = test.detail == 3 ? 1 : 0;
            model.m_state.detail_visible = test.detail != 0;
            if (test.detail == 2 || test.detail == 3 || test.detail == 8)
            {
                model.ShowAccount();
                model.m_state.detail = L"已登录\n\n用户名：音乐爱好者\n会员：SVIP\n有效期：2026-12-31\n\n使用更多菜单管理账号";
                model.m_state.status.clear();
                if (test.detail == 8) model.m_state.notice = L"提示条示例：签到成功 / 操作结果会显示在这里";
            }
            else if (test.detail == 1)
            {
                model.m_state.detail = L"预缓存下一首：已开启\n\n已用 128 MB / 1024 MB · 24 个文件\n下载中 / 排队：0 首\n下载：成功 2 首，失败 0 首\n\n下载完成";
                model.m_state.status = L"下载与缓存 · 示例状态";
            }
            else if (test.detail == 6)
            {
                model.m_state.detail_visible = false;
                model.m_state.status = L"正在换源 · 37 / 100";
            }
            else if (test.detail == 7)
            {
                model.m_local.clear();
                for (int i = 0; i < 12; ++i)
                {
                    SongInfo song; song.file_path = L"kugou://preview-" + std::to_wstring(i);
                    song.title = std::wstring(L"本地示例歌曲 ") + std::to_wstring(i + 1);
                    song.artist = L"示例歌手"; song.album = L"示例专辑"; song.end_pos.fromInt(200000);
                    model.m_local.push_back(std::move(song));
                }
                model.ShowLocal();
                model.m_state.notice = L"提示条示例：在线本地歌单已清空 / 可迁移到原生播放列表";
            }
            switch (test.detail)
            {
            case 0:
            {
                auto progress = start_progress(L"搜索歌曲", L"正在接收搜索结果");
                break;
            }
            case 1:
            {
                auto progress = start_progress(L"下载 · 预览歌曲", L"正在下载", false, true);
                progress->Transfer(6ULL * 1024 * 1024, 24ULL * 1024 * 1024);
                break;
            }
            case 2:
            {
                auto progress = start_progress(L"扫码登录 · K源", L"二维码已就绪，等待手机扫码确认");
                break;
            }
            case 3:
            {
                auto progress = start_progress(L"刷新账号信息", L"服务返回错误");
                progress->Finish(online::ProgressResult::Failed, L"服务暂时不可用，请稍后重试");
                break;
            }
            case 4:
            case 5:
            {
                auto progress = start_progress(L"准备播放 · 预览歌曲", L"正在连接音频并缓冲");
                progress->Update(L"正在缓冲", 3, 5);
                break;
            }
            case 6:
            {
                auto progress = start_progress(L"换源到B源", L"正在匹配：第 37 首", false, true);
                progress->Update(L"正在匹配：第 37 首", 37, 100);
                start_progress(L"导入外部歌单", L"等待网络任务", true, true, true);
                auto failed = start_progress(L"读取歌词", L"平台没有这首歌词", true, false);
                failed->Finish(online::ProgressResult::Failed, L"平台没有这首歌词");
                auto downloading = start_progress(L"下载 · 晚风与海", L"正在下载", true, true);
                downloading->Transfer(6 * 1024 * 1024, 24 * 1024 * 1024);
                start_progress(L"获取封面", L"正在连接封面服务", true, true);
                break;
            }
            case 7:
            {
                auto progress = start_progress(L"清理在线缓存", L"缓存已清理，手动保存的歌曲已保留", true, false);
                progress->Finish(online::ProgressResult::Succeeded, L"缓存已清理");
                break;
            }
            case 8:
            {
                auto progress = start_progress(L"领取每日权益", L"正在取消", true, true);
                online::OnlineProgress::RequestCancel(progress->Id());
                progress->Finish(online::ProgressResult::Cancelled, L"已取消");
                break;
            }
            default: break;
            }
            model.Publish();
            auto& player = CPlayer::GetInstance();
            player.m_current_position.fromInt(1500); player.m_song_length.fromInt(240000);
            theApp.m_lyric_setting_data.lyric_karaoke_disp = true;
            player.m_Lyrics = CLyrics{};
            player.m_Lyrics.LyricsFromRowString(test.detail == 5
                ? L"[00:00.00]<00:00.00>雨后的<00:01.00>街灯，<00:02.00>照亮了<00:03.00>回家的路<00:04.00>\n[00:05.00]晚风把心事轻轻吹散\n[00:10.00]远处的光一直都在"
                : L"[00:00.00]雨后的街灯，照亮了回家的路\n[00:05.00]晚风把心事轻轻吹散\n[00:10.00]远处的光一直都在");
            theApp.m_ui_data.draw_area_width = test.width;
            theApp.m_ui_data.draw_area_height = test.height;
            theApp.m_app_setting_data.dark_mode = test.dark;
            theApp.m_ui_data.narrow_mode = test.narrow;
            CImage canvas; canvas.Create(test.width, test.height, 24);
            CDC dc; dc.Attach(canvas.GetDC());
            {
                std::unique_ptr<CUserUi> skin;
                if (test.skin) skin = std::make_unique<CUserUi>(&owner, theApp.m_local_dir + L"skins\\" + test.skin, theApp.m_ui_data);
                else skin = std::make_unique<CUserUi>(&owner, test.resource, theApp.m_ui_data);
                skin->Init(&dc); skin->DrawInfo(true);
                if (test.detail < 4 || test.detail >= 6)
                {
                    success &= skin->ToggleOnlineMusic();
                    skin->DrawInfo(true);
                    success &= skin->IsOnlineMusicVisible();
                }
                check(!skin->m_activity_rect.IsRectEmpty(), "progress visible in current skin and layout");
                if (test.detail == 6)
                {
                    const auto expand = skin->m_activity_expand_rect.CenterPoint();
                    skin->LButtonDown(expand); skin->LButtonUp(expand); skin->DrawInfo(true);
                    check(skin->m_activity_expanded && skin->m_activity_rows == 4, "expand shows concurrent tasks");
                    const auto card = skin->m_activity_rect.CenterPoint();
                    skin->MouseWheel(-120, card); skin->DrawInfo(true);
                    check(skin->m_activity_offset == 1, "wheel reveals remaining tasks");
                    skin->MouseWheel(120, card); skin->DrawInfo(true);
                    check(skin->m_activity_offset == 0, "wheel returns to the active task");
                    const auto cancel = skin->m_activity_cancel_rects.front().first.CenterPoint();
                    skin->LButtonDown(cancel); skin->LButtonUp(cancel); skin->DrawInfo(true);
                    check(preview_progress.front()->Cancelled(), "cancel targets the displayed task");
                }
            }
            dc.Detach(); canvas.ReleaseDC();
            success &= SUCCEEDED(canvas.Save((directory + test.file).c_str()));
        }
        reset_progress();
        auto library_progress = start_progress(L"换源到B源", L"正在匹配：晚风与海");
        library_progress->Update(L"正在匹配：晚风与海", 7, 20);
        const auto old_config_path = theApp.m_config_path;
        theApp.m_config_path = directory + L"native-preview.ini";
        CMediaLibDlg library(1, &owner);
        if (library.Create(IDD_MEDIA_LIB_DIALOG, &owner))
        {
            library.SetWindowPos(nullptr, 0, 0, 860, 650, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            library.OnTimer(19731);
            CRect client, tabs, window, close;
            library.GetClientRect(client);
            library.GetWindowRect(window);
            library.GetDlgItem(IDCANCEL)->GetWindowRect(close); library.ScreenToClient(close);
            check(close.bottom <= client.bottom, "media library footer remains inside the client area");
            library.m_tab_ctrl.GetWindowRect(tabs); library.ScreenToClient(tabs);
            check(!library.m_online_progress_rect.IsRectEmpty() && tabs.bottom <= library.m_online_progress_rect.top,
                "media library reserves progress space outside the song list");
            CImage canvas; canvas.Create(window.Width(), window.Height(), 24);
            CDC dc; dc.Attach(canvas.GetDC());
            library.SendMessage(WM_PRINT, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
            check(library.m_online_progress.GetPos() == 35 && !library.m_online_marquee,
                "media library uses native determinate progress");
            library.m_online_cancel.SendMessage(BM_CLICK);
            check(library_progress->Cancelled(), "media library cancel targets the shared task");
            dc.Detach(); canvas.ReleaseDC();
            check(SUCCEEDED(canvas.Save((directory + L"progress-media-library.png").c_str())), "media library progress preview exported");
            auto waiting = start_progress(L"读取云歌单", L"等待服务返回");
            library.OnTimer(19731);
            check(library.m_online_marquee && (library.m_online_progress.GetStyle() & PBS_MARQUEE),
                "media library uses marquee for unknown totals");
            library.m_online_next.SendMessage(BM_CLICK);
            check(library.m_online_progress_index == 1 && !library.m_online_marquee,
                "native next button selects the other task");
            reset_progress();
            library.OnTimer(19731);
            library.m_tab_ctrl.GetWindowRect(tabs); library.ScreenToClient(tabs);
            check(!(library.m_online_progress.GetStyle() & WS_VISIBLE)
                && tabs.bottom == client.bottom - library.m_tab_bottom_gap,
                "completed tasks release the media library progress area");
            library.DestroyWindow();
        }
        else check(false, "create media library preview");
        DeleteFileW(theApp.m_config_path.c_str());
        theApp.m_config_path = old_config_path;
        reset_progress();
        model.Shutdown();
        owner.DestroyWindow(); theApp.m_pMainWnd = old_owner;
        return success;
    }
};
