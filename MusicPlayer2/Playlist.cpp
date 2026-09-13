#include "stdafx.h"
#include "Playlist.h"
#include "OnlineSource.h"
#include "Common.h"
#include "FilePathHelper.h"
#include "SongDataManager.h"
#include "TinyXml2Helper.h"
#include "nlohmann/json.hpp"
#pragma warning(disable: 4996)

const vector<wstring> CPlaylistFile::m_surpported_playlist{ PLAYLIST_EXTENSION_2, L"m3u", L"m3u8", L"wpl", L"ttpl", L"btplaylist"};

/*
播放列表文件格式说明
每行一个曲目，每一行的格式为：
文件路径|是否为cue音轨|cue音轨起始时间|cue音轨结束时间|标题|艺术家|唱片集|曲目序号|比特率|流派|年份|注释|cue文件路径
播放列表至少要保存能够在song_data.dat清空时原样恢复特定歌曲的项目
目前除了cue音轨外，其他曲目只保存文件路径
列表cue条目必要保存的项目有“文件路径”、“音轨号”、“cue文件路径”，但出于向后兼容考虑仍然保留其他项目（实际上不以这些项目为准，外部编辑会被忽略）
*/

CPlaylistFile::CPlaylistFile()
{
}


CPlaylistFile::~CPlaylistFile()
{
}

wstring DeleteInvalidCh(const wstring& str)
{
    wstring result = str;
    CCommon::StringCharacterReplace(result, L'|', L'_');
    CCommon::StringCharacterReplace(result, L'\r', L' ');
    CCommon::StringCharacterReplace(result, L'\n', L' ');
    return result;
}

bool CPlaylistFile::LoadFromFile(const wstring & file_path)
{
    m_playlist.clear();
    m_path = file_path;

    //判断文件编码
    bool utf8{};
    wstring file_extension = CFilePathHelper(file_path).GetFileExtension();
    utf8 = (file_extension != L"m3u");

    std::string file_content;
    if (CCommon::GetFileContent(file_path.c_str(), file_content))
    {
        if (file_extension == L"json" || file_extension == L"btplaylist")
        {
            try
            {
                auto root = nlohmann::json::parse(file_content);
                if (!root.is_object() || root.value("format", "") != "BoTapMusic" || root.value("version", 0) != 1
                    || !root.contains("songs") || !root["songs"].is_array() || root["songs"].size() > 100000) return false;
                vector<SongInfo> songs;
                for (const auto& value : root["songs"])
                {
                    SongInfo song;
                    song.file_path = CCommon::StrToUnicode(value.at("path").get<string>(), CodeType::UTF8);
                    if (song.file_path.empty() || song.file_path.find_first_of(L"\r\n") != wstring::npos) return false;
                    bool is_online = online::CSourceRegistry::IsVirtualPath(song.file_path);
                    bool is_url = CCommon::IsURL(song.file_path);
                    if (!is_online && !is_url)
                        song.file_path = CCommon::RelativePathToAbsolutePath(song.file_path, CFilePathHelper(m_path).GetDir());
                    if (!is_online && !is_url && !CCommon::IsPath(song.file_path)) return false;
                    song.title = CCommon::StrToUnicode(value.value("title", ""), CodeType::UTF8);
                    song.artist = CCommon::StrToUnicode(value.value("artist", ""), CodeType::UTF8);
                    song.album = CCommon::StrToUnicode(value.value("album", ""), CodeType::UTF8);
                    song.is_cue = value.value("is_cue", false);
                    song.start_pos.fromInt(value.value("start_ms", 0));
                    song.end_pos.fromInt(value.value("end_ms", 0));
                    if (song.start_pos.toInt() < 0 || song.end_pos.toInt() < song.start_pos.toInt()) return false;
                    song.track = value.value("track", 0);
                    song.cue_file_path = CCommon::StrToUnicode(value.value("cue_path", ""), CodeType::UTF8);
                    songs.push_back(song);
                }
                m_playlist = std::move(songs);
            }
            catch (const nlohmann::json::exception&) { return false; }
        }
        else if (file_extension == L"wpl")
        {
            ParseWplFile(file_content);
        }
        else if (file_extension == L"ttpl")
        {
            ParseTtplFile(file_content);
        }
        else
        {
            std::wstring file_content_wcs = CCommon::StrToUnicode(file_content, utf8 ? CodeType::UTF8 : CodeType::ANSI);
            if (file_extension == L"m3u" || file_extension == L"m3u8")
                ParseM3uFile(file_content_wcs);
            else
                ParsePlaylistFile(file_content_wcs);
        }
        return true;
    }
    return false;
}

bool CPlaylistFile::SaveToFile(const wstring& file_path, Type type) const
{
    return SavePlaylistToFile(m_playlist, file_path, type);
}

bool CPlaylistFile::SavePlaylistToFile(const vector<SongInfo>& song_list, const wstring& file_path, Type type)
{
    // 在同目录写完整临时文件，再原子替换，写失败时保留原歌单。
    wchar_t full_path[MAX_PATH]{}, temp_path[MAX_PATH]{};
    DWORD length = GetFullPathNameW(file_path.c_str(), MAX_PATH, full_path, nullptr);
    if (length == 0 || length >= MAX_PATH) return false;
    if (!GetTempFileNameW(CFilePathHelper(full_path).GetDir().c_str(), L"btp", 0, temp_path)) return false;
    ofstream stream{ temp_path };
    if (!stream.is_open())
    {
        DeleteFileW(temp_path); return false;
    }
    if (type == PL_PLAYLIST)
    {
        for (const auto& item : song_list)
        {
            if (item.file_path.empty()) continue;   // 不保存没有音频路径的项目
            stream << CCommon::UnicodeToStr(item.file_path, CodeType::UTF8_NO_BOM);
            if (item.is_cue || CCommon::IsURL(item.file_path) || online::CSourceRegistry::IsVirtualPath(item.file_path))
            {
                // 出于向后兼容考虑必要这行代码，当song_list来自LoadFromFile加载的不记录cue_file_path的播放列表时item需要从媒体库加载cue_file_path
                SongInfo song = CSongDataManager::GetInstance().GetSongInfo3(item); // 从媒体库载入数据，媒体库不存在的话会原样返回item
                //如果从媒体库中查询到的曲目的标签信息是空的，则使用原始的标签信息
                if (song.IsTagEmpty())
                    song.CopyAudioTag(item);
                CString buff;
                buff.Format(L"|%d|%d|%d|%s|%s|%s|%d|%d|%s|%s|%s|%s", song.is_cue, song.start_pos.toInt(), song.end_pos.toInt(),
                    DeleteInvalidCh(song.title).c_str(), DeleteInvalidCh(song.artist).c_str(), DeleteInvalidCh(song.album).c_str(),
                    song.track, song.bitrate,
                    DeleteInvalidCh(song.genre).c_str(), DeleteInvalidCh(song.get_year()).c_str(), DeleteInvalidCh(song.comment).c_str(),
                    song.cue_file_path.c_str()
                );
                stream << CCommon::UnicodeToStr(buff.GetString(), CodeType::UTF8_NO_BOM);
            }
            stream << "\n"; // 使用std::endl会触发flush影响效率
        }
    }
    else if (type == PL_JSON)
    {
        nlohmann::json root = {{"format", "BoTapMusic"}, {"version", 1}, {"songs", nlohmann::json::array()}};
        for (const auto& song : song_list)
        {
            if (song.file_path.empty()) continue;
            auto utf8 = [](const wstring& text) { return CCommon::UnicodeToStr(text, CodeType::UTF8_NO_BOM); };
            root["songs"].push_back({{"path", utf8(song.file_path)}, {"title", utf8(song.title)},
                {"artist", utf8(song.artist)}, {"album", utf8(song.album)}, {"is_cue", song.is_cue},
                {"start_ms", song.start_pos.toInt()}, {"end_ms", song.end_pos.toInt()},
                {"track", song.track}, {"cue_path", utf8(song.cue_file_path)}});
        }
        stream << root.dump(2);
    }
    else if (type == PL_M3U || type == PL_M3U8)
    {
        CodeType code_type{ CodeType::ANSI };
        if (type == PL_M3U8)
            code_type = CodeType::UTF8_NO_BOM;

        stream << "#EXTM3U" << '\n';
        std::set<std::wstring> saved_cue_path;      //已经保存过的cue文件的路径
        for (const auto& item : song_list)
        {
            if (item.file_path.empty()) continue;   // 不保存没有音频路径的项目
            // song_list可能来自LoadFromFile含有信息不足，此处先从媒体库载入最新数据，媒体库不存在的话会原样返回item
            SongInfo song = CSongDataManager::GetInstance().GetSongInfo3(item);
            //如果从媒体库中查询到的曲目的标签信息是空的，则使用原始的标签信息
            if (song.IsTagEmpty())
                song.CopyAudioTag(item);
            if (song.is_cue)
            {
                //如果播放列表中的项目是cue，且该cue文件没有保存过，则将其保存
                if (!song.cue_file_path.empty() && saved_cue_path.find(song.cue_file_path) == saved_cue_path.end())
                {
                    stream << "#" << '\n';
                    stream << CCommon::UnicodeToStr(song.cue_file_path, code_type) << '\n';
                    saved_cue_path.insert(song.cue_file_path);
                }
            }
            else
            {
                CString buff;
                buff.Format(_T("#EXTINF:%d,%s - %s"), song.length().toInt() / 1000, DeleteInvalidCh(song.artist).c_str(), DeleteInvalidCh(song.title).c_str());
                stream << CCommon::UnicodeToStr(buff.GetString(), code_type) << '\n';
                // 常规播放器可忽略扩展标签；本程序利用它们无损还原标题、歌手和专辑。
                stream << "#EXTART:" << CCommon::UnicodeToStr(DeleteInvalidCh(song.artist), code_type) << '\n';
                stream << "#EXTALB:" << CCommon::UnicodeToStr(DeleteInvalidCh(song.album), code_type) << '\n';
                stream << "#BOTTITLE:" << CCommon::UnicodeToStr(DeleteInvalidCh(song.title), code_type) << '\n';
                stream << CCommon::UnicodeToStr(song.file_path, code_type) << '\n';
            }
        }
    }
    stream.flush();
    bool success = stream.good();
    stream.close();
    success = success && !stream.fail();
    if (success) success = MoveFileExW(temp_path, full_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success) DeleteFileW(temp_path);
    return success;
}

const vector<SongInfo>& CPlaylistFile::GetPlaylist() const
{
    return m_playlist;
}

int CPlaylistFile::AddSongsToPlaylist(const vector<SongInfo>& songs, bool insert_begin)
{
    int added{};
    for (const auto& file : songs)
    {
        if (std::find(m_playlist.begin(), m_playlist.end(), file) != m_playlist.end())
            continue;
        m_playlist.push_back(file);
        ++added;
    }
    if (insert_begin)   // 使用循环旋转将新增条目移动到开头而不是直接插入到开头，可对参数songs去重
        std::rotate(m_playlist.rbegin(), m_playlist.rbegin() + added, m_playlist.rend());
    return added;
}

void CPlaylistFile::MoveToSongList(vector<SongInfo>& song_list)
{
    song_list = std::move(m_playlist);
}

bool CPlaylistFile::IsSongInPlaylist(const SongInfo& song)
{
    return GetSongIndexInPlaylist(song) != -1;
}

int CPlaylistFile::GetSongIndexInPlaylist(const SongInfo& song)
{
    auto iter = std::find(m_playlist.begin(), m_playlist.end(), song);
    if (iter != m_playlist.end())
        return iter - m_playlist.begin();
    else
        return -1;
}

void CPlaylistFile::RemoveSong(const SongInfo& song)
{
    std::erase(m_playlist, song);
}

bool CPlaylistFile::IsPlaylistFile(const wstring& file_path)
{
    wstring file_extension = CFilePathHelper(file_path).GetFileExtension();
    return CCommon::IsItemInVector(m_surpported_playlist, file_extension);
}

bool CPlaylistFile::IsPlaylistExt(wstring ext)
{
    if (ext.empty())
        return false;
    if (ext.front() == L'.')
        ext =  ext.substr(1);
    return CCommon::IsItemInVector(m_surpported_playlist, ext);
}

void CPlaylistFile::ParsePlaylistFile(const std::wstring& file_contents)
{
    std::vector<std::wstring> lines;
    CCommon::StringSplitLine(file_contents, lines);
    for (wstring current_line : lines)
    {
        //去掉引号
        if (!current_line.empty() && current_line.front() == L'\"')
            current_line = current_line.substr(1);
        if (!current_line.empty() && current_line.back() == L'\"')
            current_line.pop_back();

        if (current_line.size() > 3)
        {
            SongInfo item;
            size_t index = current_line.find(L'|');
            item.file_path = current_line.substr(0, index);

            //是否为URL
            bool is_url = CCommon::IsURL(item.file_path);
            // 在线音源的虚拟路径（如 kugou://xxx）既不是URL也不是本地路径，
            // 这里要单独识别，否则会被当成相对路径拼成一个错误的本地路径。
            bool is_online = online::CSourceRegistry::IsVirtualPath(item.file_path);
            //如果是相对路径，则转换成绝对路径（在线曲目不做这个转换）
            if (!is_url && !is_online)
                item.file_path = CCommon::RelativePathToAbsolutePath(item.file_path, CFilePathHelper(m_path).GetDir());

            if (index < current_line.size() - 1)
            {
                vector<wstring> result;
                CCommon::StringSplit(current_line, L'|', result, false);
                if (result.size() >= 2)
                    item.is_cue = (_wtoi(result[1].c_str()) != 0);
                if (result.size() >= 3)
                    item.start_pos.fromInt(_wtoi(result[2].c_str()));
                if (result.size() >= 4)
                    item.end_pos.fromInt(_wtoi(result[3].c_str()));
                //item.lengh = item.end_pos - item.start_pos;
                if (result.size() >= 5)
                    item.title = result[4];
                if (result.size() >= 6)
                    item.artist = result[5];
                if (result.size() >= 7)
                    item.album = result[6];
                if (result.size() >= 8)
                    item.track = _wtoi(result[7].c_str());
                if (result.size() >= 9)
                    item.bitrate = _wtoi(result[8].c_str());
                if (result.size() >= 10)
                    item.genre = result[9];
                if (result.size() >= 11)
                    item.SetYear(result[10].c_str());
                if (result.size() >= 12)
                    item.comment = result[11];
                if (result.size() >= 13)
                    item.cue_file_path = result[12];
            }
            if (is_url || CCommon::IsPath(item.file_path) || online::CSourceRegistry::IsVirtualPath(item.file_path)) // 绝对路径的语法检查（含在线音源的虚拟路径）
            {
                m_playlist.push_back(item);
            }
        }
    }
}

void CPlaylistFile::ParseM3uFile(const std::wstring& file_contents)
{
    vector<wstring> lines;
    CCommon::StringSplitLine(file_contents, lines);
    SongInfo metadata;
    for (wstring line : lines)
    {
        if (!line.empty() && line.front() == 0xfeff) line.erase(0, 1);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 8, L"#EXTINF:") == 0)
        {
            metadata = SongInfo();
            size_t comma = line.find(L',', 8);
            if (comma != wstring::npos)
            {
                int seconds = _wtoi(line.substr(8, comma - 8).c_str());
                if (seconds > 0 && seconds < 2147483) metadata.end_pos.fromInt(seconds * 1000);
                wstring display = line.substr(comma + 1);
                size_t separator = display.find(L" - ");
                if (separator != wstring::npos)
                {
                    metadata.artist = display.substr(0, separator);
                    metadata.title = display.substr(separator + 3);
                }
                else metadata.title = display;
            }
        }
        else if (line.compare(0, 8, L"#EXTART:") == 0) metadata.artist = line.substr(8);
        else if (line.compare(0, 8, L"#EXTALB:") == 0) metadata.album = line.substr(8);
        else if (line.compare(0, 10, L"#BOTTITLE:") == 0) metadata.title = line.substr(10);
        else if (line.front() != L'#')
        {
            SongInfo item = metadata;
            item.file_path = line;
            bool is_url = CCommon::IsURL(line);
            bool is_online = online::CSourceRegistry::IsVirtualPath(line);
            if (!is_url && !is_online)
                item.file_path = CCommon::RelativePathToAbsolutePath(line, CFilePathHelper(m_path).GetDir());
            if (is_url || is_online || CCommon::IsPath(item.file_path)) m_playlist.push_back(item);
            metadata = SongInfo();
        }
    }
}

void CPlaylistFile::ParseWplFile(const std::string& file_contents)
{
    tinyxml2::XMLDocument doc;
    doc.Parse(file_contents.c_str(), file_contents.size());
    auto* root = doc.RootElement();
    if (root != nullptr)
    {
        for (tinyxml2::XMLElement* child = root->FirstChildElement(); child != nullptr; child = child->NextSiblingElement())
        {
            std::string name = CTinyXml2Helper::ElementName(child);
            if (name == "body")
            {
                tinyxml2::XMLElement* seq_element = child->FirstChildElement();
                if (seq_element != nullptr)
                {
                    for (tinyxml2::XMLElement* media_element = seq_element->FirstChildElement(); media_element != nullptr; media_element = media_element->NextSiblingElement())
                    {
                        std::wstring file_path = CCommon::StrToUnicode(CTinyXml2Helper::ElementAttribute(media_element, "src"), CodeType::UTF8);
                        bool is_url = CCommon::IsURL(file_path);
                        bool is_online = online::CSourceRegistry::IsVirtualPath(file_path);
                        //如果是相对路径，则转换成绝对路径（在线曲目的虚拟路径不做这个转换）
                        if (!is_url && !is_online)
                            file_path = CCommon::RelativePathToAbsolutePath(file_path, CFilePathHelper(m_path).GetDir());
                        //绝对路径的语法检查
                        if (is_url || CCommon::IsPath(file_path) || online::CSourceRegistry::IsVirtualPath(file_path))
                        {
                            SongInfo item;
                            item.file_path = file_path;
                            m_playlist.push_back(item);
                        }
                    }
                }
            }
        }
    }
}

void CPlaylistFile::ParseTtplFile(const std::string& file_contents)
{
    tinyxml2::XMLDocument doc;
    doc.Parse(file_contents.c_str(), file_contents.size());
    auto* root = doc.RootElement();
    if (root != nullptr)
    {
        for (tinyxml2::XMLElement* child = root->FirstChildElement(); child != nullptr; child = child->NextSiblingElement())
        {
            std::string name = CTinyXml2Helper::ElementName(child);
            if (name == "items")
            {
                for (tinyxml2::XMLElement* item_element = child->FirstChildElement(); item_element != nullptr; item_element = item_element->NextSiblingElement())
                {
                    name = CTinyXml2Helper::ElementName(item_element);
                    if (name == "item")
                    {
                        std::wstring file_path = CCommon::StrToUnicode(CTinyXml2Helper::ElementAttribute(item_element, "file"), CodeType::UTF8);
                        std::wstring title = CCommon::StrToUnicode(CTinyXml2Helper::ElementAttribute(item_element, "title"), CodeType::UTF8);
                        bool is_url = CCommon::IsURL(file_path);
                        bool is_online = online::CSourceRegistry::IsVirtualPath(file_path);
                        //如果是相对路径，则转换成绝对路径（在线曲目的虚拟路径不做这个转换）
                        if (!is_url && !is_online)
                            file_path = CCommon::RelativePathToAbsolutePath(file_path, CFilePathHelper(m_path).GetDir());
                        //绝对路径的语法检查
                        if (is_url || CCommon::IsPath(file_path) || online::CSourceRegistry::IsVirtualPath(file_path))
                        {
                            SongInfo item;
                            item.file_path = file_path;
                            item.title = title;
                            m_playlist.push_back(item);
                        }
                    }
                }
            }
        }
    }
}
