# MusicPlayer2 音频标签与专辑封面 —— 源码级调查报告

**调查对象**：`zhongyang219/MusicPlayer2`，master 分支，本地副本 `D:\BoTapMusic\`（`version.info` 显示 **2.78**，GPL-3.0）
**源码根**：`MusicPlayer2/`；TagLib 头文件在 `MusicPlayer2/taglib/`（**104 个 .h，仓库内不含 TagLib 的 .cpp 实现**，只有预编译 `tag.dll`）
**标注约定**：`【源码确证】` = 我在本仓库源码里逐行读到；`【历史确证】` = 从上游 commit/release 拿到的一手证据；`【推测】` = 我的判断，无直接代码证据。

---

## 0. 先给结论：这个项目的标签子系统长什么样

三层结构，职责划得很干净：

| 层 | 文件 | 职责 |
|---|---|---|
| 入口/分派 | `AudioTag.cpp/.h` (`CAudioTag`) | 按 `AudioType` 枚举分派到具体格式；cue/osu 特判 |
| 实现 | `TagLibHelper.cpp/.h` (`CTagLibHelper`) | 全部 TagLib 调用，逐格式手写（无统一抽象） |
| 遗留 | `AudioTagOld.cpp/.h` (`CAudioTagOld`) | 早期 BASS + 手写字节解析，**仍在服役**（opus/aac/未知格式走它） |

`AudioTag.h:3` 的 `AudioTagOld.h:3` 注释是原话：

```cpp
//主要通过BASS获取音频标签位置并手动解析标签内容，目前已经基本不再使用，现在获取音频标签使用taglib库，代码在CAudioTag类中。
```

「基本不再使用」是**不准确的自我描述** —— 见第 2 节。【源码确证】(AudioTagOld.h:3)

---

## 1. 读写哪些字段，怎么映射到各容器的标签

### 1.1 通用字段：走 TagLib 的抽象 Tag 接口

**读**：`TagToSongInfo(SongInfo&, Tag*, bool to_local)` —— `TagLibHelper.cpp:90`
**【源码确证】**

```cpp
static void TagToSongInfo(SongInfo& song_info, Tag* tag, bool to_local)
{
    song_info.title   = TagStringToWstring(tag->title(), to_local);
    song_info.artist  = TagStringToWstring(tag->artist(), to_local);
    song_info.album   = TagStringToWstring(tag->album(), to_local);
    song_info.genre   = TagStringToWstring(tag->genre(), to_local);
    int genre_num{};
    if (IsStringNumber(song_info.genre, genre_num))
        song_info.genre = CAudioCommon::GetGenre(static_cast<BYTE>(genre_num));  // 数字流派 → 标准流派名
    song_info.year    = tag->year();
    song_info.track   = tag->track();
    song_info.comment = TagStringToWstring(tag->comment(), to_local);
}
```

注意 `IsStringNumber` 会先剥掉包裹的括号再判断数字（`TagLibHelper.cpp:76`），所以 `"(17)"` 这种写法也能识别成流派号。

**写**：`SongInfoToTag(const SongInfo&, Tag*)` —— `TagLibHelper.cpp:62`，只写 title/artist/album/genre/track/comment/year 七个。【源码确证】

### 1.2 扩展字段：走 TagLib 的通用 `PropertyMap`

这是本题第 1 条问的「通用 PROPERTYMAP」，答案是**有，但只用了三个 key**。

**【源码确证】`TagLibHelper.cpp:362` `OtherPropertyToSongInfo`**

```cpp
static void OtherPropertyToSongInfo(SongInfo& song_info, const std::map<std::wstring, std::wstring>& property_map)
{
    song_info.album_artist = GetMapValue(L"ALBUMARTIST", property_map);

    std::wstring track_number = GetMapValue(L"TRACKNUMBER", property_map);
    size_t index = track_number.find(L'/');            // "3/12" 形式 → 解析总数
    if (index != std::wstring::npos)
        song_info.total_tracks = static_cast<BYTE>(_wtoi(track_number.substr(index + 1).c_str()));

    std::wstring disc_number = GetMapValue(L"DISCNUMBER", property_map);
    index = disc_number.find(L'/');
    if (index != std::wstring::npos)
        song_info.total_discs = static_cast<BYTE>(CCommon::StringToInt(disc_number.substr(index + 1)));
    song_info.disc_num = static_cast<BYTE>(CCommon::StringToInt(disc_number));
}
```

**【源码确证】`TagLibHelper.cpp:385` `WriteOtherProperties`** —— 写回：

```cpp
template<class T>
static void WriteOtherProperties(const SongInfo& song_info, T& file)
{
    TagLib::PropertyMap properties = file.properties();
    properties["ALBUMARTIST"].clear();      // ← V2.77 修的重复问题就在这两行
    properties["DISCNUMBER"].clear();
    if (!song_info.album_artist.empty())
        properties["ALBUMARTIST"].append(song_info.album_artist);
    if (song_info.disc_num != 0)
        properties["DISCNUMBER"].append(std::to_wstring(static_cast<int>(song_info.disc_num)));
    file.setProperties(properties);
}
```

> `PropertyMap` 是 TagLib 的通用抽象层：你写 `properties["ALBUMARTIST"]`，TagLib 自己翻译成 ID3v2 的 `TPE2`、Vorbis 的 `ALBUMARTIST`、MP4 的 `aART`、APE 的 `Album Artist`。所以 MP2 **从不手写任何帧名**（除下面列的少数几个），靠这一层跨格式。这是整个设计里最值得抄的一点。

**总映射表（字段 → 各容器）**

| 逻辑字段 | ID3v2 | Vorbis/Opus/Speex | MP4/M4A | APE | ASF/WMA | RIFF/WAV |
|---|---|---|---|---|---|---|
| 标题/艺术家/专辑/流派/音轨/年份/注释 | `TIT2`/`TPE1`/`TALB`/`TCON`/`TRCK`/`TDRC`/`COMM` | `TITLE`/`ARTIST`/`ALBUM`/`GENRE`/`TRACKNUMBER`/`DATE`/`COMMENT` | `©nam`/`©ART`/`©alb`/`©gen`/`trkn`/`©day`/`©cmt` | `Title` 等 | `Title` 等 | ID3v2 或 INFO chunk |
| 专辑艺术家 | `TPE2` | `ALBUMARTIST` | `aART` | `ALBUMARTIST` | `WM/AlbumArtist` | 同左 |
| 碟号 | `TPOS` | `DISCNUMBER` | `disk` | `DISCNUMBER` | — | 同左 |
| 内嵌封面 | `APIC` | `METADATA_BLOCK_PICTURE` | `covr` | `COVER ART (FRONT)` | `WM/Picture` | ID3v2 `APIC` |
| 内嵌歌词 | `USLT` | `LYRICS` | `----:com.apple.iTunes:Lyrics` | — | `LYRICS` | ID3v2 `USLT` |
| 评分 | `POPM` | `RATING` | — | — | `WM/SharedUserRating` | — |
| 内嵌 cue | — | — | — | `CUESHEET` | — | — |

> 上表「各容器」列中的**具体名字**由 TagLib 内部完成，不在这份源码里（仓库无 TagLib 实现）。MP2 源码里**显式写出来的只有下面这 7 个常量**。【源码确证】`TagLibHelper.cpp:34-46`

```cpp
#define STR_MP4_COVER_TAG   "covr"
#define STR_ASF_COVER_TAG   "WM/Picture"
#define STR_APE_COVER_TAG   "COVER ART (FRONT)"

#define STR_MP4_LYRICS_TAG  "----:com.apple.iTunes:Lyrics"
#define STR_ID3V2_LYRIC_TAG "USLT"
#define STR_FLAC_LYRIC_TAG  "LYRICS"
#define STR_ASF_LYRIC_TAG   "LYRICS"

#define STR_APE_CUE_TAG     "CUESHEET"

#define STR_ID3V2_RATEING_TAG "POPM"
#define STR_FLAC_RATING_TAG   "RATING"
//#define STR_WMA_RATING_TAG "RATING WMP"
```

### 1.3 评分（POPM / RATING）

**【源码确证】`TagLibHelper.cpp:399` `ParseAudioRating`** —— 注释写得非常清楚，是 Windows 资源管理器的 POPM 约定：

```cpp
//Windows Media Player 9 Series rating=196 counter=0
/*
  rating   |    分级
    255    |     5
    196    |     4
    128    |     3
     64    |     2
      1      |     1
*/
```

读：`GetId3v2Rating` 用 `dynamic_cast<ID3v2::PopularimeterFrame*>` 取 `rating()`，再过 `ParseAudioRating` 折成 1~5。
写：`WriteId3v2Rating` **先删掉所有已有 POPM 帧**再写新帧（避免累积）。`GenerateAudioRating` 是反函数。
FLAC 走 `properties["RATING"]` 存**原始字符串数字**（`_wtoi` 读、`std::to_wstring` 写），不做 255 星映射 —— 也就是说 FLAC 和 MP3 的评分语义**不一致**。【源码确证】`TagLibHelper.cpp:1676`/`1718`

`SongInfo.rating` 默认 **255 = 未分级**，1~5 才是真值。【源码确证】`SongInfo.h`

`IsFileRatingSupport` 只对 **MP3 / FLAC / WMA-ASF** 返回 true，其他格式评分只存在媒体库（`song_data.dat`）里，不落文件。【源码确证】`AudioTag.cpp:480`

### 1.4 ReplayGain：**没有**

我在 `TagLibHelper.*`、`AudioTag.*`、`SongInfo.*` 里 grep `replaygain`/`REPLAYGAIN`，**零命中**。这个项目**不读写 ReplayGain**（响度归一化走的是播放内核的音量/增益）。`PropertyMap` 会把 ReplayGain 标签透传到「高级标签信息」页只读展示，但 MP2 不解析、不写回。【源码确证 + 注解】

### 1.5 「高级标签信息」页

**【源码确证】`PropertyAdvancedDlg.cpp:81` `ShowInfo`**

```cpp
void CPropertyAdvancedDlg::ShowInfo()
{
    EnableWindow(!m_batch_edit);
    m_list_ctrl.DeleteAllItems();
    if (!m_batch_edit)      //批量编辑（多选）模式下不支持显示高级标签信息
    {
        SongInfo cur_song = CurrentSong();
        CAudioTag audio_tag(cur_song);
        std::map<wstring, wstring> property_map;
        audio_tag.GetAudioTagPropertyMap(property_map);
        int index{};
        for (const auto& prop : property_map)
        {
            m_list_ctrl.InsertItem(index, prop.first.c_str());
            m_list_ctrl.SetItemText(index, 1, prop.second.c_str());
            index++;
        }
    }
}
```

这张表就是 `PropertyMap` 的原样 dump —— 键名直接是 TagLib 的规范键。**只读，没有编辑能力**，批量模式下整页禁用。

`GetTagPropertyMap`（`TagLibHelper.cpp:334`）有个细节值得注意：合并多个 tag 时**先到先得，但空值可被后到的覆盖**——

```cpp
auto iter = property_map.find(key);
if (iter == property_map.end())
    property_map[key] = value;
else if (iter->second.empty())
    iter->second = value;
```

多个值用 `;` 连接：`prop.second.toString(L";")`。

---

## 2. 格式覆盖：谁能读、谁能写

### 2.1 分派表

**【源码确证】`AudioTag.cpp:43` `GetAudioTag()` + `AudioType` 枚举（`AudioCommon.h:7`）**

| 格式 | 扩展名 | 读标签 | 写标签 | 读封面 | 写封面 | 读歌词 | 写歌词 | 评分 |
|---|---|---|---|---|---|---|---|---|
| MP3 | mp3/mp2/mp1 | TagLib | ✅ | TagLib | ✅ | ✅ | ✅ | ✅ |
| WMA/ASF | wma/asf | TagLib | ✅ | TagLib | ✅ | ✅ | ✅ | ✅ |
| OGG | ogg/oga | TagLib | ✅ | TagLib | ✅ | — | — | — |
| M4A/MP4 | m4a/mp4 | TagLib | ✅ | TagLib | ✅ | ✅ | ✅ | — |
| FLAC | flac/fla | TagLib | ✅ | TagLib | ✅ | ✅ | ✅ | ✅ |
| WAV | wav | TagLib | ✅ | TagLib | ✅ | ✅ | ✅ | — |
| AIFF | aif/aiff | TagLib | ✅ | TagLib | ✅ | — | — | — |
| APE | ape/mac | TagLib | ✅ | TagLib | ✅ | — | — | — |
| MPC | mpc/mp+/mpp | TagLib | ✅ | TagLib | ✅ | — | — | — |
| WV | wv | TagLib | ✅ | TagLib | ✅ | — | — | — |
| TTA | tta | TagLib | ✅ | TagLib | ✅ | — | — | — |
| SPX | spx | TagLib | ✅ | TagLib | ✅ | — | — | — |
| **OPUS** | opus | **AudioTagOld**（见下） | **❌ 实际写不进去（bug）** | **❌ 不读内嵌** | **❌** | — | — | — |
| **AAC** | aac | **AudioTagOld** | ❌ | AudioTagOld | ❌ | — | — | — |
| DSD | dff/dsf | ❌ | ❌ | ❌ | ❌ | — | — | — |
| MIDI | mid/midi/rmi/kar | ❌ | ❌ | ❌ | ❌ | — | — | — |
| CUE | cue | 自研 `CCueFile` | ✅ | — | — | — | — | — |

注意 **TAK 不在枚举里**。`AudioCommon.cpp:55 GetAudioTypeByFileExtension` 没有 `tak`，所以 TAK 文件会被判成 `AU_OTHER`，走 `AudioTagOld::GetTagDefault()`。V2.72 发布说明里的「新增 tak 格式音频的支持（仅32位版本）」是**播放**支持（BASS 插件），不是标签支持。【源码确证 + 历史确证】

### 2.2 OPUS：一个真 bug（switch 贯穿）

**【源码确证】`AudioTag.cpp:349-356`**

```cpp
case AU_DSD:
    break;
case AU_OPUS:
    //return CTagLibHelper::WriteOpusTag(m_song_info);
case AU_WV:                                              // ← 没有 break，直接贯穿！
    return CTagLibHelper::WriteWavPackTag(m_song_info);
```

`AudioTag.cpp:390-395` 的 `WriteAlbumCover` 完全一样：

```cpp
case AU_DSD:
    break;
case AU_OPUS:
    //return CTagLibHelper::WriteOpusAlbumCover(m_song_info.file_path, album_cover_path);
case AU_WV:
    return CTagLibHelper::WriteWavePackAlbumCover(m_song_info.file_path, album_cover_path);;
```

后果：对 `.opus` 文件写标签，实际调用的是 `WavPack::File` 打开它。`WavPack::File` 对 Ogg 容器会 `isValid()==false`，`WriteWavPackTag` 仍然会 `file.save()` 并返回结果。

**这里必须严谨**：我**没有**观察到 commit、issue 或注释说明 Opus 文件被写坏。`WriteWavePackAlbumCover` 里有 `if (!file.isValid()) return false;` 的守卫；`WriteWavPackTag` 则**没有**这个守卫。【源码确证】所以风险是实在的，但是否真会损坏文件，取决于 TagLib 对无效文件的 `save()` 行为 —— 我**无法从本仓库源码判定**（TagLib 实现不在仓库里）。**【推测】这是应该被修的真 bug，最坏情况可能损坏 Opus 文件。**

`IsFileTypeTagWriteSupport`/`IsFileTypeCoverWriteSupport` 里 OPUS 被显式注释掉（`AudioTag.cpp:458/468`），所以 UI 层面 Opus 的编辑框是灰的 —— 这大概是这个 bug 一直没爆的原因。**但如果以后有人把 OPUS 加回白名单，就会立刻踩到。**

### 2.3 OPUS / AAC 读标签回退到旧实现

**【源码确证】`AudioTag.cpp:91-97`**

```cpp
case AU_OPUS:
    //CTagLibHelper::GetOpusTagInfo(m_song_info);
{
    CAudioTagOld audio_tag_old(m_hStream, m_song_info, m_type);
    succeed = audio_tag_old.GetOggTag();
}
break;
```

尽管 `CTagLibHelper` 里**写好了** `GetOpusTagInfo`/`GetOpusAlbumCover`/`GetOpusPropertyMap`，全部被注释掉不用。OPUS 走 BASS 的 `BASS_TAG_OGG` 手写解析。`AudioTagOld::GetOggTag` 不在 `AudioTagOld.h` 的 public 列表里... 实际上在（`GetOggTag()` 声明存在）。**这是一个「新实现写好了但没启用」的残留状态。**【源码确证】

同理 AAC（`AudioTag.cpp:110`）走 `CAudioTagOld::GetTagDefault()`。

> V2.77.1 发布说明提到「修正 aac 文件在属性对话框中无法显示内嵌专辑封面的问题」—— 对应 `AudioTagOld::GetAlbumCoverDefault` / `FindID3V2AlbumCover` 路径的修补。【历史确证 + 推测】

### 2.4 不支持写的格式怎么处理

UII 层面就挡掉了：

**【源码确证】`PropertyDlgHelper.cpp:233`**

```cpp
bool CPropertyDlgHelper::IsSongTagWriteEnable(const SongInfo& song)
{
    return song.is_cue || (!COSUPlayerHelper::IsOsuFile(song.file_path) && !CCommon::IsURL(song.file_path)
        && CAudioTag::IsFileTypeTagWriteSupport(CFilePathHelper(song.file_path).GetFileExtension()));
}
```

`IsSongTagWriteEnable` 返回 false → `CPropertyTabDlg::SetWreteEnable`（`PropertyTabDlg.cpp:187`）把所有编辑框设为只读，并禁用三个「获取标签」按钮。批量模式下用 `IsMultiWritable()`：**只要有一首不可写就整体只读**。【源码确证】

cue 是特例：`song.is_cue` 直接算「可写」，因为 tag 写到 `.cue` 文本文件里（`CAudioTag::WriteCueTag`）。但 `IsSongAlbumCoverWriteEnable` 明确排除 cue（`!song.is_cue`）—— 封面页对 cue 只读。【源码确证】

---

## 3. 编码处理（深挖）

这一节是整个调查里最有价值的部分。

### 3.1 ID3v1 优先级：曾经有配置项，**已经被删除**

**【历史确证】commit `0eac4d42`（2020-08-29，V2.70 与 V2.71 之间）**
标题：`"优先获取ID3V2标签"的选项已经不起作用，将它去掉`

旧代码（被删掉的部分）：

```cpp
bool CAudioTagOld::GetTagDefault(bool id3v2_first)
{
    bool tag_exist{ false };
    if (id3v2_first)
    {
        tag_exist = GetID3V2Tag();
        if (!tag_exist) tag_exist = GetApeTag();
        if (!tag_exist) tag_exist = GetID3V1Tag();
    }
    else { /* APE → ID3v2 → ID3v1 */ }
```

替换成无条件固定顺序：**ID3v2 → APE → ID3v1**。

**【源码确证，重要】** 配置键 `id3v2_first` **至今仍是死键**。全仓库只有三处出现：

- `CommonData.h:289` `bool id3v2_first{ false };  //优先获取ID3V2标签`
- `MusicPlayerDlg.cpp:493` `ini.WriteBool(L"general", L"id3v2_first", ...)`（写）
- `MusicPlayerDlg.cpp:701` `... = ini.GetBool(L"general", L"id3v2_first", 1);`（读，默认 1）

**没有任何一行代码读取它做判断**。而且默认值在 `CommonData.h` 里是 `false`，在 `GetBool` 里是 `1` —— 自相矛盾，进一步说明它是遗留物。新架构下优先级由 **TagLib 的 `File::tag()` 决定**，MP2 不介入。

### 3.2 编码转换的机制（核心）

**【源码确证】`TagLibHelper.cpp:49-60`** —— 这是整个容错方案的地基，注释是作者自己写的：

```cpp
//将taglib中的字符串转换成std::wstring类型。
//由于taglib将所有非unicode编码全部作为Latin编码处理，因此无法正确处理本地代码页
//这里将Latin编码的字符串按本地代码页处理
static std::wstring TagStringToWstring(const String& str, bool to_local)
{
    std::wstring result;
    if (to_local && str.isLatin1())
        result = CCommon::StrToUnicode(str.to8Bit(), CodeType::ANSI);
    else
        result = str.toWString();
    return result;
}
```

**它为什么能工作？** 三步：

1. 一个 ID3v2 文本帧的 encoding byte = 0（ISO-8859-1）时，TagLib 把原始字节**按 Latin-1 逐字节**映射成 UTF-16 码元。所以 GBK 写的 `"周杰伦"`（字节 `D6 DC BD DC C2 D7`）变成 6 个码元 `U+00D6 U+00DC U+00BD U+00DC U+00C2 U+00D7`。
2. `String::isLatin1()` 对「所有码元 ≤ 0xFF」返回 true —— 正好命中这种情况。
3. `String::to8Bit()`（**TagLib 头文件原话**：*"The returned string is encoded in UTF8 if `unicode` is true, otherwise Latin1"*）把这 6 个码元原样还原成 6 个字节 `D6 DC BD DC C2 D7`。
4. `CCommon::StrToUnicode(..., CodeType::ANSI)` → `MultiByteToWideChar(CP_ACP, ...)` 用系统 ANSI 代码页解码 → 简体中文 Windows 上就是 GBK → 正确得到 `"周杰伦"`。

**【源码确证 + 上游文档确证】** `TagLib::String` 的 `isLatin1()` 语义由 `MusicPlayer2/taglib/tstring.h:381-383` 确认（*"Returns true if the file only uses characters required by Latin1."*）；`to8Bit()` 的 Latin1 语义由同文件 `:190-196` 确认。

**这是「字节往返」技巧**：把「TagLib 解码成 Latin-1」的中间结果重新当字节流用。非常巧，也非常脆。

### 3.3 谁传 `to_local = true`：一张不对齐的表

**【源码确证】** 逐个 `Get*TagInfo` 函数里的实参：

| 函数 | `to_local` |
|---|---|
| `GetFlacTagInfo` | `true` |
| `GetMpegTagInfo` | `true` |
| `GetApeTagInfo` | `true` |
| `GetMpcTagInfo` | `true` |
| `GetWavPackTagInfo` | `true` |
| `GetTtaTagInfo` | `true` |
| `GetM4aTagInfo` | `false` |
| `GetAsfTagInfo` | `false` |
| `GetWavTagInfo` | `false` |
| `GetOggTagInfo` | `false` |
| `GetOpusTagInfo` | `false` |
| `GetSpxTagInfo` | `false` |
| `GetAiffTagInfo` | `false` |
| `GetAnyFileTagInfo` | `false` |

**语义理由（合理）**：只有基于 **ID3v1/ID3v2(Latin1)/APE** 的容器才可能出现「非 Unicode 按 Latin-1 存」的情况。Vorbis comment 强制 UTF-8、MP4 强制 UTF-8、ASF 强制 UTF-16、AIFF/WAV 在现代实践中也用 ID3v2 UTF-16。所以 `false`。

**但这个表在演进中反复横跳过，说明它不好把握。【历史确证】**

- commit `ea6fbf76`（2020-09-03，标题：*还原之前关于编码问题对taglib库的修改*）：引入 `to_local` 参数，实参传的是 **`file.hasID3v1Tag()`** —— 即「文件里有 ID3v1 才按本地代码页解」。
- commit `70540c1a`（2020-09-05，标题：*解决taglib的编码问题*）：把 `file.hasID3v1Tag()` **全部改成 `true`**。

**为什么改？**（此段为我的分析，但证据链很强）`hasID3v1Tag()` 判的是「有没有 ID3v1 标签」，而乱码源在「ID3v2 文本帧的 encoding byte 是 0」。这两个条件**完全无关**：一个纯 ID3v2、没有 ID3v1 的 MP3 写的 GBK 标签，`hasID3v1Tag()` 返回 false → 不做本地代码页转换 → 乱码。所以第二天就全部改成无条件 `true` 了。**【推测，但时间线和 diff 高度吻合】**

### 3.4 「检测乱码并回退」——**没有**

**没有**任何「先按 UTF-16 解，发现是乱码再按 GBK 重解」的逻辑。机制是**无条件的启发式**：只要 `isLatin1()` 为真就按 ANSI 重解。

**代价（这个坑很实在）**：一首**真正的 Latin-1 文本**会被误伤。比如 ID3v2 里写 `"Motörhead"`（`ö` = `U+00F6`，属于 Latin-1），`isLatin1()` 返回 true → `to8Bit()` 得到 `4D 6F 74 F6 72 68 65 61 64` → `MultiByteToWideChar(CP_ACP)` 在简体中文代码页下把 `F6 72` 当双字节 GBK 解 → **乱码**。同理 `"Björk"`、法语 `"é"` 之后跟的字节。

> 作者的取舍很清楚：中文用户遇到网易云/千千静听写的 GBK 标签是**高频**问题，遇到西欧重音字母是**低频**问题。用「一律当本地代码页」换「中文不再乱码」，是一笔划算的买卖。**但如果你的目标是国际用户，这条策略会反过来伤你。**

`CCommon::StrToUnicode` 本身的实现（`Common.cpp:532`）是标准 `MultiByteToWideChar`：

```cpp
// 当使用ANSI，UTF8，UTF8_NO_BOM，UTF16LE，UTF16BE时不检测直接转换
// 使用默认值CodeType::AUTO时先检测BOM，如果没有BOM则按ANSI转换
// 将auto_utf8置true使AUTO检测没有BOM的字串是否为UTF8序列，如果符合则按UTF8_NO_BOM转换
// auto_utf8有可能将过短的ANSI误认为是UTF8导致乱码     ← 作者自己标了风险
...
if (!result_ready)
{
    size = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, NULL, 0);
    ...
}
```

**没有用 `CA2W`/ATL 转换宏**，全是裸 Win32 API + 手写 `new wchar_t[]` / `delete[]`。`CodeType` 枚举在 `Common.h:36`：`ANSI / UTF8 / UTF8_NO_BOM / UTF16LE / UTF16BE / AUTO`。

`UnicodeToStr` 反向，并且**会报告不可转换字符**：

```cpp
BOOL UsedDefaultChar{ FALSE };
...
WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, str, size, NULL, &UsedDefaultChar);
...
if (char_cannot_convert != nullptr)
    *char_cannot_convert = (UsedDefaultChar != FALSE);
```

**`UsedDefaultChar` 就是「Unicode 变成问号」的探测器** —— V2.73 修的那个问题（转 MP3 后 Unicode 字符变问号）正是这个 API 的经典症状。`AudioTagOld::WriteMp3Tag` 里明确在用 ANSI 转 ID3v1：【源码确证】`AudioTagOld.cpp:66`

```cpp
title = CCommon::UnicodeToStr(song_info.title, CodeType::ANSI);
```

而 V2.73 起的 `FormatConvertDlg` 已经改成走 TagLib 写标签（`FormatConvertDlg.cpp:546` `CAudioTag audio_tag_out(song_info_out); audio_tag_out.WriteAudioTag();`），TagLib 写 ID3v2 时用 UTF-16，不再经过 ANSI。【历史确证 + 源码确证】这解释了「V2.73 修正转换格式中转换成 mp3 格式后，写入的标签信息中一些 Unicode 字符会变成问号的问题」。

**V2.71 的「优化标签获取，解决乱码」对应的就是 `4ffaed44` 那次「更新taglib库，将非unicode文本转换为unicode时使用本地代码页」。** 那次 commit 的 diff 很有意思：【历史确证】

```diff
-//由于taglib将所有非unicode编码全部作为Latin编码处理，因此无法正确处理本地代码页
-//这里将Latin编码的字符串按本地代码页处理
-static wstring TagStringToWstring(const String& str)
-{
-    if (str.isLatin1())
-        result = CCommon::StrToUnicode(str.to8Bit(), CodeType::ANSI);
-    ...
-}
```
→ 全部改成裸 `tag->title().toWString()`，**把转换责任推进 TagLib DLL 内部**（同时替换了 4 个 `tag.dll` 二进制）。然后第二天 commit `ea6fbf76` 又把 `TagStringToWstring` **加回来**了。所以最后落地的方案是**双保险**：DLL 内部改 + 应用层再兜一层。

### 3.5 内嵌封面的 MIME 与 BMP

**【源码确证】`TagLibHelper.cpp:130` `GetPicType`**

```cpp
int GetPicType(const std::wstring& mimeType)
{
    int type{ -1 };
    if (mimeType == L"image/jpeg" || mimeType == L"image/jpg") type = 0;
    else if (mimeType == L"image/png") type = 1;
    else if (mimeType == L"image/gif") type = 2;
    else if (mimeType == L"image/bmp") type = 3;
    else type = -1;
    return type;
}
```

**V2.70「无法获取 bmp 格式专辑封面」的真实修复位置**：commit `2b8c01c2`（2020-08-14）。**注意它改的是旧实现 `CAudioTagOld::FindID3V2AlbumCover`，不是 TagLib 路径。**【历史确证】

```diff
 const string gif_tail{ '\x80', '\x00', '\x00', '\x3b' };
+const string bmp_head{ "BM" };
...
-    string apic_tag_content = tag_content.substr(cover_index + 8, cover_size);
+    string apic_tag_content = tag_content.substr(cover_index + 8, cover_size + 2);
     ...
+            else
+            {
+                image_index = apic_tag_content.find(bmp_head);
+                if (image_index < cover_index + 100)
+                    image_type = 3;
+            }
```

两个改动：① 加 `"BM"` 魔数探测；② **把 APIC 数据区长度 +2**（原来看漏了 2 字节）。

**这种「按魔数嗅探格式」的做法是必要的**：APIC 帧的 MIME 字段经常是错的或空的（尤其 ID3v2.2 的 `PIC` 帧、或写入方偷懒），只能看图片头。`AudioTagOld.cpp:6-12` 定义了完整的魔数表：

```cpp
const string jpg_head{ '\xff', '\xd8', '\xff' };
const string png_head{ '\x89', '\x50', '\x4e', '\x47' };
const string gif_head{ "GIF89a" };
const string bmp_head{ "BM" };
```

**TagLib 路径的隐患（新发现）**：`WriteId3v2AlbumCover`（`TagLibHelper.cpp:179`）用**文件扩展名**拼 MIME：

```cpp
std::wstring ext = CFilePathHelper(album_cover_path).GetFileExtension();
pic_frame->setMimeType(L"image/" + ext);
```

`CFilePathHelper::GetFileExtension(upper=false)` 默认小写（`FilePathHelper.cpp:15-25`，内部 `CCommon::StringTransform(file_extension, upper)`），所以读回来 `GetPicType("image/jpg")` 能命中。**这条链是通的。**【源码确证】

但如果用户的图片扩展名是 `.jfif`/`.webp`/`.tiff`，`GetPicType` 返回 -1，UI 上封面类型就显示不出来（`PropertyAlbumCoverDlg.cpp:156-172` 的 `switch(cover_type)` 只认 0/1/2/3）。**封面本身仍能显示**，因为落盘的文件没有扩展名，由 GDI+ 自己嗅探格式。【推测，但 `PropertyAlbumCoverDlg.cpp:171-173` 的 `default: break;`（什么都不填）支持这个判断】

### 3.6 文件路径：长路径 / Unicode / 网络路径

**Unicode 路径：没问题。** 全链路 `std::wstring` 传递，`file_path.c_str()` 直接给 TagLib 的宽字符构造函数。**【源码确证】** `TagLibHelper.cpp` 里全是 `FLAC::File file(file_path.c_str())` 这种写法。

**长路径 > MAX_PATH：没有处理，是真缺口。**【源码确证】
- `CFilePathHelper`（`FilePathHelper.h`）全部是朴素 `wstring` 操作，**没有 `\\?\` 前缀剥离**。
- 全仓库 grep `\\\\?\\` 零命中（除了 `RelativePathToAbsolutePath` 里的注释）。
- `Common.cpp:498 CheckFilePathLength` 只在**重命名/转换格式**时把文件名截短以避免超长；`CheckAndFixFile` 也只是修正文件名大小写。
- `Common.cpp:504` 的注释很说明态度：
  ```cpp
  //最大的文件名长度（比MAX_PATH少12个字符，留出一些余裕）
  int file_name_max_length = MAX_PATH - 12 - static_cast<int>(file_dir.size()) - ...
  ```
  这是**主动规避 MAX_PATH**，而不是突破它。

结论：**放不进 260 字符以内的音乐文件，MP2 读不到标签也写不了标签。** 想支持长路径，需要自己给 `wstring` 加 `\\?\` 前缀层的抽象 —— MP2 没有这层。

**网络路径/UNC：**
- `CCommon::IsURL`（`Common.cpp:748`）识别 `http://` `https://` `ftp://` `mms://`。URL 会被 `IsSongTagWriteEnable`/`IsSongAlbumCoverWriteEnable` 排除，即**URL 不写标签**。
- UNC 路径（`\\server\share\...`）走 `IsWindowsPath`/`IsPath`（`Common.cpp:755/764`）。`CFilePathHelper::GetFileName` 只按 `\\` 和 `/` 切，UNC 能正常解析出文件名。【源码确证】但**没有专门的网络路径容错**（超时、断线、慢速盘）——TagLib 的同步文件 IO 直接阻塞在 UI 线程上。

---

## 4. 封面的来源、缓存与显示

### 4.1 来源优先级

**【源码确证】`Player.cpp:2438` `CPlayer::SearchAlbumCover`**

```cpp
void CPlayer::SearchAlbumCover()
{
    CSingleLock sync(&m_album_cover_sync, TRUE);
    m_album_cover.Destroy();
    SongInfo song_info{ CSongDataManager::GetInstance().GetSongInfo3(GetCurrentSongInfo()) };
    bool always_use_external_album_cover{ song_info.AlwaysUseExternalAlbumCover() };
    if ((!theApp.m_app_setting_data.use_out_image || theApp.m_app_setting_data.use_inner_image_first)
        && !IsOsuFile() && !always_use_external_album_cover)
    {
        //从文件获取专辑封面
        CAudioTag audio_tag(GetCurrentSongInfo2(), GetBassHandle());
        m_album_cover_path = audio_tag.GetAlbumCover(m_album_cover_type);
        if (!m_album_cover_path.empty())
        {
            m_album_cover.Load(m_album_cover_path.c_str());
            AlbumCoverResize();
            MediaTransControlsLoadThumbnail();
        }
    }
    m_inner_cover = !m_album_cover.IsNull();

    if (/*theApp.m_app_setting_data.use_out_image && */m_album_cover.IsNull())
    {
        //获取不到专辑封面时尝试使用外部图片作为封面
        SearchOutAlbumCover();
    }
    SendMessage(theApp.m_pMainWnd->GetSafeHwnd(), WM_CURRENT_FILE_ALBUM_COVER_CHANGED, 0, 0);
}
```

**实际优先级（默认配置下）**：

1. **内嵌**（APIC / covr / METADATA_BLOCK_PICTURE / COVER ART / WM/Picture）
   —— 默认 `use_inner_image_first = true`（`CommonData.h:245`）
   —— 三种情况跳过：`use_out_image == false`（整个不用外部图）、osu! 文件、单曲设了 `AlwaysUseExternalAlbumCover()`
2. **外部图片文件**（仅当内嵌拿不到时）
3. **默认图**（`default_cover_img_data` / `default_cover_not_played_img_data`，`CommonData.h:461-464`，编译期内嵌的 PNG 资源）

### 4.2 外部图片查找规则（8 级）

**【源码确证】`MusicPlayerCmdHelper.cpp:466` `SearchAlbumCover`**

按顺序：

1. **osu! 特判** → `COSUPlayerHelper::GetAlbumCover`，再退到 `default_osu_img`
2. **与歌曲同名的图片**：`song.mp3` → `song.*`（`GetImageFiles` 过滤出图片扩展名）
3. **与「唱片集」同名的图片**：`{album}.*`（先 `FileNameNormalize` 洗掉非法字符）
4. **默认封面文件名列表** `theApp.m_app_setting_data.default_album_name`（用户可配，如 `cover`、`folder`、`front`），逐个在本目录找 `{name}.*`；**列表项若是绝对路径且存在，记为 `absolute_dir` 兜底**
5. **封面文件夹** `AbsoluteAlbumCoverPath()` 里找与歌曲同名的图
6. 同上，找与唱片集同名的图
7. **URL 播放时**，用「标题」当文件名在封面文件夹里找
8. 最后兜底 `absolute_dir`

很务实的一点：**同名图片优先于唱片集名图片**，因为合辑/单曲场景下同名图更精确。

### 4.3 内嵌封面如何"变"成文件

**【源码确证】`AudioTag.cpp:199` `CAudioTag::GetAlbumCover`**

```cpp
if (file_size != nullptr) *file_size = image_contents.size();

//将专辑封面保存到临时目录
wstring file_path{ CCommon::GetTemplatePath() };
wstring _file_name;
if (file_name == nullptr) _file_name = ALBUM_COVER_NAME;
else                      _file_name = file_name;
if (!image_contents.empty())
{
    file_path += _file_name;
    ofstream out_put{ file_path, std::ios::binary };
    out_put << image_contents;
    return file_path;
}
else { image_type = -1; return wstring(); }
```

**关键点：内存里的封面字节 → 写到一个没有扩展名的临时文件 → 再 `CImage::Load`。** 每次切歌都要落盘一次。

`Define.h:132-135` 的临时文件名故意加了随机后缀：

```cpp
#define ALBUM_COVER_NAME                    L"CurrentAlbumCover-MusicPlayer2-jMZB7TMf"
#define ALBUM_COVER_TEMP_NAME               L"TempAlbumCover-MusicPlayer2-nKWfQeJo"
#define ALBUM_COVER_TEMP_NAME2              L"TempAlbumCover-MusicPlayer2-ogNdd65B"
#define ALBUM_COVER_TEMP_NAME_FOR_PROPERTIES L"TempAlbumCover-MusicPlayer2-6vQ0kGpV"
```

**【推测】** 这个随机后缀是防别的程序扫 `%TEMP%` 时把封面当垃圾清掉、或防不同实例互相覆盖。

### 4.4 大图缩放：V2.70「专辑封面尺寸过大则缩小」的具体实现

**【源码确证】`Player.cpp:2513` `CPlayer::AlbumCoverResize`**

```cpp
void CPlayer::AlbumCoverResize()
{
    m_album_cover_info.GetInfo(m_album_cover);
    m_album_cover_info.size_exceed = false;
    if (!m_album_cover.IsNull() && theApp.m_nc_setting_data.max_album_cover_size > 0)
    {
        CSize image_size;
        image_size.cx = m_album_cover.GetWidth();
        image_size.cy = m_album_cover.GetHeight();
        if (max(image_size.cx, image_size.cy) > theApp.m_nc_setting_data.max_album_cover_size)  //超过最大值则缩小
        {
            wstring temp_img_path{ CCommon::GetTemplatePath() + ALBUM_COVER_TEMP_NAME };
            //缩小图片大小并保存到临时目录
            CDrawCommon::ImageResize(m_album_cover, temp_img_path, theApp.m_nc_setting_data.max_album_cover_size, IT_PNG);
            m_album_cover.Destroy();
            m_album_cover.Load(temp_img_path.c_str());
            m_album_cover_info.size_exceed = true;
        }
    }
}
```

阈值：`max_album_cover_size` 默认 **800**（`CommonData.h:416`），ini 键 `max_album_cover_size`（`MusicPlayerDlg.cpp:477/685`）。**长边超过 800 就等比缩到 800**，中间经过一次 PNG 落盘再 Load。

**【历史确证】** 这个函数的来历是 commit `f4f67157`（2020-08-08，标题：*如果专辑封面过大则将其缩小，解决专辑封面过大时界面卡顿的问题*）。同一次 commit 还：
- 删掉了旧的 `CDrawCommon::BitmapStretch(CImage*, CImage*, CSize)`（用 GDI `StretchBlt`），换成 GDI+ 的 `ImageResize`；
- 新增 `Define.h` 里的临时文件名和 `CommonData.h` 里的 `max_album_cover_size`。

**有意思的是它留了一整套死代码**：【源码确证】`Player.cpp:2466-2481` 里 `AlbumCoverGaussBlur()` 调用被注释掉，后面还跟着一段被注释掉的旧缩放逻辑（用 `CCommon::SizeZoom` + `BitmapStretch`，注释写 *"如果专辑封面过大，则将其缩小，以提高性能"*）。这段历史层层叠叠，能看出性能问题被反复折腾过。

缩放实现本身（`DrawCommon.cpp:681`）：

```cpp
void CDrawCommon::ImageResize(const CImage& img_src, CImage& img_dest, CSize size)
{
    img_dest.Destroy();
    int bpp = img_src.GetBPP();
    if (bpp < 24) bpp = 24;       //总是将目标图片转换成24位图
    img_dest.Create(size.cx, size.cy, bpp);
    //使用GDI+更改图片大小时，左侧和上面会有一像素的灰边，因此在这里将其裁剪掉
    size.cx++;
    size.cy++;
    //使用GDI+高质量绘图
    img_src.Draw(img_dest.GetDC(), CRect(CPoint(-1, -1), size), Gdiplus::InterpolationMode::InterpolationModeHighQuality);
    img_dest.ReleaseDC();
}
```

**「GDI+ 缩放会留一像素灰边，所以画在 (-1,-1) 并各加 1 像素」** —— 这是纯经验性 workaround，注释写得明明白白。用 `InterpolationModeHighQuality`（不是 `HighQualityBicubic`，所以没有做预乘 alpha 处理）。

### 4.5 缓存策略：**内存 + 临时文件，没有缩略图缓存**

**【源码确证】**
- 内存缓存只有一份：`CPlayer::m_album_cover`（`ATL::CImage`），受 `CSingleLock m_album_cover_sync` 保护（`Player.cpp:1700-1718`）。
- 高斯模糊版则是另一份常驻内存：`m_album_cover_blur`（`Player.cpp:1706 GetAlbumCoverBlur()`）。
- **没有磁盘缩略图缓存**。全仓库 grep `thumbnail` 只命中任务栏缩略图（`ITaskbarList3::ThumbNailClip`）和 Windows SMTC 的 `loadThumbnail`，**与专辑封面缓存无关**。
- 也没有"按专辑缓存封面"的结构 —— 每次切歌重新从音频文件读一遍内嵌封面字节，重新落盘临时文件，重新 Load，重新可能缩放到 800。

**【推测】** 这是本项目在标签/封面子系统里最容易改进的一点：列表里滚动浏览千张专辑时，每张都要解一次内嵌 APIC 并落盘。

### 4.6 高斯模糊背景

**【源码确证】`Player.cpp:2486` `CPlayer::AlbumCoverGaussBlur`**

```cpp
void CPlayer::AlbumCoverGaussBlur()
{
    if (!theApp.m_app_setting_data.background_gauss_blur || !theApp.m_app_setting_data.enable_background)
        return;
    CSingleLock sync(&m_album_cover_sync, TRUE);
    if (m_album_cover.IsNull()) { m_album_cover_blur.Destroy(); }
    else
    {
        CImage image_tmp;
        CSize image_size(m_album_cover.GetWidth(), m_album_cover.GetHeight());
        //将图片缩小以减小高斯模糊的计算量
        CCommon::SizeZoom(image_size, 300);     //图片大小按比例缩放，使长边等于300
        CDrawCommon::ImageResize(m_album_cover, image_tmp, image_size);     //拉伸图片
        //执行高斯模糊
        CGaussBlur gauss_blur;
        gauss_blur.SetSigma(static_cast<double>(theApp.m_app_setting_data.gauss_blur_radius) / 10);
        gauss_blur.DoGaussBlur(image_tmp, m_album_cover_blur);
    }
}
```

两个性能决策值得学：
1. **先把图缩到长边 300 再做模糊**（`SizeZoom(image_size, 300)`），因为朴素高斯是 O(w·h·r)，缩图把计算量砍掉一个数量级。
2. 半径配置 `gauss_blur_radius` 默认 60，**除以 10 才是 sigma**（`CommonData.h:250-251`），即 sigma=6 → 模板半径 `r = int(sigma*3+0.5) = 18`。

`CGaussBlur`（`GaussBlur.cpp`）是**手写的可分离卷积**，不是 GDI+ 的 `Blur`：
- `SetSigma` 建一维高斯模板：`m_pTempl[i] = exp(-0.5 * i² / sigma²)`，归一化，`m_r = sigma*3+0.5`。
- `Filter` 做**水平一遍 + 垂直一遍**（`bHorz` 标志切换），中间结果存 `pTemp`。
- 支持 8/24/32 bpp，**其他 bpp 直接返回 false**（`if (bpp != 24 && bpp != 8 && bpp != 32) return false;`）—— 这就是为什么 `ImageResize` 要强制 `bpp = 24`。
- 边界处理取**边缘像素扩散**（Photoshop 方法1），代码里还留着方法2的注释。
- `GaussBlurThreadProc8/24` 名字带 Thread 但**实际是同步调用**（`Filter` 里直接调），没有真正开线程。

### 4.7 圆角

**【源码确证】`DrawCommon.cpp:390` `CDrawCommon::DrawRoundImage`**

```cpp
    //先将图像转换为目标大小，避免使用纹理画刷缩放时质量下降
    CImage image_dest;
    ImageResize(image, image_dest, size);
    Gdiplus::Bitmap bm(image_dest, NULL);

    // 创建圆角矩形路径
    CRect rect(start_point, size);
    Gdiplus::GraphicsPath round_rect_path;
    //创建的矩形路径为变换前矩形和变换后矩形的交集，避免当"专辑封面契合度"选项设置为"填充"时，矩形的角在专辑封面区域的外面
    CGdiPlusTool::CreateRoundRectPath(round_rect_path, rect & rect_ori, radius);

    Gdiplus::TextureBrush brush(&bm, Gdiplus::WrapModeTile);
    brush.TranslateTransform(rect.left, rect.top);
    m_pGraphics->SetSmoothingMode(Gdiplus::SmoothingMode::SmoothingModeAntiAlias);
    m_pGraphics->FillPath(&brush, &round_rect_path);
    m_pGraphics->SetSmoothingMode(Gdiplus::SmoothingMode::SmoothingModeNone);
```

思路：**GDI+ `GraphicsPath` 圆角矩形 + `TextureBrush` 填充**。先用 `ImageResize` 把图缩到目标尺寸（避免纹理画刷边缩放边采样导致糊），再平铺填充进路径。抗锯齿靠 `SmoothingModeAntiAlias`。

`CGdiPlusTool::CreateRoundRectPath`（`GdiPlusTool.cpp:38`）手工拼 4 段 `AddArc` + 4 条 `AddLine`。

UI 侧入口：【源码确证】`CPlayerUIBase.cpp:2205`

```cpp
void CPlayerUIBase::DrawAlbumCover(CRect rect)
{
    ...
    m_draw.DrawRoundImage(CPlayer::GetInstance().GetAlbumCover(), CalculateRoundRectRadius(rect),
                          rect.TopLeft(), rect.Size(), theApp.m_app_setting_data.album_cover_fit);
```

`album_cover_fit` 默认 `StretchMode::FILL`（`CommonData.h:235`），可选 FIT。V2.78 的新增功能里有「使用GDI+绘制专辑封面时，如果启用了圆角风格，将专辑封面也剪裁为圆角」，指的就是这里。【历史确证 + 源码确证】

背景模糊使用点：【源码确证】`CPlayerUIBase.cpp:886-890`

```cpp
if (theApp.m_app_setting_data.enable_background && m_ui_data.enable_background)
{
    CImage& back_image{ theApp.m_app_setting_data.background_gauss_blur
        ? CPlayer::GetInstance().GetAlbumCoverBlur()
        : CPlayer::GetInstance().GetAlbumCover() };
```

---

## 5. 写入的安全性与一致性

### 5.1 原地改，不是临时文件 + 重命名

**【源码确证】** 所有写函数都是「构造 File 对象 → 改内存中的 tag → `file.save()`」，**没有一层临时文件包装**。

对 TagLib 而言 `File::save()` 是原地重写（不同格式实现不同，但语义是就地更新）。**MP2 这一侧没有任何回滚、备份、原子性保护。**

**唯一的"安全措施"是播放器层面把正在播放的文件释放掉。**【源码确证】`Player.h:559` `CPlayer::ReOpen`

```cpp
// 用于在执行某些操作时，播放器需要关闭当前播放的歌曲，操作完成后再次打开
// 当reopen为true时，在构造函数中关闭，析构时再次打开
// 使用后需要先检查IsLockSuccess，如果返回false（极小概率）那么此次操作应当放弃并让出主线程，在主线程等待会死锁
struct ReOpen
{
    ReOpen(bool reopen) : m_reopen{ reopen }
    {
        if (m_reopen && !m_instance.m_loading
            && m_instance.GetPlayStatusMutex().try_lock_for(std::chrono::milliseconds(1000)))
        {
            lock_success = true;
            current_position = m_instance.GetCurrentPosition();
            is_playing = m_instance.IsPlaying();
            current_song = m_instance.GetCurrentSongInfo();
            m_instance.MusicControl(Command::CLOSE);     // 关掉 BASS 流
        }
    }
    ~ReOpen()
    {
        if (lock_success)
        {
            m_instance.MusicControl(Command::OPEN);
            m_instance.SeekTo(current_position);         // 恢复播放位置
            if (is_playing) m_instance.MusicControl(Command::PLAY);
            m_instance.GetPlayStatusMutex().unlock();
        }
    }
    bool IsLockSuccess() { return !m_reopen || lock_success; }
};
```

用 RAII 保证恢复，用 `try_lock_for(1000ms)` 避免死锁，**用 `IsLockSuccess()` 让调用方可以放弃**。这是个写得很漂亮的模式，非常值得抄。调用点：【源码确证】`PropertyDlg.cpp:164`

```cpp
void CPropertyDlg::OnBnClickedSaveToFileButton()
{
    IPropertyTabDlg* cur_tab = dynamic_cast<IPropertyTabDlg*>(m_tab_ctrl.GetCurrentTab());
    if (cur_tab != nullptr)
    {
        CPlayer::ReOpen reopen(true);   // ReOpen需要在IsLockSuccess失败时撤销操作所以改在这里，
                                        // 但这里判断是否为修改当前播放有点困难所以总是先关闭
        if (reopen.IsLockSuccess())
        {
            int saved_num = cur_tab->SaveModified();
            ...
        }
        else
        {
            const wstring& info = theApp.m_str_table.LoadText(L"MSG_WAIT_AND_RETRY");
            MessageBox(info.c_str(), NULL, MB_ICONINFORMATION | MB_OK);
        }
    }
}
```

### 5.2 失败的处理

**【源码确证】`TagLibHelper.cpp` 的写函数返回 `file.save()` 的返回值**。上层：

- `CPropertyTabDlg::SaveModified`（`:553`）返回 `saved` 或 `saved_count`
- 单曲模式：`saved == 0` → 弹 `MSG_FILE_WRITE_FAILED`
- 批量模式：弹 `MSG_PROPERTY_PARENT_TAG_BATCH_EDIT_SAVE_INFO` 报告成功数量
- 读取侧写完后**会重新读一遍文件来刷新**（`PropertyTabDlg.cpp:627-632`），而不是信任内存里的 `SongInfo`：
  ```cpp
  CAudioTag audio_tag(m_all_song_info[m_index], hStream);
  audio_tag.GetAudioTag();
  ```
  这个「写完重读」的做法很实在 —— 避免内存状态和文件真实状态脱节。

**没有回滚。** 半写坏的文件不会被恢复。

### 5.3 批量编辑

**【源码确证】`PropertyDlg.cpp:24`** —— 批量模式由构造函数重载决定：

```cpp
CPropertyDlg::CPropertyDlg(vector<SongInfo>& all_song_info, CWnd* pParent)
    : ..., m_batch_edit{ true }
```

批量模式下 **不创建「高级标签信息」页**（`PropertyDlg.cpp:124-131`），不显示翻页按钮，标题改成 `TITLE_PROPERTY_PARENT_BATCH`。

**【源码确证】`PropertyTabDlg.cpp:583`** 循环写：

```cpp
if (m_batch_edit)
{
    int saved_count{};
    for (int i{}; i < m_song_num; i++)
    {
        SongInfo& cur_song = m_all_song_info[i];
        CopyMultiTagInfo(song_info.title, cur_song.title);      // ← 关键：只覆盖"被改过"的字段
        CopyMultiTagInfo(song_info.artist, cur_song.artist);
        CopyMultiTagInfo(song_info.album, cur_song.album);
        if (str_year != theApp.m_str_table.LoadText(L"TXT_MULTI_VALUE").c_str())
            cur_song.SetYear(str_year);
        CopyMultiTagInfo(song_info.genre, cur_song.genre);
        CopyMultiTagInfo(song_info.comment, cur_song.comment);
        if (str_track != theApp.m_str_table.LoadText(L"TXT_MULTI_VALUE").c_str())
            cur_song.track = static_cast<BYTE>(_wtoi(str_track));
        CAudioTag audio_tag(cur_song);
        if (audio_tag.WriteAudioTag()) { UpdateSongInfo(cur_song); saved_count++; }
    }
    ...
    return saved_count;
}
```

**`CopyMultiTagInfo` + `TXT_MULTI_VALUE` 是这里的设计精华**：多选时字段显示为「多个数值」，只有用户**真正改过**的字段才覆盖到每一首；没改的保持原值。年/音轨号因为是数字框不好显示"多值"，用字符串比较代替。【源码确证】

**没有事务、没有两阶段提交**：写到第 37 首失败，前 36 首已经改了。只报告成功数量。

**批量封面**：【源码确证】`PropertyAlbumCoverDlg.cpp:319/350` 两个函数，`SaveAlbumCover` 遍历所有歌写同一张图（可选 `delete_file` 清理），`SaveEnbedLinkedCoverForBatchEdit` 则为**每首歌分别查找自己的外部封面**再嵌入。后者更符合直觉。

`DeleteLinkedPic` 的清理逻辑很谨慎：【源码确证】`:368`

```cpp
void CPropertyAlbumCoverDlg::DeleteLinkedPic(const wstring& file_path, const wstring& album_cover_path)
{
    //将外部专辑封面嵌入到音频文件后，如果图片的文件名和音频文件的文件名相同，则删除此外部专辑封面图片，
    //因此这个图片已经没有作用了
    wstring album_cover_file_name = CFilePathHelper(album_cover_path).GetFilePathWithoutExtension();
    wstring file_name = CFilePathHelper(file_path).GetFilePathWithoutExtension();
    if (file_name == album_cover_file_name)
        CommonDialogMgr::DeleteAFile(theApp.m_pMainWnd->GetSafeHwnd(), album_cover_path);
}
```

**只在「图片名 == 音频名」时才删**，避免误删用户的 `cover.jpg`（那可能是整个专辑共享的）。这个判断很到位。

### 5.4 ID3v2 写入版本可选（V2.73）

**【源码确证】`TagLibHelper.h:12/122/125` + `TagLibHelper.cpp:1746`**

```cpp
static void SetWriteId3V2_3(bool write_id3v2_3);    //设置是否写入ID3V2.3，否则写入ID2V2.4
...
static bool m_write_id3v2_3;   //写入ID3V2标签时是否使用2.3版本，否则使用2.4
...
TagLib::ID3v2::Version CTagLibHelper::GetWriteId3v2Version()
{
    return (m_write_id3v2_3 ? ID3v2::Version::v3 : ID3v2::Version::v4);
}
```

**全局静态变量**（不是每文件），由主窗口启动/保存设置时注入：

```cpp
// MusicPlayerDlg.cpp:800, 1449
CTagLibHelper::SetWriteId3V2_3(theApp.m_media_lib_setting_data.write_id3_v2_3);
```

UI 在「选项设置 - 媒体库」的 `IDC_ID3V2_TYPE_COMBO`（`MediaLibSettingDlg.cpp` 里 `AddString(L"ID3v2.3"); AddString(L"ID3v2.4");`），ini 键 `media_lib / write_id3_v2_3`，**默认 true 即 ID3v2.3**（`MusicPlayerDlg.cpp:789`）。注意 `CommonData.h:392` 里字段默认值是 `false`，又被读取时的默认 `true` 覆盖 —— 和 `id3v2_first` 一样的默认值不一致毛病。

**【历史确证】** commit 涉及 `03045520`（2021-05-09，标题：*写入ID3v2标签时默认使用ID3v2.3，媒体库设置中增加ID3v2写入设置*）。

**哪些格式用得上这个版本号？** 只有 ID3v2 系：`WriteMpegTag`/`WriteWavTag`/`WriteMpegLyric`/`WriteWavLyric`/`WriteMp3AlbumCover`/`WriteWavAlbumCover`/`WriteAiffAlbumCover`/`WriteMpegRating`。`.save(tags, File::StripOthers, GetWriteId3v2Version())`。【源码确证】

### 5.5 `File::StripOthers` —— 顺手删掉别家标签

**【源码确证】** MP3/WAV/TTA 的写路径都传 `File::StripOthers`。`WriteMpegTag`：

```cpp
bool CTagLibHelper::WriteMpegTag(const SongInfo& song_info)
{
    MPEG::File file(song_info.file_path.c_str());
    auto tag = file.tag();
    SongInfoToTag(song_info, tag);
    int tags = MPEG::File::ID3v2;
    //if (file.hasID3v1Tag())
    //    tags |= MPEG::File::ID3v1;              ← 被注释掉：明确不写 ID3v1
    if (file.hasAPETag())
        tags |= MPEG::File::APE;
    WriteOtherProperties(song_info, file);
    bool saved = file.save(tags, File::StripOthers, GetWriteId3v2Version());
    return saved;
}
```

**【历史确证】** commit `6377047b`（2020-08-28）标题：*写入mp3标签时不写入id3v1*。

`StripOthers` 的语义是「只保留 `tags` 里指定的标签类型，其他全删」。所以**保存一次 MP3 标签，文件里的 ID3v1 就被删掉了**（因为没在 `tags` 里）。APE 标签只在原本就有时才保留。

**【推测，但影响很大】** 这是个有争议的取舍：好处是彻底消除 ID3v1/ID3v2 不一致导致的乱码；坏处是**用户如果依赖 ID3v1 兼容老设备/老播放器，保存一次标签就丢了**。而且这是**静默**发生的，UI 上没有任何提示。

---

## 6. 在线标签 / 封面获取

### 6.1 走哪个源

**【源码确证】** 抽象接口 `CLyricDownloadCommon`（`LyricDownloadCommon.h:5`），实现由配置 `lyric_download_service` 选择：

- **网易云音乐** —— `NeteaseLyricDownload.cpp`，类 `CNeteaseLyricDownload`（`GetAlbumCoverURL` 在 `:69`）
- **QQ 音乐**（V2.78 新增）—— `QQMusicLyricDownload.cpp`

`theApp.GetLyricDownload()` 返回接口指针。所有在线功能**共用同一个抽象**：歌词、封面、标签走同一套搜索 + 匹配。

### 6.2 怎么匹配

**【源码确证】`LyricDownloadCommon.cpp:40` `SelectMatchedItem`** —— 加权相似度打分：

```cpp
/*
    匹配度计算：
    通过计算以下的匹配度，并设置权值，将每一项的匹配度乘以权值再相加，得到的值最大的就是最匹配的一项
    项目			 权值
    标题——标题         0.4
    艺术家——艺术家     0.4
    唱片集——唱片集     0.3
    文件名——标题       0.3
    文件名——艺术家     0.2
    列表中的排序       0.05
    时长              0.6
*/
for (size_t i{}; i < down_list.size(); i++)
{
    double weight;
    weight = 0;
    weight += (CInternetCommon::StringSimilarDegree_LD(title, down_list[i].title) * 0.4);
    weight += (CInternetCommon::StringSimilarDegree_LD(artist, down_list[i].artist) * 0.4);
    weight += (CInternetCommon::StringSimilarDegree_LD(album, down_list[i].album) * 0.3);
    weight += (CInternetCommon::StringSimilarDegree_LD(filename, down_list[i].title) * 0.3);
    weight += (CInternetCommon::StringSimilarDegree_LD(filename, down_list[i].artist) * 0.3);
    weight += ((1 - i * 0.02) * 0.05);	  //列表中顺序的权值，第一项为1，之后每项减0.02
    weights.push_back(weight);
}
...
//如果权值最大项的权值小于0.3，则判定没有匹配的项，返回-1
if (max_weight < 0.3) max_index = -1;
```

**用编辑距离 `StringSimilarDegree_LD`**（Levenshtein），不是子串匹配 —— 对拼写差异容忍度好。

**两个要指出的问题：**

1. **注释里的权值表和代码不一致**：注释写「文件名——艺术家 0.2」，代码是 `* 0.3`（第 66 行）。注释**过时了**。【源码确证】
2. **「时长 0.6」这条权值只写在注释里，代码里完全没有实现**。我逐行读了 40-88 行，没有任何地方用到 `down_list[i].duration` 或本地音频时长。【源码确证】

第二条很关键，因为题目问「有没有时长校验」—— **答案是没有**。`ItemInfo` 里有 `duration` 字段（`LyricDownloadCommon.h:39`），搜索结果的时长**只在列表里显示**（`CoverDownloadDlg.cpp:195` `SetItemText(i, 4, CPlayTime(m_down_list[i].duration).toString())`），**不参与匹配打分**。

**【历史确证】** V2.78 发布说明：「专辑封面下载/在线获取标签信息对话框搜索列表增加时长显示」—— 时长是**给人眼看的**，不是给算法用的。设计意图应该是「让用户自己核对」，这是合理的低成本方案。

**另外还有一条"强关联"路径**：如果这首歌在媒体库里已经关联了 `song_id`，就直接按 ID 命中，跳过相似度计算。【源码确证】

```cpp
// CoverDownloadDlg.cpp:350-365
CSongDataManager::GetInstance().GetSongID(m_song, song_id);
m_song.SetSongId(song_id);
if (!song_id.empty())        // 如果当前歌曲已经有关联的ID，则根据该ID在搜索结果列表中查找对应的项目
{
    for (size_t i{}; i < m_down_list.size(); i++)
        if (m_song.GetSongId() == m_down_list[i].id) { id_releated = true; best_matched = i; break; }
}
if (!id_releated)
    best_matched = CLyricDownloadCommon::SelectMatchedItem(m_down_list, m_title, m_artist, m_album, m_file_name, true);
```

**这个 ID 缓存是整套在线功能的关键优化**：第一次模糊匹配确认后，ID 存进媒体库（`song_data.dat`），以后再也不必搜索、不会匹错。

### 6.3 下载的封面存哪

**【源码确证】`CoverDownloadDlg.cpp:43` `CoverDownloadThreadFunc` + `MusicPlayerDlg.cpp:4437` 自动下载路径**

命名规则（两处逻辑一致）：

```cpp
if (match_item.album == pThis->m_song.album)    // 在线结果的唱片集名 == 本地唱片集名 → 用"唱片集"当文件名
    album_name = match_item.album;
else                                            // 否则用歌曲文件名
    album_name = pThis->m_song.GetFileName();
CCommon::FileNameNormalize(album_name);         // 洗掉 Windows 非法字符
```

存哪由 `GetSavedDir()` 决定（`CoverDownloadDlg.cpp:105`）：

```cpp
wstring CCoverDownloadDlg::GetSavedDir()
{
    if (m_save_to_song_folder || !CCommon::FolderExist(theApp.m_app_setting_data.AbsoluteAlbumCoverPath()))
        return CFilePathHelper(m_song.file_path).GetDir();
    else
        return theApp.m_app_setting_data.AbsoluteAlbumCoverPath();
}
```

即：**默认存到歌曲所在目录**，否则存到用户配的专辑封面文件夹。ini 键 `album_download / save_to_song_folder`，默认 `true`。

**存到歌曲目录时会设成隐藏属性**（不然会污染用户的文件夹视图）：

```cpp
//将下载的专辑封面改为隐藏属性
if (pThis->m_save_to_song_folder)
    SetFileAttributes(cover_file_path.GetFilePath().c_str(), FILE_ATTRIBUTE_HIDDEN);
```

代价是**别的程序（含 MP2 自己的某些路径）读隐藏文件会出岔子**，所以有对应的补偿逻辑：【源码确证】`Player.cpp:2645`

```cpp
void CPlayer::MediaTransControlsLoadThumbnail()
{
    if (CCommon::FileExist(m_album_cover_path))
    {
        if (CCommon::IsFileHidden(m_album_cover_path))
        {
            //如果专辑封面图片文件已隐藏，先将文件复制到Temp目录，再取消隐藏属性
            wstring temp_img_path{ CCommon::GetTemplatePath() + ALBUM_COVER_TEMP_NAME2 };
            CopyFile(m_album_cover_path.c_str(), temp_img_path.c_str(), FALSE);
            CCommon::SetFileHidden(temp_img_path, false);
            m_controls.loadThumbnail(temp_img_path);
        }
        else
        {
            m_controls.loadThumbnail(m_album_cover_path);
        }
    }
    else MediaTransControlsLoadThumbnailDefaultImage();
}
```

**为了让 Windows SMTC 能读到封面，还要再复制一份到 temp 去掉隐藏属性** —— 隐藏属性这个"小聪明"带来的连锁代价。

下载本身用 `URLDownloadToFile`（同步、无超时控制），在**工作线程**里跑（`AfxBeginThread`），完成发 `WM_DOWNLOAD_COMPLATE` 回主线程。**没有进度、没有断点续传、没有重试。**

### 6.4 失败标记

**【源码确证】** 搜索/下载失败时会**把"别再来烦我"标记写进媒体库**：

```cpp
// MusicPlayerDlg.cpp:4428
if (result == DR_DOWNLOAD_ERROR)     //如果搜索歌曲失败，则标记为没有在线歌词和专辑封面
{
    song_info_ori.SetNoOnlineAlbumCover(true);     // flags bit1
    song_info_ori.SetNoOnlineLyric(true);          // flags bit0
    CSongDataManager::GetInstance().AddItem(song_info_ori);
    return 0;
}
```

`SongInfo.flags` 是一个位域（`SongInfo.cpp:6-44`）：

| bit | 含义 |
|---|---|
| 0 | `NoOnlineLyric` |
| 1 | `NoOnlineAlbumCover` |
| 2 | `AlwaysUseExternalAlbumCover` |
| 3 | `ChannelInfoAcquired` |

**这个设计很实用**：一次网络失败不会导致每次播放这首歌都重试一次。**代价是永久性的** —— 如果只是临时断网，这首歌就再也不会自动下载了，用户得手动去媒体库清标记。【推测：这是有意的取舍】

---

## 7. 坑与代价

### 7.1 【源码确证】已确认的问题

**坑 1：ALBUMARTIST / DISCNUMBER 每保存一次就重复一次（V2.77 修）**

历史证据：commit `3a6e9808`（2023-11-05），标题 *fix #631: Correct the issue where 'ALBUMARTIST' and 'DISCNUMBER' (if present) are duplicated every time tags are saved.*

```diff
 static void WriteOtherProperties(const SongInfo& song_info, T& file)
 {
     TagLib::PropertyMap properties = file.properties();
+    properties["ALBUMARTIST"].clear();
+    properties["DISCNUMBER"].clear();
     if (!song_info.album_artist.empty())
         properties["ALBUMARTIST"].append(song_info.album_artist);
```

**根因**：`properties["ALBUMARTIST"]` 是 `StringList`，`.append()` 是**追加**。`file.properties()` 已经把文件里原有的值读进来了，再 append 一次就变两条。第 2 次保存 3 条，第 3 次 4 条……

**教训**：用 `PropertyMap` 改字段时，**必须先 `clear()` 再 `append()`**，或者直接赋值 `properties["KEY"] = list`。这是 TagLib `PropertyMap` 最容易踩的陷阱之一，而且症状是**渐进式字段膨胀**，测试一遍根本发现不了。这个 bug 在代码里活了 **3 年**（2020-09 引入 `WriteOtherProperties` → 2023-11 修）。

**坑 2：`to_local` 的判据选错，导致必须全量打开（一次改错 + 一次退回）**

证据链（第 3.3 节详述）：`ea6fbf76` 用 `file.hasID3v1Tag()` 当判据 → 一天后 `70540c1a` 全改 `true`。

教训：「文件里有没有 ID3v1」和「ID3v2 文本帧是不是 ANSI 编码」是两件正交的事。**判据必须落在真正的因（帧的 encoding byte）上，而不是碰巧相关的果。**

**坑 3：OPUS 的 switch 贯穿（现存 bug）**

`AudioTag.cpp:351` 和 `:392` 的 `case AU_OPUS:` 缺 `break`，落到 `case AU_WV:` 用 WavPack 的写入器处理 Opus 文件。目前被 `IsFileTypeTagWriteSupport` 的注释挡在 UI 之外，但这是个**定时炸弹**。

**坑 4：`id3v2_first` 死配置键，两个默认值互相矛盾**

`CommonData.h:289` 是 `false`，`MusicPlayerDlg.cpp:701` 读的时候默认 `1`。而**没有任何代码读它**。删了选项却留着配置读写，属于历史残留。

**坑 5：`File::StripOthers` 静默删除 ID3v1**

用户保存一次标签，MP3 里的 ID3v1 就没了，没有任何提示。对需要兼容老设备的用户是**数据丢失**。

**坑 6：`WriteM4aTag` 忘了 `WriteOtherProperties`**

【源码确证】`TagLibHelper.cpp:1546`

```cpp
bool CTagLibHelper::WriteM4aTag(const SongInfo& song_info)
{
    MP4::File file(song_info.file_path.c_str());
    auto tag = file.tag();
    SongInfoToTag(song_info, tag);          // ← 只有基础 7 字段
    bool saved = file.save();               // ← 没有 WriteOtherProperties！
    return saved;
}
```

对比同族的 `WriteFlacTag`/`WriteOggTag`/`WriteAsfTag` 都有 `WriteOtherProperties(song_info, file);` —— 全文件里 `WriteOtherProperties` 被调用了 **12 次**（`TagLibHelper.cpp:1531,1541,1560,1570,1580,1590,1600,1610,1620,1630,1640,1650`），覆盖全部 `Write*Tag` **除了 `WriteM4aTag`**。

**结果：编辑 M4A 的专辑艺术家/碟号不会被保存到文件。** 我又 grep 了全仓库确认没有别处补偿：【源码确证】`album_artist` 只出现在「读」（`SongInfo.cpp:56`/`CueFile.cpp:203`）、媒体库序列化（`SongDataManager.cpp:70/245`）、SMTC（`MediaTransControls.cpp:140`）、Last.fm（`LastFM.cpp:231`）和界面显示（`CUIDrawer.cpp:101`）里，**没有任何一处写回 M4A 文件**。这是复制粘贴时漏了一行，而且是 13 个写函数里唯一的一处。

**坑 7：`WriteTtaAlbumCover` 有不可达代码**

【源码确证】`TagLibHelper.cpp:1412-1414`

```cpp
    bool saved = file.save();
    return saved;
    return false;              // ← 永远执行不到
}
```

**坑 8：封面写入用扩展名拼 MIME，非标准扩展名会丢类型**

`setMimeType(L"image/" + ext)`。读到 `.jfif`/`.webp` 时 `GetPicType` 返回 -1，属性页的"文件类型"栏空着（封面本身还能显示，靠 GDI+ 嗅探）。

**坑 9：`GetId3v2AlbumCover` 只取第一帧**

【源码确证】`TagLibHelper.cpp:150`

```cpp
auto pic_frame_list = id3v2->frameListMap()["APIC"];
if (!pic_frame_list.isEmpty())
{
    ID3v2::AttachedPictureFrame* frame = dynamic_cast<...>(pic_frame_list.front());
```

`front()` 只拿第一张。**ID3v2 允许一个 APIC 帧带多种类型**（FrontCover / BackCover / Artist / Leaflet…），规范上应该优先挑 `type() == FrontCover` 的那张。MP2 不管类型，**谁在前面用谁** —— 有些文件第一张是封底或 CD 盘面图，就会显示错。FLAC / XiphComment / MP4 路径同样是 `front()`/`cover_list.front()`。**【源码确证，全部四个路径】**

**坑 10：没有长路径支持**

第 3.6 节。>260 字符的路径读不到也写不了，项目选择"主动规避"（`CheckFilePathLength` 截断文件名）而不是"突破"。

### 7.2 【历史确证】版本说明里提到、我在源码中定位到的

| 版本 | 说明 | 源码位置 |
|---|---|---|
| V2.70 | 如果专辑封面尺寸过大则将其缩小，以解决界面卡顿的问题 | commit `f4f67157` → `Player.cpp:2513 AlbumCoverResize` |
| V2.70 | 修正无法获取 bmp 格式专辑封面的问题 | commit `2b8c01c2` → `AudioTagOld.cpp:620 FindID3V2AlbumCover`（加 `"BM"` 魔数 + APIC 长度 +2） |
| V2.70 | 修正无法获取 wav 格式 id3v2 标签的问题 | `TagLibHelper.cpp:875 GetWavTagInfo`（走 `RIFF::WAV::File::ID3v2Tag()`） |
| V2.71 | 使用 taglib 库获取音频标签和专辑封面，新增十几种格式 | 整个 `TagLibHelper.cpp`；起点 commit `2661106a`（2020-08-27，*为项目添加taglib库*） |
| V2.71 | 「优先获取ID3V2标签」的选项已经不起作用，将它去掉 | commit `0eac4d42`（2020-08-29） |
| V2.71 前后 | 更新 taglib 库，将非 unicode 文本转换为 unicode 时使用本地代码页 | commit `4ffaed44` → 引入 `TagStringToWstring` 的初版；`ea6fbf76` 加 `to_local` 参数；`70540c1a` 全改 `true` |
| V2.73 | 新增歌曲标签 ID3v2 写入版本的设置 | commit `03045520` → `SetWriteId3V2_3` / `GetWriteId3v2Version` |
| V2.73 | 修正转换格式中转换成 mp3 格式后，写入的标签信息中一些 Unicode 字符会变成问号 | `FormatConvertDlg.cpp:546` 改走 TagLib（ID3v2 用 UTF-16），不再走 `AudioTagOld::WriteMp3Tag` 的 ANSI 路径 |
| V2.77 | 修正编辑音频文件标签信息时会导致"ALBUMARTIS"和"DISCNUMBER"字段重复 | commit `3a6e9808` → `WriteOtherProperties` 加 `clear()` |
| V2.78 | 使用 GDI+ 绘制专辑封面时，如果启用了圆角风格，将专辑封面也剪裁为圆角 | `DrawCommon.cpp:390 DrawRoundImage` + `CGdiPlusTool::CreateRoundRectPath` |

### 7.3 【推测】无直接代码证据，但我认为是真问题

1. **`isLatin1()` 启发式会误伤真正的 Latin-1 文本**（`"Björk"` / `"Motörhead"`）。机制清楚，但我在仓库里没找到相关 issue 或注释。中文语境下这是可接受的取舍。
2. **每次切歌都要把内嵌封面落盘到临时文件、再 Load、再可能缩放**，没有磁盘缩略图缓存，列表快速滚动时 IO 压力大。
3. **在线下载失败会被永久标记**，临时断网也要手动清。
4. **TagLib 的同步文件 IO 直接跑在 UI 线程**（读标签、写标签都不例外），网络盘/慢盘上会卡界面。
5. **`WriteWavPackTag` 缺 `isValid()` 守卫**，配合 OPUS 贯穿如果被触发，行为不可预测。

### 7.4 与 BASS 旧实现的对比：为什么换掉了

**【源码确证】`AudioTagOld.cpp` 全是手写字节解析**：

- `GetID3V2TagContents()`：自己按同步安全整数（7 位）算标签大小
  ```cpp
  const int tag_size{ (size[0] & 0x7F) * 0x200000 + (size[1] & 0x7F) * 0x4000 + (size[2] & 0x7F) * 0x80 + (size[3] & 0x7F) };
  ```
- `GetSpecifiedId3V2Tag()`：`tag_contents.find("TIT2")` **找字节串**当帧定位 —— 如果图片数据里碰巧含 `"TIT2"` 就会解析错位。有 `"TPE1"→"TPE2"`、`"TYER"→"TDRC"` 的 fallback。
- **但它做了一件新实现没做的事：真的读 encoding byte**
  ```cpp
  CodeType default_code;
  switch (tag_contents[tag_index + 10])   // 帧体的第 1 字节 = 文本编码
  {
  case 1: case 2: default_code = CodeType::UTF16LE; break;
  case 3:        default_code = CodeType::UTF8;     break;
  default:       default_code = CodeType::ANSI;     break;    // ← 0 = ISO-8859-1 → 当本地代码页
  }
  tag_info = CCommon::StrToUnicode(tag_info_str, default_code);
  ```
  **旧实现在这一点上反而比新实现更"正确"**（它看的是真正的因）。新实现放弃了帧级信息，改用 `isLatin1()` 猜 —— 更通用（跨格式），但更粗。
- `FindID3V2AlbumCover()`：在 APIC 区域里 `find(jpg_head)` / `find(png_head)` / … 按魔数嗅探。**这是新实现该学而没学的**。
- 硬编码 `D:\Temp\audio_tags\` 调试转储（`#ifdef _DEBUG`）。

**为什么被换掉**：`GetSpecifiedId3V2Tag` 的 `find()` 定位方式本质上不可靠；FLAC 靠找 `\0KEY=` 字节串并读前两字节当长度（`FindOneFlacTag`）同样脆弱；每个格式都要手写一遍。TagLib 一次性解决了 13 种格式。

**但代价是**：TagLib 的 `String` 抽象丢掉了「原始字节」信息，导致 MP2 必须用 `isLatin1()` + `to8Bit()` 这个字节往返技巧把它捞回来。**这是换库必然要付的税。**

**为什么没删干净**：Opus/AAC/未知格式仍然依赖它（第 2.3 节）。另外 `GetAudioLyric()`、`GetAlbumCoverDefault()`、`GetTagDefault()` 仍有活跃调用点。

---

## 8. 如果要抄，抄哪些 / 避什么

### 值得抄

1. **`PropertyMap` 做跨格式字段抽象**：不手写 ID3v2 帧名 / Vorbis key / MP4 atom，让库翻译。MP2 只用 7 个常量（都是库里没有抽象的特殊项：封面、歌词、cue、评分），其余全走 `PropertyMap`。**但必须 `clear()` 再 `append()`**（坑 1）。
2. **`CPlayer::ReOpen` RAII 守卫**（`Player.h:559`）：写文件前关掉播放器持有的流，析构时恢复播放位置和状态，用 `try_lock_for` 防死锁，用 `IsLockSuccess()` 给调用方放弃的机会。这是「改正在播放的文件」的标准解法。
3. **「Latin-1 字节往返」容错**（`TagStringToWstring`）：如果目标用户以中文为主，这是用最小代码量解决 GBK 乱码的有效手段。**但要有意识地接受它的代价**（真 Latin-1 文本会误伤），并把开关按容器是否可能用 ANSI 编码来设，而不是按"有没有 ID3v1"。
4. **写完重读 + 封面按需缩放**：`SaveModified` 成功后重新 `GetAudioTag()` 从文件读一遍（不信任内存）；封面长边超 `max_album_cover_size`（800）就缩到 800 再落临时文件。两个都是"花一点 IO 换一致性/流畅度"的务实选择。

### 明确避开

- **`PropertyMap` 字段膨胀**：这是最隐蔽的坑，症状是渐进式的，单元测试抓不到。
- **`File::StripOthers` 无条件用**：会静默删掉用户的 ID3v1。要么别删，要么至少给个选项。
- **用 `isLatin1()` 当编码判据而不看帧的 encoding byte**：如果库提供了帧级编码信息，优先用它。
- **`switch` 里 `case X: //注释掉的代码` 不给 `break`**：OPUS 那个 bug 就是这么来的。
- **下载失败永久标记**：区分"没这首歌"和"网络挂了"。
- **没有长路径支持**：现代 Windows 上这是个真实缺口，`\\?\` 前缀层要提前设计。

---

## 附：关键文件与函数索引

| 文件 | 关键符号 | 作用 |
|---|---|---|
| `TagLibHelper.cpp` | `TagStringToWstring` :52 | **编码容错的核心** |
| | `TagToSongInfo` :90 / `SongInfoToTag` :62 | 基础 7 字段读写 |
| | `OtherPropertyToSongInfo` :362 / `WriteOtherProperties` :386 | ALBUMARTIST/DISCNUMBER |
| | `GetPicType` :130 | MIME → 类型码 |
| | `GetId3v2AlbumCover` :146 / `WriteId3v2AlbumCover` :179 | APIC 读写 |
| | `ParseAudioRating` :399 / `GenerateAudioRating` :428 | POPM ↔ 1~5 星 |
| | `GetFlacPropertyMap` :490 … `getSpxPropertyMap` :580 | 每格式的 property 合并 |
| | `GetWriteId3v2Version` :1746 | v2.3 / v2.4 |
| `AudioTag.cpp` | `GetAudioTag` :43 | 格式分派（读） |
| | `WriteAudioTag` :321 / `WriteAlbumCover` :368 | 格式分派（写）★ **OPUS 贯穿 bug** |
| | `GetAlbumCover` :199 | 内嵌封面 → 临时文件 |
| | `IsFileTypeTagWriteSupport` :452 等 | 能力查询白名单 |
| `AudioTagOld.cpp` | `GetSpecifiedId3V2Tag` :435 | **旧实现真的读 encoding byte** |
| | `FindID3V2AlbumCover` :620 | 魔数嗅探封面格式（含 BMP 修复） |
| `Player.cpp` | `SearchAlbumCover` :2438 | 封面来源优先级 |
| | `AlbumCoverResize` :2513 | 大图缩到 800 |
| | `AlbumCoverGaussBlur` :2486 | 缩到 300 再模糊 |
| | `MediaTransControlsLoadThumbnail` :2645 | 隐藏属性补偿 |
| `Player.h` | `struct ReOpen` :563 | **写文件时的播放器守卫** |
| `MusicPlayerCmdHelper.cpp` | `SearchAlbumCover` :466 | 外部封面 8 级查找 |
| `DrawCommon.cpp` | `ImageResize` :681 / `DrawRoundImage` :390 | GDI+ 缩放 / 圆角 |
| `GaussBlur.cpp` | `SetSigma` :16 / `Filter` :78 | 可分离高斯卷积 |
| `GdiPlusTool.cpp` | `CreateRoundRectPath` :38 | 圆角路径 |
| `PropertyTabDlg.cpp` | `SaveModified` :553 | 标签保存（单曲 + 批量） |
| | `ShowInfo` :59 | 含标签类型显示 :148 |
| `PropertyAlbumCoverDlg.cpp` | `ShowInfo` :103 / `SaveModified` :59 / `SaveAlbumCover` :319 / `DeleteLinkedPic` :368 / `OnCompressSize` :641 | 封面页全流程 |
| `PropertyDlg.cpp` | `OnBnClickedSaveToFileButton` :164 | ReOpen 调用点 |
| `PropertyDlgHelper.cpp` | `IsSongTagWriteEnable` :233 / `IsMultiWritable` :153 | 可写性判定 |
| `PropertyAdvancedDlg.cpp` | `ShowInfo` :81 | PropertyMap 只读展示 |
| `CoverDownloadDlg.cpp` | `CoverDownloadThreadFunc` :43 / `OnSearchComplate` :322 | 在线下载 |
| `LyricDownloadCommon.cpp` | `SelectMatchedItem` :40 | 加权相似度匹配 |
| `MusicPlayerDlg.cpp` | `DownloadLyricAndCoverThreadFunc` :4397 | 自动下载主流程 |
| `Common.cpp` | `StrToUnicode` :532 / `UnicodeToStr` :608 / `JudgeCodeType` :727 / `IsUTF8Bytes` :687 | 编码工具 |
| | `GetImageFiles` :1209 / `FileIsImage` :1273 | 图片查找 |
| | `CheckFilePathLength` :498 | MAX_PATH 规避 |
| `Define.h` | :132-135 | 封面临时文件名 |

**外部证据（GitHub commit / release，本仓库源码不含）**
`4ffaed44` `ea6fbf76` `70540c1a` `0eac4d42` `f4f67157` `2b8c01c2` `2661106a` `6377047b` `03045520` `3a6e9808` `65da13ce` `ce1276da`；release V2.70 / V2.71 / V2.72 / V2.73 / V2.77 / V2.77.1 / V2.78。
