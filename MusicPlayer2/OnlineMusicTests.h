#pragma once
#include "Playlist.h"
#include "OnlineJson.h"
#include "OnlineMusicModel.h"
#include "OnlineMediaCache.h"
#include "OnlinePlaylistImport.h"
#include "KugouKrc.h"
#include "BodianSource.h"
#include "KugouSource.h"
#include "MciCore.h"
#include "bass.h"
#include <sstream>

inline bool DecodeOnlineTestAudio(const std::wstring& url, const std::wstring& runtime_dir, std::ostream& log);

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
        model.m_state.page = Model::Page::Account; model.SelectPage();
        {
            const auto account = model.Snapshot();
            check(account->page == Model::Page::Account && account->detail_visible
                && !account->detail.empty() && account->items.empty(), "account page shows detail instead of list");
        }
        model.SetNotice(L"测试提示");
        check(model.Snapshot()->notice == L"测试提示", "notice is published to UI state");
        model.m_state.page = Model::Page::Search; model.SelectPage();
        check(!model.m_interrupted && model.Snapshot()->items.empty() && !model.Snapshot()->detail_visible
            && !model.Snapshot()->status.empty(), "page switch clears interrupted request");
        model.m_task = std::make_shared<Model::Task>(); model.m_task->import_all = true;
        model.Suspend();
        check(!model.m_interrupted, "cancelled full import is not resumed as browsing");
        for (bool rematch : {false, true})
        {
            model.m_task = std::make_shared<Model::Task>();
            model.m_task->rematch = rematch;
            model.m_task->external_import = !rematch;
            model.m_state.importing = true;
            model.m_state.import_done = 2; model.m_state.import_total = 5;
            model.Suspend();
            check(!model.m_interrupted && !model.Snapshot()->busy && !model.Snapshot()->importing
                && model.Snapshot()->import_done == 0 && model.Snapshot()->import_total == 0,
                rematch ? "cancelled rematch clears progress" : "cancelled external import does not resume browsing");
        }
        model.m_task = std::make_shared<Model::Task>();
        model.m_task->rematch = true; model.m_task->done = true;
        model.m_task->error = L"fixture failure";
        model.Poll(false);
        check(!model.Snapshot()->busy && !model.Snapshot()->importing
            && model.Snapshot()->status == L"fixture failure", "failed rematch reports error instead of completion");
        model.Shutdown();
    }
    // 播放地址备忘的边界：它让切歌不必再发一次解析请求，但只能记真实网络地址，
    // 也不能在失败结论后卡住用户的重试。这里只验证边界，不发请求。
    {
        auto& registry = online::CSourceRegistry::Instance();
        registry.ForgetAllPlayUrls();
        check(registry.CachedPlayUrl(song.file_path).empty(), "no play address memo before the first resolve");
        registry.RememberPlayUrl(song.file_path, L"https://example.test/memo.mp3");
        check(registry.CachedPlayUrl(song.file_path) == L"https://example.test/memo.mp3",
            "resolved address is remembered for the next song switch");
        registry.ForgetPlayUrl(song.file_path);
        check(registry.CachedPlayUrl(song.file_path).empty(), "forgotten address is not reused");
        // 本地缓存文件与非虚拟路径不能进备忘，否则会把本地路径当播放地址交给播放核心
        registry.RememberPlayUrl(song.file_path, L"C:\\local\\cached.flac");
        registry.RememberPlayUrl(L"C:\\local\\song.flac", L"https://example.test/song.mp3");
        check(registry.CachedPlayUrl(song.file_path).empty() && registry.CachedPlayUrl(L"C:\\local\\song.flac").empty(),
            "only http addresses of virtual paths enter the memo");
        registry.ForgetAllPlayUrls();
        check(registry.CachedPlayUrl(song.file_path).empty(), "clearing the memo drops every entry");
        // 音质说明跟着地址走：预缓存线程解析出的说明，界面线程命中备忘时也能拿到；
        // 备忘被丢掉时说明一起丢，不会把上一次的「无损」贴到重新解析后的 128k 上。
        registry.RememberPlayUrl(song.file_path, L"https://example.test/memo.flac", L"当前播放：无损");
        check(registry.QualityNote(song.file_path) == L"当前播放：无损", "quality note travels with the remembered address");
        check(registry.QualityNote(second.file_path).empty(), "quality note is per address, not per source");
        registry.ForgetPlayUrl(song.file_path);
        check(registry.QualityNote(song.file_path).empty(), "forgetting the address drops its quality note");
        check(registry.PlayError(song.file_path).empty(), "no failure reason before any failed resolve");
        registry.ForgetAllPlayUrls();
    }
    // 列表快照里的收藏标记与「重试播放」：红心跟着在线本地歌单走，失败标记在再次点播时撤掉
    {
        using Model = COnlineMusicModel;
        Model model;
        online::BrowseItem first_item; first_item.track = online::BodianTrack(nlohmann::json{{"id",42},{"name","fixture"}});
        online::BrowseItem second_item; second_item.track = online::BodianTrack(nlohmann::json{{"id",43},{"name","other"}});
        SongInfo saved; saved.file_path = L"bodian://43"; saved.title = L"other";
        model.m_local = {saved};
        model.m_state.items = {first_item, second_item}; model.Publish(true);
        {
            const auto snapshot = model.Snapshot();
            check(snapshot->saved_local.size() == 2 && !snapshot->saved_local[0] && snapshot->saved_local[1],
                "snapshot marks rows that are already in the online local playlist");
            check(!model.IsSavedLocally(L"bodian://42") && model.IsSavedLocally(L"bodian://43"), "saved-locally lookup follows the local playlist");
        }
        model.MarkUnplayable(L"bodian://42");
        check(model.Snapshot()->unplayable[0] && !model.Snapshot()->unplayable[1], "playback failure greys out only the failed row");
        const auto before = model.Snapshot()->revision;
        Model::Command play{Model::Action::Play}; play.rows = {0}; play.revision = before;
        model.Execute(play);
        Model::Playback playback;
        check(model.TakePlayback(playback) && playback.songs.size() == 1, "replaying a failed row still dispatches playback");
        check(!model.Snapshot()->unplayable[0] && model.Snapshot()->revision == before,
            "replaying a failed row drops the grey mark without invalidating the selection");
        model.SetQualityNote(L"当前播放：无损");
        check(model.Snapshot()->quality_note == L"当前播放：无损" && model.Snapshot()->status != L"当前播放：无损",
            "quality note is published separately from the status line");
        model.SetStatus(L"已加入播放队列：1 首。");
        check(model.Snapshot()->status_stamp != 0, "operation results are stamped so they can fall back to the list summary");
        model.Shutdown();
    }
    // 来源判定：本地、两个在线平台必须严格分开，而且只看地址本身，不看别的字段
    {
        using online::CSourceRegistry;
        const auto& all = CSourceRegistry::Instance().GetAll();
        check(all.size() >= 2, "at least two online sources are registered");
        check(CSourceRegistry::OriginLabel(L"C:\\music\\local file.flac") == L"本地", "local path is labelled local");
        check(CSourceRegistry::OriginIndex(L"C:\\music\\local file.flac") == 0, "local path has origin index zero");
        check(CSourceRegistry::OriginLabel(L"kugou://0123456789ABCDEF") == all[0]->GetShortName(),
            "kugou virtual path is labelled with its own source");
        check(CSourceRegistry::OriginLabel(L"bodian://42") == all[1]->GetShortName(),
            "bodian virtual path is labelled with its own source");
        check(CSourceRegistry::OriginIndex(L"kugou://x") == 1 && CSourceRegistry::OriginIndex(L"bodian://42") == 2,
            "origin index follows the registry order");
        check(CSourceRegistry::OriginLabel(L"https://example.test/song.mp3") == L"本地",
            "a plain http url is not mistaken for an online source");
        check(all[0]->GetShortName() != all[1]->GetShortName(), "the two sources have distinct short names");
    }
    {
        using namespace online;
        auto snapshot = [](std::uint64_t id) {
            for (const auto& task : OnlineProgress::Snapshot()) if (task.id == id) return task;
            return ProgressSnapshot{};
        };
        auto browse = OnlineProgress::Start(L"搜索测试", L"正在连接", false, true);
        auto download = OnlineProgress::Start(L"下载测试", L"排队中", false, true, true);
        check(snapshot(browse->Id()).Percent() == -1, "unknown totals do not invent a percentage");
        download->Update(L"下载中", 1024, 2048, ProgressUnit::Bytes);
        check(snapshot(download->Id()).Percent() == 50 && snapshot(download->Id()).Counter() == L"1.0 KB / 2.0 KB",
            "download progress exposes real transferred bytes");
        download->Transfer(2048, 2048);
        check(snapshot(download->Id()).Percent() == 99, "download waits for validation before 100 percent");
        OnlineProgress::RequestCancel(browse->Id());
        check(browse->Cancelled() && !download->Cancelled() && snapshot(browse->Id()).cancelling,
            "cancelling one operation does not cancel concurrent tasks");
        OnlineProgress::Dismiss(download->Id());
        check(snapshot(download->Id()).id == download->Id(), "active tasks cannot be dismissed");
        download->Finish(ProgressResult::Succeeded, L"保存完成");
        download->Update(L"过期更新");
        check(snapshot(download->Id()).Percent() == 100 && snapshot(download->Id()).detail == L"保存完成",
            "finished progress ignores stale worker updates");
        const auto browse_id = browse->Id(); browse.reset();
        check(snapshot(browse_id).result == ProgressResult::Cancelled, "abandoned task cannot remain running");
        auto stalled = snapshot(download->Id()); stalled.result = ProgressResult::Running;
        check(stalled.Timing(stalled.updated + 16000).find(L"仍在等待") != std::wstring::npos,
            "slow service explicitly reports continued waiting");
        for (const auto& task : OnlineProgress::Snapshot()) if (!task.Running()) OnlineProgress::Dismiss(task.id);

        const auto source_count = CSourceRegistry::Instance().GetAll().size();
        check(ID_ONLINE_SWITCH_SOURCE_START != ID_FILE_OPEN_PLAYLIST
            && COnlineMusicModel::IsSwitchSourceCommand(ID_ONLINE_SWITCH_SOURCE_START)
            && !COnlineMusicModel::IsSwitchSourceCommand(ID_FILE_OPEN_PLAYLIST)
            && !COnlineMusicModel::IsSwitchSourceCommand(ID_ONLINE_SWITCH_SOURCE_START + static_cast<unsigned>(source_count)),
            "source menu commands are distinct and bounded");

        SongInfo replacement = song; replacement.file_path = L"bodian://54321"; replacement.end_pos.fromInt(125000);
        auto reordered = std::vector<SongInfo>{second, song};
        reordered[1].lyric_file = L"old.lrc"; reordered[1].bitrate = 320;
        check(ApplySourceMatches(reordered, {song}, {replacement}) == 1 && reordered[0] == second
            && reordered[1] == replacement && reordered[1].lyric_file.empty() && reordered[1].bitrate == 0,
            "source replacement follows song identity after rows are reordered");
        auto removed = std::vector<SongInfo>{second};
        check(ApplySourceMatches(removed, {song}, {replacement}) == 0 && removed.front() == second,
            "removed songs are never reinserted by a late match");

        const auto original_path = root + L"source-original.playlist";
        const auto current_path = root + L"source-current.playlist";
        files.insert(files.end(), {original_path, current_path, original_path + L".bak", current_path + L".bak"});
        CPlaylistFile::SavePlaylistToFile(songs, original_path);
        CPlaylistFile::SavePlaylistToFile({second}, current_path);
        CPlayer player; player.m_playlist_mode = CPlayer::PM_PLAYLIST;
        player.m_playlist_path = current_path; player.m_playlist = {second}; player.m_index = 0;
        std::wstring error;
        check(player.ApplyOnlineSourceChanges(original_path, {song}, {replacement}, error) == 1
            && player.m_playlist.front() == second, "switching playlists during matching still writes the original target");
        CPlaylistFile changed, backup;
        changed.LoadFromFile(original_path); backup.LoadFromFile(original_path + L".bak");
        check(changed.GetPlaylist().front() == replacement && backup.GetPlaylist() == songs,
            "source changes save a recoverable original playlist backup");
        player.m_playlist_path = original_path; player.m_playlist = songs;
        CPlaylistFile::SavePlaylistToFile(songs, original_path);
        HANDLE locked_backup = CreateFileW((original_path + L".bak").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked_backup != INVALID_HANDLE_VALUE && player.ApplyOnlineSourceChanges(original_path, {song}, {replacement}, error) == -1
            && player.m_playlist == songs, "backup failure preserves the current in-memory playlist");
        if (locked_backup != INVALID_HANDLE_VALUE) CloseHandle(locked_backup);
        HANDLE locked_target = CreateFileW(original_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked_target != INVALID_HANDLE_VALUE && player.ApplyOnlineSourceChanges(original_path, {song}, {replacement}, error) == -1
            && player.m_playlist == songs, "write failure preserves the current in-memory playlist");
        if (locked_target != INVALID_HANDLE_VALUE) CloseHandle(locked_target);
        changed.LoadFromFile(original_path);
        check(changed.GetPlaylist() == songs, "failed replacement leaves the original file intact");
        player.m_loading = true;
        check(player.ApplyOnlineSourceChanges(original_path, {song}, {replacement}, error) == -2,
            "busy playback defers applying source changes");
        player.m_loading = false;
        check(player.ApplyOnlineSourceChanges(original_path, {song}, {replacement}, error) == 1
            && player.m_playlist.front() == replacement && player.m_total_time == 248000,
            "current playlist source change refreshes songs and total duration");
    }
    {
        using Model = COnlineMusicModel;
        class MatchSource : public online::IOnlineSource
        {
        public:
            online::Track match;
            int searches{};
            std::wstring GetScheme() const override { return L"bodian"; }
            std::wstring GetDisplayName() const override { return L"测试音源"; }
            std::wstring ResolvePlayUrl(const std::wstring&) override { return {}; }
            bool Search(const std::wstring& keyword, int, std::vector<online::Track>& result) override
            {
                ++searches;
                auto candidate = match;
                if (keyword != match.title) candidate.title = L"完全不相关的歌曲";
                result = {candidate}; return true;
            }
        };
        auto source = std::make_shared<MatchSource>();
        source->match.virtual_path = L"bodian://54321"; source->match.title = song.title;
        source->match.artist = song.artist; source->match.album = song.album; source->match.duration_ms = 123000;
        Model model;
        auto task = std::make_shared<Model::Task>();
        task->rematch = true; task->source = source; task->rematch_songs = {song, second};
        task->progress = online::OnlineProgress::Start(L"后台换源测试", L"等待匹配", false, true);
        model.m_library_task = task;
        model.m_task = std::make_shared<Model::Task>();
        auto browse = model.m_task;
        model.Suspend();
        check(browse->cancelled && !task->cancelled && model.m_library_task == task,
            "hiding online page cancels browsing but preserves library work");
        model.m_state.page = Model::Page::Search; model.SelectPage();
        check(model.m_library_task == task && !task->cancelled, "page navigation preserves source switching");
        model.EnqueueTask(task);
        const auto deadline = GetTickCount64() + 3000;
        while (!task->done && GetTickCount64() < deadline) Sleep(5);
        check(task->done && task->success && source->searches == 2 && task->rematch_moved == 1 && task->rematch_kept == 1
            && task->switched.size() == 2 && task->switched.front().file_path == source->match.virtual_path,
            "source worker retries title-only search after unrelated results and retains same-source songs");
        online::OnlineProgress::RequestCancel(task->progress->Id());
        model.Poll(false);
        check(task->cancelled && !model.m_library_task && !model.Snapshot()->importing,
            "cancelled matching result is discarded before it can write a playlist");
        auto shutdown_task = std::make_shared<Model::Task>();
        model.m_library_task = shutdown_task; model.Shutdown();
        check(shutdown_task->cancelled, "shutdown cancels outstanding library work");
    }
    // 使用无音频输出的内核替身走真实播放器 OPEN/SEEK/PLAY 路径。
    class RecoveryCore : public CMciCore
    {
    public:
        int position{}, error{}, opens{};
        bool fail_open{};
        PlayingState state{PS_STOPED};
        std::wstring opened;
        void Open(const wchar_t* path) override { opened = path; ++opens; position = 0; error = fail_open ? 1 : 0; }
        void Close() override { state = PS_STOPED; }
        void Play() override { if (!error) state = PS_PLAYING; }
        void Pause() override { state = PS_PAUSED; }
        void Stop() override { state = PS_STOPED; position = 0; }
        void InitCore() override {}
        void UnInitCore() override {}
        void SetVolume(int) override {}
        void SetPitch(int) override {}
        void ClearReverb() override {}
        int GetCurPosition() override { return position; }
        int GetSongLength() override { return error ? 0 : 123000; }
        void SetCurPosition(int value) override { position = value; }
        std::wstring GetAudioType() override { return L"mp3"; }
        int GetChannels() override { return 2; }
        int GetFReq() override { return 44100; }
        int GetBitrate() override { return 320; }
        int GetErrorCode() override { return error; }
        std::wstring GetErrorInfo() override { return error ? L"测试音频无法打开" : L""; }
        std::wstring GetErrorInfo(int) override { return L"测试音频无法打开"; }
        PlayingState GetPlayingState() override { return state; }
    };
    {
        using Model = COnlineMusicModel;
        using namespace online;
        class RecoverySource : public IOnlineSource
        {
        public:
            Track candidate;
            std::wstring url;
            std::wstring GetScheme() const override { return L"bodian"; }
            std::wstring GetDisplayName() const override { return L"测试音源"; }
            std::wstring ResolvePlayUrl(const std::wstring&) override { return url; }
            bool Search(const std::wstring&, int, std::vector<Track>& result) override { result = {candidate}; return true; }
        };
        auto source = std::make_shared<RecoverySource>();
        source->candidate.virtual_path = L"bodian://54321";
        source->candidate.title = song.title; source->candidate.artist = song.artist;
        source->candidate.duration_ms = 123000;
        Model model;
        auto run_match = [&]() {
            auto task = std::make_shared<Model::Task>();
            task->source = source; task->automatic_switch = true; task->rematch_songs = {song};
            task->progress = OnlineProgress::Start(L"自动换源测试", L"匹配中", false, true);
            model.EnqueueTask(task);
            const auto deadline = GetTickCount64() + 3000;
            while (!task->done && GetTickCount64() < deadline) Sleep(5);
            return task;
        };
        auto unavailable = run_match();
        check(unavailable->done && !unavailable->success && unavailable->switched.empty(),
            "automatic source match requires a playable URL");
        source->url = L"https://audio.invalid/fixture.mp3";
        source->candidate.title = L"另一首完全不同的歌曲";
        auto unrelated = run_match();
        check(unrelated->done && !unrelated->success, "automatic source switch rejects unrelated songs");
        source->candidate.title = song.title;
        auto matched = run_match();
        check(matched->done && matched->success && matched->switched.size() == 1
            && CSourceRegistry::Instance().CachedPlayUrl(source->candidate.virtual_path) == source->url,
            "automatic source worker prepares a reliable match and reusable URL");

        CWnd owner;
        const bool created = owner.CreateEx(0, AfxRegisterWndClass(0), L"Playback recovery test", WS_POPUP, CRect(0, 0, 1, 1), nullptr, 0) != FALSE;
        check(created, "create isolated playback recovery owner");
        if (created && matched->done && matched->success)
        {
            auto* previous_owner = theApp.m_pMainWnd; theApp.m_pMainWnd = &owner;
            CPlayer player;
            auto* core = new RecoveryCore(); player.m_pCore = core; player.m_player_core_inited = true;
            player.m_playlist = songs; player.m_index = 0; player.m_playlist_mode = CPlayer::PM_PLAYLIST;
            player.m_playlist_path = root + L"auto-original.playlist";
            files.push_back(player.m_playlist_path);
            CPlaylistFile::SavePlaylistToFile(songs, player.m_playlist_path);
            auto prepare = [&](bool done = true) {
                player.m_playback_requested = true; player.m_file_opend = true;
                player.m_error_state = CPlayer::ES_FILE_CANNOT_BE_OPEN; player.m_current_position.fromInt(42000);
                ++player.m_playback_generation;
                model.m_auto_switch_generation = player.m_playback_generation;
                model.m_auto_switch_stopped = false;
                model.m_auto_switch_started = GetTickCount64();
                model.m_auto_switch_position = 42000;
                model.m_auto_switch_tried = {L"kugou", L"bodian"};
                auto task = std::make_shared<Model::Task>(); task->source = source; task->automatic_switch = true;
                task->rematch_songs = {song}; task->switched = matched->switched; task->success = true; task->done = done;
                task->progress = OnlineProgress::Start(L"自动换源恢复测试", L"等待播放", false, true);
                model.m_auto_switch_progress = task->progress; model.m_auto_switch_task = task;
                return task;
            };
            prepare();
            check(!model.RecoverPlayback(player) && player.IsPlaying() && core->opens == 1 && core->position == 42000
                && core->opened == source->url && player.GetOnlinePlaybackPath() == source->candidate.virtual_path,
                "automatic switch reopens matched audio and resumes the original position");
            CPlaylistFile original; original.LoadFromFile(player.m_playlist_path);
            check(player.GetCurrentFilePath() == song.file_path && original.GetPlaylist() == songs,
                "automatic switching preserves original playlist identities and saved file");
            player.m_error_state = CPlayer::ES_FILE_CANNOT_BE_OPEN;
            check(!model.RecoverPlayback(player) && !model.m_auto_switch_task && core->opens == 1,
                "failed alternative does not switch back or repeat an attempted source");
            auto pending = prepare(false);
            check(model.RecoverPlayback(player), "pending automatic switch holds failure skipping");
            player.MusicControl(Command::STOP);
            pending->done = true;
            check(!model.RecoverPlayback(player) && pending->cancelled && core->opens == 1,
                "stopping playback rejects a late automatic match");
            auto cancelled = prepare();
            OnlineProgress::RequestCancel(cancelled->progress->Id());
            check(!model.RecoverPlayback(player) && cancelled->cancelled && core->opens == 1
                && !model.RecoverPlayback(player), "cancelled automatic switching does not restart on the next tick");
            auto stale = prepare();
            player.m_playlist[0].file_path = root + L"local.wav"; ++player.m_playback_generation;
            check(!model.RecoverPlayback(player) && stale->cancelled && core->opens == 1,
                "switching to another track discards the previous automatic match");
            player.m_playlist = songs;
            auto paused = prepare(); player.MusicControl(Command::PAUSE);
            check(!model.RecoverPlayback(player) && paused->cancelled && core->opens == 1,
                "pausing playback prevents automatic playback resuming");
            auto disabled = prepare(); model.m_state.auto_switch_source = false;
            check(!model.RecoverPlayback(player) && disabled->cancelled && core->opens == 1,
                "disabled automatic switching leaves playback unchanged");
            model.m_state.auto_switch_source = true;
            auto timed_out = prepare(false); model.m_auto_switch_started = GetTickCount64() - 60001;
            check(!model.RecoverPlayback(player) && timed_out->cancelled && core->opens == 1,
                "automatic switch has a bounded wait and releases failure handling");
            prepare(); core->fail_open = true;
            check(!model.RecoverPlayback(player) && core->opens == 2 && !player.IsPlaying()
                && player.GetCurrentFilePath() == song.file_path && !model.m_auto_switch_task,
                "an unusable matched stream stops without changing the playlist or looping");
            player.MusicControl(Command::CLOSE);
            check(player.GetOnlinePlaybackPath() == song.file_path && !player.PlaybackRequested(),
                "closing a stream clears its temporary source override");
            model.Shutdown();
            theApp.m_pMainWnd = previous_owner;
            owner.DestroyWindow();
        }
        else model.Shutdown();
        CSourceRegistry::Instance().ForgetPlayUrl(source->candidate.virtual_path);
        COnlineSettings settings; settings.Configure(root);
        auto preferences = settings.Get();
        check(preferences.auto_switch_source, "automatic switching is enabled by default");
        preferences.auto_switch_source = false;
        files.push_back(root + L"online_settings.ini"); files.push_back(root + L"online_settings.ini.tmp");
        const bool saved = settings.Save(preferences);
        COnlineSettings reloaded; reloaded.Configure(root);
        check(saved && !reloaded.Get().auto_switch_source, "automatic switch preference survives settings reload");
    }
    if (network)
    {
        // 公共接口检查不领取权益，也不改变磁盘上的偏好。
        bodian::SetAdRewardEnabled(false, {});
        // 使用公开搜索结果走实际换源 worker，并写回独立测试歌单。
        for (const auto& source_scheme : {L"kugou", L"bodian"})
        {
            using Model = COnlineMusicModel;
            auto* origin = online::CSourceRegistry::Instance().FindByScheme(source_scheme);
            auto& registry = online::CSourceRegistry::Instance();
            auto clone_target = [&]() -> std::shared_ptr<online::IOnlineSource> {
                // 与实际菜单操作一致，副本必须携带已初始化的设备身份。
                if (std::wstring(source_scheme) == L"kugou")
                    return std::make_shared<bodian::CBodianSource>(*static_cast<bodian::CBodianSource*>(registry.FindByScheme(L"bodian")));
                return std::make_shared<kugou::CKugouSource>(*static_cast<kugou::CKugouSource*>(registry.FindByScheme(L"kugou")));
            };
            Model model;
            SongInfo input;
            std::wstring selected_target;
            bool searched{};
            for (const auto* query : {L"卡农", L"生日快乐", L"天空之城 纯音乐", L"晴天"})
            {
                std::vector<online::Track> originals;
                if (!origin || !origin->Search(query, 1, originals) || originals.empty()) continue;
                searched = true;
                for (size_t index = 0; index < originals.size() && index < 8; ++index)
                {
                    const auto& original = originals[index];
                    SongInfo candidate; candidate.file_path = original.virtual_path; candidate.title = original.title;
                    candidate.artist = original.artist; candidate.album = original.album;
                    candidate.end_pos.fromInt(original.duration_ms);
                    auto probe = std::make_shared<Model::Task>();
                    probe->source = clone_target(); probe->automatic_switch = true; probe->rematch_songs = {candidate};
                    probe->progress = online::OnlineProgress::Start(L"自动换源联网样本", L"查找可播放的同曲", false, true);
                    model.EnqueueTask(probe);
                    const auto deadline = GetTickCount64() + 60000;
                    while (!probe->done && GetTickCount64() < deadline) Sleep(20);
                    probe->progress->Finish(probe->success ? online::ProgressResult::Succeeded : online::ProgressResult::Failed,
                        probe->success ? L"已找到可播放样本" : probe->error);
                    if (!probe->done || !probe->success || probe->switched.empty()) continue;
                    input = std::move(candidate);
                    selected_target = probe->switched.front().file_path;
                    break;
                }
                if (!selected_target.empty()) break;
            }
            check(searched, "live source-switch original track search");
            check(!selected_target.empty(), "find a reliably matched track playable on the other public source");
            if (selected_target.empty()) { model.Shutdown(); continue; }
            // 样本探测只负责挑选曲目；清掉备忘，下面的自动恢复必须自己再次解析地址。
            registry.ForgetPlayUrl(selected_target);
            auto task = std::make_shared<Model::Task>();
            task->source = clone_target();
            task->rematch = task->in_place = true; task->rematch_songs = {input};
            task->target_playlist = root + source_scheme + L"-switch.playlist";
            files.push_back(task->target_playlist); files.push_back(task->target_playlist + L".bak");
            check(CPlaylistFile::SavePlaylistToFile({input, second}, task->target_playlist), "create live source-switch fixture");
            task->progress = online::OnlineProgress::Start(L"换源验证", L"匹配公开曲目", false, true);
            model.m_library_task = task; model.EnqueueTask(task);
            const auto deadline = GetTickCount64() + 90000;
            while (!task->done && GetTickCount64() < deadline) Sleep(20);
            const bool matched = task->done && task->success && task->rematch_moved == 1;
            log << "source-switch " << kugou::ToUtf8(source_scheme) << " title=" << kugou::ToUtf8(input.title)
                << " artist=" << kugou::ToUtf8(input.artist) << " duration_ms=" << input.length().toInt()
                << " moved=" << task->rematch_moved << " missing=" << task->rematch_missing
                << " error=" << kugou::ToUtf8(task->error) << '\n';
            if (task->done && !matched)
            {
                std::vector<online::Track> candidates;
                if (task->source->Search(input.title, 1, candidates))
                {
                    const auto best = online::PickBest({input.title, input.artist, input.album, input.length().toInt()}, candidates);
                    log << "source-switch candidate title=" << kugou::ToUtf8(best.candidate.title)
                        << " artist=" << kugou::ToUtf8(best.candidate.artist) << " duration_ms=" << best.candidate.duration_ms
                        << " score=" << best.score << " level=" << static_cast<int>(best.level) << '\n';
                }
            }
            check(matched, std::wstring(source_scheme) == L"kugou" ? "live Kugou to Bodian matching" : "live Bodian to Kugou matching");
            if (matched)
            {
                model.Poll(false);
                CPlaylistFile result; result.LoadFromFile(task->target_playlist);
                check(task->success && result.GetPlaylist().size() == 2
                    && online::CSourceRegistry::GetScheme(result.GetPlaylist().front().file_path) == task->source->GetScheme()
                    && result.GetPlaylist().back() == second, "live source match updates only the chosen playlist song");
            }
            CWnd owner;
            const bool created = owner.CreateEx(0, AfxRegisterWndClass(0), L"Live playback recovery test", WS_POPUP,
                CRect(0, 0, 1, 1), nullptr, 0) != FALSE;
            check(created, "create isolated live recovery owner");
            if (created)
            {
                auto* previous_owner = theApp.m_pMainWnd; theApp.m_pMainWnd = &owner;
                CPlayer player;
                auto* core = new RecoveryCore(); player.m_pCore = core; player.m_player_core_inited = true;
                player.m_playlist = {input}; player.m_index = 0; player.m_file_opend = true;
                player.m_error_state = CPlayer::ES_FILE_CANNOT_BE_OPEN;
                player.m_current_position.fromInt(42000);
                player.MusicControl(Command::PLAY);
                const bool recovering = model.RecoverPlayback(player);
                auto recovery = model.m_auto_switch_task;
                check(recovering && recovery && recovery->source->GetScheme() != source_scheme,
                    "failed online playback automatically queues the other source");
                if (recovery)
                {
                    const auto recovery_deadline = GetTickCount64() + 60000;
                    while (!recovery->done && GetTickCount64() < recovery_deadline) Sleep(20);
                    model.RecoverPlayback(player);
                    const bool resumed = recovery->done && recovery->success && player.IsPlaying()
                        && core->opens == 1 && core->position == 42000 && player.GetCurrentFilePath() == input.file_path
                        && online::CSourceRegistry::GetScheme(player.GetOnlinePlaybackPath()) == recovery->source->GetScheme();
                    log << "auto-switch " << kugou::ToUtf8(source_scheme) << " error=" << kugou::ToUtf8(recovery->error) << '\n';
                    check(resumed, "live automatic matching restores playback without replacing the playlist identity");
                    if (resumed)
                        check(DecodeOnlineTestAudio(core->opened, theApp.m_local_dir, log),
                            "live automatically selected audio decodes through BASS");
                }
                theApp.m_pMainWnd = previous_owner;
                owner.DestroyWindow();
            }
            model.Shutdown();
        }
        // 外部歌单导入：外部平台是两步接口（先拿 trackIds，再分批补曲目信息），
        // 用公开歌单验证完整抓取链路；匹配算法由上面的离线用例覆盖。
        {
            using namespace online;
            const auto reference = ParseShareText(L"https://music.163.com/playlist?id=3778678");
            std::vector<ImportTrack> imported;
            std::wstring error, playlist_name;
            const bool fetched = FetchPlaylist(reference, imported, error, []() { return false; }, &playlist_name);
            log << "netease playlist tracks=" << imported.size()
                << " name=" << kugou::ToUtf8(playlist_name)
                << " error=" << kugou::ToUtf8(error) << "\n";
            check(fetched && !imported.empty() && !imported.front().title.empty(), "live Netease playlist import");
            check(!playlist_name.empty(), "live external playlist name");
            if (fetched && !imported.empty())
            {
                auto* source = CSourceRegistry::Instance().GetAll().front();
                int matched = 0;
                for (size_t i = 0; source && i < imported.size() && i < 5; ++i)
                {
                    const auto& item = imported[i];
                    wstring artist = item.artist;
                    const size_t separator = artist.find_first_of(L"/&、;；,，|");
                    if (separator != wstring::npos) artist = artist.substr(0, separator);
                    if (artist.size() > 15) artist.resize(15);
                    const wstring query = artist.empty() ? item.title : artist + L" " + item.title;
                    vector<Track> candidates;
                    if (source->Search(query, 1, candidates) && !candidates.empty()
                        && PickBest(item, candidates).level != MatchLevel::NotFound) ++matched;
                }
                log << "external playlist matched first five=" << matched << "\n";
                check(matched > 0, "live external playlist matching");
            }
        }
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
                        // K源歌词：现在优先取带逐字时间轴的版本，验证解密和格式转换能走通
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
                        // B源歌词：优先取 LRCX 逐字版本，验证单位换算和行/译文配对在真实数据上走通
                        if (source->GetScheme() == L"bodian")
                        {
                            online::Lyric lyric;
                            const bool got_lyric = source->GetLyric(track.virtual_path, lyric);
                            log << "bodian lyric chars=" << lyric.content.size()
                                << " error=" << kugou::ToUtf8(source->GetLastError()) << "\n";
                            check(got_lyric, "live Bodian lyric lookup");
                            if (got_lyric)
                            {
                                CLyrics parsed;
                                parsed.LyricsFromRowString(lyric.content);
                                int translated_lines = 0;
                                for (int li = 0; li < parsed.GetLyricCount(); ++li)
                                {
                                    if (!parsed.GetLyric(li).translate.empty()) ++translated_lines;
                                }
                                log << "bodian lyric lines=" << parsed.GetLyricCount()
                                    << " word_lines=" << parsed.GetWordTimingLineCount()
                                    << " translated=" << translated_lines << "\n";
                                check(parsed.HasWordTiming(), "live Bodian lyric carries word timing");
                            }
                            // 有译文、而且开头就有一句没有原文可挂的译文槽位的歌，单独核对一次：
                            // 译文的时间标签标的是下一句，只有配对正确才会落到上一句原文上；
                            // 开头那句槽位丢不掉的话，第一句就会是空歌词。逐字接口只认 rid，
                            // 不需要先搜索，所以这里直接写定曲目（Hey Jude）。
                            online::Lyric translated_lyric;
                            if (source->GetLyric(L"bodian://2180093", translated_lyric))
                            {
                                CLyrics parsed;
                                parsed.LyricsFromRowString(translated_lyric.content);
                                int translated_lines = 0;
                                int empty_lines = 0;
                                for (int li = 0; li < parsed.GetLyricCount(); ++li)
                                {
                                    const auto line = parsed.GetLyric(li);
                                    if (!line.translate.empty()) ++translated_lines;
                                    if (line.text.empty()) ++empty_lines;
                                }
                                log << "bodian translated lyric lines=" << translated_lines
                                    << "/" << parsed.GetLyricCount() << " empty=" << empty_lines << "\n";
                                check(parsed.HasWordTiming() && translated_lines > 0,
                                    "live Bodian translation pairs with the line above");
                                check(empty_lines == 0, "live Bodian lyric drops the leading slot line");
                            }
                            else
                            {
                                log << "bodian translated lyric skipped: "
                                    << kugou::ToUtf8(source->GetLastError()) << "\n";
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
    // ---- K源 KRC 逐字歌词：格式转换（离线）----
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
    // ---- B源 LRCX 逐字歌词（离线）----
    {
        // [kuwo:NNN] 是八进制的权重，<a,b> 是字位置和时长的和差编码
        const std::string sample =
            "[kuwo:027]\n"
            "[00:01.500]<0,0>\n"
            "[00:01.500]<600,-600>INTRO<1300,-500>LINE\n"
            "[00:04.000]<0,0>translation one\n"
            "[00:04.000]<4184,-4184>SINGLE\n"
            "[00:08.000]<0,0>translation two\n"
            "[00:08.000]<600,-600>THIRD<1200,0>LINE<1750,250>NOW\n"
            "[00:13.000]<0,0>translation three\n"
            "[00:13.000]<0,0>NO TIMING\n"
            "[00:14.000]<0,0>plain translation\n";
        const std::wstring converted = bodian::LrcxToExtendedLyric(sample);
        check(!converted.empty(), "lrcx converts to extended lyric");
        check(converted.find(L"[00:08.000]<00:08.000>THIRD<00:08.200><00:08.300>LINE<00:08.500>NOW<00:08.750>")
            != std::wstring::npos, "lrcx keeps per word start and end with rests");
        CLyrics parsed;
        parsed.LyricsFromRowString(converted);
        check(parsed.GetLyricCount() == 4, "lrcx drops the leading translation slot");
        check(parsed.GetLyric(0).text == L"INTROLINE" && parsed.GetLyric(0).translate == L"translation one"
            && parsed.GetLyric(0).time_span == 2500, "lrcx first line keeps text, translation and span");
        check(parsed.GetLyric(1).text == L"SINGLE" && parsed.GetLyric(1).translate == L"translation two"
            && parsed.GetLyric(1).HasWordTiming(), "lrcx single word line is word timed");
        const auto third = parsed.GetLyric(2);
        check(third.text == L"THIRDLINENOW" && third.translate == L"translation three"
            && third.HasWordTiming() && third.word_time.size() == 5
            && third.word_time[0] + third.word_time[1] + third.word_time[2] + third.word_time[3] == 750,
            "lrcx last word stops where the singing stops");
        check(parsed.GetLyric(3).text == L"NO TIMING" && parsed.GetLyric(3).translate == L"plain translation"
            && !parsed.GetLyric(3).HasWordTiming(), "lrcx line without word timing stays a plain line");
        check(parsed.HasWordTiming() && parsed.GetWordTimingLineCount() == 3,
            "lrcx word lyric is detected as a whole");
        auto measure = [](const std::wstring& str) { return static_cast<int>(str.size()) * 100; };
        const auto progress_at = [&](int ms)
        {
            CPlayTime time; time.fromInt(ms);
            return parsed.GetLyricProgress(time, false, false, measure);
        };
        check(progress_at(8000) == 0, "lrcx word line starts unfilled");
        const int p_first = progress_at(8200);
        const int p_second = progress_at(8400);
        check(p_first > 0 && p_first < p_second && p_second < 1000, "lrcx word line fills word by word");
        check(progress_at(8750) == 1000 && progress_at(10000) == 1000, "lrcx fill holds after the last word");
        check(bodian::LrcxToExtendedLyric("[00:00.000]<600,-600>AB\n").empty(),
            "lrcx without kuwo weights falls back to plain lyric");
        check(bodian::LrcxToExtendedLyric("[kuwo:027]\n[00:00.000]<0,0>AB\n[00:02.000]<0,0>CD\n").empty(),
            "lrcx without any word timing falls back to plain lyric");
        check(bodian::LrcxToExtendedLyric("not a lyric").empty(), "non lrcx input rejected");
        check(bodian::LrcxToExtendedLyric("").empty(), "empty lrcx input rejected");
    }
    // ---- 歌单跨平台导入：解析与匹配（纯离线，不联网）----
    {
        using namespace online;
        const auto ref_netease = ParseShareText(L"https://music.163.com/playlist?id=3778678");
        check(ref_netease.source == ImportSource::Netease && ref_netease.id == L"3778678", "netease playlist link");
        const auto ref_qq = ParseShareText(L"https://y.qq.com/n/ryqq/playlist/7011264340");
        check(ref_qq.source == ImportSource::QQ && ref_qq.id == L"7011264340", "qq playlist link");
        // 用户从 App 复制出来的是一整段话，链接夹在中间
        const auto ref_prose = ParseShareText(L"分享歌单《测试》http://music.163.com/playlist?id=12345 来自@外部平台");
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

// 只解码，不创建音频输出设备，也不把播放地址写入日志。
inline bool DecodeOnlineTestAudio(const std::wstring& url, const std::wstring& runtime_dir, std::ostream& log)
{
    if (!BASS_Init(0, 44100, 0, nullptr, nullptr)) { log << "FAIL BASS init: " << BASS_ErrorGetCode() << "\n"; return false; }
    HPLUGIN flac = BASS_PluginLoad((runtime_dir + L"Plugins\\bassflac.dll").c_str(), BASS_UNICODE);
    BASS_SetConfig(BASS_CONFIG_NET_TIMEOUT, 15000);
    HSTREAM stream = BASS_StreamCreateURL(reinterpret_cast<const char*>(url.c_str()), 0, BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT, nullptr, nullptr);
    bool success = false;
    if (!stream) log << "FAIL BASS stream: " << BASS_ErrorGetCode() << "\n";
    else
    {
        double seconds = BASS_ChannelBytes2Seconds(stream, BASS_ChannelGetLength(stream, BASS_POS_BYTE));
        std::vector<float> pcm(44100 * 2);
        DWORD bytes = 0;
        std::uint64_t decoded_bytes{};
        bool decoded = false;
        bool signal = false;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        do
        {
            bytes = BASS_ChannelGetData(stream, pcm.data(), static_cast<DWORD>(pcm.size() * sizeof(float)));
            if (bytes == static_cast<DWORD>(-1)) break;
            if (bytes == 0)
            {
                Sleep(50); // 网络流可能已经读到文件头，但首批音频数据仍在缓冲。
                continue;
            }
            decoded = true;
            decoded_bytes += bytes;
            for (size_t i = 0; i < bytes / sizeof(float); ++i)
                if (std::isfinite(pcm[i]) && std::fabs(pcm[i]) > 0.00001f) { signal = true; break; }
            // 某些正常歌曲开头有短暂静音，最多检查约 15 秒音频。
        } while (!signal && decoded_bytes < 44100ull * 2 * sizeof(float) * 15 && GetTickCount64() < deadline);
        const int decode_error = BASS_ErrorGetCode();
        success = seconds > 0 && decoded && signal;
        log << "duration_seconds=" << seconds << "\n" << "decoded_bytes=" << decoded_bytes
            << "\ndecode_error=" << decode_error << "\nnon_silent_signal=" << signal << "\n" << (success ? "PASS" : "FAIL") << " native audio decoding\n";
        BASS_StreamFree(stream);
    }
    if (flac) BASS_PluginFree(flac);
    BASS_Free();
    return success && log.good();
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
    return DecodeOnlineTestAudio(url, runtime_dir, log);
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
    if (lyric.empty()) log << "lyric_debug status=" << kugou::ToUtf8(cache.Describe()) << '\n';
    if (!audio.empty())
    {
        // 无损缓存直接命中；有损缓存按设计会再解析一次去争取更高音质（解析不到才回退
        // 缓存），所以两种情况要分开核对，不能一律要求返回缓存文件
        {
            const auto repeat_url = online::CSourceRegistry::Instance().ResolvePlayUrl(song->file_path);
            log << "repeat_audio=" << kugou::ToUtf8(audio) << "\nrepeat_url=" << kugou::ToUtf8(repeat_url) << "\n";
            if (online::COnlineMediaCache::IsLossless(audio))
                check(repeat_url == audio, "repeat playback resolves lossless cache before network");
            else
                check(repeat_url == audio || repeat_url.starts_with(L"http://") || repeat_url.starts_with(L"https://"),
                    "lossy cache is re-resolved for a better quality");
        }
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
        if (cover.empty())
        {
            // 看不到封面时把中间结果写出来，便于区分「搜不到图片地址」和「图片下载失败」
            online::Track probe; probe.virtual_path = song->file_path;
            probe.title = song->GetTitle(); probe.artist = song->GetArtist(); probe.album = song->GetAlbum();
            auto* probe_source = online::CSourceRegistry::Instance().FindByPath(song->file_path);
            log << "cover_debug url=" << kugou::ToUtf8(probe_source ? probe_source->GetCoverUrl(probe) : std::wstring()) << "\n";
            log << "cover_debug status=" << kugou::ToUtf8(cache.Describe()) << "\n";
        }
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
