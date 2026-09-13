#include "stdafx.h"
#include "OnlineMediaCache.h"
#include "OnlineSettings.h"
#include "KugouSource.h"
#include "BodianSource.h"
#include "KugouCrypto.h"
#include "Lyric.h"
#include "AudioTag.h"
#include "TagLibHelper.h"
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <map>
#include <atomic>
#pragma comment(lib, "winhttp.lib")

namespace online
{
namespace fs = std::filesystem;
using namespace std;
static constexpr uintmax_t CACHE_LIMIT = 1024ULL * 1024 * 1024;
static constexpr uintmax_t FILE_LIMIT = 256ULL * 1024 * 1024;
static const wchar_t* AUDIO_EXTENSIONS[] = { L".mp3", L".flac", L".ogg", L".m4a", L".wav" };
static const wchar_t* COVER_EXTENSIONS[] = { L".jpg", L".png", L".gif" };
struct COnlineMediaCache::Work
{
    Track track;
    wstring directory;
    DownloadNameOrder name_order{DownloadNameOrder::ArtistTitle};
    shared_ptr<IOnlineSource> source;
    unsigned generation{};
    unsigned prefetch_generation{};
    bool prefetch{};
    bool lyric{};
    bool cover{};
};
struct COnlineMediaCache::State
{
    fs::path root, settings;
    mutable mutex lock;
    mutex resource_lock;
    condition_variable ready;
    deque<Work> audio, lyrics;
    set<wstring> lyric_pending;
    set<wstring> cover_pending;
    atomic<bool> stopping{false};
    atomic<unsigned> generation{0};
    atomic<unsigned> prefetch_generation{0};
    wstring prefetch_target;
    bool enabled{true}, audio_active{}, lyric_active{};
    size_t saved{}, failed{};
    wstring status;
    map<wstring, wstring> downloads;
};
static wstring Key(const wstring& path) { return kugou::FromUtf8(kugou::Md5Hex(kugou::ToUtf8(path))); }
static uintmax_t FileSize(const fs::path& path)
{
    error_code error; auto size = fs::file_size(path, error); return error ? 0 : size;
}
static bool OwnFile(const fs::path& path)
{
    const auto stem = path.stem().wstring();
    const auto extension = path.extension().wstring();
    bool managed = extension == L".lrc" || extension == L".part" || extension == L".lyrpart" || extension == L".coverpart";
    for (auto* audio : AUDIO_EXTENSIONS) managed = managed || extension == audio;
    for (auto* cover : COVER_EXTENSIONS) managed = managed || extension == cover;
    return managed && stem.size() == 32 && all_of(stem.begin(), stem.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
}
static wstring Find(const fs::path& root, const wstring& key)
{
    error_code error;
    for (auto* extension : AUDIO_EXTENSIONS)
    {
        auto file = root / (key + extension);
        if (fs::is_regular_file(file, error) && FileSize(file) > 0) return file.wstring();
    }
    return {};
}
shared_ptr<IOnlineSource> CloneSource(const wstring& path)
{
    auto* source = CSourceRegistry::Instance().FindByPath(path);
    if (auto* kg = dynamic_cast<kugou::CKugouSource*>(source)) return make_shared<kugou::CKugouSource>(*kg);
    if (auto* bd = dynamic_cast<bodian::CBodianSource*>(source)) return make_shared<bodian::CBodianSource>(*bd);
    return {};
}
COnlineMediaCache& COnlineMediaCache::Instance() { static COnlineMediaCache cache; return cache; }
void COnlineMediaCache::Configure(const wstring& config_dir)
{
    if (m_state) return;
    auto state = make_shared<State>();
    state->root = fs::path(config_dir) / L"online_cache";
    state->settings = fs::path(config_dir) / L"online_media.ini";
    state->enabled = GetPrivateProfileIntW(L"cache", L"prefetch_next", 1, state->settings.c_str()) != 0;
    error_code error; fs::create_directories(state->root, error);
    if (error) state->status = L"缓存目录无法写入";
    else
    {
        // Incomplete transfers from an earlier process are never reusable media.
        for (fs::directory_iterator it(state->root, error), end; !error && it != end; it.increment(error))
            if (OwnFile(it->path()) && (it->path().extension() == L".part" || it->path().extension() == L".lyrpart" || it->path().extension() == L".coverpart"))
            { error_code ignored; fs::remove(it->path(), ignored); }
    }
    m_state = state;
    Trim(state);
    thread([state] { Run(state, false); }).detach();
    thread([state] { Run(state, true); }).detach();
}
void COnlineMediaCache::Shutdown()
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock);
    m_state->stopping = true; ++m_state->generation;
    m_state->audio.clear(); m_state->lyrics.clear(); m_state->ready.notify_all();
}
bool COnlineMediaCache::IsBusy() const
{
    if (!m_state) return false;
    lock_guard<mutex> guard(m_state->lock);
    return m_state->audio_active || m_state->lyric_active || !m_state->audio.empty() || !m_state->lyrics.empty();
}
bool COnlineMediaCache::Enabled() const
{
    if (!m_state) return false;
    lock_guard<mutex> guard(m_state->lock); return m_state->enabled;
}
void COnlineMediaCache::SetEnabled(bool enabled)
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock); m_state->enabled = enabled;
    if (!enabled)
    {
        ++m_state->prefetch_generation; m_state->prefetch_target.clear();
        erase_if(m_state->audio, [](const Work& work) { return work.prefetch; });
    }
    WritePrivateProfileStringW(L"cache", L"prefetch_next", enabled ? L"1" : L"0", m_state->settings.c_str());
}
wstring COnlineMediaCache::FindAudio(const wstring& path) const
{
    return m_state ? Find(m_state->root, Key(path)) : L"";
}
wstring COnlineMediaCache::FindLyric(const wstring& path) const
{
    if (!m_state) return {};
    auto file = m_state->root / (Key(path) + L".lrc");
    error_code error;
    return fs::is_regular_file(file, error) && FileSize(file) > 0 ? file.wstring() : L"";
}
void COnlineMediaCache::PrefetchNext(const wstring& path)
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock);
    const auto target = m_state->enabled ? path : L"";
    if (target == m_state->prefetch_target) return;
    m_state->prefetch_target = target; ++m_state->prefetch_generation;
    erase_if(m_state->audio, [](const Work& work) { return work.prefetch; });
    if (target.empty() || !FindAudio(target).empty()) return;
    auto source = CloneSource(target); if (!source) return;
    Work work; work.track.virtual_path = target; work.source = source; work.generation = m_state->generation;
    work.prefetch = true; work.prefetch_generation = m_state->prefetch_generation;
    m_state->audio.push_back(std::move(work)); m_state->ready.notify_all();
}
static wstring FindCoverFile(const fs::path& root, const wstring& key)
{
    for (auto* extension : COVER_EXTENSIONS)
    {
        auto file = root / (key + extension);
        if (FileSize(file) > 0) return file.wstring();
    }
    return {};
}
wstring COnlineMediaCache::FindCover(const wstring& path) const
{
    return m_state ? FindCoverFile(m_state->root, Key(path)) : L"";
}
void COnlineMediaCache::QueueCover(const Track& track)
{
    if (!m_state || !FindCover(track.virtual_path).empty()) return;
    auto source = CloneSource(track.virtual_path); if (!source) return;
    lock_guard<mutex> guard(m_state->lock);
    if (!m_state->cover_pending.insert(track.virtual_path).second) return;
    Work work; work.track = track; work.source = source; work.cover = true; work.generation = m_state->generation;
    m_state->lyrics.push_back(std::move(work)); m_state->ready.notify_all();
}
void COnlineMediaCache::QueueLyric(const wstring& path)
{
    if (!m_state || !FindLyric(path).empty()) return;
    auto source = CloneSource(path); if (!source) return;
    lock_guard<mutex> guard(m_state->lock);
    if (!m_state->lyric_pending.insert(path).second) return;
    Work work; work.track.virtual_path = path; work.source = source; work.lyric = true; work.generation = m_state->generation;
    m_state->lyrics.push_back(std::move(work)); m_state->ready.notify_all();
}
void COnlineMediaCache::SaveAudio(const Track& track, const wstring& directory)
{
    if (!m_state) return;
    auto source = CloneSource(track.virtual_path); if (!source) return;
    lock_guard<mutex> guard(m_state->lock);
    if (m_state->downloads[track.virtual_path] == L"下载中") return;
    m_state->downloads[track.virtual_path] = L"下载中";
    Work work; work.track = track; work.directory = directory; work.source = source; work.generation = m_state->generation;
    work.name_order = COnlineSettings::Instance().Get().name_order;
    m_state->audio.push_back(std::move(work));
    m_state->status = L"已加入保存队列"; m_state->ready.notify_all();
}
wstring COnlineMediaCache::DownloadLabel(const wstring& path) const
{
    if (!m_state) return L"下载";
    lock_guard<mutex> guard(m_state->lock);
    const auto it = m_state->downloads.find(path);
    return it == m_state->downloads.end() ? L"下载" : it->second;
}
wstring COnlineMediaCache::AudioExtension(const string& h)
{
    if (h.size() < 12) return {};
    if (h.compare(0, 4, "fLaC") == 0) return L".flac";
    if (h.compare(0, 4, "OggS") == 0) return L".ogg";
    if (h.compare(0, 4, "RIFF") == 0 && h.compare(8, 4, "WAVE") == 0) return L".wav";
    if (h.compare(4, 4, "ftyp") == 0) return L".m4a";
    if (h.compare(0, 3, "ID3") == 0 || (static_cast<unsigned char>(h[0]) == 0xff && (static_cast<unsigned char>(h[1]) & 0xe0) == 0xe0)) return L".mp3";
    return {};
}
static bool Download(const wstring& url, const fs::path& temporary, const function<bool()>& cancelled,
    wstring& extension, wstring& error, bool cover = false)
{
    const uintmax_t limit = cover ? 8ULL * 1024 * 1024 : FILE_LIMIT;
    URL_COMPONENTS parts{sizeof(URL_COMPONENTS)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    const bool secure = url.starts_with(L"https://");
    if ((!secure && !url.starts_with(L"http://")) || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    { error = L"音频地址无效"; return false; }
    wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    auto close = [](void* p) { if (p) WinHttpCloseHandle(p); };
    using Handle = unique_ptr<void, decltype(close)>;
    Handle session(WinHttpOpen(L"MusicPlayer2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), close);
    if (!session) { error = L"无法初始化下载"; return false; }
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 15000);
    Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), close);
    Handle request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr, close);
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.get(), nullptr)) { error = L"下载连接失败或超时"; return false; }
    DWORD status{}, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { error = L"下载失败（HTTP " + to_wstring(status) + L"）"; return false; }
    DWORD expected{}; size = sizeof(expected);
    const bool has_length = WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &expected, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (has_length && expected > limit) { error = L"文件超过大小上限"; return false; }
    ofstream output(temporary, ios::binary | ios::trunc);
    if (!output) { error = L"无法写入缓存目录"; return false; }
    char buffer[65536]; uintmax_t total{}; string header;
    while (!cancelled())
    {
        DWORD read{};
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read)) { error = L"音频下载中断"; return false; }
        if (!read) break;
        total += read;
        if (total > limit) { error = L"文件超过大小上限"; return false; }
        if (header.size() < 32) header.append(buffer, (std::min)(size_t(read), 32 - header.size()));
        output.write(buffer, read);
        if (!output) { error = L"磁盘空间不足或写入失败"; return false; }
    }
    output.close();
    if (cancelled()) { error = L"下载已取消"; return false; }
    if (cover)
    {
        if (header.starts_with("\x89PNG\r\n\x1a\n")) extension = L".png";
        else if (header.starts_with("\xff\xd8\xff")) extension = L".jpg";
        else if (header.starts_with("GIF87a") || header.starts_with("GIF89a")) extension = L".gif";
        CImage picture;
        if (extension.empty() || FAILED(picture.Load(temporary.c_str())) || picture.GetWidth() > 8192 || picture.GetHeight() > 8192)
        { error = L"封面图片无效"; return false; }
    }
    else extension = COnlineMediaCache::AudioExtension(header);
    if (total < (cover ? 32 : 1024) || (has_length && total != expected) || extension.empty() || !output)
    { error = L"音频不完整或格式不支持，未加入缓存"; return false; }
    return true;
}
void COnlineMediaCache::Run(const shared_ptr<State>& state, bool lyrics)
{
    for (;;)
    {
        Work work;
        {
            unique_lock<mutex> guard(state->lock);
            auto& queue = lyrics ? state->lyrics : state->audio;
            state->ready.wait(guard, [&] { return state->stopping || !queue.empty(); });
            if (state->stopping) return;
            work = std::move(queue.front()); queue.pop_front();
            (lyrics ? state->lyric_active : state->audio_active) = true;
            if (!lyrics) state->status = work.directory.empty() ? L"正在预缓存下一首…" : L"正在下载…";
        }
        try { Process(state, work); }
        catch (const exception&) {
            lock_guard<mutex> guard(state->lock); state->status = L"下载失败";
            if (!work.directory.empty()) { ++state->failed; state->downloads[work.track.virtual_path] = L"下载失败"; }
        }
        lock_guard<mutex> guard(state->lock);
        (lyrics ? state->lyric_active : state->audio_active) = false;
        if (lyrics) (work.cover ? state->cover_pending : state->lyric_pending).erase(work.track.virtual_path);
    }
}
void COnlineMediaCache::Process(const shared_ptr<State>& state, const Work& work)
{
    auto cancelled = [&] { return state->stopping || state->generation != work.generation
        || (work.prefetch && state->prefetch_generation != work.prefetch_generation); };
    if (cancelled()) return;
    const auto key = Key(work.track.virtual_path);
    if (work.lyric)
    {
        lock_guard<mutex> resource_guard(state->resource_lock);
        if (cancelled() || !work.source) return;
        if (FileSize(state->root / (key + L".lrc")) > 0) return;
        Lyric lyric;
        if (!work.source->GetLyric(work.track.virtual_path, lyric) || !lyric.HasContent() || cancelled()) return;
        CLyrics parsed; parsed.LyricsFromRowString(lyric.content);
        if (parsed.IsEmpty()) return;
        auto temporary = state->root / (key + L".lyrpart"), target = state->root / (key + L".lrc");
        ofstream output(temporary, ios::binary | ios::trunc);
        const auto bytes = kugou::ToUtf8(lyric.content);
        output.write("\xef\xbb\xbf", 3); output.write(bytes.data(), bytes.size()); output.close();
        lock_guard<mutex> guard(state->lock);
        if (output && !cancelled()) MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        error_code error; fs::remove(temporary, error); return;
    }
    if (work.cover)
    {
        lock_guard<mutex> resource_guard(state->resource_lock);
        if (cancelled() || !work.source || !FindCoverFile(state->root, key).empty()) return;
        const auto url = work.source->GetCoverUrl(work.track);
        if (url.empty() || cancelled()) return;
        const auto temporary = state->root / (key + L".coverpart"); wstring extension, error;
        if (Download(url, temporary, cancelled, extension, error, true))
        {
            lock_guard<mutex> guard(state->lock);
            const auto target = state->root / (key + extension);
            if (!cancelled()) MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        error_code ignored; fs::remove(temporary, ignored); return;
    }
    wstring file = Find(state->root, key), error;
    if (file.empty())
    {
        auto url = work.source ? work.source->ResolvePlayUrl(work.track.virtual_path) : L"";
        if (url.empty()) error = work.source ? work.source->GetLastError() : L"音频地址无效";
        else if (!cancelled())
        {
            const auto temporary = state->root / (key + L".part"); wstring extension;
            if (Download(url, temporary, cancelled, extension, error))
            {
                lock_guard<mutex> guard(state->lock);
                const auto target = state->root / (key + extension);
                if (!cancelled() && MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) file = target.wstring();
                else if (!cancelled()) error = L"缓存保存失败";
            }
            error_code ignored; fs::remove(temporary, ignored);
        }
    }
    if (cancelled()) return;
    wstring missing;
    if (!file.empty() && !work.directory.empty())
    {
        Work resource = work; resource.directory.clear(); resource.lyric = true;
        try { Process(state, resource); } catch (const exception&) { }
        resource.lyric = false; resource.cover = true;
        try { Process(state, resource); } catch (const exception&) { }
        if (cancelled()) return;
        const auto name = COnlineSettings::DownloadName(work.track, work.name_order);
        bool saved = false;
        wchar_t temporary[MAX_PATH]{};
        struct RemoveTemporary { const wchar_t* path; ~RemoveTemporary() { if (*path) DeleteFileW(path); } } cleanup{temporary};
        if (GetTempFileNameW(work.directory.c_str(), L"btm", 0, temporary) && CopyFileW(file.c_str(), temporary, FALSE))
        {
            SongInfo metadata; metadata.file_path = temporary;
            const auto type = CAudioCommon::GetAudioTypeByFileName(file);
            CAudioTag tags(metadata, type);
            tags.GetAudioTag();
            if (!work.track.title.empty()) metadata.title = work.track.title;
            if (!work.track.artist.empty()) metadata.artist = work.track.artist;
            if (!work.track.album.empty()) metadata.album = work.track.album;
            bool tagged = tags.WriteAudioTag();
            if (!tagged) error = L"写入歌曲信息失败";
            const auto lyric_path = state->root / (key + L".lrc");
            string bytes;
            if (CCommon::GetFileContent(lyric_path.c_str(), bytes) && !bytes.empty())
            {
                if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
                if (!tags.WriteAudioLyric(kugou::FromUtf8(bytes))) { tagged = false; error = L"写入内嵌歌词失败"; }
            }
            else missing = L"歌词";
            const auto cover = FindCoverFile(state->root, key);
            if (!cover.empty())
            {
                if (!tags.WriteAlbumCover(cover)) { tagged = false; error = L"写入内嵌封面失败"; }
            }
            else { if (!missing.empty()) missing += L"、"; missing += L"封面"; }
            // Only publish the private copy after every available tag has been written.
            for (int i = 0; tagged && i < 1000 && !cancelled(); ++i)
            {
                auto target = fs::path(work.directory) / (name + (i ? L" (" + to_wstring(i) + L")" : L"") + fs::path(file).extension().wstring());
                if (MoveFileExW(temporary, target.c_str(), MOVEFILE_WRITE_THROUGH)) { saved = true; break; }
                if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
            }
        }
        if (!saved && error.empty()) error = L"歌曲保存失败，请检查目标目录和磁盘空间";
    }
    {
        lock_guard<mutex> guard(state->lock);
        if (!work.directory.empty()) {
            const bool saved = error.empty() && !file.empty();
            if (saved) ++state->saved; else ++state->failed;
            state->downloads[work.track.virtual_path] = saved ? (missing.empty() ? L"已下载" : L"标签不全") : L"下载失败";
        }
        state->status = !error.empty() ? error : work.directory.empty() ? L"下一首已缓存"
            : missing.empty() ? L"下载完成，歌词和封面已内嵌" : L"歌曲已下载，未获取到" + missing;
    }
    Trim(state);
}
void COnlineMediaCache::Trim(const shared_ptr<State>& state)
{
    lock_guard<mutex> guard(state->lock);
    error_code error; vector<fs::directory_entry> files; uintmax_t total{};
    for (fs::directory_iterator it(state->root, error), end; !error && it != end; it.increment(error))
    {
        if (!it->is_regular_file(error) || !OwnFile(it->path()) || it->path().extension() == L".part" || it->path().extension() == L".lyrpart" || it->path().extension() == L".coverpart") continue;
        total += FileSize(it->path()); files.push_back(*it);
    }
    sort(files.begin(), files.end(), [](const auto& a, const auto& b) { error_code e; return a.last_write_time(e) < b.last_write_time(e); });
    for (const auto& file : files)
    {
        if (total <= CACHE_LIMIT) break;
        auto bytes = FileSize(file.path());
        if (fs::remove(file.path(), error)) total -= bytes;
    }
}
void COnlineMediaCache::Clear()
{
    if (!m_state) return;
    lock_guard<mutex> guard(m_state->lock);
    ++m_state->generation; m_state->audio.clear(); m_state->lyrics.clear();
    ++m_state->prefetch_generation; m_state->prefetch_target.clear();
    for (auto& [path, status] : m_state->downloads) if (status == L"下载中") status = L"已取消";
    m_state->lyric_pending.clear();
    m_state->cover_pending.clear();
    error_code error; bool failed = false;
    for (fs::directory_iterator it(m_state->root, error), end; !error && it != end; it.increment(error))
    {
        if (!it->is_regular_file(error) || !OwnFile(it->path())) continue;
        if (!fs::remove(it->path(), error)) failed = true;
        error.clear();
    }
    m_state->status = failed ? L"已清理可删除的缓存，正在使用的文件会保留" : L"缓存已清理，手动保存的歌曲已保留";
}
wstring COnlineMediaCache::Describe() const
{
    if (!m_state) return L"缓存尚未初始化";
    lock_guard<mutex> guard(m_state->lock);
    error_code error; uintmax_t total{}; size_t count{};
    for (fs::directory_iterator it(m_state->root, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error) && OwnFile(it->path())) { total += FileSize(it->path()); ++count; }
    return wstring(L"预缓存下一首：") + (m_state->enabled ? L"已开启" : L"已关闭")
        + L"\n\n已用 " + to_wstring(total / 1024 / 1024) + L" MB / 1024 MB · " + to_wstring(count)
        + L" 个文件\n下载中 / 排队：" + to_wstring(m_state->audio.size() + (m_state->audio_active ? 1 : 0))
        + L" 首\n下载：成功 " + to_wstring(m_state->saved) + L" 首，失败 " + to_wstring(m_state->failed) + L" 首\n\n" + m_state->status;
}

bool RunMediaCacheFileTests(const wstring& root, ostream& log)
{
    bool success = true;
    auto check = [&](bool condition, const char* name) { log << (condition ? "PASS " : "FAIL ") << name << '\n'; success = success && condition; };
    COnlineMediaCache cache;
    cache.m_state = make_shared<COnlineMediaCache::State>();
    auto state = cache.m_state;
    state->root = fs::path(root) / L"cache-fixture";
    const auto exports = fs::path(root) / L"saved-fixture";
    error_code error; fs::create_directories(state->root, error); fs::create_directories(exports, error);
    auto write = [](const fs::path& path, const string& text) { ofstream file(path, ios::binary); file << text; };
    const wstring virtual_path = L"bodian://42", key = Key(virtual_path);
    write(state->root / (key + L".part"), "incomplete");
    check(cache.FindAudio(virtual_path).empty(), "partial download is not a cache hit");
    string wave(44 + 2048, '\0');
    wave.replace(0, 4, "RIFF"); wave.replace(8, 8, "WAVEfmt "); wave.replace(36, 4, "data");
    auto put32 = [&](size_t at, unsigned value) { for (int i = 0; i < 4; ++i) wave[at + i] = static_cast<char>(value >> (i * 8)); };
    put32(4, 36 + 2048); put32(16, 16); wave[20] = 1; wave[22] = 1;
    put32(24, 8000); put32(28, 16000); wave[32] = 2; wave[34] = 16; put32(40, 2048);
    write(state->root / (key + L".wav"), wave);
    check(cache.FindAudio(virtual_path) == (state->root / (key + L".wav")).wstring(), "completed cache uses detected file extension");
    COnlineMediaCache::Work save;
    save.track.virtual_path = virtual_path; save.track.title = L"fixture";
    save.directory = exports.wstring();
    COnlineMediaCache::Process(state, save); COnlineMediaCache::Process(state, save);
    size_t exported{};
    for (const auto& file : fs::directory_iterator(exports)) if (file.path().extension() == L".wav") ++exported;
    check(exported == 2 && state->saved == 2, "manual save creates distinct files without overwriting");
    save.directory = (exports / L"missing-directory").wstring();
    COnlineMediaCache::Process(state, save);
    check(state->failed == 1 && !cache.FindAudio(virtual_path).empty(), "failed manual save preserves cached audio");
    class LyricsFixture : public IOnlineSource
    {
        wstring GetScheme() const override { return L"fixture"; }
        wstring GetDisplayName() const override { return L"fixture"; }
        bool Search(const wstring&, int, vector<Track>&) override { return false; }
        wstring ResolvePlayUrl(const wstring&) override { return {}; }
        bool GetLyric(const wstring&, Lyric& lyric) override { lyric.content = L"[00:00.00]Fixture\n[00:01.00]Next line"; return true; }
    };
    COnlineMediaCache::Work lyric;
    lyric.track.virtual_path = virtual_path; lyric.lyric = true; lyric.source = make_shared<LyricsFixture>();
    ++state->generation;
    COnlineMediaCache::Process(state, lyric);
    check(cache.FindLyric(virtual_path).empty(), "cancelled generation cannot publish lyrics");
    lyric.generation = state->generation;
    COnlineMediaCache::Process(state, lyric);
    CLyrics parsed(cache.FindLyric(virtual_path));
    check(!cache.FindLyric(virtual_path).empty() && !parsed.IsEmpty(), "cached lyrics are readable by native lyric parser");
    check(!parsed.GetLyric(0).HasWordTiming(), "ordinary LRC remains line highlighted");
    CLyrics mixed;
    mixed.LyricsFromRowString(L"[00:00.00]<00:00.00>First <00:01.00>line<00:02.00>\n[00:03.00]Plain line\n[00:06.00]End");
    check(mixed.GetLyric(0).HasWordTiming() && !mixed.GetLyric(1).HasWordTiming(), "mixed lyrics use word highlighting only with word timestamps");
    CPlayTime position; position.fromInt(4500);
    const auto progress = mixed.GetLyricProgress(position, false, false, [](const wstring& text) { return static_cast<int>(text.size()); });
    check(progress > 0 && progress < 1000, "line progress remains available for scrolling and transitions");
    COnlineMediaCache::Work stale_prefetch;
    stale_prefetch.track.virtual_path = L"fixture://stale"; stale_prefetch.prefetch = true;
    stale_prefetch.generation = state->generation; stale_prefetch.prefetch_generation = state->prefetch_generation;
    ++state->prefetch_generation;
    COnlineMediaCache::Process(state, stale_prefetch);
    check(cache.FindAudio(stale_prefetch.track.virtual_path).empty(), "obsolete next-song work cannot resolve or publish audio");
    save.generation = state->generation; save.directory = exports.wstring();
    CImage fixture_cover; fixture_cover.Create(8, 8, 24);
    check(SUCCEEDED(fixture_cover.Save((state->root / (key + L".png")).c_str())), "create native cover fixture");
    COnlineMediaCache::Process(state, save);
    check(state->saved == 3, "changing next song does not cancel manual downloads");
    const auto tagged = exports / L"fixture (2).wav";
    int image_type{};
    check(!CTagLibHelper::GetWavLyric(tagged.wstring()).empty() && !CTagLibHelper::GetWavAlbumCover(tagged.wstring(), image_type).empty(),
        "download embeds lyric and cover inside the audio file");
    check(std::distance(fs::directory_iterator(exports), fs::directory_iterator{}) == 3, "download does not export separate lyrics or cover files");
    check(FileSize(state->root / (key + L".wav")) == wave.size() && CTagLibHelper::GetWavLyric(cache.FindAudio(virtual_path)).empty(),
        "tagging the download does not modify the playback cache");
    write(state->root / L"keep.txt", "unmanaged");
    write(state->root / (key + L".zip"), "unmanaged extension");
    cache.Clear();
    check(cache.FindAudio(virtual_path).empty() && cache.FindLyric(virtual_path).empty(), "clear removes managed cached audio and lyrics");
    exported = 0;
    for (const auto& file : fs::directory_iterator(exports)) if (file.path().extension() == L".wav") ++exported;
    check(exported == 3 && fs::exists(state->root / L"keep.txt") && fs::exists(state->root / (key + L".zip")), "clear preserves manual exports and unrelated files");
    // These two directories were created exclusively for this test under its temporary root.
    fs::remove_all(state->root, error); fs::remove_all(exports, error);
    return success;
}
}
