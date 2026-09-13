#pragma once
#include "Playlist.h"
#include "OnlineJson.h"
#include "OnlineMusicModel.h"
#include "OnlineMediaCache.h"
#include "OnlinePlaylistImport.h"
#include "KugouKrc.h"
#include "BodianSource.h"
#include "KugouSource.h"
#include "bass.h"
#include <sstream>

// 显式命令行自检，不自动播放、不修改用户歌单、不记录账号或播放地址。
inline bool RunOnlineMusicTests(const std::wstring& log_path, bool network)
{
    std::ostringstream log;
    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        log << (condition ? "PASS " : "FAIL ") << name << "\n";
        if (!condition) ++failures;
    };
    wchar_t temp_dir[MAX_PATH]{}, temp_name[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_dir);
    if (!GetTempFileNameW(temp_dir, L"bot", 0, temp_name)) return false;
    DeleteFileW(temp_name);
    if (!CreateDirectoryW(temp_name, nullptr)) return false;
    std::wstring root = std::wstring(temp_name) + L"\\";
    std::vector<std::wstring> files;
    auto fixture = [&](const wchar_t* name, const std::string& text) {
        std::wstring path = root + name; files.push_back(path);
        std::ofstream stream(path, std::ios::binary); stream << text; return path;
    };
    SongInfo song;
    song.file_path = L"kugou://0123456789ABCDEF0123456789ABCDEF?aaid=42";
    song.title = L"测试, 旋律 - 第二乐章"; song.artist = L"歌手 - 合奏"; song.album = L"专辑, 一";
    song.end_pos.fromInt(123000);
    SongInfo second = song; second.file_path = L"bodian://12345"; second.title = L"第二首";
    std::vector<SongInfo> songs{song, second};
    {
        CPlayer player;
        player.m_playlist = songs; player.m_index = 0; player.m_repeat_mode = RM_PLAY_ORDER;
        check(player.GetNextTrack() == second, "prefetch follows sequential playback");
        player.m_next_tracks = {0};
        check(player.GetNextTrack() == song, "explicit next queue takes priority");
        player.m_next_tracks.clear(); player.m_repeat_mode = RM_PLAY_RANDOM;
        player.PrepareNextTrack(); const auto candidate = player.GetNextTrack();
        player.PrepareNextTrack();
        check(!candidate.IsEmpty() && player.GetNextTrack() == candidate && player.m_random_list.empty()
            && player.m_index == 0, "random prefetch keeps one candidate without changing playback history");
        player.m_playlist[player.m_prepared_random].file_path = L"bodian://replacement";
        check(player.GetNextTrack().IsEmpty(), "replaced random candidate invalidates prefetch");
        player.PrepareNextTrack();
        check(!player.GetNextTrack().IsEmpty(), "random next song is prepared again after playlist changes");
        player.m_playlist = songs; player.m_repeat_mode = RM_PLAY_SHUFFLE;
        player.m_shuffle_list = {0, 1}; player.m_shuffle_index = 1; player.m_is_shuffle_list_played = true;
        player.PrepareNextTrack(); const auto shuffled = player.GetNextTrack();
        check(!shuffled.IsEmpty() && player.m_shuffle_index == 1 && player.m_shuffle_list == std::vector<int>({0, 1}),
            "shuffle prefetch prepares next cycle without changing current history");
        player.InitShuffleList();
        check(player.m_playlist[player.m_shuffle_list.front()] == shuffled && player.GetNextTrack() == shuffled,
            "next shuffle cycle consumes the prefetched candidate");
        player.m_repeat_mode = RM_LOOP_PLAYLIST; player.m_playlist.clear();
        check(player.GetNextTrack().IsEmpty(), "empty loop playlist has no next song");
    }
    for (auto type : {CPlaylistFile::PL_PLAYLIST, CPlaylistFile::PL_M3U8, CPlaylistFile::PL_JSON})
    {
        const wchar_t* name = type == CPlaylistFile::PL_JSON ? L"roundtrip.btplaylist" : type == CPlaylistFile::PL_M3U8 ? L"roundtrip.m3u8" : L"roundtrip.playlist";
        std::wstring path = root + name; files.push_back(path);
        check(CPlaylistFile::SavePlaylistToFile(songs, path, type), "write playlist");
        CPlaylistFile loaded;
        bool ok = loaded.LoadFromFile(path);
        const auto& read = loaded.GetPlaylist();
        check(ok && read.size() == 2 && read[0].file_path == song.file_path && read[1].file_path == second.file_path, "virtual paths roundtrip");
        check(ok && read.size() == 2 && read[0].title == song.title && read[0].artist == song.artist && read[0].album == song.album
            && read[0].length().toInt() == 123000, "metadata roundtrip");
        loaded.LoadFromFile(path);
        check(loaded.GetPlaylist().size() == 2, "reload does not accumulate");
        HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (locked != INVALID_HANDLE_VALUE)
        {
            check(!CPlaylistFile::SavePlaylistToFile({second}, path, type), "locked destination reports failure");
            CloseHandle(locked);
            loaded.LoadFromFile(path);
            check(loaded.GetPlaylist().size() == 2, "failed save preserves destination");
        }
        else check(false, "lock fixture");
    }
    auto mixed = fixture(L"comments.m3u8", "\xef\xbb\xbf#EXTM3U\n# ignored\n\n#EXTINF:12,Artist - Title, Part 2\n# another comment\nbodian://42\n\nrelative.mp3\n");
    CPlaylistFile loaded;
    loaded.LoadFromFile(mixed);
    check(loaded.GetPlaylist().size() == 2 && loaded.GetPlaylist()[0].title == L"Title, Part 2"
        && loaded.GetPlaylist()[0].artist == L"Artist" && loaded.GetPlaylist()[0].length().toInt() == 12000
        && loaded.GetPlaylist()[1].file_path == root + L"relative.mp3", "M3U comments BOM commas duration relative path");
    auto invalid = fixture(L"invalid.btplaylist", R"({"format":"BoTapMusic","version":1,"songs":[{"path":"bodian://42"},{"path":null}]})");
    check(!loaded.LoadFromFile(invalid) && loaded.GetPlaylist().empty(), "invalid JSON rejects complete import");
    auto future = fixture(L"future.btplaylist", R"({"format":"BoTapMusic","version":99,"songs":[]})");
    check(!loaded.LoadFromFile(future), "unknown JSON version rejected");
    CPlaylistFile dedupe;
    check(dedupe.AddSongsToPlaylist({song, song, second}) == 2, "playlist deduplication");
    check(!online::IsServiceId(L"42?token=secret") && online::IsServiceId(L"collection_42"), "service identifiers validated");
    auto kg = online::KugouTrack(nlohmann::json{{"FileHash","0123456789abcdef0123456789abcdef"},{"MixSongID",42},{"Duration","123"},{"OriSongName","title"}});
    check(kg.virtual_path.find(L"?aaid=42") != std::wstring::npos && kg.duration_ms == 123000, "numeric and string API fields");
    check(!online::KugouTrack(nlohmann::json{{"hash","not-a-hash"}}).IsValid(), "invalid track identifiers rejected");
    check(online::BodianTrack(nlohmann::json{{"id",42},{"duration",123}}).virtual_path == L"bodian://42", "recommendation numeric IDs");
    std::wstring playlist_id, playlist_source;
    check(online::ParseBodianPlaylistId(L"12345_4", playlist_id, playlist_source) && playlist_id == L"12345" && playlist_source == L"4", "Bodian playlist source retained");
    check(online::ParseBodianPlaylistId(L"12345_13", playlist_id, playlist_source) && playlist_source == L"13", "Bodian native home playlist source retained");
    check(!online::ParseBodianPlaylistId(L"12345_6", playlist_id, playlist_source)
        && !online::ParseBodianPlaylistId(L"123?uid=x_4", playlist_id, playlist_source), "Bodian playlist rejects invalid source and query injection");
    online::BrowseResult playlists;
    online::AddBodianPlaylist(playlists, nlohmann::json{{"id", 12345}, {"source", 4}, {"name", "fixture"}});
    check(playlists.items.size() == 1 && playlists.items.front().id == L"12345_4", "Bodian search playlist identity roundtrip");
    auto reward = nlohmann::json{{"list", {{{"day", "2026-01-02"}, {"receive_vip", "1"}}}}};
    check(online::KugouRewardReceived(reward, "2026-01-02") && !online::KugouRewardReceived(reward, "2026-01-03"), "daily reward checks exact day and received flag");
    reward["list"][0]["receive_vip"] = 0;
    check(!online::KugouRewardReceived(reward, "2026-01-02"), "unclaimed reward never reports VIP received");
    check(!bodian::CBodianSource::Account{"-1", "token"}.IsLoggedIn()
        && !bodian::CBodianSource::Account{"42", "token\r\ninjected: header"}.IsLoggedIn(), "Bodian account validates ID and header boundaries");
    {
        bodian::CBodianSource account;
        account.SetAccount({"42", "synthetic-test-token"}); account.SaveIdentity(root);
        files.push_back(root + L"bodian_account.dat"); files.push_back(root + L"bodian.ini");
        bodian::CBodianSource restored; restored.LoadIdentity(root);
        check(restored.IsLoggedIn() && restored.GetAccount().token == "synthetic-test-token", "Bodian DPAPI credentials roundtrip");
        std::ifstream encrypted(root + L"bodian_account.dat", std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(encrypted)), std::istreambuf_iterator<char>());
        check(bytes.find("synthetic-test-token") == std::string::npos, "Bodian persisted credentials are encrypted");
        encrypted.close(); restored.Logout(); restored.SaveIdentity(root);
        check(GetFileAttributesW((root + L"bodian_account.dat").c_str()) == INVALID_FILE_ATTRIBUTES, "Bodian logout removes persisted credentials");
    }
    check(online::COnlineMediaCache::AudioExtension("fLaC" + std::string(20, '\0')) == L".flac"
        && online::COnlineMediaCache::AudioExtension("ID3" + std::string(20, '\0')) == L".mp3", "cache detects audio format from bytes");
    check(online::COnlineMediaCache::AudioExtension("<html>not audio</html>").empty(), "cache rejects HTML as audio");
    online::AccountProfile membership;
    online::ReadKugouMembership(nlohmann::json{{"is_vip", 0}, {"busi_vip", nlohmann::json::array({
        {{"busi_type", "concept"}, {"product_type", "svip"}, {"is_vip", 1}, {"vip_end_time", "2030-01-02 03:04:05"}}})}}, membership);
    check(membership.membership == L"SVIP" && !membership.expires.empty(), "concept VIP overrides standard-account non-VIP flag");
    online::AccountProfile missing_membership; online::ReadKugouMembership(nlohmann::json::object(), missing_membership);
    check(missing_membership.membership.empty(), "missing membership data is not reported as non-VIP");
    auto bd_profile = online::ReadBodianProfile(nlohmann::json{{"userInfo", {{"nickname", "Fixture"}, {"isVip", 0}}}, {"payInfo", {{"isVipBoolean", true}}}});
    check(bd_profile.name == L"Fixture" && bd_profile.membership == L"VIP", "Bodian profile reads display name and paid VIP flag");
    check(online::CoverUrl(nlohmann::json{{"trans_param", {{"union_cover", "https://example.test/{size}/cover.jpg"}}}})
        == L"https://example.test/500/cover.jpg", "cover template selects image size");
    if (!online::RunMediaCacheFileTests(root, log)) ++failures;
    auto play_response = nlohmann::json{{"status", 1}, {"priv_status", 1}, {"url", {"https://example.test/song.mp3"}}, {"timeLength", 293}};
    check(online::ParseKugouPlayback(play_response, true).url == L"https://example.test/song.mp3", "Kugou root URL array");
    check(online::ParseKugouPlayback(nlohmann::json{{"data", {{"url", "https://example.test/legacy.mp3"}}}}, true).url
        == L"https://example.test/legacy.mp3", "Kugou legacy wrapped URL string");
    play_response["url"] = nlohmann::json::array(); play_response["backupUrl"] = {nullptr, "https://example.test/backup.mp3"};
    check(online::ParseKugouPlayback(play_response, true).url == L"https://example.test/backup.mp3", "Kugou camel-case backup URL array");
    play_response["priv_status"] = 0;
    auto restricted = online::ParseKugouPlayback(play_response, true);
    check(restricted.url.empty() && restricted.retry_quality, "restricted quality is not treated as playable");
    auto signature_error = online::ParseKugouPlayback(nlohmann::json{{"status", 0}, {"errcode", "20006"}, {"errmsg", "err signature"}}, true);
    check(signature_error.url.empty() && !signature_error.retry_quality && signature_error.error.find(L"20006") != std::wstring::npos,
        "Kugou signature failure reported without quality retries");
    check(online::ParseKugouPlayback(nlohmann::json{{"status", 1}, {"url", {42, "file:///invalid"}}}, true).url.empty(),
        "invalid playback URL types rejected");
    {
        using Model = COnlineMusicModel;
        Model model;
        online::BrowseItem item; item.track = online::BodianTrack(nlohmann::json{{"id",42},{"name","fixture"}});
        model.m_state.items = {item}; model.Publish(true);
        auto first = model.Snapshot();
        Model::Command play{Model::Action::Play}; play.rows = {0}; play.revision = first->revision;
        model.Execute(play);
        Model::Playback playback;
        check(model.TakePlayback(playback) && playback.songs.size() == 1 && !playback.append, "selected song dispatch");
        model.Publish(true); model.Execute(play);
        check(!model.TakePlayback(playback), "stale selection cannot play replaced rows");
        check(first->revision != model.Snapshot()->revision && first->items.size() == 1, "published snapshots remain immutable");
        auto revision = model.Snapshot()->revision;
        model.m_task = std::make_shared<Model::Task>();
        model.m_task->append = true; model.m_task->success = true;
        model.m_task->request.page = 2; model.m_task->result.items = {item}; model.m_task->done = true;
        model.Poll(false);
        check(model.Snapshot()->revision == revision && model.Snapshot()->items.size() == 2
            && model.Snapshot()->request.page == 2, "pagination appends without invalidating selection");
        model.m_task = std::make_shared<Model::Task>();
        auto cancelled = model.m_task;
        model.Suspend();
        check(cancelled->cancelled && !model.Snapshot()->busy && model.m_interrupted
            && model.Snapshot()->request.page == 1, "hidden page cancels browse and resumes from first page");
        cancelled->result.items.clear(); cancelled->success = true; cancelled->done = true;
        model.Poll(false);
        check(model.Snapshot()->items.size() == 2, "cancelled completion cannot replace current rows");
        model.m_state.page = Model::Page::Search; model.SelectPage();
        check(!model.m_interrupted && model.Snapshot()->items.empty(), "page switch clears interrupted request");
        model.m_task = std::make_shared<Model::Task>(); model.m_task->import_all = true;
        model.Suspend();
        check(!model.m_interrupted, "cancelled full import is not resumed as browsing");
        model.Shutdown();
    }
    if (network)
    {
        for (auto* source : online::CSourceRegistry::Instance().GetAll())
        {
            log << "\nSOURCE " << kugou::ToUtf8(source->GetScheme()) << "\n";
            for (auto kind : {online::BrowseKind::Hot, online::BrowseKind::Charts, online::BrowseKind::Search})
            {
                try
                {
                    online::BrowseResult result;
                    bool ok = source->Browse({kind, L"晴天", 1}, result);
                    log << "browse " << static_cast<int>(kind) << " count=" << result.items.size()
                        << " error=" << kugou::ToUtf8(source->GetLastError()) << "\n";
                    check(ok && !result.items.empty(), "live public browse");
                    if (ok && kind == online::BrowseKind::Search && !result.items.empty())
                    {
                        auto track = result.items.front().track; track.cover_url.clear();
                        check(!source->GetCoverUrl(track).empty(), "live cover lookup for saved track metadata");
                        // 酷狗歌词：现在优先取带逐字时间轴的版本，验证解密和格式转换能走通
                        if (source->GetScheme() == L"kugou")
                        {
                            online::Lyric lyric;
                            const bool got_lyric = source->GetLyric(track.virtual_path, lyric);
                            log << "kugou lyric chars=" << lyric.content.size()
                                << " error=" << kugou::ToUtf8(source->GetLastError()) << "\n";
                            check(got_lyric, "live kugou lyric lookup");
                            if (got_lyric)
                            {
                                CLyrics parsed;
                                parsed.LyricsFromRowString(lyric.content);
                                bool word_timing = false;
                                for (int li = 0; li < 40; ++li)
                                {
                                    const auto line = parsed.GetLyric(li);
                                    if (line.text.empty()) break;
                                    if (line.HasWordTiming()) { word_timing = true; break; }
                                }
                                log << "kugou lyric word_timing=" << word_timing << "\n";
                                check(word_timing, "live kugou lyric carries word timing");
                            }
                        }
                    }
                    if (ok && kind == online::BrowseKind::Charts && !result.items.empty())
                    {
                        online::BrowseResult tracks;
                        bool drill = source->Browse({online::BrowseKind::ChartTracks, result.items.front().id, 1}, tracks);
                        log << "chart tracks count=" << tracks.items.size() << " error=" << kugou::ToUtf8(source->GetLastError()) << "\n";
                        check(drill && !tracks.items.empty() && tracks.items.front().track.IsValid(), "live chart drilldown");
                        if (drill && tracks.has_more && source->GetScheme() == L"bodian")
                        {
                            online::BrowseResult next;
                            check(source->Browse({online::BrowseKind::ChartTracks, result.items.front().id, 2}, next)
                                && !next.items.empty() && next.items.front().track.virtual_path != tracks.items.front().track.virtual_path,
                                "live Bodian rank pagination advances");
                        }
                        if (drill && !tracks.items.empty())
                        {
                            online::Lyric lyric;
                            bool has_lyric = source->GetLyric(tracks.items.front().track.virtual_path, lyric);
                            log << "lyric available=" << has_lyric << " characters=" << lyric.content.size() << "\n";
                        }
                    }
                }
                catch (const std::exception&) { check(false, "live response parsing"); }
            }
            if (source->GetScheme() == L"bodian")
            {
                online::BrowseResult result;
                check(source->Browse({online::BrowseKind::Recommend}, result) && !result.items.empty(), "live Bodian recommendations");
                bodian::CBodianSource anonymous;
                online::BrowseResult curated;
                bool curated_ok = anonymous.Browse({online::BrowseKind::Playlists}, curated);
                check(curated_ok && !curated.items.empty(), "live Bodian home curated playlists");
                if (curated_ok && !curated.items.empty())
                {
                    online::BrowseResult tracks;
                    check(anonymous.Browse({online::BrowseKind::PlaylistTracks, curated.items.front().id, 1}, tracks)
                        && !tracks.items.empty(), "live Bodian native source 13 playlist tracks");
                }
                result = {};
                bool found = source->Browse({online::BrowseKind::PlaylistSearch, L"纯音乐", 1}, result);
                check(found && !result.items.empty(), "live Bodian playlist search");
                if (found && !result.items.empty())
                {
                    online::BrowseResult tracks;
                    check(source->Browse({online::BrowseKind::PlaylistTracks, result.items.front().id, 1}, tracks)
                        && !tracks.items.empty() && tracks.items.front().track.IsValid(), "live Bodian playlist tracks with source");
                    if (tracks.has_more)
                    {
                        online::BrowseResult second_page;
                        check(source->Browse({online::BrowseKind::PlaylistTracks, result.items.front().id, 2}, second_page)
                            && !second_page.items.empty() && second_page.items.front().track.virtual_path != tracks.items.front().track.virtual_path,
                            "live Bodian playlist pagination advances");
                    }
                }
                bodian::CBodianSource login(*dynamic_cast<bodian::CBodianSource*>(source));
                std::wstring qr;
                bool qr_ok = login.GetQrCode(qr);
                check(qr_ok && qr.starts_with(L"https://bodian-oia.kuwo.cn/"), "live Bodian signed login QR request");
                if (qr_ok)
                {
                    const auto status = login.CheckQrCode();
                    check(status == online::QrStatus::Waiting, "live Bodian QR waiting status");
                    if (status != online::QrStatus::Waiting) log << "Bodian QR error=" << kugou::ToUtf8(login.GetLastError()) << "\n";
                }
            }
            else if (auto* account = dynamic_cast<kugou::CKugouSource*>(source); account && account->IsLoggedIn())
            {
                std::wstring day; bool received{};
                check(account->GetDailyRewardRecord(day, received) && day.size() == 10, "live Kugou daily reward record read only");
                online::AccountProfile profile;
                const bool loaded = account->GetProfile(profile);
                check(loaded && !profile.name.empty(), "live Kugou account display name");
                check(loaded && !profile.membership.empty(), "live Kugou concept membership status");
            }
        }
    }
    for (const auto& path : files) DeleteFileW(path.c_str());
    RemoveDirectoryW(temp_name);
    // ---- 酷狗 KRC 逐字歌词：格式转换（离线）----
    {
        // KRC 是「[行起始,行时长]<字相对起始,字时长,0>字…」，要转成播放器能识别的
        // 扩展歌词格式「[行绝对时间]<字绝对时间>字」。
        // 这里用 ASCII 歌词：MSVC 会把窄字符串字面量转成系统 ANSI 编码，
        // 用中文的话测试数据的编码会和运行时的 UTF-8 数据不一致。
        const std::string sample =
            "[ti:Title]\n[ar:Artist]\n[offset:0]\n"
            "[0,1000]<0,300,0>AB<300,300,0>CD\n"
            "[1000,1500]<0,500,0>EF<500,500,0>GH<1000,500,0>\n";
        const std::wstring converted = kugou::KrcToExtendedLyric(sample);
        check(!converted.empty(), "krc converts to extended lyric");
        check(converted.find(L"[00:00.000]<00:00.000>AB<00:00.300>CD") != std::wstring::npos,
            "krc word times are absolute from line start");
        // 第二行的字时间要加上行起始 1000ms
        check(converted.find(L"[00:01.000]<00:01.000>EF<00:01.500>GH<00:02.000>") != std::wstring::npos,
            "krc second line offsets by line start");
        // 元数据行不该被当成歌词
        check(converted.find(L"ti:") == std::wstring::npos, "krc metadata lines skipped");
        // 转换结果要能被播放器自己的歌词解析器认出来，并且识别为逐字
        {
            CLyrics parsed;
            parsed.LyricsFromRowString(converted);
            const auto first_line = parsed.GetLyric(0);
            const auto second_line = parsed.GetLyric(1);
            const auto past_end = parsed.GetLyric(2);
            check(first_line.text == L"ABCD" && second_line.text == L"EFGH" && past_end.text.empty(),
                "krc converted lyric parses as two lines");
            check(first_line.HasWordTiming() && second_line.HasWordTiming(),
                "krc converted lyric carries word timing");
        }
        // 不是 KRC 的输入要返回空，好让调用方退回普通歌词
        check(kugou::KrcToExtendedLyric("not a lyric").empty(), "non krc input rejected");
        check(kugou::KrcToExtendedLyric("").empty(), "empty krc input rejected");
        check(kugou::DecryptKrc("").empty(), "empty krc decrypt rejected");
    }
    // ---- 歌单跨平台导入：解析与匹配（纯离线，不联网）----
    {
        using namespace online;
        const auto ref_netease = ParseShareText(L"https://music.163.com/playlist?id=3778678");
        check(ref_netease.source == ImportSource::Netease && ref_netease.id == L"3778678", "netease playlist link");
        const auto ref_qq = ParseShareText(L"https://y.qq.com/n/ryqq/playlist/7011264340");
        check(ref_qq.source == ImportSource::QQ && ref_qq.id == L"7011264340", "qq playlist link");
        // 用户从 App 复制出来的是一整段话，链接夹在中间
        const auto ref_prose = ParseShareText(L"分享歌单《测试》http://music.163.com/playlist?id=12345 来自@网易云音乐");
        check(ref_prose.source == ImportSource::Netease && ref_prose.id == L"12345", "share text with prose");
        check(ParseShareText(L"http://163cn.tv/abc123").source == ImportSource::Netease, "netease short link recognized");
        check(!ParseShareText(L"https://example.com/whatever").IsValid(), "unknown link rejected");

        check(Similarity(L"abc", L"abc") == 100, "similarity identical");
        check(Similarity(L"", L"abc") == 0, "similarity empty");

        {   // 正常命中
            ImportTrack source; source.title = L"戒烟"; source.artist = L"李荣浩"; source.duration_ms = 293000;
            std::vector<Track> candidates;
            Track good; good.virtual_path = L"kugou://AAA"; good.title = L"戒烟"; good.artist = L"李荣浩"; good.duration_ms = 293000;
            Track other; other.virtual_path = L"kugou://BBB"; other.title = L"晴天"; other.artist = L"周杰伦"; other.duration_ms = 269000;
            candidates.push_back(good); candidates.push_back(other);
            const auto best = PickBest(source, candidates);
            check(best.level == MatchLevel::Auto && best.candidate.virtual_path == L"kugou://AAA", "match picks correct song");
        }
        {   // 版本词：带 (Live) 的不能压过原版，哪怕它歌名歌手都接近满分
            ImportTrack source; source.title = L"Lip & Hip"; source.artist = L"泫雅"; source.duration_ms = 209000;
            std::vector<Track> candidates;
            Track live; live.virtual_path = L"kugou://LIVE"; live.title = L"Lip & Hip (Live)"; live.artist = L"泫雅"; live.duration_ms = 208300;
            Track studio; studio.virtual_path = L"kugou://STUDIO"; studio.title = L"Lip & Hip"; studio.artist = L"泫雅"; studio.duration_ms = 209000;
            candidates.push_back(live); candidates.push_back(studio);
            const auto best = PickBest(source, candidates);
            check(best.candidate.virtual_path == L"kugou://STUDIO", "live version not preferred");
        }
        {   // 歌手别名：中文名 vs 括号里的外文名，拆分后两两比较才匹配得上
            ImportTrack source; source.title = L"Lip & Hip"; source.artist = L"泫雅/郑镒勋";
            std::vector<Track> candidates;
            Track one; one.virtual_path = L"kugou://MIX"; one.title = L"Lip & Hip"; one.artist = L"泫雅 (HyunA)&정일훈";
            candidates.push_back(one);
            const auto best = PickBest(source, candidates);
            check(best.candidate.virtual_path == L"kugou://MIX", "artist alias with brackets matches");
        }
        {   // 候选没返回时长时，这一项不该被当成 0 分拖低总分
            ImportTrack source; source.title = L"戒烟"; source.artist = L"李荣浩"; source.duration_ms = 293000;
            std::vector<Track> candidates;
            Track no_duration; no_duration.virtual_path = L"kugou://NODUR"; no_duration.title = L"戒烟"; no_duration.artist = L"李荣浩";
            candidates.push_back(no_duration);
            const auto best = PickBest(source, candidates);
            check(best.level == MatchLevel::Auto, "missing candidate duration is not penalized");
        }
        {   // 60 秒试听片段要被时长规则筛掉
            ImportTrack source; source.title = L"Lip & Hip"; source.artist = L"泫雅"; source.duration_ms = 209000;
            std::vector<Track> candidates;
            Track clip; clip.virtual_path = L"kugou://CLIP"; clip.title = L"Lip & Hip"; clip.artist = L"泫雅"; clip.duration_ms = 60000;
            candidates.push_back(clip);
            const auto best = PickBest(source, candidates);
            check(best.level == MatchLevel::NotFound, "60s preview rejected by duration");
        }
    }
    log << "\nfailures=" << failures << "\n";
    std::ofstream output(log_path, std::ios::binary); output << log.str();
    return failures == 0 && output.good();
}

// 仅在显式 --playback 自检时读取当前临时歌单，通过解码流检查真实音频，不输出声音或地址。
inline bool RunOnlinePlaybackTest(const std::wstring& log_path, const std::wstring& playlist_path, const std::wstring& runtime_dir)
{
    std::ofstream log(log_path, std::ios::binary);
    CPlaylistFile playlist;
    if (!playlist.LoadFromFile(playlist_path)) { log << "FAIL read playlist\n"; return false; }
    auto song = std::find_if(playlist.GetPlaylist().begin(), playlist.GetPlaylist().end(), [](const SongInfo& item) {
        return online::CSourceRegistry::GetScheme(item.file_path) == L"kugou";
    });
    if (song == playlist.GetPlaylist().end()) { log << "FAIL no Kugou song in current temporary playlist\n"; return false; }
    auto* source = online::CSourceRegistry::Instance().FindByPath(song->file_path);
    std::wstring url;
    try { url = source->ResolvePlayUrl(song->file_path); }
    catch (const std::exception&) { log << "FAIL playback response parsing\n"; return false; }
    if (url.empty()) { log << "FAIL resolve: " << kugou::ToUtf8(source->GetLastError()) << "\n"; return false; }
    log << "PASS resolve playable URL\n";
    if (!BASS_Init(0, 44100, 0, nullptr, nullptr)) { log << "FAIL BASS init: " << BASS_ErrorGetCode() << "\n"; return false; }
    HPLUGIN flac = BASS_PluginLoad((runtime_dir + L"Plugins\\bassflac.dll").c_str(), BASS_UNICODE);
    BASS_SetConfig(BASS_CONFIG_NET_TIMEOUT, 15000);
    HSTREAM stream = BASS_StreamCreateURL(reinterpret_cast<const char*>(url.c_str()), 0, BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT, nullptr, nullptr);
    bool success = false;
    if (!stream) log << "FAIL BASS stream: " << BASS_ErrorGetCode() << "\n";
    else
    {
        double seconds = BASS_ChannelBytes2Seconds(stream, BASS_ChannelGetLength(stream, BASS_POS_BYTE));
        std::vector<float> pcm(44100 * 2 * 4);
        DWORD bytes = 0;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        do
        {
            bytes = BASS_ChannelGetData(stream, pcm.data(), static_cast<DWORD>(pcm.size() * sizeof(float)));
            if (bytes != 0) break;
            Sleep(50); // 网络流可能已经读到文件头，但首批音频数据仍在缓冲。
        } while (GetTickCount64() < deadline);
        const int decode_error = BASS_ErrorGetCode();
        const bool decoded = bytes != static_cast<DWORD>(-1) && bytes > 0;
        bool signal = false;
        if (decoded)
            for (size_t i = 0; i < bytes / sizeof(float); ++i)
                if (std::isfinite(pcm[i]) && std::fabs(pcm[i]) > 0.00001f) { signal = true; break; }
        success = seconds > 0 && decoded && signal;
        log << "duration_seconds=" << seconds << "\n" << "decoded_bytes=" << (decoded ? bytes : 0)
            << "\ndecode_error=" << decode_error << "\nnon_silent_signal=" << signal << "\n" << (success ? "PASS" : "FAIL") << " native audio decoding\n";
        BASS_StreamFree(stream);
    }
    if (flac) BASS_PluginFree(flac);
    BASS_Free();
    return success && log.good();
}

// Explicit integration check: download one already-selected track into an isolated temporary cache.
inline bool RunOnlineMediaIntegrationTest(const std::wstring& log_path, const std::wstring& playlist_path, const std::wstring& runtime_dir)
{
    std::ofstream log(log_path, std::ios::binary);
    int failures{};
    auto check = [&](bool condition, const char* name) { log << (condition ? "PASS " : "FAIL ") << name << '\n'; if (!condition) ++failures; };
    CPlaylistFile playlist;
    if (!playlist.LoadFromFile(playlist_path)) { check(false, "read selected playlist"); return false; }
    const auto song = std::find_if(playlist.GetPlaylist().begin(), playlist.GetPlaylist().end(), [](const SongInfo& item) {
        return online::CSourceRegistry::GetScheme(item.file_path) == L"kugou";
    });
    if (song == playlist.GetPlaylist().end()) { check(false, "selected Kugou song required"); return false; }
    wchar_t root[MAX_PATH]{};
    if (!GetTempFileNameW(runtime_dir.c_str(), L"btm", 0, root)) return false;
    DeleteFileW(root); if (!CreateDirectoryW(root, nullptr)) return false;
    auto& cache = online::COnlineMediaCache::Instance();
    cache.Configure(root);
    std::wstring resolved;
    try { resolved = online::CSourceRegistry::Instance().ResolvePlayUrl(song->file_path); }
    catch (const std::exception&) { check(false, "resolve playable media"); }
    check(resolved.starts_with(L"https://") || resolved.starts_with(L"http://"), "first playback resolves authorized network URL");
    check(!cache.IsBusy() && cache.FindAudio(song->file_path).empty(), "playing current song does not start audio caching");
    cache.PrefetchNext(song->file_path);
    cache.QueueLyric(song->file_path);
    const auto deadline = GetTickCount64() + 60000;
    while (cache.IsBusy() && GetTickCount64() < deadline) Sleep(50);
    const auto audio = cache.FindAudio(song->file_path), lyric = cache.FindLyric(song->file_path);
    check(!cache.IsBusy() && !audio.empty(), "next song prefetch downloads complete audio");
    if (audio.empty()) log << "cache_status=" << kugou::ToUtf8(cache.Describe()) << '\n';
    CLyrics parsed(lyric);
    check(!lyric.empty() && !parsed.IsEmpty(), "automatic lyric cache parses in native player");
    if (!audio.empty())
    {
        check(online::CSourceRegistry::Instance().ResolvePlayUrl(song->file_path) == audio, "repeat playback resolves local cache before network");
        bool initialized = BASS_Init(0, 44100, 0, nullptr, nullptr) != FALSE;
        check(initialized, "initialize silent native decoder");
        if (initialized)
        {
            auto plugin = BASS_PluginLoad((runtime_dir + L"Plugins\\bassflac.dll").c_str(), BASS_UNICODE);
            auto stream = BASS_StreamCreateFile(FALSE, audio.c_str(), 0, 0, BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
            bool decoded = false;
            if (stream)
            {
                const auto seconds = BASS_ChannelBytes2Seconds(stream, BASS_ChannelGetLength(stream, BASS_POS_BYTE));
                std::vector<float> pcm(44100 * 4);
                const auto bytes = BASS_ChannelGetData(stream, pcm.data(), static_cast<DWORD>(pcm.size() * sizeof(float)));
                bool signal = bytes != static_cast<DWORD>(-1) && bytes > 0
                    && std::any_of(pcm.begin(), pcm.begin() + bytes / sizeof(float), [](float value) { return std::isfinite(value) && std::fabs(value) > 0.00001f; });
                decoded = seconds > 30 && signal;
                log << "cached_duration_seconds=" << seconds << "\nnon_silent_signal=" << signal << '\n';
                BASS_StreamFree(stream);
            }
            check(decoded, "cached full song decodes to native PCM");
            if (plugin) BASS_PluginFree(plugin); BASS_Free();
        }
        const auto saved = std::filesystem::path(root) / L"saved";
        std::error_code error; std::filesystem::create_directories(saved, error);
        online::Track track; track.virtual_path = song->file_path; track.title = song->GetTitle(); track.artist = song->GetArtist(); track.album = song->GetAlbum();
        cache.SaveAudio(track, saved.wstring());
        const auto save_deadline = GetTickCount64() + 60000;
        while (cache.IsBusy() && GetTickCount64() < save_deadline) Sleep(50);
        std::vector<std::filesystem::path> saved_files;
        for (const auto& file : std::filesystem::directory_iterator(saved)) saved_files.push_back(file.path());
        check(!cache.IsBusy() && saved_files.size() == 1, "manual save publishes one audio file without sidecars");
        const auto cover = cache.FindCover(song->file_path);
        CImage picture;
        check(!cover.empty() && SUCCEEDED(picture.Load(cover.c_str())) && picture.GetWidth() > 0, "online album cover decodes as a native image");
        auto& player = CPlayer::GetInstance(); player.GetPlayList() = {*song};
        check(!player.LoadOnlineCover(L"bodian://stale", cover), "stale cover cannot replace current song");
        check(player.LoadOnlineCover(song->file_path, cover) && player.AlbumCoverExist(), "downloaded cover enters native player display");
        player.GetPlayList().clear();
        bool embedded = false, preserved = false;
        if (saved_files.size() == 1)
        {
            SongInfo metadata; metadata.file_path = saved_files.front().wstring();
            CAudioTag tags(metadata); int image_type{}; size_t image_bytes{};
            const auto embedded_cover = std::filesystem::path(root).filename().wstring() + L"-embedded-cover";
            const auto extracted = tags.GetAlbumCover(image_type, embedded_cover.c_str(), &image_bytes);
            CImage extracted_picture;
            embedded = tags.GetAudioTag() && metadata.title == track.title && metadata.artist == track.artist
                && !tags.GetAudioLyric().empty() && image_bytes > 0 && !extracted.empty()
                && SUCCEEDED(extracted_picture.Load(extracted.c_str()));
            if (!extracted.empty()) DeleteFileW(extracted.c_str());
            if (BASS_Init(0, 44100, 0, nullptr, nullptr))
            {
                auto plugin = BASS_PluginLoad((runtime_dir + L"Plugins\\bassflac.dll").c_str(), BASS_UNICODE);
                auto original = BASS_StreamCreateFile(FALSE, audio.c_str(), 0, 0, BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
                auto exported = BASS_StreamCreateFile(FALSE, metadata.file_path.c_str(), 0, 0, BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
                if (original && exported)
                {
                    std::vector<float> before(44100 * 4), after(before.size());
                    auto a = BASS_ChannelGetData(original, before.data(), static_cast<DWORD>(before.size() * sizeof(float)));
                    auto b = BASS_ChannelGetData(exported, after.data(), static_cast<DWORD>(after.size() * sizeof(float)));
                    preserved = a != static_cast<DWORD>(-1) && a > 0 && a == b && before == after
                        && BASS_ChannelGetLength(original, BASS_POS_BYTE) == BASS_ChannelGetLength(exported, BASS_POS_BYTE);
                }
                if (original) BASS_StreamFree(original); if (exported) BASS_StreamFree(exported);
                if (plugin) BASS_PluginFree(plugin); BASS_Free();
            }
        }
        check(embedded, "download contains readable song tags, embedded lyrics and album cover");
        check(preserved, "tagged download preserves decoded audio and full duration");
        cache.Clear();
        check(cache.FindAudio(song->file_path).empty() && !std::filesystem::is_empty(saved), "cache clear retains manually saved audio");
    }
    cache.Shutdown();
    const auto stop_deadline = GetTickCount64() + 20000;
    while (cache.IsBusy() && GetTickCount64() < stop_deadline) Sleep(50);
    if (!cache.IsBusy()) { std::error_code error; std::filesystem::remove_all(root, error); }
    log << "failures=" << failures << '\n';
    return failures == 0 && log.good();
}
