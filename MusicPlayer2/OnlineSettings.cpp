#include "stdafx.h"
#include "OnlineSettings.h"
#include "IniHelper.h"
#include <filesystem>

using namespace std;
namespace online
{
COnlineSettings& COnlineSettings::Instance() { static COnlineSettings settings; return settings; }
void COnlineSettings::Configure(const wstring& directory)
{
    lock_guard<mutex> guard(m_mutex);
    if (!m_path.empty()) return;
    m_path = (filesystem::path(directory) / L"online_settings.ini").wstring();
    CIniHelper ini(m_path);
    m_data.download_directory = ini.GetString(L"download", L"directory", L"");
    m_data.name_order = static_cast<DownloadNameOrder>(clamp(ini.GetInt(L"download", L"name_order", 0), 0, 2));
    m_data.playlist_subfolder = ini.GetBool(L"download", L"playlist_subfolder", true);
    m_data.auto_lyrics = ini.GetBool(L"resources", L"lyrics", true);
    m_data.auto_cover = ini.GetBool(L"resources", L"cover", true);
    m_data.auto_switch_source = ini.GetBool(L"playback", L"auto_switch_source", true);
}
OnlineSettingsData COnlineSettings::Get() const { lock_guard<mutex> guard(m_mutex); return m_data; }
bool COnlineSettings::Save(const OnlineSettingsData& data)
{
    lock_guard<mutex> guard(m_mutex);
    if (m_path.empty()) return false;
    const auto temporary = m_path + L".tmp";
    CIniHelper ini(temporary);
    ini.WriteString(L"download", L"directory", data.download_directory);
    ini.WriteInt(L"download", L"name_order", static_cast<int>(data.name_order));
    ini.WriteBool(L"download", L"playlist_subfolder", data.playlist_subfolder);
    ini.WriteBool(L"resources", L"lyrics", data.auto_lyrics);
    ini.WriteBool(L"resources", L"cover", data.auto_cover);
    ini.WriteBool(L"playback", L"auto_switch_source", data.auto_switch_source);
    if (!ini.Save() || !MoveFileExW(temporary.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { DeleteFileW(temporary.c_str()); return false; }
    m_data = data; return true;
}
wstring COnlineSettings::SafeFileName(wstring name)
{
    for (auto& c : name) if (c < 32 || wstring(L"<>:\"/\\|?*").find(c) != wstring::npos) c = L'_';
    if (name.size() > 120) { name.resize(120); if (name.back() >= 0xd800 && name.back() <= 0xdbff) name.pop_back(); }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"未命名歌曲";
    auto stem = name.substr(0, name.find(L'.')); transform(stem.begin(), stem.end(), stem.begin(), towupper);
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL"
        || (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9')) name.insert(0, L"_");
    return name;
}
wstring COnlineSettings::DownloadName(const Track& track, DownloadNameOrder order)
{
    const auto title = track.title.empty() ? L"未命名歌曲" : track.title;
    return SafeFileName(track.artist.empty() || order == DownloadNameOrder::TitleOnly ? title
        : order == DownloadNameOrder::TitleArtist ? title + L" - " + track.artist : track.artist + L" - " + title);
}
const wchar_t* COnlineSettings::NameOrderText(int order)
{
    static const wchar_t* names[] = {L"歌手 - 歌名", L"歌名 - 歌手", L"歌名"};
    return names[clamp(order, 0, 2)];
}
}
