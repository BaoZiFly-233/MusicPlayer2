#pragma once
#include "OnlineMusicModel.h"
#include "OnlineDailyRewards.h"
#include "UserUi.h"
#include "ColorConvert.h"
#include "Player.h"
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
        model.Publish(true);
        SongInfo playing; playing.file_path = L"bodian://preview"; playing.title = L"雨后街灯";
        playing.artist = L"林间钢琴"; playing.album = L"日常旋律";
        CPlayer::GetInstance().GetPlayList() = {playing};

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
            {0, 1020, 740, false, L"playing-groove-win11.png", L"03_grooveMusicWin11.xml", false, 4}
        };
        bool success = true;
        for (const auto& test : cases)
        {
            model.m_state.page = test.detail >= 2 ? COnlineMusicModel::Page::Account : COnlineMusicModel::Page::Search;
            model.m_state.source = test.detail == 3 ? 1 : 0;
            model.m_state.detail_visible = test.detail != 0;
            if (test.detail == 2 || test.detail == 3)
            {
                model.ShowAccount();
                model.m_state.detail = L"已登录\n\n用户名：音乐爱好者\n会员：SVIP\n有效期：2026-12-31\n\n使用更多菜单管理账号";
                model.m_state.status.clear();
            }
            else if (test.detail == 1)
            {
                model.m_state.detail = L"预缓存下一首：已开启\n\n已用 128 MB / 1024 MB · 24 个文件\n下载中 / 排队：0 首\n下载：成功 2 首，失败 0 首\n\n下载完成";
                model.m_state.status = L"下载与缓存 · 示例状态";
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
                if (test.detail < 4)
                {
                    success &= skin->ToggleOnlineMusic();
                    skin->DrawInfo(true);
                    success &= skin->IsOnlineMusicVisible();
                }
            }
            dc.Detach(); canvas.ReleaseDC();
            success &= SUCCEEDED(canvas.Save((directory + test.file).c_str()));
        }
        model.Shutdown();
        owner.DestroyWindow(); theApp.m_pMainWnd = old_owner;
        return success;
    }
};
