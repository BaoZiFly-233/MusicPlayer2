#pragma once
#include "OnlineSource.h"
#include <mutex>

namespace online
{
enum class DownloadNameOrder { ArtistTitle, TitleArtist, TitleOnly };
struct OnlineSettingsData
{
    std::wstring download_directory;
    DownloadNameOrder name_order{DownloadNameOrder::ArtistTitle};
    bool playlist_subfolder{true};
    bool auto_lyrics{true}, auto_cover{true};
};

// Online preferences have their own file and lifetime; upstream settings structs remain unchanged.
class COnlineSettings
{
public:
    static COnlineSettings& Instance();
    void Configure(const std::wstring& directory);
    OnlineSettingsData Get() const;
    bool Save(const OnlineSettingsData& data);
    static std::wstring SafeFileName(std::wstring name);
    static std::wstring DownloadName(const Track& track, DownloadNameOrder order);
    static const wchar_t* NameOrderText(int order);
private:
    mutable std::mutex m_mutex;
    std::wstring m_path;
    OnlineSettingsData m_data;
};
}
