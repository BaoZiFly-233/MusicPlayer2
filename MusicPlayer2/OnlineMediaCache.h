#pragma once
#include "OnlineSource.h"
#include <memory>
#include <iosfwd>

namespace online
{
// Completed media and lyrics only. Expiring addresses and account data stay in memory.
class COnlineMediaCache
{
    friend bool RunMediaCacheFileTests(const std::wstring& root, std::ostream& log);
public:
    static COnlineMediaCache& Instance();
    void Configure(const std::wstring& config_dir);
    void Shutdown();
    std::wstring FindAudio(const std::wstring& path) const;
    // 无损缓存（flac/wav）可以直接复用；有损缓存不应该把音质永久锁在低档。
    static bool IsLossless(const std::wstring& path);
    std::wstring FindLyric(const std::wstring& path) const;
    std::wstring FindCover(const std::wstring& path) const;
    void PrefetchNext(const std::wstring& path);
    void QueueLyric(const std::wstring& path);
    void QueueCover(const Track& track);
    void SaveAudio(const Track& track, const std::wstring& directory);
    std::wstring DownloadLabel(const std::wstring& path) const;
    bool Enabled() const;
    bool IsBusy() const;
    void SetEnabled(bool enabled);
    void Clear();
    std::wstring Describe() const;
    static std::wstring AudioExtension(const std::string& header);
private:
    struct Work;
    struct State;
    // 队列编号：0 音频、1 歌词、2 封面。三者各自一个工作线程，
    // 封面下载不再挡住歌词，歌词也不再挡住封面。
    enum class Queue { Audio, Lyric, Cover };
    std::shared_ptr<State> m_state;
    static void Run(const std::shared_ptr<State>& state, Queue queue);
    static void Process(const std::shared_ptr<State>& state, const Work& work);
    static void Trim(const std::shared_ptr<State>& state);
};
std::shared_ptr<IOnlineSource> CloneSource(const std::wstring& path);
bool RunMediaCacheFileTests(const std::wstring& root, std::ostream& log);
}
