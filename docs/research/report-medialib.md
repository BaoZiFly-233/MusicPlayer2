# MusicPlayer2 媒体库 / 曲库管理 —— 源码级考古报告

- 仓库：`zhongyang219/MusicPlayer2`（C++ / MFC / Win32，GPL-3.0），调查基线 `master` 分支。
- 取证方式：`gh api "/repos/.../contents/<path>?ref=master" -H "Accept: application/vnd.github.raw"` 取源码，经 cmd 层重定向落盘（**不要走 PowerShell 管道**，会把 LF 归一化成 CRLF），共 119 个文件，逐个与 git tree 的 `size` 校验字节数。
- 标注约定：`【源码确证】`＝已在源码中亲眼读到并附文件:行号/函数名；`【推测】`＝基于代码结构的推断，未经运行验证。

---

## 0. 一句话结论

**MusicPlayer2 的曲库没有数据库。** 它是「一个常驻内存的 `std::unordered_map<SongKey, SongInfo>` + 退出/定时用 MFC `CArchive` 整体序列化成一个二进制文件 `song_data.dat`」的方案。没有 SQLite、没有索引、没有增量落盘、没有事务。分类（艺术家/专辑/流派/年份/类型/比特率/分级）是**每次需要时在内存里现算**的，搜索结果也不落盘。

这个选择直接决定了它后面所有的优点（简单、零依赖、便携）和缺点（全量重写、无查询优化、UI 线程干重活）。

---

## 1. 存储：到底是什么，存在哪

### 1.1 没有数据库

全仓库 grep 不到任何 SQLite / sqlite3 / 数据库相关符号；`MusicPlayer2.vcxproj` 里也没有数据库依赖。曲库唯一的持久化文件是：

【源码确证】`MusicPlayer2/MusicPlayer2.cpp:100`

```cpp
m_config_path   = m_config_dir + L"config.ini";
m_song_data_path = m_config_dir + L"song_data.dat";
m_recent_list_dat_path = m_config_dir + L"recent_list.dat";
m_lastfm_path = m_config_dir + L"lastfm.dat";
m_ui_data_path = m_config_dir + L"user_ui.dat";
```

`m_config_dir` 的取值：【源码确证】`MusicPlayer2.cpp:94-97`

```cpp
if (m_general_setting_data.portable_mode)
    m_config_dir = m_module_dir;        // 便携模式：exe 同目录
else
    m_config_dir = m_appdata_dir;       // 否则 %APPDATA%\MusicPlayer2\
```

所以曲库文件完整路径是二者之一：
- 便携模式：`<exe目录>\song_data.dat`
- 安装模式：`%APPDATA%\MusicPlayer2\song_data.dat`

旁证：便携模式判定在 `LoadGlobalConfig()`（`MusicPlayer2.cpp:444-468`），默认值靠「`global_cfg.ini` 存在与否 + 当前目录可写」推断。

### 1.2 格式：MFC `CArchive` 顺序二进制，不是 XML/INI/JSON

【源码确证】`SongDataManager.cpp:21-83` `CSongDataManager::SaveSongData`

```cpp
CFile file;
BOOL bRet = file.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite);
std::shared_lock<std::shared_mutex> readLock(m_shared_mutex);
CArchive ar(&file, CArchive::store);
ar << CString(_T("2.781"));                          // 数据版本
ar << static_cast<int>(m_song_data.size());          // 条目数
for (const auto& song_data : m_song_data) {
    ar << CString(song_data.first.path.c_str())
       << song_data.second.start_pos.toInt()
       << song_data.second.end_pos.toInt()
       ... // 逐字段 <<
}
ar.Close(); file.Close();
m_song_data_modified = false;
```

要点：
- 文件**第一个字段是数据版本字符串**，当前 `"2.781"`。读的时候据此逐字段做兼容分支。
- 写是**全量重写**：`modeCreate` 直接重建文件，没有任何增量/追加机制。
- 遍历 `unordered_map` 的顺序即文件中的顺序，所以**曲库文件的内存布局不稳定**（每次哈希表重建后写出的字节顺序都可能不同），但因为是按 key 查找所以无所谓。
- 没有校验和、没有尾部标记。**写一半崩溃 = 曲库文件损坏**。`LoadSongData` 用 `try/catch (CArchiveException*)` 兜底，只记日志不恢复：【源码确证】`SongDataManager.cpp:257-261`。

### 1.3 版本演进：就地读旧版

`LoadSongData`（`SongDataManager.cpp:85-266`）是一长串 `if (m_data_version >= _T("2.6xx"))` 分支，历史版本留下的兼容逻辑都还在，例如：

```cpp
if (m_data_version >= _T("2.664")) { ar >> size; }          // 条目数类型改过
else { size_t size_1; ar >> size_1; size = static_cast<int>(size_1); }
if (m_data_version >= _T("2.731")) { ar >> song_start_pos; }  // 新版本才存 start_pos
if (m_data_version == _T("2.661")) { ar >> song_info.is_favourite; }  // 只有这个版本存过 is_favourite
```

**为什么这么选（线索）**：代码里没有一行注释解释「为什么不用数据库」。但从实现风格能读出来——这是个从 v1 一路长上来的 MFC 单进程桌面程序，曲库上限靠 `MAX_SONG_NUM` 卡在 99999 首（`Define.h:75` `#define MAX_SONG_NUM 99999`），在这个量级下「内存 map + 整体序列化」是够用且零依赖的。**这是 `【推测】`，源码中没有明文的设计动机记录。**

顺带一提：`is_favourite` 只在 2.661 这个版本被序列化过，之后被注释掉了：

【源码确证】`SongDataManager.cpp:57` `//<< song_data.second.is_favourite`

原因见下节——红心状态后来被移出曲库，改由「我喜欢的音乐」播放列表承载。

---

## 2. 表结构 / 内存结构

### 2.1 唯一的「表」= `SongDataMap`

【源码确证】`SongDataManager.h:9` 和 `:59`

```cpp
using SongDataMap = std::unordered_map<SongKey, SongInfo>;
...
SongDataMap m_song_data;        //键是每一个音频文件的绝对路径，对象是每一个音频文件的信息
std::atomic<bool> m_song_data_modified{ false };
mutable std::shared_mutex m_shared_mutex;   // 遍历/查找加读锁，添加/删除加写锁
```

单例：`static CSongDataManager m_instance;`（`SongDataManager.h:56`，`SongDataManager.cpp:6`）。

### 2.2 行：`SongInfo`

【源码确证】`SongInfo.h:46-140`。字段清单（`<仅在媒体库内使用>` 是原注释）：

| 字段 | 类型 | 落盘 | 说明 |
|---|---|---|---|
| `file_path` | `wstring` | ✔ | 绝对路径，也是主键的一部分 |
| `lyric_file` | `wstring` | ✔ | 已匹配的歌词文件路径 |
| `title` / `artist` / `album` / `comment` / `genre` / `album_artist` | `wstring` | ✔ | 标签 |
| `cue_file_path` | `wstring` | ✔ | cue 文件路径 |
| `song_id_netease` | `unsigned __int64` | ✔ | 网易云歌曲 ID |
| `song_id_qq_music` | `char[16]` | ✔ | QQ 音乐歌曲 ID（**固定 16 字节数组**） |
| `last_played_time` | `__int64` | ✔ | 上次播放时间，媒体库专用 |
| `modified_time` | `unsigned __int64` | ✔ | 文件修改时间，增量扫描的依据 |
| `track` | `int` | ✔ | 音轨号 |
| `listen_time` | `int` | ✔ | 累计听歌秒数，媒体库专用 |
| `freq` / `bits` / `channels` | `int`/`BYTE`/`BYTE` | ✔ | 采样率/位深/声道 |
| `start_pos` / `end_pos` | `CPlayTime` | ✔ | 位域结构（`PlayTime.h`：`negative:1, min:15, sec:6, msec:10`），cue 分轨起止 |
| `year` | `unsigned short` | ✔ | |
| `bitrate` | `short` | ✔ | |
| `flags` | `WORD` | ✔ | 位标志，**4 个 bit 已用** |
| `tag_type` | `BYTE` | ✔ | ID3v1/ID3v2/APE/RIFF/MP4 位标志 |
| `genre_idx` | `BYTE` | ✔ | ID3v1 流派号，默认 255 |
| `info_acquired` | `bool` | ✔ | **作者自己标注「实际上已完全没有作用」**（`SongInfo.h:71`） |
| `is_favourite` | `bool` | ✘ | 「仅在播放列表内使用」，**不落盘** |
| `is_cue` | `bool` | ✔ | |
| `rating` | `BYTE` | ✔ | 分级 1-5，默认 255 = 未分级 |
| `total_tracks` / `disc_num` / `total_discs` | `BYTE` | ✔ | |
| `is_prefered` | `bool` | ✔ | 同一曲目多版本时选中的那个 |

`flags` 的位分配：【源码确证】`SongInfo.h:82-97` + `SongInfo.cpp:6-44`

```cpp
bit0 NoOnlineLyric()               // 不下载在线歌词
bit1 NoOnlineAlbumCover()          // 不下载在线封面
bit2 AlwaysUseExternalAlbumCover() // 强制使用外部封面
bit3 ChannelInfoAcquired()         // 采样率/位深/声道信息已获取
```

注意 `SongInfo.h:71` 那句自白——这是作者留下的技术债标记，非常值得学：**旧字段不敢删，但明确标注「已无用，考虑移除」**，比悄悄留着强。

### 2.3 主键：`SongKey` = 完整路径 + cue 音轨号

【源码确证】`SongInfo.h:142-183`

```cpp
struct SongKey
{
    wstring path;
    int cue_track{};    // 当存储cue时用来保存音轨号，其他情况为0
    ...
};
```

哈希与相等：【源码确证】`SongInfo.h:185-198`

```cpp
namespace std {
    template <> struct hash<SongKey> {
        std::size_t operator()(const SongKey& key) const {
            return std::hash<wstring>()(key.path) ^ std::hash<int>()(key.cue_track);
        }
    };
    template <> struct equal_to<SongKey> {
        bool operator()(const SongKey& lhs, const SongKey& rhs) const {
            return lhs.path == rhs.path && lhs.cue_track == rhs.cue_track;
        }
    };
}
```

**去重机制 = 完整路径字符串精确比较。**

- 没有文件名哈希，没有大小+时长指纹，没有 inode/FileID。
- **大小写敏感**（`wstring::operator==`），所以 `D:\Music\a.mp3` 和 `d:\music\A.MP3` 在曲库里是**两条独立记录**。项目里没有任何 `ToLower()`/`_wcsicmp` 归一化路径的地方（在 `SongInfo.h/.cpp`、`SongDataManager.cpp` 中 grep 确认无此逻辑）。
- 目录分隔符也不归一化（`\` 和 `/` 混用会产生不同 key；`GetAudioFiles` 生成路径时统一用 `\`，所以正常扫描不会混，但外部传入的路径可能混）。

`SongKey == SongInfo` 的语义（**cue 分轨去重的关键**）：【源码确证】`SongInfo.h:173-182`

```cpp
bool operator==(const SongInfo& other) const
{
    if ((cue_track > 0) != other.is_cue)   return false;   // is_cue 不同
    if (path != other.file_path)           return false;   // 路径不同
    if ((cue_track > 0) && cue_track != other.track) return false;  // cue 时音轨号不同
    return true;
}
```

注释也写了「原比较代码有漏洞，请迁移所有比较到使用此方法」（`SongInfo.h:138`）——即历史上曾经用错的比较函数，靠人工迁移修掉。**这是一条很实在的教训：主键相等语义一定要集中到一个地方。**

### 2.4 多值字段：艺术家靠**分隔符切分**，其余全是单值

**艺术家是多值的，而且是运行时切分的，不是存储时就拆成多条目。**

【源码确证】`SongInfo.cpp:98-143` `SongInfo::GetArtistList`

```cpp
static const wstring split_char = L"/;&、";
if (artist.find_first_of(split_char) == wstring::npos) {
    artist_list.push_back(artist);   // 不含分割字符的字符串不需要处理，直接返回
    return;
}
// 现在是保守分割方案，理论上有可能少切分但不会多，处理后艺术家仍然按照原顺序排列
vector<bool> char_flag(artist.size(), false);
for (size_t i{}; i < artist.size(); ++i)
    if (split_char.find(artist[i]) == wstring::npos) char_flag[i] = true;

const vector<wstring>& split_ext = theApp.m_media_lib_setting_data.artist_split_ext;
for (const wstring& str : split_ext) {          // 例外名单
    size_t index{ artist.find(str) };
    while (index != wstring::npos) {
        for (size_t i{}; i < str.size(); ++i) char_flag[index + i] = true;   // 标记为“不切”
        index = artist.find(str, index + 1);
    }
}
```

设计很巧：先假设「所有分隔符都该切」，再用 `char_flag` 把例外名单（如 `AC/DC`、`22/7`、`+/-`）里的位置**标记回「不切」**，最后按 flag 切一遍。所以：
- 存储层只有一个 `artist` 字符串；**多值信息完全依赖标签里的分隔符约定**。
- 例外名单用户可配，默认值：【源码确证】`MusicPlayerDlg.cpp:770` `vector<wstring>{ L"AC/DC", L"+/-", L"22/7" }`
- 只支持单一读法：`GetFirstArtist()` 取 `artist_list.at(0)`（`SongInfo.cpp:145-152`）。

**专辑 / 流派 / 年份都是严格单值**，不做切分。分类代码里也是 `item_names.push_back(str_album)` 单个推入（`MediaLibHelper.cpp:56-84`）。

对比参考：`CSongMultiVersion::MakeKey` 里把艺术家列表用 `;` 重新拼回字符串当 key：【源码确证】`SongMultiVersion.cpp:149-179`

```cpp
wss << song_info.title << L'|';
std::vector<std::wstring> artist_list;
song_info.GetArtistList(artist_list);
for (const auto& artist : artist_list) wss << artist << L';';
wss << L'|' << song_info.album;
if (song_info.is_cue) { wss << L'|' << std::to_wstring(song_info.track); }
```

### 2.5 分类容器：`std::map` + 不区分大小写比较器

【源码确证】`MediaLibHelper.h`

```cpp
struct StringComparerNoCase {
    bool operator()(const std::wstring& a, const std::wstring& b) const;
};
class CMediaClassifier {
public:
    typedef std::map<std::wstring, std::vector<SongInfo>, StringComparerNoCase> MediaList;
    ...
private:
    MediaList m_media_list;
    ListItem::ClassificationType m_type;
    bool m_hide_only_one_classification;
};
```

比较器实现走 Windows 本地化比较：【源码确证】`MediaLibHelper.cpp:8-11` → `Common.cpp` `StringCompareInLocalLanguage`

```cpp
int rtn = CompareStringEx(LOCALE_NAME_USER_DEFAULT,
    (no_case ? NORM_IGNORECASE : 0) | SORT_DIGITSASNUMBERS,
    str1.c_str(), str1.size(), str2.c_str(), str2.size(), NULL, NULL, 0);
```

注意 `SORT_DIGITSASNUMBERS`：排序时「第 2 首」排在「第 10 首」前面，这是刻意的。`【推测】`这是为了避免文件名里的数字被字典序排错。

**分类结果不缓存**（`CMediaClassifier` 是局部对象，`ClassifyMedia()` 每次调用都 `m_media_list.clear()` 重算）——见第 6 节。

### 2.6 曲库「列表」的元数据存在别处

曲库条目（艺术家/专辑/文件夹/播放列表）**不在 `song_data.dat` 里**，而是存在 `recent_list.dat`（`ListItem` 列表），媒体库的 `LT_MEDIA_LIB` 条目也混在里面：

【源码确证】`ListItem.h:14-45`

```cpp
struct ListItem {
    ListType type{};              // LT_FOLDER / LT_PLAYLIST / LT_MEDIA_LIB
    wstring path;                 // 媒体库模式存储具体项目名
    SortMode sort_mode{ SM_UNSORT };
    SongKey last_track{};         // 最后播放到的曲目
    int last_position{};
    int total_time{};
    int total_num{};
    uint64_t last_played_time{};
    uint64_t create_time{};
    ClassificationType medialib_type{};   // 媒体库列表种类
    bool contain_sub_folder{};
};

enum ListType { LT_FOLDER, LT_PLAYLIST, LT_MEDIA_LIB, LT_MAX };

// type为LT_MEDIA_LIB时path为此字符串表示<其他>分类
static inline const wstring STR_OTHER_CLASSIFY_TYPE = L"eRk0Q6ov";
```

那个 `L"eRk0Q6ov"` 是个**魔法哨兵字符串**：因为「其他」分类需要占一个 map key 但又不能和真实艺术家/专辑重名，作者就用了一串随机字符。`【推测】`这种做法能工作但很脆——如果真有艺术家叫 `eRk0Q6ov` 就会撞车。

**媒体库文件夹白名单**存在 `config.ini`：【源码确证】`MusicPlayerDlg.cpp:540` / `:769`

```cpp
// 写
ini.WriteStringList(L"media_lib", L"media_folders", theApp.m_media_lib_setting_data.media_folders);
// 读，默认值 = 我的音乐
ini.GetStringList(L"media_lib", L"media_folders", ...,
                  vector<wstring>{CCommon::GetSpecialDir(CSIDL_MYMUSIC)});
```

---

## 3. 扫描流程：从「添加文件夹」到「进曲库」

### 3.1 完整调用链

```
用户在媒体库设置对话框点「添加」→ CMusicPlayerDlg.cpp:4292
  theApp.StartUpdateMediaLib(false)          ← 见下方“重要坑”
```

自动/手动两条入口：

1. **启动时**（若 `update_media_lib_when_start_up`，默认 true）：【源码确证】`MusicPlayerDlg.cpp:4287-4292`
```cpp
if (theApp.m_media_lib_setting_data.update_media_lib_when_start_up)
    ...
    theApp.StartUpdateMediaLib(false);   // 获取不存在的项目以及更新修改时间变化的项目
```

2. **手动点「刷新媒体库」**：【源码确证】`MediaLibSettingDlg.cpp:554`
```cpp
theApp.StartUpdateMediaLib(true);  // 刷新媒体库按钮强制重新获取所有元数据
```

两者都进 `CMusicPlayerApp::StartUpdateMediaLib(bool force)`：【源码确证】`MusicPlayer2.cpp:637-661`

```cpp
void CMusicPlayerApp::StartUpdateMediaLib(bool force)
{
    if (!m_media_lib_updating)                       // 单飞保护：已在更新则直接返回
    {
        m_media_lib_updating = true;
        m_media_update_para.num_added = 0;
        m_media_update_para.force = force;
        m_media_lib_update_thread = AfxBeginThread([](LPVOID lpParam)->UINT
        {
            if (theApp.m_media_lib_setting_data.remove_file_not_exist_when_update)
            {
                CMusicPlayerCmdHelper::CleanUpRecentFolders();
                // 虽然仍有线程安全问题不过这行在“启动时更新媒体库”时立刻进行冲突的可能性比较小
                CMusicPlayerCmdHelper::CleanUpSongData();
            }
            CMusicPlayerCmdHelper::UpdateMediaLib();
            theApp.m_media_lib_updating = false;
            //更新UI中我喜欢的音乐、所有曲目和媒体库项目列表
            CUiMyFavouriteItemMgr::Instance().UpdateMyFavourite();
            CUiFolderExploreMgr::Instance().UpdateFolders();
            CUiAllTracksMgr::Instance().UpdateAllTracks();
            CUiMediaLibItemMgr::Instance().Init();
            return 0;
        }, nullptr);
    }
}
```

**注意那句自白**：`// 虽然仍有线程安全问题不过这行在“启动时更新媒体库”时立刻进行冲突的可能性比较小`。作者明知这里有竞态但选择接受。

继续往下：【源码确证】`MusicPlayerCmdHelper.cpp:597-618`

```cpp
int CMusicPlayerCmdHelper::UpdateMediaLib()
{
    if (CPlayer::GetInstance().IsMciCore())
        return 0;                                   // MCI 内核下直接放弃

    vector<SongInfo> all_media_songs;
    //获取所有音频文件的路径
    for (const auto& item : theApp.m_media_lib_setting_data.media_folders)
        CAudioCommon::GetAudioFiles(item, all_media_songs, MAX_SONG_NUM, true);   // 递归

    CAudioCommon::GetAudioInfo(all_media_songs,
        theApp.m_media_update_para.num_added,
        theApp.m_media_update_para.thread_exit,
        theApp.m_media_update_para.process_percent,
        theApp.m_media_update_para.force ? MR_FOECE_FULL : MR_FILE_MODIFICATION,
        theApp.m_media_lib_setting_data.ignore_too_short_when_update);
    return theApp.m_media_update_para.num_added;
}
```

递归枚举：【源码确证】`AudioCommon.cpp:156-196` `CAudioCommon::GetAudioFiles`

```cpp
if ((hFile = _wfindfirst((path + L"\\*.*").c_str(), &fileinfo)) != -1)
{
    do {
        if (files.size() >= max_file) break;          // 硬上限，见坑 §8
        wstring file_name = fileinfo.name;
        if (file_name == L"." || file_name == L"..") continue;
        if (CCommon::IsFolder(path + file_name)) {
            if (include_sub_dir) GetAudioFiles(path + file_name, files, max_file, true);
        }
        else if (FileIsAudio(wstring(fileinfo.name))) {
            song_info.file_path = path + fileinfo.name;
            files.push_back(song_info);
        }
    } while (_wfindnext(hFile, &fileinfo) == 0);
}
```

用的是 `_wfindfirst`/`_wfindnext`（ANSI/UTF-16 Win32 老 API），**同步、单线程、递归**。遇到 osu! 的 `Songs` 目录会走专用分支 `COSUPlayerHelper::GetOSUAudioFiles`（`AudioCommon.cpp:160-164`）。

### 3.2 增量扫描：靠**文件修改时间**，不是扫描时间戳

【源码确证】`AudioCommon.cpp:526-593` `CAudioCommon::GetAudioInfo` 是核心：

```cpp
void CAudioCommon::GetAudioInfo(vector<SongInfo>& files, int& update_cnt, bool& exit_flag,
                                int& process_percent, MediaLibRefreshMode refresh_mode, bool ignore_short)
{
    // GetCueTracks计算进度太难，直接为其分配5%进度
    process_percent = 5;
    GetCueTracks(files, update_cnt, exit_flag, refresh_mode);
    int file_too_short_ms{ theApp.m_media_lib_setting_data.file_too_short_sec * 1000 };
    unsigned int process_cnt{}, process_all = max(files.size(), 1);   // 防止除0
    std::set<wstring> too_short_remove;
    for (const SongInfo& song : files)
    {
        if (exit_flag) return;                                        // ← 唯一的中断点
        process_percent = ++process_cnt * 95 / process_all + 5;
        if (song.is_cue || song.file_path.empty()) continue;

        SongInfo song_info{ CSongDataManager::GetInstance().GetSongInfo3(song) };
        // 这里的info_acquired和ChannelInfoAcquired是旧版兼容
        bool need_get_info{ song_info.modified_time == 0 || !song_info.info_acquired
                            || !song_info.ChannelInfoAcquired() };
        if (refresh_mode == MR_MIN_REQUIRED && !need_get_info) continue;

        unsigned __int64 modified_time{};
        if (CCommon::IsURL(song_info.file_path)) continue;
        if (!CCommon::GetFileLastModified(song_info.file_path, modified_time))
            continue;                                            // 跳过当前不存在的文件

        // ★ 增量判定的核心：修改时间一致就跳过
        if (refresh_mode != MR_FOECE_FULL && song_info.modified_time == modified_time && !need_get_info)
            continue;
        song_info.modified_time = modified_time;

        CAudioTag audio_tag(song_info);
        bool get_tag_succeed = true;
        if (COSUPlayerHelper::IsOsuFile(song_info.file_path))
            COSUPlayerHelper::GetOSUAudioTitleArtist(song_info);
        else
            get_tag_succeed = audio_tag.GetAudioTag();
        audio_tag.GetAudioRating();

        IPlayerCore::AudioInfo audio_info;
        IPlayerCore::AudioTag audio_tag_info;
        CPlayer::GetInstance().GetPlayerCore()->GetAudioInfo(
            song_info.file_path.c_str(), &audio_info,
            get_tag_succeed ? nullptr : &audio_tag_info);         // 标签读失败时用播放内核兜底
        AudioInfoToSongInfo(audio_info, song_info);
        if (!get_tag_succeed) AudioTagInfoToSongInfo(audio_tag_info, song_info);

        if (ignore_short && song_info.length().toInt() < file_too_short_ms)
            too_short_remove.insert(song_info.file_path);
        else {
            song_info.info_acquired = true;
            song_info.SetChannelInfoAcquired(true);
            CSongDataManager::GetInstance().AddItem(song_info);   // ← 逐条落内存
            ++update_cnt;
        }
    }
    ...
}
```

三档刷新级别：【源码确证】`AudioCommon.h:55-61`

```cpp
enum MediaLibRefreshMode
{
    MR_MIN_REQUIRED,        // 仅获取不存在于媒体库的条目(最小化文件读取，最快但不保证最新)
    MR_FILE_MODIFICATION,   // 重新获取修改时间与媒体库记录不同的条目(需要读取修改时间略耗时)
    MR_FOECE_FULL           // 强制重新获取所有条目
};
```

**判定依据就是 `SongInfo::modified_time == 文件当前 ftLastWriteTime`。**
- `modified_time == 0`（新条目）→ 必读
- `info_acquired == false` 或 `ChannelInfoAcquired()` 为 false（旧版兼容）→ 必读
- 时间一致且两个标志都好 → 跳过

时间获取：【源码确证】`Common.cpp` `CCommon::GetFileLastModified`

```cpp
WIN32_FILE_ATTRIBUTE_DATA file_attributes;
if (GetFileAttributesEx(file_path.c_str(), GetFileExInfoStandard, &file_attributes))
{
    ULARGE_INTEGER last_modified_time{};
    last_modified_time.HighPart = file_attributes.ftLastWriteTime.dwHighDateTime;
    last_modified_time.LowPart = file_attributes.ftLastWriteTime.dwLowDateTime;
    modified_time = last_modified_time.QuadPart;
    return true;
}
```

注释写了「使用 GetFileAttributesEx，耗时大约为 FindFirstFile 的 2/3」——**这是刻意做的微优化，作者真的量过**。存的是 100ns 精度的原始 FILETIME，不做任何取整。

**关键结论：它是「每文件一次 stat + 时间戳精确比较」，不是「记录上次扫描时间、只看更新的文件」。** 所以每次扫描都要对全库每个文件做一次 `GetFileAttributesEx`。

### 3.3 并发：**单线程**，但可中断

- 扫描全程跑在**一个** `AfxBeginThread` 工作线程里（`MusicPlayer2.cpp:644`），内部没有任何 `std::thread` / 线程池 / 并行遍历。
- 唯一的并发保护是 `CSongDataManager` 的 `std::shared_mutex`（`SongDataManager.h:63`）：读写分离，`GetSongData`/`GetSongInfo` 加读锁，`AddItem`/`RemoveItem` 加写锁。
- 中断机制是**轮询一个裸 bool**：

【源码确证】`CommonData.h:493-499`

```cpp
struct MediaUpdateThreadPara
{
    int num_added{};        //更新媒体库时新增（包括更新）的音频文件数量
    int process_percent{};  // 更新媒体库进度%
    bool thread_exit{};     //如果为true，则线程应该退出
    bool force;             // 为true时无视修改时间强制刷新
};
```

退出请求在 **`MusicPlayer2.cpp:324-329`**（主程序 `InitInstance` 结束、退出时）：

```cpp
//如果媒体库正在更新，通知线程退出
m_media_update_para.thread_exit = true;
if (theApp.m_media_lib_updating)
    WaitForSingleObject(m_media_lib_update_thread->m_hThread, 1000);	//等待线程退出
SaveSongData();
```

**注意这是个非原子的 `bool` 被两个线程读写**（工作线程在 `GetAudioInfo` 循环里读，主线程在退出时写），`num_added` / `process_percent` 同样是裸 `int` 被工作线程写、UI 线程读。这是**实打实的数据竞争**（技术上 UB），只是后果是「进度数字可能跳变」所以没被重视。`【源码确证】`是结构本身；`【推测】`是「后果轻微所以作者没管」这个判断。

另一个同步点在 `GetAudioInfo` 里逐条调 `AddItem`，每条都要拿一次写锁，和 UI 线程/播放线程的读锁交替——**锁粒度是「每首歌一次」**，粒度偏细，重扫大库时会产生大量锁竞争。

### 3.4 进度反馈给 UI

【源码确证】`CPlayerUIBase.cpp:1942-1949`

```cpp
//显示媒体库更新状态
else if (theApp.IsMeidaLibUpdating() && theApp.m_media_update_para.num_added > 0)
{
    wstring info = theApp.m_str_table.LoadTextFormat(L"UI_TXT_MEDIA_LIB_UPDATING_INFO",
        { theApp.m_media_update_para.num_added, theApp.m_media_update_para.process_percent });
    static CDrawCommon::ScrollInfo scroll_info2;
    m_draw.DrawScrollText(rect, info.c_str(), m_colors.color_text, GetScrollTextPixel(), false, scroll_info2, reset);
}
```

**UI 线程只是「顺手读一眼」那两个字段，没有任何消息/回调机制。** 进度条都没有，就是主界面上一行滚动文字。而且这个分支被放在 `else if` 链里——它排在 AB 重复提示之后、播放信息之前，只在「没别的东西可显示」时才显示。

进度百分比计算有个小细节：`process_percent = ++process_cnt * 95 / process_all + 5;` —— `GetCueTracks` 拿固定 5%，剩下的 95% 按文件数均分。

更新完成后，**工作线程直接调用 4 个 UI 单例的 Update**（`MusicPlayer2.cpp:653-657`）：

```cpp
CUiMyFavouriteItemMgr::Instance().UpdateMyFavourite();
CUiFolderExploreMgr::Instance().UpdateFolders();
CUiAllTracksMgr::Instance().UpdateAllTracks();
CUiMediaLibItemMgr::Instance().Init();
```

这几个函数内部都是「加 `m_loading = true` → 整体重建 vector → `m_loading = false`」的模式（见 §6.3），UI 侧的 `GetSongCount()` 在 `m_loading` 期间返回 0，所以刷新期间列表会短暂变空/显示「正在加载」。

### 3.5 大曲库（几万首）时的策略

**坦白说：没有专门策略。** 全部是「一把梭」。能数出来的只有这些：

| 机制 | 位置 | 作用 |
|---|---|---|
| `MAX_SONG_NUM = 99999` | `Define.h:75` | 每**个**媒体库文件夹的硬上限，超了静默丢弃 |
| 修改时间增量跳过 | `AudioCommon.cpp:551` | 只有变化的文件才读标签，**这是唯一有效的规模化手段** |
| `GetFileAttributesEx` 替代 `FindFirstFile` | `Common.cpp` | 单次 stat 快 1/3 |
| `ignore_too_short_when_update` | `CommonData.h:379` | 跳过短于阈值的文件（默认 30 秒） |
| `remove_file_not_exist_when_update` | `CommonData.h:381` | 扫描前先剔除已不存在的文件 |
| 标签 tab 懒加载 | `MediaClassifyDlg.cpp:319-334` | 见 §6.4 |
| 分类结果**不**缓存 | `CMediaClassifier` | ← 这是**反向**的，见 §6 |

另外注意 `GetAudioFiles` 的 `max_file` 是**每个文件夹独立计数**，而 `all_media_songs` 是累加的。所以 3 个媒体库文件夹各 5 万首 → 总共能装 15 万首，但内存里只是一个 `vector<SongInfo>` 顺序处理，没有分批/流式。

---

## 4. 统计：最近播放 / 播放次数 / 红心 / 分级 / 累计听歌时间

先说一个反直觉的结论：**MusicPlayer2 根本没有「播放次数」这个字段。** `SongInfo` 里没有 `play_count`。界面上那个「累计次数」是**用累计听歌时间除以歌曲时长推算出来的**：

【源码确证】`CListenTimeStatisticsDlg.cpp:89-106` `SongInfoToListItem`
```cpp
list_item.path = song.file_path;
list_item.total_time = CPlayTime(song.listen_time * 1000);
list_item.length = song.length();
list_item.times = static_cast<double>(song.listen_time) / song.length().toInt() * 1000;   // ← 推算的“次数”
return list_item;
```
（`listen_time` 单位是秒，`length().toInt()` 单位是毫秒，所以乘 1000 换算。）

入榜门槛与排序：【源码确证】`CListenTimeStatisticsDlg.cpp:144-169`
```cpp
CSongDataManager::GetInstance().GetSongData([&](const CSongDataManager::SongDataMap& song_data_map) {
    for (const auto& data : song_data_map) {
        SongInfo song{ data.second };
        if (song.listen_time >= 20 && song.length() > 0)      // 只听满 20 秒才统计
            m_data_list.push_back(SongInfoToListItem(song));
    }
});
//先按累计收听时间从大到小排序
std::sort(...);
//再按累计次数从大到小排序，并确保累计次数相同的项目按累计时间从大到小排序
std::stable_sort(...);   // 两级排序用 sort + stable_sort 组合实现
```

**含义**：这个「次数」是**估算值**，一首 4 分钟的歌听满 2 分钟就会显示 0.5 次。它统计的是「听了多少遍的等效值」，不是「播放了几次」。这是个聪明的省字段设计，但**如果产品需求是精确播放次数，这套存储结构做不到**。

「清除播放时间统计数据」也是把 `listen_time` 归零：【源码确证】`SongDataManager.cpp:452-460`
```cpp
void CSongDataManager::ClearPlayTime()
{
    std::unique_lock<std::shared_mutex> writeLock(m_shared_mutex);
    for (auto& data : m_song_data) { data.second.listen_time = 0; }
    m_song_data_modified = true;
}
```

### 4.1 一览表

| 统计项 | 存在哪 | 何时写 | 落盘时机 |
|---|---|---|---|
| **累计听歌时间** `listen_time` | `SongInfo::listen_time`（int 秒） | 播放中每秒 +1 | `m_song_data_modified = true`，由主窗口定时器批量保存 |
| **上次播放时间** `last_played_time` | `SongInfo::last_played_time`（__int64） | 切歌时 | 同上 |
| **红心（我喜欢的音乐）** | **特殊播放列表文件**，不在曲库 | 立即写播放列表 | 立即落盘（另一个文件） |
| **分级 rating** | `SongInfo::rating`（BYTE）**+ 同时写音频文件标签** | 用户操作时 | 同上（曲库）+ 立即写标签 |
| **最近播放列表** | `recent_list.dat` 的 `ListItem::last_played_time` | 列表被播放时 | `CRecentList::SaveData()` |

### 4.2 累计听歌时间：每秒一次，48 秒批量落盘

【源码确证】`MusicPlayerDlg.cpp:2696-2702`（1 秒定时器）

```cpp
else if (nIDEvent == TIMER_1_SEC)
{
    m_one_sec_timer_counter++;
    if (CPlayer::GetInstance().IsPlaying())
    {
        CPlayer::GetInstance().AddListenTime(1);
    }
    ...
```

`SetTimer(TIMER_1_SEC, 1000, NULL);`：【源码确证】`MusicPlayerDlg.cpp:2358`

【源码确证】`Player.cpp:2196-2204` `CPlayer::AddListenTime`

```cpp
void CPlayer::AddListenTime(int sec)
{
    if (m_index >= 0 && m_index < GetSongNum())
    {
        m_playlist[m_index].listen_time += sec; // m_playlist的信息不会保存到媒体库，此处仅为排序维护
        SongInfo song_info{ CSongDataManager::GetInstance().GetSongInfo3(m_playlist[m_index]) };
        song_info.listen_time += sec;
        CSongDataManager::GetInstance().AddItem(song_info);   // ← 拿写锁 + 标记 modified
    }
    ...
}
```

**注意这是每秒一次的「读整条 SongInfo → 改一个字段 → 整条写回」**（`GetSongInfo3` 返回拷贝，`AddItem` 整体覆盖）。每秒一次 `SongInfo` 的深拷贝（含 6 个 `wstring`）。对单条来说无所谓，但这是每秒都在发生的固定开销。

【源码确证】`SongDataManager.cpp:410-416` `AddItem` 顺便置脏：

```cpp
void CSongDataManager::AddItem(const SongInfo& song)
{
    std::unique_lock<std::shared_mutex> writeLock(m_shared_mutex);
    ASSERT(!song.file_path.empty());
    m_song_data[song] = song;
    m_song_data_modified = true;
}
```

**落盘时机**：【源码确证】`MusicPlayerDlg.cpp:2686-2692`（主定时器回调）

```cpp
if (m_timer_count % 600 == 599)
{
    if (CSongDataManager::GetInstance().IsSongDataModified())   //在歌曲信息被修改过的情况下，每隔一定的时间保存一次
        theApp.SaveSongData();
}
```

主定时器是 `TIMER_ELAPSE = 80` ms：【源码确证】`Define.h:88` + `MusicPlayerDlg.cpp:2357` `SetTimer(TIMER_ID, TIMER_ELAPSE, NULL);`

所以落盘周期 = `600 × 80ms ≈ 48 秒`。加上退出时的无条件保存：

【源码确证】`MusicPlayer2.cpp:329`（`InitInstance` 对话框返回之后）
```cpp
SaveSongData();
```
【源码确证】`MusicPlayer2.cpp:370-373`
```cpp
void CMusicPlayerApp::SaveSongData()
{
    CSongDataManager::GetInstance().SaveSongData(m_song_data_path);
}
```

**结论：不是「立刻落盘」，是「脏标记 + 48 秒定时全量重写 + 退出时兜底」。**

这意味着：
- `listen_time` 每秒 +1 但每 48 秒才写盘 → 崩溃最多丢 48 秒统计（可接受）。
- 但**每次写盘都是全库重写**（`CFile::modeCreate`）。3 万首的库，每 48 秒写一次完整的 `song_data.dat`，**而且是在主线程（`CMusicPlayerDlg` 的定时器回调）里**。这是一个明确的卡顿来源。`【源码确证】`是「全量重写 + 主线程定时器」这两点；`【推测】`是「3 万首会卡」这个量级判断（需要实测文件大小与磁盘延迟）。

### 4.3 红心（我喜欢的音乐）：**不在曲库里**，是特殊播放列表

这是最容易猜错的一点。`SongInfo::is_favourite` 存在，但注释明说：

【源码确证】`SongInfo.h:72`
```cpp
bool is_favourite{ false };         // 是否在我喜欢的音乐列表内<仅在播放列表内使用>
```

并且 `SaveSongData` 里这行是**注释掉的**：【源码确证】`SongDataManager.cpp:57` `//<< song_data.second.is_favourite`

真正的存储是「我喜欢的音乐」这个特殊播放列表文件：

【源码确证】`UiMediaLibItemMgr.cpp:185-198` `CUiMyFavouriteItemMgr::UpdateMyFavourite`

```cpp
void CUiMyFavouriteItemMgr::UpdateMyFavourite()
{
    m_loading = true;
    std::shared_lock<std::shared_mutex> lock(m_shared_mutex);

    ListItem list_item = CRecentList::Instance().GetSpecPlaylist(CRecentList::PT_FAVOURITE);
    CPlaylistFile playlist_file;
    playlist_file.LoadFromFile(list_item.path);           // ← 从播放列表文件读
    playlist_file.MoveToSongList(m_may_favourite_song_list);
    CSongDataManager::GetInstance().LoadSongsInfo(m_may_favourite_song_list);  // 从媒体库加载歌曲属性

    m_loading = false;
    m_inited = true;
}
```

特殊播放列表有三个：【源码确证】`CRecentList.h:43-49`
```cpp
enum PlaylistType { PT_DEFAULT, PT_FAVOURITE, PT_TEMP, PT_MAX };
```
（默认播放列表 / 我喜欢的音乐 / 临时播放列表）

**所以「红心」= 一个 `.playlist` 文件里有没有这条路径。** 好处：可以直接把播放列表文件拷走/分享/备份。坏处：判断「某首歌是否红心」必须做**线性查找**：

【源码确证】`UiMediaLibItemMgr.cpp:205-209`
```cpp
bool CUiMyFavouriteItemMgr::Contains(const SongInfo& song) const
{
    auto iter = std::find(m_may_favourite_song_list.begin(), m_may_favourite_song_list.end(), song);
    return iter != m_may_favourite_song_list.end();
}
```

而 `Contains` 在构建「所有曲目」时**被每首歌调一次**：【源码确证】`UiMediaLibItemMgr.cpp:324-331`

```cpp
std::transform(tmp_song_list.begin(), tmp_song_list.end(), std::back_inserter(m_all_tracks_list), [](const SongInfo& song_info) {
    UTrackInfo item;
    item.song_key = song_info;
    item.name = CSongInfoHelper::GetDisplayStr(song_info, theApp.m_media_lib_setting_data.display_format);
    item.length = song_info.length();
    item.is_favourite = CUiMyFavouriteItemMgr::Instance().Contains(song_info);   // O(F) 线性查找
    return item;
});
```

**总复杂度 O(N × F)**（N = 曲库曲目数，F = 红心数）。如果红心 5000 首、曲库 3 万首，就是 1.5 亿次 `SongInfo` 比较（每次比较要逐个比 `wstring` 和一堆字段）——**这是一处明确的性能地雷**。

### 4.4 分级 rating：**同时写曲库和音频文件标签**

【源码确证】`MusicPlayerCmdHelper.cpp:553-570` `SetRating`

```cpp
bool CMusicPlayerCmdHelper::SetRating(const SongInfo& song, int rating)
{
    SongInfo song_info{ CSongDataManager::GetInstance().GetSongInfo3(song) };
    song_info.rating = static_cast<BYTE>(rating);
    bool succeed{};
    // cue、osu!、不支持写入的文件分级只保存到媒体库
    if (CAudioTag::IsFileRatingSupport(CFilePathHelper(song_info.file_path).GetFileExtension())
        && !song_info.is_cue && !COSUPlayerHelper::IsOsuFile(song_info.file_path))
    {
        CAudioTag audio_tag(song_info);
        succeed = audio_tag.WriteAudioRating();    // 写进文件的 POPM 标签
    }
    else
    {
        succeed = true;     //如果文件格式不支持写入分级，也返回true
    }
    CSongDataManager::GetInstance().AddItem(song_info);   // 同时写曲库
    return succeed;
}
```

- 默认值 `rating{ 255 }`（`SongInfo.h:74`）= 未分级。有效值 1-5。
- 分级容器在分类标签页里被当一等公民：`MediaLibHelper.cpp:115-124` `CT_RATING` 分支把 `>=1 && <=5` 的转成数字，其余归到「未分级」文案。
- 读取路径反过来：【源码确证】`MusicPlayerCmdHelper.cpp:572-583` `GetRating` —— 如果 `rating > 5`（即没读到过），且不是 cue，**且有 osu! 前缀判断（`!song_info.is_cue && COSUPlayerHelper::IsOsuFile(...)`，注意这个条件里 `IsOsuFile` 前面没有 `!`，看起来像笔误，导致真正的 osu! 文件反而不触发重新读取）**。

  `【源码确证】` 该条件文本；`【推测】` 它是笔误——因为同一函数上方的 `SetRating` 写的是 `!COSUPlayerHelper::IsOsuFile(...)`，两处不对称。**这是一处值得在参考文档里点名的疑似 bug。**

- 支持写分级的扩展名白名单在 `CAudioTag::IsFileRatingSupport`（在 `AudioTag.cpp` / `TagLibHelper.cpp` 中，本次未逐行展开）。

### 4.5 上次播放时间 & 最近播放列表

`last_played_time` 是 `SongInfo` 字段，同时也是 `ListItem::last_played_time`。列表维度的「最近播放」排序在 `CListCache::BuildSubList`：【源码确证】`ListCache.cpp:105`
```cpp
case SubType::ST_RECENT: add = (item.last_played_time > 0); break;
```
`CRecentList::m_list` 的注释明说：「列表的列表，总是保持按最近播放时间降序排序」（`CRecentList.h:100`）。

### 4.6 LastFM 同步：**完全旁路，不影响曲库统计**

【源码确证】`MusicPlayer2.cpp:803-842`

```cpp
void CMusicPlayerApp::LoadLastFMData() { m_lastfm.LoadData(m_lastfm_path); }   // lastfm.dat
void CMusicPlayerApp::SaveLastFMData() { m_lastfm.SaveData(m_lastfm_path); }
void CMusicPlayerApp::UpdateLastFMNowPlaying() { AfxBeginThread(UpdateLastFMNowPlayingFunProc, ...); }
void CMusicPlayerApp::UpdateLastFMFavourite(bool favourite) { AfxBeginThread(UpdateLastFMFavouriteFunProc, ...); }
void CMusicPlayerApp::LastFMScrobble() { AfxBeginThread(LastFMScrobbleFunProc, (LPVOID)NULL); }
```

所有网络操作都另起线程（不阻塞 UI），数据存独立的 `lastfm.dat`。

交界只有一处：【源码确证】`Player.cpp:2205-2216`（就在 `AddListenTime` 尾部）

```cpp
if (m_enable_lastfm) {
    int speed = static_cast<int>(m_speed * 1000);
    theApp.m_lastfm.AddCurrentPlayedTime(sec * speed);
    if (!theApp.m_lastfm.IsPushed()) {
        if (theApp.m_lastfm.CurrentTrackScrobbleable()) {
            theApp.m_lastfm.PushCurrentTrackToCache();
        }
    }
    if (theApp.m_media_lib_setting_data.lastfm_auto_scrobble && theApp.m_lastfm.IsScrobbeable()) {
        theApp.LastFMScrobble();
    }
}
```

另一处是红心同步：【源码确证】`Player.cpp:2170-2172`
```cpp
if (theApp.m_media_lib_setting_data.enable_lastfm) {
    theApp.UpdateLastFMFavourite(favourite);   // 红心时顺便通知 Last.fm 的 Love/Unlove
}
```

**结论：LastFM 是纯单向旁路 —— 曲库 → Last.fm。曲库的 `listen_time` / `rating` / 红心完全不受 Last.fm 影响，也不从 Last.fm 拉数据回填。** 唯一的耦合是「红心时顺手上报」。

注意 `LastFM.h:4-5` 里 API key 和 shared secret 是**硬编码在头文件里**的（GPL 开源项目的常规做法，但要提醒读者别抄）。

---

## 5. 搜索与筛选

项目里存在**两套互不相干的搜索实现**，容易混淆：

| | 媒体库对话框内的搜索 | 自绘 UI 的搜索框 |
|---|---|---|
| 实现文件 | `MediaClassifyDlg.cpp` `QuickSearch()` | `ListSearchCache.cpp` / `AbstractListElement::QuickSearch()` |
| 匹配范围 | 分类**名称**（艺术家名/专辑名…） | 列表**每一行的每一列文本** |
| 拼音支持 | 有（`IsItemMatchKeyWord`） | 有（`IsStringMatchWithPingyin`） |

### 5.1 搜索算法：**全量线性遍历，无索引、无前缀树、无缓存**

【源码确证】`ListSearchCache.cpp:14-32`

```cpp
bool CListSearchCache::reload()
{
    if (!m_list_cache.reload())          // ← 注意：没有 return，见下

    m_ui_searched_playing_index = -1;
    m_ui_search_result.clear();
    if (!m_ui_search_str.empty())
    {
        for (size_t i{}; i < m_list_cache.size(); ++i)     // 全量遍历
        {
            if (theApp.m_chinese_pingyin_res.IsStringMatchWithPingyin(
                    m_ui_search_str, m_list_cache.at(i).GetDisplayName()))
                m_ui_search_result.push_back(i);
        }
        ...
    }
    return true;
}
```

**两个发现：**

1. **【源码确证】`if (!m_list_cache.reload())` 后面缺了 `return`。** 这行 `if` 是个空语句体，无论列表缓存有没有变化，搜索都会照常重算一遍。看起来是漏写 `return;` 的 bug（原意应该是「列表没变就别重算了」）。**这直接废掉了 `CListCache` 的版本号优化在搜索路径上的作用。**

2. **每次搜索都是 O(N)，而且 N 行 × 每行都要现算拼音串。**

### 5.2 拼音：`unordered_map<wchar_t, wstring>` + **每次比较现拼字符串**

【源码确证】`ChinesePingyinRes.h:17-19` / `.cpp:14-40`

```cpp
class CChinesePingyinRes {
    void Init();
    static bool IsChineseCharactor(wchar_t ch);
    //判断一个字符串是否匹配关键字，关键字支持直接匹配、拼音首字母和全拼匹配，但是只支持一种方式匹配，不支持混合匹配
    bool IsStringMatchWithPingyin(const std::wstring& key_words, const std::wstring& compared_str);
private:
    std::unordered_map<wchar_t, std::wstring> m_pingyin_map;   // 保存每个汉字的拼音
};
```

初始化：从内嵌资源读一张表，**每个汉字只保留第一次出现的读音**：

```cpp
std::wstring pingyin_res = CCommon::GetTextResource(IDR_CHINESE_PINGYIN, CodeType::UTF8_NO_BOM);
...
wchar_t charactor = str_line[0];
if (!IsChineseCharactor(charactor)) continue;
std::wstring str_pingyin = str_line.substr(2);
if (!m_pingyin_map.contains(charactor))     // ← 只存第一个读音
    m_pingyin_map[charactor] = str_pingyin;
```

在 `InitInstance` 里初始化一次：【源码确证】`MusicPlayer2.cpp:299` `m_chinese_pingyin_res.Init();`

匹配主逻辑：【源码确证】`ChinesePingyinRes.cpp:47-89`

```cpp
bool CChinesePingyinRes::IsStringMatchWithPingyin(const std::wstring& key_words, const std::wstring& compared_str)
{
    //全部转换为小写
    std::wstring key_words_tmp{ key_words };
    std::wstring compared_str_tmp{ compared_str };
    CCommon::StringTransform(key_words_tmp, false);
    CCommon::StringTransform(compared_str_tmp, false);

    //直接匹配
    if (compared_str_tmp.find(key_words_tmp) != std::wstring::npos) return true;

    //构建被查找字符串的拼音全拼和首字母形式
    std::wstring full_pinyin_str;
    std::wstring initial_pinyin_str;
    for (const wchar_t& ch : compared_str_tmp)
    {
        if (IsChineseCharactor(ch) && m_pingyin_map.find(ch) != m_pingyin_map.end())
        {
            const std::wstring& pinyin{ m_pingyin_map[ch] };
            full_pinyin_str += pinyin;
            if (!pinyin.empty()) initial_pinyin_str += pinyin[0];
        }
        else
        {
            full_pinyin_str.push_back(ch);
            initial_pinyin_str.push_back(ch);
        }
    }

    if (full_pinyin_str.find(key_words_tmp) != std::wstring::npos) return true;    //全拼匹配
    if (initial_pinyin_str.find(key_words_tmp) != std::wstring::npos) return true; //首字母匹配
    return false;
}
```

评判：
- **优点**：`ChinesePingyinRes.cpp` 只有 89 行，很轻。三种匹配（原文 / 全拼 / 首字母）覆盖了绝大多数中文用户习惯。
- **缺点 1**：**每比较一行就重建两个完整拼音串**（`std::wstring` 逐字符 `+=`，会反复分配）。这是搜索慢的主要原因。
- **缺点 2**：没有「逐字累加拼音」的支持，即输入 `zhongyang` 能匹配「中阳」，但输入 `zy` 匹配「中阳」也可以（首字母串）。而混合形式（`中yang`）不支持——**头文件注释里明确承认了：「只支持一种方式匹配，不支持混合匹配」**。
- **缺点 3**：只存第一个读音，多音字（如「长」「乐」「重」）的次常用读音搜不到。
- **缺点 4**：拼音表是**内嵌资源** `IDR_CHINESE_PINGYIN`（`resource.h`），不是数据文件，想换表得重编译。

**没有任何地方缓存「这首歌的拼音全拼/首字母」**——grep `full_pinyin`/`initial_pinyin` 只在 `IsStringMatchWithPingyin` 内部出现，没有成员缓存、没有 `SongInfo` 字段。`【源码确证】`

### 5.3 两套缓存分别缓存什么，何时失效

#### `CListCache` —— 缓存 `CRecentList` 的 ListItem 列表（列表的列表，不是曲目）

【源码确证】`ListCache.h:24-65`

```cpp
////////////////////////////////////////////////////////////////////////////////
//  以下所有小写的方法使用的数据对象没有使用锁保护，要求总是在同一个线程调用才能够保证线程安全
//  如果reload返回true表示数据可能有变化，必要重绘

    bool reload();
    size_t size() const;
    const ListItem& at(size_t index) const;
    int playing_index() const;

////////////////////////////////////////////////////////////////////////////////
//  以下方法直接基于CRecentList的数据提供，有线程安全保证，但因为锁粒度问题不能用于绘制UI
//  作为CRecentList的友元，使用其中数据时同时也要维持其互斥量保护的数据不外露
//  当上面的小写方法在自绘UI线程中使用时以下方法供主线程的消息处理使用
//  但要求一次消息处理期间只能调用一次方法一次，以避开多次加锁间的同步问题 （可以在这里添加适合各自情况的各种方法）

//  这里的多线程安全特性维持太过艰难，取消独立UI线程才是正道，暂时这部分仍有些效果，今后应当移除
```

**这段注释是整份源码里信息密度最高的一段**，它把设计意图、使用约定、以及作者的自我否定都写清楚了。

缓存内容与失效：【源码确证】`ListCache.cpp:11-35`

```cpp
bool CListCache::reload()
{
    // 如果m_ver没变则返回false表示CRecentList数据没有变化
    if (m_ui_ver == CRecentList::m_instance.m_ver)
        return false;
    // 加锁后不允许再调用CRecentList的方法防止锁重入（为保证同步，reload期间只能加这一次锁，不能反复加解锁）
    std::lock_guard<std::mutex> lock(CRecentList::m_instance.m_mutex);
    const auto& instance = CRecentList::m_instance;
    ASSERT(!instance.m_list.empty());
    // m_ui_list
    m_ui_list.clear();
    vector<const ListItem*> sub_list;
    BuildSubList(sub_list);
    for (const ListItem* pListItem : sub_list)
        m_ui_list.push_back(*pListItem);        // ← 深拷贝一份
    auto iter = std::find(m_ui_list.begin(), m_ui_list.end(), instance.m_list.front());
    m_ui_current_play_index = (iter != m_ui_list.end()) ? iter - m_ui_list.begin() : -1;
    m_ui_ver = instance.m_ver;
    return true;
}
```

**失效依据是一个版本号 `m_ver`**：【源码确证】`CRecentList.h:98`
```cpp
std::atomic<int> m_ver{};   // 每次m_list/m_sort_mode变化时加一，即数据的修改标记
```

`SubsetType` 决定缓存哪个子集：【源码确证】`ListCache.h:9-19` + `ListCache.cpp:94-115`
```cpp
enum class SubsetType {
    ST_CURRENT,             // 仅含当前播放的列表
    ST_RECENT,              // 最近播放的列表
    ST_FOLDER,              // 文件夹
    ST_PLAYLIST,            // 播放列表
    ST_PLAYLIST_NO_SPEC,    // 除特殊播放列表外的播放列表
    ST_MEDIA_LIB,           // 媒体库
    ST_ALL,
};
```

注意 `reload()` 的调用点在**每个列表元素的 `Draw()` 里**：【源码确证】`UIElement__MediaLibFolder.cpp:10-14`
```cpp
void UiElement::MediaLibFolder::Draw()
{
    m_list_cache.reload();      // ← 每帧调用，靠 m_ver 挡住实际工作
    AbstractListElement::Draw();
}
```
同样模式见 `UIElement__RecentPlayedList.cpp:9-13`、`UIElement__MediaLibPlaylist.cpp:10-14`。所以「每帧一次 `reload()`」是靠一个 int 比较挡住的——设计上是对的。

#### `CListSearchCache` —— 在 `CListCache` 外面再包一层搜索结果索引

【源码确证】`ListSearchCache.h` 全文

```cpp
class CListSearchCache
{
public:
    CListSearchCache(CListCache::SubsetType type);
    bool reload();                                  // 重新载入数据 & 执行搜索
    size_t size() const;
    const ListItem& at(size_t index) const;
    wstring display_index(size_t index) const;      // 获取特定index的显示序号
    int playing_index() const;
    void SetSearchStr(const wstring& search_str = {});   // 设置空字符串即取消搜索
    ListItem GetItem(int index) const;
    int GetIndex(const ListItem& list_item) const;
private:
    CListCache m_list_cache;
    wstring m_ui_search_str;
    vector<size_t> m_ui_search_result;      // 存的是索引，不是副本
    int m_ui_searched_playing_index{ -1 };
};
```

**它缓存的是「匹配行的下标数组」**，不复制数据。优点：搜索结果和原列表天然一致（退出搜索只清空 `m_ui_search_str` 即可，见 `size()`/`at()` 的 `if (!m_ui_search_str.empty())` 分支）。缺点：`size()` 返回的是搜索结果数，导致「显示序号」需要用 `m_ui_search_result.at(index) + 1` 反查原序号（`display_index()`）——这个索引间接层贯穿了所有查询方法。

注意 `GetIndex()` 是**线性查找**（`ListSearchCache.cpp:74-80`），而 `ListCache::GetIndex()` 因为要 `BuildSubList` 也是线性的。`【源码确证】`

### 5.4 `AbstractListElement` 的快速搜索（自绘 UI 用）

【源码确证】`AbstractListElement.cpp:718-770`

```cpp
void UiElement::AbstractListElement::QuickSearch(const std::wstring& key_word)
{
    ... // 填充 search_result
}

bool UiElement::AbstractListElement::IsItemMatchKeyWord(int row, const std::string& key_word)
{
    // 默认匹配每一列中的文本，只要有一列的文本匹配就返回true
}
```

派生类可以重写 `IsItemMatchKeyWord`。按列匹配意味着**每一行的每一列都要调一次 `GetItemText`**——而 `GetItemText` 是虚函数，在曲目列表里每次都要 `GetSongListData()->GetItem(row).name`（返回 `const UTrackInfo&`，**无锁**）。**搜索是 N 行 × M 列次虚函数调用 + 无锁随机访问。** `【源码确证】`是调用结构；`【推测】`是性能影响程度。

### 5.5 媒体库对话框内的搜索

【源码确证】`MediaClassifyDlg.cpp:279-301` `QuickSearch`

```cpp
void CMediaClassifyDlg::QuickSearch(const wstring& key_word)
{
    m_search_result.clear();
    std::vector<SongInfo> other_list;
    for (const auto& item : m_classifer.GetMeidaList())
    {
        if (item.first == ListItem::STR_OTHER_CLASSIFY_TYPE)
        {
            for (const auto& song : item.second)          // 遍历“其他”里的每一首歌
                if (IsItemMatchKeyWord(song, key_word)) other_list.push_back(song);
        }
        else
        {
            if (IsItemMatchKeyWord(item.first, key_word))  // 只匹配分类名
                m_search_result[item.first] = item.second;
        }
    }
    ...
}
```

【源码确证】`MediaClassifyDlg.cpp:274-277`
```cpp
bool CMediaClassifyDlg::IsItemMatchKeyWord(const wstring& str, const wstring& key_word)
{
    return theApp.m_chinese_pingyin_res.IsStringMatchWithPingyin(key_word, str);
}
```

即：**在分类标签页里搜索，匹配的是艺术家名/专辑名/流派名，不是歌名。** 要搜歌名得去「所有曲目」标签页或文件夹浏览。这是个 UI 语义上的差异，参考文档里应该点明。

---

## 6. 大数据量下的流畅度

这一节是本次调查里**问题最集中**的地方。

### 6.1 列表：**没有真正的虚拟化** —— 每帧遍历全部行

`AbstractListElement::DrawScrollArea()` 的绘制循环：

【源码确证】`AbstractListElement.cpp:45-57`

```cpp
//displayed_row_index为显示的行号，for循环中的i为实际的行号
int displayed_row_index{};
for (int i{}; i < GetRowCount(); i++)          // ★ 遍历全部 N 行
{
    if (i < 0 || i >= static_cast<int>(item_rects.size())) break;
    //跳过不显示的行
    if (!IsRowDisplayed(i)) continue;          // ★ 线性查找 search_result
    CRect rect_item{ item_rects[displayed_row_index] };
    rect_item &= m_scroll_area_rect;
    //如果绘制的行在播放列表区域之外，则不绘制该行
    if (!(rect_item & rect).IsRectEmpty())     // ★ 只有这一步才做可见性判断
    {
        ...
```

**这是关键设计缺陷**：`for (int i = 0; i < GetRowCount(); i++)` 从第 0 行遍历到最后一行，**可见性判断（`rect_item & rect`）在循环体内部很靠后的位置（第 57 行）才做**。也就是说：

- 曲库 30000 首时，每一帧都要迭代 30000 次。
- 每次迭代至少调用 `IsRowDisplayed(i)`（搜索状态下是 `std::find` 线性查找，`AbstractListElement.cpp:754-762`）。
- 屏幕外的行虽然跳过了 `GetItemText` 和 GDI 绘制，但**循环开销、`CRect` 运算、`IsHighlightRow(i)` 都已经发生了**。

`IsHighlightRow(i)` 是每行都调的分支（`AbstractListElement.cpp:92`）：

【源码确证】`UIElement__AbstractTracksList.cpp:64-75`
```cpp
bool UiElement::AbstractTracksList::IsHighlightRow(int row)
{
    CUISongListMgr* ui_song_list_mgr = GetSongListData();
    if (ui_song_list_mgr != nullptr)
    {
        SongKey song_key = ui_song_list_mgr->GetItem(row).song_key;
        const auto& current_song = CPlayer::GetInstance().GetSafeCurrentSongInfo();
        if (!song_key.path.empty() && !current_song.IsEmpty())
            return (std::equal_to<SongKey>()(SongKey(current_song), song_key));
    }
    return false;
}
```

**这里要澄清一个容易误判的点**：`GetSafeCurrentSongInfo()` 名字里的 "Safe" **不是线程安全**，而是「边界安全」。它的实现完全不加锁、不拷贝：

【源码确证】`Player.cpp:2104-2109`
```cpp
const SongInfo& CPlayer::GetSafeCurrentSongInfo() const
{
    if (!m_loading && m_index >= 0 && m_index < GetSongNum())
        return m_playlist[m_index];
    else return m_no_use;
}
```
配套注释（`Player.h:481-482`）：「获取当前SongInfo常引用，m_index无效或正在加载播放列表时返回m_no_use，用于在UI中访问」——**是为了避免越界，不是为了同步**。（顺带说明它自己也不安全：读写 `m_playlist` 而无锁，只是 UI 场景下不易炸。）

所以这一行的真实开销是：构造一个临时 `SongKey`（拷贝一次 `wstring` 路径）+ `SongKey` 相等比较（`path` 字符串比较 + `cue_track` 比较）。虽然比「深拷贝 + 加锁」轻得多，但**每帧 × 每一行**累加，30000 行就是 30000 次临时 `wstring` 拷贝和路径比较。

> 修法很直接：把当前歌曲提到循环外取一次，或者循环直接从「第一个可见行」起步。这是**本报告成本最低、收益最大的建议**。

每次 `Draw()` 还会重建整个矩形数组：【源码确证】`AbstractListElement.cpp:592-606`

```cpp
void UiElement::AbstractListElement::CalculateItemRects()
{
    item_rects.resize(GetRowCount());              // ★ 每帧 resize 到 N
    for (size_t i{}; i < item_rects.size(); i++)
    {
        int start_y = -scroll_offset + rect.top + i * ItemHeight();
        CRect rect_item{ rect };
        rect_item.top = start_y;
        rect_item.bottom = rect_item.top + ItemHeight();
        item_rects[i] = rect_item;
    }
}
```

30000 行 → 每帧构造 30000 个 `CRect`。行高统一下，这些矩形完全可以用「i × item_height + scroll_offset」现算，**根本不需要数组**。

而 `Draw()` 每帧都调它：【源码确证】`AbstractListElement.cpp:261-271`
```cpp
void UiElement::AbstractListElement::Draw()
{
    CalculateRect();
    RestrictOffset();
    CalculateItemRects();          // ← 每帧
    if (last_row_count != GetRowCount()) { OnRowCountChanged(); last_row_count = GetRowCount(); }
    ...
```

**结论：这是一个「半虚拟化」实现 —— 只虚拟化了绘制，没虚拟化遍历。** 分页机制：**没有**（grep 不到任何分页/page 概念，只有 `scroll_offset` 的像素级滚动）。

### 6.2 滚动与行高

滚动是**像素级偏移量**，不是行索引：【源码确证】`AbstractScrollArea.cpp:189-197`
```cpp
bool UiElement::AbstractScrollArea::MouseWheel(int delta, CPoint point)
{
    if (rect.PtInRect(point))
    {
        scroll_offset += (-delta * ui->DPI(60) / 120);  //120为鼠标滚轮一行时delta的值
        return true;
    }
    return false;
}
```

滚动条把手长度计算：【源码确证】`AbstractScrollArea.cpp:62-73`
```cpp
int scroll_handle_length{ rect.Height() * rect.Height() / m_scroll_area_rect.Height() };
scroll_handle_length_comp = 0;
if (scroll_handle_length < MIN_SCROLLBAR_LENGTH) {
    scroll_handle_length_comp = MIN_SCROLLBAR_LENGTH - scroll_handle_length;
    scroll_handle_length = MIN_SCROLLBAR_LENGTH;
}
```

滚动区域总高度直接就等于「行高 × 总行数」：【源码确证】`AbstractListElement.cpp:256-259`
```cpp
int UiElement::AbstractListElement::GetScrollAreaHeight()
{
    return ItemHeight() * GetDisplayRowCount();
}
```

行高是可配的（DPI 缩放后）：`item_height{ 28 }`（`AbstractListElement.h:100`），可由 `playlist_item_height` 设置覆盖，范围 `MIN_PLAYLIST_ITEM_HEIGHT`..`MAX_PLAYLIST_ITEM_HEIGHT`（`Define.h:103` `#define MAX_PLAYLIST_ITEM_HEIGHT 64`），默认 24（`MusicPlayerDlg.cpp:784`）。

**所以滚动条在 3 万首时长度会是 `高度² / (28 × 30000)`，几乎只有最小长度 16px** —— 拖动精度极差。这是像素级滚动的副作用。`【源码确证】`公式 + `【推测】`用户体验结论。

### 6.3 打开媒体库对话框的卡顿：**靠懒加载缓解，不是靠快**

这是更新日志里提到的 V2.70 那类问题的真实处理方式。**代码注释里直接写了：**

【源码确证】`FolderExploreDlg.cpp:154-162`
```cpp
void CFolderExploreDlg::OnTabEntered()
{
    if (!m_initialized)
    {
        CWaitCursor wait_cursor;
        //注意，在这里向左侧树填充数据可能会比较缓慢，因此放到这里处理，而不在OnInitDialog()中处理，
        //即只有当标签切换到“文件夹浏览”时才填充数据，以加快“媒体库”对话框的打开速度
        ShowFolderTree();
    }
    ...
```

艺术家的分类标签页同理：【源码确证】`MediaClassifyDlg.cpp:319-334`
```cpp
void CMediaClassifyDlg::OnTabEntered()
{
    SetButtonsEnable();
    if (!m_initialized)
    {
        CWaitCursor wait_cursor;
        m_classifer.ClassifyMedia();          // ★ 在 UI 线程里全量分类
        ShowClassifyList();
        //设置左侧列表默认选中项
        if (CPlayer::GetInstance().IsMediaLibMode())
            SetLeftListSel(CPlayer::GetInstance().GetMedialibItemName());
        m_initialized = true;
    }
}
```

**这两处给出了完整答案：**
- 媒体库对话框本身很快打开，因为它**不预计算任何分类**；所有 `CMediaClassifyDlg` 都是空壳，等用户点进那个标签页才 `ClassifyMedia()`。
- 但点进去的那一刻，**`ClassifyMedia()` 在 UI 主线程同步执行**，带个 `CWaitCursor` 让用户知道在忙。3 万首的库 + 7 种分类，就是 7 次全库遍历（每次都要为每首歌算 `GetArtistList` 并做 `StringNormalize`）。
- 而且 `ClassifyMedia()` 的结果**不缓存在 `CUiMediaLibItemMgr`** —— `CUiMediaLibItemMgr::Init()` 会调用 7 次 `GetClassifiedMeidaLibItemList`（每个分类各建一个**临时** `CMediaClassifier` 跑一遍）：【源码确证】`UiMediaLibItemMgr.cpp:41-56` + `:23-39`

```cpp
void CUiMediaLibItemMgr::Init()
{
    m_loading = true;
    std::shared_lock<std::shared_mutex> lock(m_shared_mutex);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_ARTIST);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_ALBUM);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_GENRE);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_YEAR);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_TYPE);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_BITRATE);
    GetClassifiedMeidaLibItemList(ListItem::ClassificationType::CT_RATING);
    m_loading = false;
    m_inited = true;
}
```

`Init()` 里那个 `std::shared_lock` 其实是**语义错误**：它要写 `m_item_map`（`GetClassifiedMeidaLibItemList` 里 `auto& item_list{ m_item_map[type] }; item_list.clear();`），却只加了**读锁**（`shared_lock`）。多个 `CUiMediaLibItemMgr::Init()` 并发（比如启动更新完成后 + 用户手动刷新）会同时改 `m_item_map`。`【源码确证】`锁类型与写操作；`【推测】`并发触发的实际概率。

而且 `m_loading` 是在**加锁前**置 true、**解锁前**置 false（`m_loading = false; m_inited = true;` 之后才析构 shared_lock），所以这个 `m_loading` 并不能真正保护读者。**这是一处实打实的同步缺陷，值得在参考文档里点名。**

`TrackList::SetListItem` 里也有一处 UI 线程全量分类：【源码确证】`UIElement__TracksList.cpp:15-27`

```cpp
void UiElement::TrackList::SetListItem(const ListItem& list_item)
{
    m_list_item = list_item;
    std::vector<SongInfo> song_list;
    if (list_item.type == LT_MEDIA_LIB)
    {
        if (list_item.medialib_type != ListItem::ClassificationType::CT_NONE)
        {
            CMediaClassifier classifer(list_item.medialib_type);   // ★ 每次选中分类都重建
            classifer.ClassifyMedia();                              // ★ 全库遍历
            song_list = classifer.GetMeidaList()[list_item.path];
        }
    }
    ...
```

**「在媒体库左栏点一下艺术家」= 一次全库分类。** 而 `OnSelectionChanged()` 会在每次列表选择变化时调它——**包括用键盘上下键连续划过列表**。`【源码确证】`调用链 `MediaLibItemList::OnSelectionChanged` → `track_list->SetListItem`；`【推测】`连续按键时的卡顿程度。

文件夹分支还更重（同步做文件枚举 + cue 展开）：`UIElement__TracksList.cpp:30-45`
```cpp
else if (list_item.type == LT_FOLDER)
{
    CAudioCommon::GetAudioFiles(list_item.path, song_list, MAX_SONG_NUM, list_item.contain_sub_folder);
    int cnt{}; bool flag{};
    CAudioCommon::GetCueTracks(song_list, cnt, flag, MR_MIN_REQUIRED);   // 展开 cue
    auto sort_fun = SongInfo::GetSortFunc(list_item.sort_mode == SM_UNSORT ? SM_U_FILE : list_item.sort_mode);
    std::stable_sort(song_list.begin(), song_list.end(), sort_fun);
    for (auto& cur_song : song_list)
        cur_song = CSongDataManager::GetInstance().GetSongInfo3(cur_song);   // ★ 每条一次加锁查询
}
```

**最后这个循环是 N 次加锁 + N 次 `SongInfo` 拷贝**（`GetSongInfo3` 返回副本）。有 `LoadSongsInfo(vector&)` 批量版本（一次加锁处理一批，`SongDataManager.cpp:346-367`）却没用在这里——`FolderExploreDlg.cpp:99` 用了批量版本，`TracksList` 没用。**同一个项目里两种写法并存，`LoadSongsInfo` 是对的那一种。**

### 6.4 更新期间 UI 会「变空」

`CUiAllTracksMgr::UpdateAllTracks()`：【源码确证】`UiMediaLibItemMgr.cpp:305-334`

```cpp
void CUiAllTracksMgr::UpdateAllTracks()
{
    m_loading = true;
    std::shared_lock<std::shared_mutex> lock(m_shared_mutex);   // ← 又是 shared_lock
    m_all_tracks_list.clear();
    vector<SongInfo> tmp_song_list;
    CSongDataManager::GetInstance().GetSongData([&](const CSongDataManager::SongDataMap& song_data_map) {
        tmp_song_list.reserve(song_data_map.size());
        std::transform(song_data_map.begin(), song_data_map.end(), std::back_inserter(tmp_song_list),
                       [](const auto& item) { return item.second; });   // ★ 全库深拷贝
    });
    ListItem list_item{ LT_MEDIA_LIB, L"", ListItem::ClassificationType::CT_NONE };
    CRecentList::Instance().LoadItem(list_item);
    auto sort_fun = SongInfo::GetSortFunc(list_item.GetDefaultSortMode());
    std::stable_sort(tmp_song_list.begin(), tmp_song_list.end(), sort_fun);   // ★ 全量排序
    std::transform(...);                                            // ★ 每条再拷一次 + 算显示名 + Contains
    m_loading = false;
    m_inited = true;
}
```

**数据量放大链条**：`SongDataMap`(3万条) → `tmp_song_list` 拷贝(3万条) → 排序 → `m_all_tracks_list`(3万条 `UTrackInfo`)。峰值内存约 2 倍全库 `SongInfo`。而 `UTrackInfo` 是精简版：

【源码确证】`UiMediaLibItemMgr.h:76-82`
```cpp
struct UTrackInfo
{
    SongKey song_key;
    std::wstring name;      // 显示名（已按 display_format 算好）
    CPlayTime length;
    bool is_favourite{};
};
```

**这是个好设计**——UI 列表只持有渲染需要的最小字段集，而不是完整 `SongInfo`。这是本报告要推荐的做法之一。

但 `GetSongInfo(index)` 又回去查全库了：【源码确证】`UiMediaLibItemMgr.cpp:253-262`
```cpp
SongInfo CUISongListMgr::GetSongInfo(int index) const
{
    if (!m_loading) {
        if (index >= 0 && index < GetSongCount())
            return CSongDataManager::GetInstance().GetSongInfo(m_all_tracks_list[index].song_key);  // ★ 查库 + 拷贝
    }
    static SongInfo empty_song;
    return empty_song;
}
```
而这个函数在 `OnDoubleClicked`、`OnHoverButtonClicked`、`GetEmptyString` 等热路径上被调用。

### 6.5 专辑封面缩略图：**没有缩略图缓存**

【源码确证】`UIElement__AlbumCover.cpp` 全文只有 43 行，`Draw()` 就是：

```cpp
void UiElement::AlbumCover::Draw()
{
    CalculateRect();
    if (show_info) ui->DrawAlbumCoverWithInfo(rect);
    else           ui->DrawAlbumCover(rect);
    Element::Draw();
}
```

`CPlayerUIBase::DrawAlbumCover`：【源码确证】`CPlayerUIBase.cpp:2205-2220`
```cpp
void CPlayerUIBase::DrawAlbumCover(CRect rect)
{
    if (theApp.m_app_setting_data.show_album_cover && CPlayer::GetInstance().AlbumCoverExist())
    {
        ...
        m_draw.DrawRoundImage(CPlayer::GetInstance().GetAlbumCover(), ...);
        ...
    }
}
```

封面数据在 `CPlayer` 里**只有当前这一首**的两份 `CImage`：【源码确证】`Player.cpp:1700-1710`
```cpp
CImage& CPlayer::GetAlbumCover()
{
    CSingleLock sync(&m_album_cover_sync, TRUE);
    return m_album_cover;
}
ATL::CImage& CPlayer::GetAlbumCoverBlur()
{
    CSingleLock sync(&m_album_cover_sync, TRUE);
    return m_album_cover_blur;
}
```

切歌时重新加载 + 缩放：【源码确证】`Player.cpp:2438-2466` `CPlayer::SearchAlbumCover`
```cpp
m_album_cover.Destroy();
...
m_album_cover_path = audio_tag.GetAlbumCover(m_album_cover_type);
if (!m_album_cover_path.empty()) {
    m_album_cover.Load(m_album_cover_path.c_str());
    AlbumCoverResize();                      // 超过 max_album_cover_size 就缩
    MediaTransControlsLoadThumbnail();
}
m_inner_cover = !m_album_cover.IsNull();
if (m_album_cover.IsNull()) SearchOutAlbumCover();
```

高斯模糊是按需算的：【源码确证】`Player.cpp:2486-2511` `AlbumCoverGaussBlur`
```cpp
void CPlayer::AlbumCoverGaussBlur()
{
    if (m_album_cover.IsNull()) { m_album_cover_blur.Destroy(); return; }
    CSize image_size(m_album_cover.GetWidth(), m_album_cover.GetHeight());
    ...
    CGaussBlur gauss_blur;
    gauss_blur.DoGaussBlur(image_tmp, m_album_cover_blur);
}
```

而 `SearchAlbumCover` 里这行是**被注释掉的**：【源码确证】`Player.cpp:2466` `//AlbumCoverGaussBlur();`
→ 说明模糊是**延迟到真正需要当背景时才计算**（`CPlayerUIBase.cpp:890`：`background_gauss_blur ? GetAlbumCoverBlur() : GetAlbumCover()`）。`【源码确证】`这两行；`【推测】`「延迟计算」这个意图。

**结论：**
- **封面缩略图没有磁盘缓存，也没有内存 LRU 缓存。** 内存里只有当前歌曲的 `m_album_cover` + `m_album_cover_blur` 两张图（`CPlayer.h`）。
- 媒体库列表**不显示专辑封面缩略图**——`MediaLibItemList`/`AbstractTracksList` 的 `GetIcon()` 是 `IconMgr::IconType` 图标，不是图片。所以「列表滚动时读几百张封面」这个常见性能坑在这个项目里**不存在**。`【源码确证】`（`AbstractListElement.h:48-49` `GetIcon` 返回 `IconMgr::IconType`）
- 唯一的封面尺寸控制是 `max_album_cover_size`（默认 800，`CommonData.h:416`），`AlbumCoverResize` 把过大的封面缩到该尺寸（`Player.cpp:2513-2529`）。
- `GaussBlur.cpp` / `GaussBlur.h` 是一个自实现的模糊，只用于「专辑封面当背景」这一处。

### 6.6 其他流畅度相关事实

- **独立 UI 线程 + 帧率自调节**：【源码确证】`MusicPlayerDlg.cpp:4538-4610` `CMusicPlayerDlg::UiThreadFunc`，末尾 `Sleep(pThis->m_ui_refresh_interval)`。帧率控制逻辑（`MusicPlayerDlg.cpp:2738-2742`）：
```cpp
//限制帧率
if (theApp.m_fps > MAX_FPS + FPS_LIMIT_MARGIN)  m_ui_refresh_interval++;
if (m_ui_refresh_interval > theApp.m_app_setting_data.ui_refresh_interval && theApp.m_fps < MAX_FPS - FPS_LIMIT_MARGIN)
    m_ui_refresh_interval--;
```
`MAX_FPS = 90`、`FPS_LIMIT_MARGIN = 10`、`UI_INTERVAL_DEFAULT = 50`（`Define.h:97,202,203`）。**即帧率超标就自动降频**——这是「宁可掉帧也不卡死」的取舍，对 3 万行列表来说是被迫的。
- UI 线程在媒体库更新时会强制刷新：【源码确证】`MusicPlayerDlg.cpp:4565-4568`
```cpp
if (pThis->IsWindowVisible() && !pThis->IsIconic()
    && (CPlayer::GetInstance().IsPlaying() || pPara->is_active_window || pPara->draw_reset
        || pPara->ui_force_refresh || CPlayer::GetInstance().m_loading || theApp.IsMeidaLibUpdating())
    && (!pPara->is_completely_covered || theApp.m_nc_setting_data.always_on_top))
```
- **没有「双击排序」的重排缓存**：排序靠 `SongInfo::GetSortFunc(SortMode)` 返回 `std::function` 谓词（`SongInfo.h:133`），比较走 `CompareStringEx`（本地化比较，比 `wcscmp` 慢不少）。
- **`m_folder_audio_files_num` 缓存**：【源码确证】`UiMediaLibItemMgr.cpp:420-461` `CUiFolderExploreMgr::GetAudioFilesNum` 用 `std::map<wstring,int>` 缓存每个文件夹的音频数，避免重复递归。这是项目里**少数真正的缓存**，值得学。
- 但 `CUiFolderExploreMgr::UpdateFolders` 里有个忙等：【源码确证】`UiMediaLibItemMgr.cpp:368-372`
```cpp
//这里等待播放内核加载完成，否则无法判断文件夹内音频文件数量
while (!CPlayer::GetInstance().IsPlayerCoreInited())
{
    Sleep(20);
}
```
这是一个**没有超时的自旋等待**。如果播放内核初始化失败，这个线程就永远卡住（并且它可能持有调用方的上下文）。`【源码确证】`是代码事实；`【推测】`是后果。

---

## 7. 文件组织：媒体库 / 文件夹 / 播放列表三种模式怎么统一

**核心机制：三种模式全部归约到同一个 `ListItem` + 同一个 `CPlayer::m_playlist`。**

【源码确证】`ListItem.h:5-11`
```cpp
enum ListType
{
    LT_FOLDER,
    LT_PLAYLIST,
    LT_MEDIA_LIB,
    LT_MAX,
};
```

**`ListItem` 是「一个可播放对象」的统一抽象**（`ListItem.h:13` 注释原文：「ListItem描述一个"作为列表的可播放对象"的全部信息」）。三种模式共享同一套字段：`sort_mode` / `last_track` / `last_position` / `total_time` / `total_num` / `last_played_time` / `create_time`。媒体库模式额外用 `medialib_type` 记分类种类、`path` 记分类名。

播放器侧统一入口：【源码确证】`Player.cpp:1034-1095` `CPlayer::SetList(ListItem list_item, bool play, bool force)`

```cpp
switch (list_item.type)
{
case LT_FOLDER:
    m_path = list_item.path;
    m_playlist_path.clear();
    m_playlist_mode = PM_FOLDER;
    m_sort_mode = list_item.GetDefaultSortMode();
    m_contain_sub_folder = list_item.contain_sub_folder;
    m_index = 0;
    m_media_lib_playlist_type = {};
    m_media_lib_playlist_name.clear();
    break;
case LT_PLAYLIST:
    m_path = {};
    m_playlist_path = list_item.path;
    m_playlist_mode = PM_PLAYLIST;
    m_sort_mode = SM_UNSORT;   // 考虑今后允许播放列表维持排序状态，ui怎样设计(菜单)还没想好
    ...
    break;
case LT_MEDIA_LIB:
    m_path = {};
    m_playlist_path = {};
    m_playlist_mode = PM_MEDIA_LIB;
    m_sort_mode = list_item.GetDefaultSortMode();
    m_contain_sub_folder = false;
    m_index = 0;
    m_media_lib_playlist_type = list_item.medialib_type;
    m_media_lib_playlist_name = list_item.path;
    break;
default:
    ASSERT(FALSE);
    break;
}
IniPlayList(play, {}, play_song);
```

模式查询就是三个薄封装：【源码确证】`Player.cpp:2586-2599`
```cpp
bool CPlayer::IsPlaylistMode() const { return m_playlist_mode == PM_PLAYLIST; }
bool CPlayer::IsFolderMode()   const { return m_playlist_mode == PM_FOLDER; }
bool CPlayer::IsMediaLibMode() const { return m_playlist_mode == PM_MEDIA_LIB; }
```

**统一到「一份播放队列」**：无论哪种模式，最终都是把曲目灌进 `CPlayer::m_playlist`（一个 `vector<SongInfo>`），然后播放器只认这个 vector。模式只影响「怎么填 `m_playlist`」。

命令层也是统一入口：【源码确证】`MusicPlayerCmdHelper.cpp:891-900`
```cpp
void CMusicPlayerCmdHelper::OnListItemSelected(const ListItem& list_item, bool play, bool force)
{
    if (list_item.empty()) return;
    if (!CPlayer::GetInstance().SetList(list_item, play, force))
    {
        const wstring& info = theApp.m_str_table.LoadText(L"MSG_WAIT_AND_RETRY");
        GetOwner()->MessageBox(info.c_str(), NULL, MB_ICONINFORMATION | MB_OK);
    }
}
```

UI 层则统一到 `AbstractListElement` 的**模板方法模式**：

【源码确证】`AbstractListElement.h:44-76` 定义了一组纯虚/可重写钩子：
```cpp
virtual std::wstring GetItemText(int row, int col) = 0;
virtual int GetRowCount() = 0;
virtual int GetColumnCount() = 0;
virtual int GetColumnWidth(int col, int total_width) = 0;
virtual IconMgr::IconType GetIcon(int row) { return IconMgr::IT_NO_ICON; }
virtual std::wstring GetEmptyString() { return std::wstring(); }
virtual bool IsHighlightRow(int row) { return false; }
virtual int GetColumnScrollTextWhenSelected() { return -1; }
virtual CMenu* GetContextMenu(bool item_selected) { return nullptr; }
virtual void OnDoubleClicked() {}
virtual void OnSelectionChanged() {}
virtual int GetHoverButtonCount(int row) { return 0; }
...
virtual bool IsMultipleSelectionEnable() { return false; }
```

继承链：
```
UIElement::Element
  └ UIElement::AbstractScrollArea          (滚动条 + scroll_offset)
      └ UIElement::AbstractListElement     (行绘制模板 + 选择集 + 搜索)
          ├ UIElement::AbstractTracksList  (曲目列：序号/曲目/时间)
          │   ├ UIElement::AllTracksList   (转调 CUiAllTracksMgr)
          │   ├ UIElement::MyFavouriteList (转调 CUiMyFavouriteItemMgr)
          │   └ UIElement::TrackList       (文件夹/播放列表/媒体库分类 → CUISongListMgr)
          ├ UIElement::MediaLibItemList    (媒体库左栏：分类项列表)
          ├ UIElement::MediaLibFolder      (文件夹列表)
          ├ UIElement::MediaLibPlaylist    (播放列表列表)
          ├ UIElement::RecentPlayedList    (最近播放)
          ├ UIElement::Playlist            (主界面播放列表)
          └ UIElement::TreeElement         (文件夹浏览树)
```

三种数据来源的「适配器」写法非常一致——每个子类只实现 4-6 个钩子，其余全继承。**这是本报告最值得借鉴的架构点。**

例：【源码确证】`UIElement__AllTracksList.cpp`（整个文件只有 8 行！）
```cpp
#include "stdafx.h"
#include "AllTracksList.h"
#include "UiMediaLibItemMgr.h"

CUISongListMgr* UiElement::AllTracksList::GetSongListData()
{
    return &CUiAllTracksMgr::Instance();
}
```
其余全部来自 `AbstractTracksList`。

**UI 布局是数据驱动的**：三种模式的面板由 XML 皮肤定义（`skins/panels/folder_explore_panel.xml`、`skins/02_grooveMusic.xml` 等），`ElementFactory.cpp` 按 `type` 属性创建对应元素类。`MediaLibItemList::FromXmlNode` 读 `type="artist|album|genre|year|file_type|bitrate|rating"` 和 `track_list_element_id`：【源码确证】`UIElement__MediaLibItemList.cpp:199-218`。所以「左栏分类列表 + 右栏曲目列表」的联动是通过 **XML 里的元素 id 引用**连接的：

```cpp
void UiElement::MediaLibItemList::FindTrackList()
{
    if (!find_track_list) {
        track_list = FindRelatedElement<TrackList>(track_list_element_id);
        find_track_list = true;  // 找过一次没找到就不找了
    }
}
```

**注意 `find_track_list` 这个「找一次就记」的标志位** —— 意味着 XML 加载完成后元素关系就固定了，运行期不支持动态增删关联元素。

「媒体库模式」的 `m_sort_mode` 用 `SM_UNSORT` 并在注释里明确说明「不进行持久化」：【源码确证】
- `SongInfo.h:40` `SM_UNSORT = 100,  // 未排序（进入播放列表模式时总是设置为此排序方式，且不进行持久化）`
- `Player.cpp:1133` `m_sort_mode = SM_UNSORT;    // 播放列表模式下默认未排序`

---

## 8. 坑与限制

**每条都标注「源码确证」还是「推测」。**

### 8.1 【源码确证】没有数据库 → 无查询能力，一切靠内存遍历

- 想按「比特率 > 320 且流派 = 摇滚」筛选？只能全库遍历一次。
- 想增量写入一条记录？做不到，只能全量重写整个文件。
- 想并发访问？靠一个 `shared_mutex` 扛。
- 曲库文件**不是稳定格式**（`unordered_map` 遍历顺序），第三方工具难以直接读。

### 8.2 【源码确证】全量重写 + 主线程定时器 = 周期性卡顿

`SaveSongData` 用 `CFile::modeCreate | CFile::modeWrite` 重写整个文件（`SongDataManager.cpp:25-26`），由 `CMusicPlayerDlg` 的 80ms 定时器每 600 拍（约 48 秒）触发一次（`MusicPlayerDlg.cpp:2686-2692`），**运行在主线程**。同时 `AddListenTime` 每秒都在置脏（`Player.cpp:2203`），所以只要在播放，这个 48 秒重写就会持续发生。

`【推测】`：3 万首库 → 估算文件 8-12 MB → 每次写入 + 序列化开销，可能造成可见卡顿。**建议实测验证。**

### 8.3 【源码确证】列表绘制每帧遍历全部行

`AbstractListElement::DrawScrollArea()` 的 `for (int i{}; i < GetRowCount(); i++)`（`AbstractListElement.cpp:47`）里，**可见性判断（第 57 行 `rect_item & rect`）在循环体内很靠后的位置才做**，所以屏幕外的行也要完整走一遍循环体前置逻辑；同时每行调用 `IsHighlightRow(i)`（内部为 `GetItem(row)` 的 `shared_mutex` 读锁 + 临时 `SongKey` 构造 + 路径字符串比较，`AbstractTracksList.cpp:64-75`），每帧还重建 N 个 `CRect`（`AbstractListElement.cpp:592-606`）。

**这是最常见的卡顿根因，也是最容易修的（由 `scroll_offset / item_height` 直接算出可见区间，只遍历该区间；`IsHighlightRow` 的当前歌曲提到循环外取一次；行矩形现算不入数组）。**

### 8.4 【源码确证】分类结果完全不缓存，每次现算

`CMediaClassifier::ClassifyMedia()` 每次 `m_media_list.clear()` 后全库遍历（`MediaLibHelper.cpp:33-138`）。它在这些路径上被调用：
- `CMediaClassifyDlg::OnTabEntered()`（进入标签页，UI 线程）
- `CMediaClassifyDlg::RefreshData()`（**每次媒体库设置变化**，`MusicPlayerDlg.cpp:1432-1436` 连调 5 个 RefreshData）
- `TrackList::SetListItem()`（**左栏每换一个选中项**，`UIElement__TracksList.cpp:24-26`）
- `CUiMediaLibItemMgr::Init()` × 7 次（`UiMediaLibItemMgr.cpp:41-56`）

一个 7 分类 × 3 万首的库，光是媒体库更新完成后的 `Init()` 就要跑 7 遍全库遍历，每遍还要为每首歌调 `GetArtistList`（含 `StringNormalize`）。

**「在媒体库左栏用键盘上下键翻艺术家」= 每按一次一次全库分类。** 这是最严重的交互级性能问题。

### 8.5 【源码确证】`CUiMyFavouriteItemMgr::Contains` 让全库构建变成 O(N×F)

`UiMediaLibItemMgr.cpp:236` 和 `:329` 对每首歌调用一次 `Contains`，而 `Contains` 是 `std::find` 线性查找（`:205-209`）。

红心越多越慢。**改成 `unordered_set<SongKey>` 就是一行代码的事。**

### 8.6 【源码确证】共享锁误用 + 读取路径完全无锁

**问题一：`std::shared_lock` 用在要写数据的函数上。**
- `CUiMediaLibItemMgr::Init()`（`UiMediaLibItemMgr.cpp:44`）后面跟着 `m_item_map[type]` 的写入
- `CUiAllTracksMgr::UpdateAllTracks()`（`UiMediaLibItemMgr.cpp:308`）后面跟着 `m_all_tracks_list.clear()` 和填充
- `CUiMyFavouriteItemMgr::UpdateMyFavourite()`（`UiMediaLibItemMgr.cpp:188`）后面跟着 `m_may_favourite_song_list` 的填充
- `CUISongListMgr::Update()`（`UiMediaLibItemMgr.cpp:228`）后面跟着 `m_all_tracks_list` 的填充

四个都是「更新函数」却只加读锁。`shared_lock` 允许多个持有者同时进入，所以**两个更新函数并发执行会同时改同一个容器**。

**问题二（更严重）：所有读取路径根本没有加锁。**

`m_shared_mutex` 在整个 `UiMediaLibItemMgr.cpp` 里只出现在上面那 4 行的 `shared_lock` 声明处，**没有任何一处 `unique_lock`**。而读取侧的一大批方法：`GetSongCount()`、`GetItem(int)`、`GetSongInfo(int)`、`GetItemCount()`、`GetItemDisplayName()`、`GetItemInfo()`、`GetCurrentIndex()`、`GetSongList()`、`Contains()`、`GetRootNodes()`——**一个锁都没加，直接读 `m_all_tracks_list` / `m_item_map` / `m_may_favourite_song_list` / `m_root_nodes`。**

于是实际情形是：
- 更新线程持有 `shared_lock` 并 `clear()` + 重建 vector；
- 绘制线程（独立 UI 线程！）无锁地遍历同一个 vector；

**两边的锁完全不构成互斥** —— 读者绕过了锁。这是 `std::vector` 在「清空 + 重新填充」期间被并发读取，属于教科书级的迭代器失效 / 越界崩溃场景。

`【源码确证】`：锁的存在位置、读取方法的内容、以及 UI 独立线程（`MusicPlayerDlg.cpp:4538` `UiThreadFunc`）。
`【推测】`：实际崩溃频率取决于「媒体库更新完成」与「用户正好在滚动列表」的时间重叠概率。项目里 `m_loading` 这个 `std::atomic<bool>` 似乎是作者用来兜底的意图（`GetSongCount()` 在 `m_loading` 期间返回 0），但 `m_loading` 在 `Init()` 里是**加锁前置 true、解锁前置 false**，且 `GetItem(int)` 这类方法**根本没检查 `m_loading`**（只检查了 `index` 越界），所以这层兜底是漏的。

### 8.7 【源码确证】进度/退出标志是无保护的裸变量

`MediaUpdateThreadPara`（`CommonData.h:493-499`）的 `num_added` / `process_percent` / `thread_exit` 都是裸 `int`/`bool`，被工作线程写、UI 线程读、主线程写 `thread_exit`（`MusicPlayer2.cpp:325`）。技术上数据竞争（UB）。作者自己在 `MusicPlayer2.cpp:648` 留了注释承认线程安全问题。

### 8.8 【源码确证】`MAX_SONG_NUM` 是静默截断

`Define.h:75` `#define MAX_SONG_NUM 99999`，`GetAudioFiles` 里 `if (files.size() >= max_file) break;`（`AudioCommon.cpp:176`、`:217`）。超了不报错、不提示，**用户不会知道有文件没进曲库**。而且 `max_file` 是每个媒体库文件夹独立计数的。

`【推测】`：对绝大多数用户不会触发，但作为参考文档应该点明「硬上限 + 静默失败」这个反模式。

### 8.9 【源码确证】路径大小写敏感 → 重复条目

`SongKey` 的 `hash` / `equal_to` 用 `std::hash<wstring>` 和 `wstring::operator==`（`SongInfo.h:185-198`），**大小写敏感**。Windows 文件系统不区分大小写，所以：
- 同一文件用 `d:\music\a.mp3` 和 `D:\Music\A.MP3` 两种写法加入，会产生**两条记录**。
- 曲库文件在磁盘上移动/改盘符后路径变化，`ChangeFilePath`（`SongDataManager.cpp:472-483`）只在重命名流程里被调用，没有全局的路径修复迁移（对比：播放列表有 `FixPlaylistPathError`，`MusicPlayerCmdHelper.cpp:797`）。

### 8.10 【源码确证】`modified_time` 精度过高导致「假增量」

`modified_time` 存的是 **100 纳秒精度的原始 FILETIME**（`Common.cpp::GetFileLastModified`），比较是**精确相等**（`AudioCommon.cpp:551`）。任何把文件重写一遍的操作（即使内容完全相同，如某些同步工具、媒体库迁移脚本）都会让 mtime 变化 → 触发重新读标签。

反向更糟：**如果 mtime 没变但内容变了**（某些工具会保留时间戳，或 FAT/exFAT 只有 2 秒精度的粗时间戳），曲库会**永久保留过期元数据**，只能靠「刷新媒体库」（`force=true`）手动修。`【源码确证】`比较逻辑；`【推测】`是具体工具行为。

### 8.11 【推测】网络盘 / 慢速盘

- 扫描是**单线程递归**，每个文件一次 `GetFileAttributesEx`。网络盘上一次 stat 几十毫秒的话，1 万个文件就是几分钟到几十分钟。
- `WaitForSingleObject(m_media_lib_update_thread->m_hThread, 1000)`（`MusicPlayer2.cpp:327`）**只等 1 秒**。超时后主程序继续走 `SaveSongData()` 并退出，而扫描线程可能还在跑 → **进程退出过程中的竞态**。`【源码确证】`这行代码与 1 秒超时；`【推测】`后果的严重程度。
- `CUiFolderExploreMgr::UpdateFolders` 里没有超时的 `while (!IsPlayerCoreInited()) Sleep(20);`（`UiMediaLibItemMgr.cpp:369-372`）在慢速环境下会拖住。

### 8.12 【源码确证】cue 分轨与原始音频的去重

这是**做对了**的地方，但机制值得说明：

`SongKey` 用 `(path, cue_track)` 双键（`SongInfo.h:142-183`），所以 `album.flac` 和它的 12 条 cue 分轨 (`album.flac`, 1..12) 是不同的 key，不会互相覆盖。

扫描时先展开 cue，再把被 cue 引用的原始音频条目**从列表里剔除**，避免「整轨 + 分轨」重复出现在曲库里：【源码确证】`AudioCommon.cpp:521-523`

```cpp
// 移除files中的cue关联原始音频文件条目（在这之后才能进行普通音频的处理以避免cue关联音轨进入媒体库）
if (!audio_file.empty())
    std::erase_if(files, [&](const SongInfo& song_info) {
        return !song_info.is_cue && audio_file.contains(song_info.file_path);
    });
```

但注意：**这个剔除发生在扫描流程的内存列表里，不影响已经在曲库里的旧条目**。如果先扫了整轨（在没有 cue 的时候），之后才加了 cue 文件，旧条目仍在曲库中，且 `GetAudioInfo` 对 `song.is_cue` 的条目会 `continue` 跳过（`AudioCommon.cpp:539-540`），所以**整轨条目不会被 cue 分轨替换掉**。`【源码确证】`两个分支的代码；`【推测】`实际会不会出现重复条目取决于用户的目录变化顺序。

另外 `CleanUpSongData` 只判断 `CCommon::FileExist(song.file_path)`（`MusicPlayerCmdHelper.h:52`），对 cue 分轨来说 `file_path` 是那个原始音频文件，**所以删掉 cue 文件但保留 flac 时，分轨条目不会被清理**。`【源码确证】`判定条件；`【推测】`是清理不彻底的结论。

### 8.13 【源码确证】添加媒体库文件夹**不会自动触发扫描**

`CMediaLibSettingDlg::OnBnClickedAddButton`（`MediaLibSettingDlg.cpp:474-487`）只往 `m_data.media_folders` 里 push 一个路径，**不调用任何扫描**：

```cpp
void CMediaLibSettingDlg::OnBnClickedAddButton()
{
    CFolderPickerDialog dlg;
    if (dlg.DoModal() == IDOK)
    {
        CString dir_str = dlg.GetPathName();
        if (!CCommon::IsItemInVector(m_data.media_folders, wstring(dir_str)))
        {
            m_data.media_folders.push_back(wstring(dir_str));
            m_dir_list_ctrl.AddString(dir_str);
        }
    }
}
```

`media_folders` 的变化经由 `ApplyMediaLibSettings`（`MusicPlayerDlg.cpp:1404`）生效，但那里只更新 UI（`CUiFolderExploreMgr::UpdateFolders()`，`:1440-1444`），**依然不扫描曲库**。

**所以添加文件夹之后，曲库不会自动更新**——用户必须再打开设置点「刷新媒体库」，而那个按钮是 `force=true`（`MediaLibSettingDlg.cpp:554`）**全量重读所有标签**。

也就是说，**「刚加了一个 5 万首的文件夹」的最自然操作路径是「一次完整的全量标签重读」**。代码里其实已经准备好了中间档 `MR_MIN_REQUIRED`（`AudioCommon.h:58` 「仅获取不存在于媒体库的条目(最小化文件读取，最快但不保证最新)」），**但设置界面上没有把这个选项暴露给用户**。这是一个「底层能力已具备、产品层没接上」的典型缺口。

### 8.14 【源码确证】多处已知 bug / 疑似 bug

| 位置 | 问题 |
|---|---|
| `ListSearchCache.cpp:16` | `if (!m_list_cache.reload())` 后**缺 `return`**，空语句体，缓存优化失效 |
| `MusicPlayerCmdHelper.cpp:576` | `!song_info.is_cue && COSUPlayerHelper::IsOsuFile(...)` —— 与上方 `SetRating` 的 `!IsOsuFile` 不对称，疑似漏了 `!` |
| `UiMediaLibItemMgr.cpp:44,188,308` | 更新函数用 `shared_lock` |
| `SongInfo.h:71` | 作者自述 `info_acquired`「实际上已完全没有作用」（技术债标记） |
| `ListItem.h:39` | `STR_OTHER_CLASSIFY_TYPE = L"eRk0Q6ov"` 魔法哨兵字符串 |
| `UiMediaLibItemMgr.cpp:369-372` | `while (!IsPlayerCoreInited()) Sleep(20);` 无超时 |
| `SongMultiVersion.cpp:32` | `percent = index * 100 / vec_size;` 且 `index` 在 erase 分支也自增，进度百分比可能不准 |

### 8.15 【源码确证】曲库文件损坏时静默降级

`LoadSongData` 的 `catch (CArchiveException*)` 只写日志（`SongDataManager.cpp:257-261`），**不提示用户**，然后程序带着一个**部分加载**（甚至空）的曲库继续运行。如果用户接着正常退出，`SaveSongData` 会把这份不完整的曲库**覆盖写回文件** → **原始数据永久丢失**。

`【源码确证】`catch 块内容 + `InitInstance` 末尾无条件 `SaveSongData()`（`MusicPlayer2.cpp:329`）；`【推测】`是「损坏即丢数据」的最坏路径。

**这是整份报告里我认为最该避免的一个坑：读取失败时绝不能允许覆盖写回。**

---

## 9. 最值得借鉴的 5 条

1. **`ListItem` + `AbstractListElement` 的双层统一抽象。**
   三种模式（文件夹/播放列表/媒体库）共用同一个 `ListItem` 结构（`ListItem.h:14`）和同一个播放队列 `CPlayer::m_playlist`；UI 层用「纯虚钩子 + 模板方法」的 `AbstractListElement`（`AbstractListElement.h:44-76`）把 8 个列表子类压到 4-6 个函数的实现量（`AllTracksList.cpp` 全文只有 8 行）。**新加一种列表来源的成本极低。**

2. **UI 列表只持有渲染所需的最小字段集。**
   `UTrackInfo { SongKey song_key; wstring name; CPlayTime length; bool is_favourite; }`（`UiMediaLibItemMgr.h:76-82`），显示名在构建时按当前 `display_format` 一次性算好。列表绘制不再碰完整 `SongInfo`。**这就是「视图模型」的思路，在 C++ 桌面程序里同样适用。**

3. **增量扫描用「每文件 mtime 精确比对」，并做三级刷新模式。**
   `MediaLibRefreshMode { MR_MIN_REQUIRED, MR_FILE_MODIFICATION, MR_FOECE_FULL }`（`AudioCommon.h:56-61`）+ `song_info.modified_time == modified_time` 判定（`AudioCommon.cpp:551`）。简单、无状态、不需要「上次扫描时间戳」，也不会因为时钟回拨出错。加上 `GetFileAttributesEx` 替代 `FindFirstFile` 的实测微优化（`Common.cpp`，作者注明「耗时约为 FindFirstFile 的 2/3」）。

4. **可中断的长任务 + 进度反馈 + 单飞保护。**
   工作线程轮询 `exit_flag`（`AudioCommon.cpp:536`），退出时主线程置位并 `WaitForSingleObject`（`MusicPlayer2.cpp:325-327`）；`StartUpdateMediaLib` 用 `if (!m_media_lib_updating)` 防止重复启动（`MusicPlayer2.cpp:639`）。粗糙但有效，且**没有引入任何线程池/任务队列依赖**。

5. **懒加载 + 明确的「慢操作放这里」注释。**
   `CFolderExploreDlg::OnTabEntered`（`FolderExploreDlg.cpp:154-162`）和 `CMediaClassifyDlg::OnTabEntered`（`MediaClassifyDlg.cpp:319-334`）都把重活推迟到标签页首次进入，并配 `CWaitCursor`。注释原文：「注意，在这里向左侧树填充数据可能会比较缓慢，因此放到这里处理，而不在OnInitDialog()中处理，即只有当标签切换到"文件夹浏览"时才填充数据，以加快"媒体库"对话框的打开速度」。

   配套的还有 `CUiFolderExploreMgr::GetAudioFilesNum` 的 `std::map` 文件夹计数缓存（`UiMediaLibItemMgr.cpp:420-461`）。

---

## 10. 最该避开的 3 个坑

1. **「列表绘制每帧遍历全量行」。**
   `AbstractListElement.cpp:47` 的 `for (int i{}; i < GetRowCount(); i++)` 里，可见性判断在第 57 行才做；每行还调用 `IsHighlightRow(i)`（内部 `GetItem(row)` + 构造临时 `SongKey` + 路径字符串比较，`AbstractTracksList.cpp:64-75`），并且每帧 `CalculateItemRects()` 重建 N 个 `CRect`（`:592-606`）。
   **正确做法**：由 `scroll_offset / item_height` 直接算出 `[first_visible, last_visible]`，只遍历这个区间；行矩形现算不入数组；当前歌曲在循环外取一次。

2. **「分类/搜索/红心判定结果不缓存，每次全库遍历」。**
   `CMediaClassifier::ClassifyMedia()` 每次 `clear()` 重算（`MediaLibHelper.cpp:33`），却在 `TrackList::SetListItem`（左栏每次换选中项）、`RefreshData`（每次设置变化）、`Init`（7 次）里反复调用；`CUiMyFavouriteItemMgr::Contains` 用 `std::find` 线性查找，导致「所有曲目」构建是 **O(N×F)**（`UiMediaLibItemMgr.cpp:236,329`）。
   **正确做法**：分类结果用 `(type, 设置版本)` 做键缓存；红心用 `unordered_set<SongKey>`；分类/搜索放到工作线程，UI 只读快照。

3. **「全量重写 + 读取失败仍允许写回」。**
   `SaveSongData` 用 `modeCreate` 全量重写（`SongDataManager.cpp:25-26`），由主线程定时器每约 48 秒触发（`MusicPlayerDlg.cpp:2686-2692`）；而 `LoadSongData` 的 `catch` 只记日志不提示（`SongDataManager.cpp:257-261`），随后 `InitInstance` 退出路径**无条件** `SaveSongData()`（`MusicPlayer2.cpp:329`）。
   **正确做法**：写临时文件 + 原子替换（`MoveFileEx` / `ReplaceFile`），保留一份 `.bak`；加载失败必须置「只读/禁止保存」标志并明确告知用户。

---

## 附录 A. 关键文件与函数索引

| 主题 | 文件 | 关键符号 |
|---|---|---|
| 曲库单例 | `SongDataManager.h/.cpp` | `CSongDataManager`, `SongDataMap`, `SaveSongData`, `LoadSongData` |
| 曲目结构 | `SongInfo.h/.cpp` | `SongInfo`, `SongKey`, `GetArtistList`, `flags` 位标志 |
| 文件路径 | `MusicPlayer2.cpp:99-111` | `m_song_data_path = "song_data.dat"` |
| 扫描入口 | `MusicPlayer2.cpp:637` | `CMusicPlayerApp::StartUpdateMediaLib` |
| 扫描主体 | `MusicPlayerCmdHelper.cpp:597` | `CMusicPlayerCmdHelper::UpdateMediaLib` |
| 文件枚举 | `AudioCommon.cpp:156` | `CAudioCommon::GetAudioFiles` |
| 增量读标签 | `AudioCommon.cpp:526` | `CAudioCommon::GetAudioInfo` |
| cue 展开 | `AudioCommon.cpp:365` | `CAudioCommon::GetCueTracks` |
| 分类 | `MediaLibHelper.h/.cpp` | `CMediaClassifier::ClassifyMedia` |
| 内存 UI 列表 | `UiMediaLibItemMgr.h/.cpp` | `CUiMediaLibItemMgr`, `CUISongListMgr`, `CUiAllTracksMgr`, `CUiMyFavouriteItemMgr`, `CUiFolderExploreMgr` |
| 列表缓存 | `ListCache.h/.cpp` | `CListCache::reload`, `CRecentList::m_ver` |
| 搜索缓存 | `ListSearchCache.h/.cpp` | `CListSearchCache::reload` |
| 拼音 | `ChinesePingyinRes.h/.cpp` | `IsStringMatchWithPingyin` |
| 自绘列表 | `UIElement/AbstractListElement.cpp` | `DrawScrollArea`, `CalculateItemRects` |
| 滚动 | `UIElement/AbstractScrollArea.cpp` | `MouseWheel`, `RestrictScrollOffset` |
| 统计落盘 | `MusicPlayerDlg.cpp:2686`, `Player.cpp:2196` | `AddListenTime`, 48 秒定时保存 |
| Last.fm | `LastFM.h/.cpp`, `MusicPlayer2.cpp:803-842` | `lastfm.dat`，全线程隔离 |
| 多版本合并 | `SongMultiVersion.h/.cpp` | `MakeKey`, `MergeSongsMultiVersion` |

## 附录 B. 持久化文件清单

| 文件 | 相对路径 | 内容 | 格式 |
|---|---|---|---|
| `song_data.dat` | `<config_dir>\` | **曲库全部曲目元数据** | MFC `CArchive` 二进制，首字段版本 `"2.781"` |
| `config.ini` | `<config_dir>\` | 媒体库文件夹白名单、艺术家分隔符例外、显示设置等 | INI（`CIniHelper`） |
| `recent_list.dat` | `<config_dir>\` | 所有 `ListItem`（文件夹/播放列表/媒体库分类条目） | MFC `CArchive`（`std::list<ListItem>`） |
| `lastfm.dat` | `<config_dir>\` | Last.fm 会话与缓存曲目 | 自研 `LastFMDataArchive` |
| `user_ui.dat` | `<config_dir>\` | 用户自定义界面 | — |
| `playlist\*.playlist` | `<config_dir>\playlist\` | 播放列表（含「我喜欢的音乐」「默认」「临时」） | 自研（`CPlaylistFile`） |
| `global_cfg.ini` | `<exe_dir>\` | 便携模式开关 | INI |

`<config_dir>` = `<exe_dir>`（便携模式）或 `%APPDATA%\MusicPlayer2\`（默认）。

## 附录 C. 本次调查的取证与局限

- **取得**：119 个源文件（含 `MediaLibHelper`、`SongDataManager`、`SongInfo`、`UiMediaLibItemMgr`、`ListCache`、`ListSearchCache`、`AudioCommon`、`MusicPlayerCmdHelper`、`ChinesePingyinRes`、`AbstractListElement`、全部 `UIElement/MediaLib*` 与 `UIElement/*TracksList`、各媒体库对话框、`LastFM`、`PlayTime.h`、`Define.h`、`Common.cpp`），逐文件与 GitHub git tree 的 `size` 校验通过。（临时源码副本调查后已清理。）
- **未取**：`README`/`update_log.md`（仓库 `Documents/` 目录下的更新日志，父 agent 已有）。因此**本报告没有引用任何更新日志作为证据**，所有 V2.70/V2.76 相关说法都以源码现状为准。
- **未展开**：`TagLibHelper.cpp`（51KB，标签读写细节）、`AudioTag.cpp` 的分级读写实现、`CPlaylistFile` 的播放列表文件格式、`CUserUi`/`ElementFactory` 的皮肤加载细节。这些属于相邻主题，建议由其他专项报告覆盖。
- **未运行验证**：本报告全部结论来自静态阅读。§8.2、§8.4、§8.6、§8.11 中涉及「实际有多卡」「并发概率多大」的判断已明确标注为 `【推测】`，**建议用真实大曲库（3 万首以上）实测后再写进最终文档。**
