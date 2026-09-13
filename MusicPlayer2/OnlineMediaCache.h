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
    std::shared_ptr<State> m_state;
    static void Run(const std::shared_ptr<State>& state, bool lyrics);
    static void Process(const std::shared_ptr<State>& state, const Work& work);
    static void Trim(const std::shared_ptr<State>& state);
};
std::shared_ptr<IOnlineSource> CloneSource(const std::wstring& path);
bool RunMediaCacheFileTests(const std::wstring& root, std::ostream& log);
}
