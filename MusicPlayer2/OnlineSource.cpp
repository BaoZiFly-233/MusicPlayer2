#include "stdafx.h"
#include "OnlineSource.h"
#include "OnlineMediaCache.h"
#include "OnlineJson.h"
#include <chrono>

bool online::IOnlineSource::Browse(const BrowseRequest& request, BrowseResult& result)
{
    result = {};
    if (request.kind != BrowseKind::Search)
        return false;
    std::vector<Track> tracks;
    if (!Search(request.id, request.page, tracks))
        return false;
    // 统一走 AddTrack：副标题里会带上专辑，避免这里和别处拼法不一致
    // （以前这里只写艺术家，搜索结果的专辑名就是这么丢的）。
    for (const auto& track : tracks) AddTrack(result, track);
    result.has_more = tracks.size() >= 30;
    return true;
}
#include "KugouSource.h"
#include "BodianSource.h"

using namespace std;

namespace online
{

// 解析结果备忘的有效期。平台签发的是有时效的链接，不能长期复用；
// 这里只要覆盖「后台预缓存刚解析完 → 用户切到这一首」这段时间就够，
// 而这一步正是切歌不再卡住的关键。
static constexpr std::uint64_t PLAY_URL_TTL_MS = 4 * 60 * 1000;
// 解析失败也记一小会儿。预缓存拿不到地址的曲目（会员曲、下架曲）在切歌时
// 不必让界面再等一遍完整超时；超过这个时间就会重新问平台。
static constexpr std::uint64_t PLAY_URL_FAIL_TTL_MS = 10 * 1000;
// 备忘条数上限。超出时先清过期的，仍然超就整个清掉（忘了比记错安全）。
static constexpr size_t PLAY_URL_MEMO_LIMIT = 256;
// 第一次解析超过这个时间就不再重试。解析在切歌的关键路径上，重试会把等待时间翻倍。
static constexpr std::uint64_t PLAY_URL_SLOW_MS = 3000;

// 这些是真实可播放的协议，不算虚拟路径
static bool IsRealProtocol(const wstring& scheme)
{
    return scheme == L"http" || scheme == L"https" || scheme == L"ftp" || scheme == L"mms";
}

wstring CSourceRegistry::GetScheme(const wstring& path)
{
    if (path.empty())
        return wstring();

    size_t pos = path.find(L"://");
    if (pos == wstring::npos || pos == 0)
        return wstring();

    wstring scheme = path.substr(0, pos);
    // 统一转小写，便于比较
    for (wchar_t& ch : scheme)
        ch = static_cast<wchar_t>(towlower(ch));

    if (IsRealProtocol(scheme))
        return wstring();

    return scheme;
}

bool CSourceRegistry::IsVirtualPath(const wstring& path)
{
    return !GetScheme(path).empty();
}

CSourceRegistry& CSourceRegistry::Instance()
{
    static CSourceRegistry instance;
    return instance;
}

void CSourceRegistry::Register(IOnlineSource* source)
{
    if (source == nullptr)
        return;
    // 避免重复注册同一个 scheme
    if (FindByScheme(source->GetScheme()) != nullptr)
        return;
    m_sources.push_back(source);
}

IOnlineSource* CSourceRegistry::FindByScheme(const wstring& scheme)
{
    if (scheme.empty())
        return nullptr;

    wstring lower = scheme;
    for (wchar_t& ch : lower)
        ch = static_cast<wchar_t>(towlower(ch));

    for (IOnlineSource* source : m_sources)
    {
        if (source != nullptr && source->GetScheme() == lower)
            return source;
    }
    return nullptr;
}

IOnlineSource* CSourceRegistry::FindByPath(const wstring& path)
{
    return FindByScheme(GetScheme(path));
}

std::uint64_t CSourceRegistry::NowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void CSourceRegistry::StorePlayUrl(const wstring& path, const wstring& url, bool usable, const wstring& error)
{
    lock_guard<mutex> guard(m_url_mutex);
    if (m_urls.size() >= PLAY_URL_MEMO_LIMIT)
    {
        const std::uint64_t now = NowMs();
        for (auto it = m_urls.begin(); it != m_urls.end();)
        {
            const std::uint64_t ttl = it->second.usable ? PLAY_URL_TTL_MS : PLAY_URL_FAIL_TTL_MS;
            if (now - it->second.stamp >= ttl) it = m_urls.erase(it); else ++it;
        }
        if (m_urls.size() >= PLAY_URL_MEMO_LIMIT) m_urls.clear();
    }
    PlayUrlMemo memo;
    memo.value = url;
    memo.stamp = NowMs();
    memo.usable = usable;
    m_urls[path] = std::move(memo);
    // 失败原因跟地址一起记；解析成功就把旧的失败原因清掉
    if (usable || error.empty()) m_errors.erase(path);
    else
    {
        if (m_errors.size() >= PLAY_URL_MEMO_LIMIT && m_errors.find(path) == m_errors.end()) m_errors.clear();
        m_errors[path] = error;
    }
}

void CSourceRegistry::RememberPlayUrl(const wstring& path, const wstring& url, const wstring& quality_note)
{
    // 只记可直接交给播放核心的网络地址，本地缓存文件走 FindAudio
    if (path.empty() || !IsVirtualPath(path)) return;
    if (url.compare(0, 7, L"http://") != 0 && url.compare(0, 8, L"https://") != 0) return;
    StorePlayUrl(path, url, true);
    if (!quality_note.empty()) StoreQualityNote(path, quality_note);
}

void CSourceRegistry::StoreQualityNote(const wstring& path, const wstring& note)
{
    lock_guard<mutex> guard(m_url_mutex);
    if (note.empty()) { m_notes.erase(path); return; }
    // 和地址备忘同一个上限：说明只对最近播过的那些歌有意义
    if (m_notes.size() >= PLAY_URL_MEMO_LIMIT && m_notes.find(path) == m_notes.end()) m_notes.clear();
    m_notes[path] = note;
}

std::wstring CSourceRegistry::QualityNote(const wstring& path) const
{
    if (path.empty()) return std::wstring();
    lock_guard<mutex> guard(m_url_mutex);
    auto it = m_notes.find(path);
    return it == m_notes.end() ? std::wstring() : it->second;
}

std::wstring CSourceRegistry::PlayError(const wstring& path) const
{
    if (path.empty()) return std::wstring();
    lock_guard<mutex> guard(m_url_mutex);
    auto it = m_errors.find(path);
    return it == m_errors.end() ? std::wstring() : it->second;
}

std::wstring CSourceRegistry::OriginLabel(const wstring& path)
{
    // 只有地址是唯一依据：本地路径算本地，虚拟地址归它 scheme 对应的音源。
    // 界面据此把本地文件和两个平台分开显示，不额外维护一份可能与地址矛盾的来源字段。
    IOnlineSource* source = Instance().FindByPath(path);
    return source != nullptr ? source->GetShortName() : std::wstring(L"本地");
}

int CSourceRegistry::OriginIndex(const wstring& path)
{
    IOnlineSource* source = Instance().FindByPath(path);
    if (source == nullptr) return 0;
    const auto& sources = Instance().GetAll();
    for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i] == source) return static_cast<int>(i) + 1;
    return 0;
}

std::wstring CSourceRegistry::CachedPlayUrl(const wstring& path) const
{
    if (path.empty()) return std::wstring();
    lock_guard<mutex> guard(m_url_mutex);
    auto it = m_urls.find(path);
    // 只认成功过的地址：失败结论留给 ResolvePlayUrl 自己用，后台任务该试还是要试
    if (it == m_urls.end() || !it->second.usable) return std::wstring();
    if (NowMs() - it->second.stamp >= PLAY_URL_TTL_MS) return std::wstring();
    return it->second.value;
}

void CSourceRegistry::ForgetPlayUrl(const wstring& path)
{
    lock_guard<mutex> guard(m_url_mutex);
    m_urls.erase(path);
    m_notes.erase(path);
    // 失败原因留着：界面要在「播放失败」之后还能告诉用户是为什么，下次解析成功时才清
}

void CSourceRegistry::ForgetAllPlayUrls()
{
    lock_guard<mutex> guard(m_url_mutex);
    m_urls.clear();
    m_notes.clear();
    m_errors.clear();
}

// 本地缓存命中时的音质说明：文件扩展名就是音质的全部依据（缓存键里不含音质）。
static wstring CachedQualityNote(const wstring& cached)
{
    if (cached.empty()) return wstring();
    return COnlineMediaCache::IsLossless(cached) ? L"当前播放：本地缓存 · 无损" : L"当前播放：本地缓存（有损，联网后会争取更高音质）";
}

wstring CSourceRegistry::ResolvePlayUrl(const wstring& path)
{
    if (path.empty())
        return wstring();

    // 本地文件或已经是真实地址，原样返回，调用方不必自己判断
    if (!IsVirtualPath(path))
        return path;

    IOnlineSource* source = Instance().FindByPath(path);
    if (source == nullptr)
        return wstring();

    auto& cache = COnlineMediaCache::Instance();
    auto cached = cache.FindAudio(path);
    // 无损缓存直接用。有损缓存不再一票否决：再解析一次，平台给到更高音质就按网络流播放
    // （后台会把新音质缓存下来），取不到时仍回退到缓存，保证离线也能播。
    if (!cached.empty() && COnlineMediaCache::IsLossless(cached))
    {
        StoreQualityNote(path, CachedQualityNote(cached));
        return cached;
    }

    // 短期内解析过的结果直接用。解析是同步的，而且每个平台要发一到三个请求，
    // 这一步放在切歌的路径上，所以命中备忘和命中缓存一样重要。
    const std::uint64_t now = NowMs();
    {
        lock_guard<mutex> guard(m_url_mutex);
        auto it = m_urls.find(path);
        if (it != m_urls.end())
        {
            const std::uint64_t ttl = it->second.usable ? PLAY_URL_TTL_MS : PLAY_URL_FAIL_TTL_MS;
            if (now - it->second.stamp < ttl)
            {
                // 命中成功条目时说明已经跟着地址记好了；失败条目回退到缓存则按缓存说明
                if (!it->second.usable && !cached.empty()) m_notes[path] = CachedQualityNote(cached);
                return it->second.usable ? it->second.value : cached;
            }
            m_urls.erase(it);
        }
    }

    const std::uint64_t started = NowMs();
    auto resolved = source->ResolvePlayUrl(path);

    // 重试一次，但只在不是权限问题、而且第一次没拖太久的时候。
    // 地址签发接口偶发超时会直接返回空，重试往往就能拿到；而「需要会员」「已下架」
    // 这类结论是确定的，再问一次只是白跑一趟。第一次已经卡了几秒的话也不再重试：
    // 这条路径在切歌的关键路径上，重试等于把那几十秒原样再加一遍。
    if (resolved.empty())
    {
        const std::wstring error = source->GetLastError();
        const bool decided = error.find(L"会员") != std::wstring::npos
            || error.find(L"权限") != std::wstring::npos
            || error.find(L"版权") != std::wstring::npos
            || error.find(L"下架") != std::wstring::npos
            || error.find(L"登录") != std::wstring::npos;
        const bool slow = NowMs() - started > PLAY_URL_SLOW_MS;
        if (!decided && !slow) resolved = source->ResolvePlayUrl(path);
    }

    // 音源返回的必须是可直接交给播放器的地址，防止把错误提示误当链接传下去
    if (!resolved.empty() && !resolved.starts_with(L"http://") && !resolved.starts_with(L"https://"))
        resolved.clear();

    StorePlayUrl(path, resolved, !resolved.empty(), resolved.empty() ? source->GetLastError() : std::wstring());
    // 音质说明跟着这次解析走：成功就记音源给的说明，失败回退缓存就记缓存的
    StoreQualityNote(path, resolved.empty() ? CachedQualityNote(cached) : source->GetQualityNote());

    return resolved.empty() ? cached : resolved;
}

// 内置音源在这里注册。注册顺序即界面上的显示顺序。
void InitOnlineSources()
{
    CSourceRegistry::Instance().Register(new kugou::CKugouSource());
    CSourceRegistry::Instance().Register(new bodian::CBodianSource());
}

} // namespace online
