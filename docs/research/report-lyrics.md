# MusicPlayer2 歌词系统 · 源码级调查报告

调查对象：`zhongyang219/MusicPlayer2`（master 分支），C++ / MFC / Win32，GPL-3.0。
源码根目录为仓库里的 `MusicPlayer2/` 子目录；Scintilla 是内嵌的完整副本（仓库根 `scintilla/`）。
所有结论按 `【源码确证】`（附文件:函数）或 `【推测】` 标注。凡是我在文件里没找到的东西，都明确写了"没有"。

---

## 0. 一句话总览

歌词系统是一个**纯 LRC 家族解析器 + 手写位图渲染 + WinINet 下载**的组合：

- 解析内核是 `CLyrics`（注意是复数 **CLyrics**，不是 `CLyric`），只认 **LRC / 扩展 LRC（逐字）/ KSC / WebVTT**，外加一个**网易云半 JSON 变体**。
- 逐字高亮**不是逐字绘制**，而是"同一行文字画两遍 + 裁剪矩形"。
- 在线歌词走**网易云 / QQ音乐两个 API**，`CLyricDownloadCommon` 抽象基类 + 两个子类。
- 编辑器直接内嵌 **Scintilla**，甚至给 Scintilla 写了一个自定义词法分析器（`LexLyric.cxx`）。
- 桌面歌词是 `UpdateLayeredWindow` 的分层窗口；Cortana 歌词是**往系统任务栏搜索框的 HWND 上直接绘图**（相当硬核）。

---

## 1. 格式支持

### 1.1 枚举与扩展名分派

【源码确证】`MusicPlayer2/Lyric.h:CLyrics::LyricType`

```cpp
enum class LyricType { LY_AUTO, LY_LRC, LY_LRC_NETEASE, LY_KSC, LY_VTT };
const static vector<wstring> m_surpported_lyric;   // 声明
```

【源码确证】`MusicPlayer2/Lyric.cpp:6`
```cpp
const vector<wstring> CLyrics::m_surpported_lyric{ L"lrc", L"ksc", L"vtt" };
```
（变量名 `m_surpported_lyric` 是源码里的拼写错误，照抄。）

【源码确证】`Lyric.cpp:CLyrics::CLyrics(wstring, LyricType)` —— 只有 `LY_AUTO` 时才按扩展名判断，`lrc→LY_LRC, ksc→LY_KSC, vtt→LY_VTT`，**其余一律 fallback 到 LY_LRC**。

【源码确证】`Lyric.cpp:CLyrics::LyricsFromRowString()` —— 唯一的解析分派点：
```cpp
switch (m_lyric_type) {
case LY_LRC_NETEASE: DisposeLrcNetease(); break;
case LY_KSC:         DisposeKsc();         break;
case LY_VTT:         m_code_type = CodeType::UTF8; DisposeWebVTT(); break;
default:             DisposeLrc();         break;   // LY_LRC 走这里
}
NormalizeLyric();
```

【源码确证】`MusicPlayer2/AudioCommon.cpp:CAudioCommon::GetLyricFiles()` 用 `CLyrics::FileIsLyric()`（即上面那张扩展名表）来枚举目录里的歌词文件。

### 1.2 用户问到的格式，逐个交代

| 格式 | 是否支持 | 依据 |
|---|---|---|
| **LRC** | ✅ 主力格式 | `DisposeLrc()` |
| **扩展 LRC / 逐字（Enhanced LRC）** | ✅ 有 | `DisposeLrc()` 里第二次 `ParseLyricTimeTag` 用 `<` `>` 括号，注释明写"按 ESLyric 0.5.x 解析" |
| **网易云 LRC（半 JSON）** | ⚠️ 特殊处理 | `LY_LRC_NETEASE` → `DisposeLrcNetease()` |
| **KSC（酷狗）** | ✅ 有 | `DisposeKsc()`，识别 `karaoke.add(` |
| **WebVTT** | ✅ 有 | `DisposeWebVTT()`，`-->` + `<00:00:00.000>` 内联时间标签 |
| **内嵌歌词（ID3 USLT/SYLT、FLAC、M4A、WMA、WAV）** | ✅ 有，但**不解析 SYLT 帧** | `CPlayer::IniLyrics()`、`CAudioTag::GetAudioLyric()` |
| **KRC（酷狗加密二进制）** | ❌ **没有任何代码** | 全仓 grep `KRC` 无有效命中（仅 `LyricSavePolicy`、`KSC` 之类的噪声） |
| **QRC / YRC（QQ音乐）** | ❌ **没有任何代码** | 全仓 grep `QRC`/`YRC` 零命中 |
| **TRC（天天动听）** | ❌ **没有任何代码** | 全仓 grep `TRC` 零命中；从 QQ音乐下载回来的是**明文 LRC**（见 §5.3） |

> 结论：**逐字歌词在本项目里只有两个来源**——扩展 LRC（`<mm:ss.xx>`）和 WebVTT（`<hh:mm:ss.mmm>`），KSC 额外有"词块时长数组"形式的逐字。QQ音乐那套 QRC 逐字格式完全没被采用（它压根没用 QRC 接口）。

### 1.3 各格式的时间标签语法（源码里的实际写法）

**标准 LRC**（`DisposeLrc` / `ParseLyricTimeTag`）
```
[mm:ss.xx]歌词文本
[mm:ss.xxx]歌词文本      // 毫秒 3 位也吃
[mm:ss:xx]歌词文本       // 用冒号分隔毫秒也兼容
[mm:ss.xx][mm:ss.xx]文本 // 压缩 LRC：一行多个时间标签，展开成多行
[m:ss]文本               // 位数不固定
```

**扩展 LRC 逐字**（`DisposeLrc`，`Lyric.cpp:225-245`）
```
[00:12.34]<00:12.34>逐<00:12.78>字<00:13.10>歌<00:13.55>词
```
规则（源码注释所称 "ESLyric 0.5.x"）：**首个 `[..]` 是行起始时间，之后每个 `<..>` 之前的那段文字属于该 `<..>` 起始的匀速段**。`time_start_raw` 取 `[..]`，逐段时长 = 相邻 `<..>` 之差。

**KSC**（`DisposeKsc`）
```
[ti:歌名]  [ar:歌手]            // 实际是 karaoke.songname='..' / karaoke.singer='..'
karaoke.add('00:12.340', '00:15.120', '逐[字]歌[词]', '120,340,280,190');
                ↑起始       ↑行结束       ↑文本            ↑每段时长(ms)
```
- 文本里 `[x]` 表示"x 之前不切分"（把多字符词锁成一段），`''` 是单引号转义。
- `karaoke.add` 之前的行**原样保留**（源码注释："不清楚规则故暂不修改非歌词行"）。
- 时间标签用单引号作括号 —— 这是 `ParseLyricTimeTag` 的 `bracket_left/right` 参数唯一一次传 `'\''`。

**WebVTT**（`DisposeWebVTT`）
```
WEBVTT

1
00:00:12.340 --> 00:00:15.120
逐<00:00:12.780>字<00:00:13.100>歌词
```
- 只接受 12 位（`hh:mm:ss.mmm`）或 9 位（`mm:ss.mmm`）的时间串，长度不符就丢弃该 cue。
- 实体只解 6 个：`&amp; &lt; &gt; &quot; &apos; &nbsp;`。
- 帧内多行文本会**用空格拼成一行**（`text_with_tag += line_str + L' '`）—— 多行 VTT 会丢换行。

---

## 2. 解析实现

### 2.1 数据结构

【源码确证】`Lyric.h:CLyrics::Lyric`

```cpp
struct Lyric {
    int time_start_raw{};   // 行开始时间(初始化时写入之后只读)
    int time_span_raw{};    // 行持续时间(初始化时写入之后只读)
    int time_start{};       // 行开始时间(偏移量即时应用)
    int time_span{};        // 行持续时间(偏移量即时应用)
    wstring text;           // 歌词文本
    wstring translate;      // 翻译
    vector<int> split;      // 逐字歌词对 text 的分割位置（字符下标，非字节）
    vector<int> word_time;  // 各段持续时长（毫秒）
    bool operator<(const Lyric& l) const { return l.time_start_raw > time_start_raw; }
};
```

设计要点（源码注释直接说明了动机，值得抄）：

1. **`_raw` 与"生效值"双份**。`raw` 是文件里读出来的原始值，永远不改；`time_start/time_span` 是叠加了用户偏移、去重叠、补默认值之后的结果。改偏移时只需重跑 `NormalizeLyric()`，不用回头改原文。
2. **`split` 存的是字符下标**，`word_time` 与它一一对应。注释明确警告："未经 Normalize 仅限 GetLyricProgress 使用，其他位置不应使用防止出现意料之外的行为"。
3. **`m_lyrics_str` 单独保存"未拆时间标签的原始每一行"**，`GetLyricsString()` 在偏移为 0 时直接吐这一份，保证"没改过就逐字节还原"。

类里还存了 `[id:] [ti:] [ar:] [al:] [by:]` 五个标签（`m_id` 是作者自己加的网易云歌曲 ID）+ `[offset:]`，都用 `bool m_xx_tag` 记"见过没有"。用 `m_text_and_translatein_in_same_line` 记"原文和译文是否同一行（` / ` 分隔）"。

【源码确证】`MusicPlayer2/PlayTime.h:CPlayTime` —— 一个 **80 bit 的位域**（`negative:1, min:15, sec:6, msec:10`），提供 `fromInt/toInt/operator±/toLyricTimeTag/toVttTimeTag`。`min` 只有 15 位 → **最大约 546 小时**，够用。注意 `toLyricTimeTag()` 输出的是 `[%.2d:%.2d.%.2d]`，**毫秒字段是 `msec/10`，即两位厘秒** —— 这是 LRC 写回时的精度损失点（10ms 粒度）。

### 2.2 时间标签解析：手写扫描，没有正则

【源码确证】`Lyric.cpp:CLyrics::ParseLyricTimeTag(const wstring&, CPlayTime&, int& pos_start, int& pos_end, wchar_t bracket_left, wchar_t bracket_right)`

关键手法：

```cpp
index = lyric_text.find_first_of(bracket_left, index + 1);
if (index == npos) break;
else if (index > static_cast<int>(lyric_text.size() - 9)) break;   // 倒数第9个字符之后不再找
else if ((lyric_text[index+1] > L'9' || lyric_text[index+1] < L'0') && lyric_text[index+1] != L'-') continue;
index1 = lyric_text.find_first_of(L':', index);         // 分
index2 = lyric_text.find_first_of(L".:", index1 + 1);   // 秒（圆点，也兼容冒号）
index3 = lyric_text.find_first_of(bracket_right, index2 + 1);
```

- 括号字符是**参数化的**，所以同一函数能处理 `[...]`、`<...>`、`'...'`（KSC）。
- 毫秒位数自适应：`0位→0, 1位→×100, 2位→×10, ≥3位→%1000`（`case 0` 没有 `break`，会**贯穿**到 `case 1` —— 这是有意的还是笔误不好说，但 `time.msec = 0` 然后又被 `_wtoi("")*100 = 0` 覆盖，结果一样，属于无害的漏 break）。
- `pos_start/pos_end` 通过引用返回，调用方靠它做"上一次扫描到哪里"的游标推进，避免每行从头扫。

**没有用任何正则**，没有 `std::regex`，全是 `find_first_of` + `substr` + `_wtoi`。对歌词这种短行是合理选择（不会踩正则回溯，也不吃 `<regex>` 的构建成本）。

### 2.3 编码判定

【源码确证】`Lyric.cpp:CLyrics::CLyrics()` 构造函数：
```cpp
string lyric_str;
if (!CCommon::GetFileContent(m_file.c_str(), lyric_str)) return;   // 以字节读入
m_code_type = CCommon::JudgeCodeType(lyric_str, m_code_type, true);
wstring lyric_wcs = CCommon::StrToUnicode(lyric_str, m_code_type, true);
```

【源码确证】`Common.cpp:CCommon::JudgeCodeType(str, default_code, auto_utf8)`：
- UTF-8 BOM（`EF BB BF`）→ `UTF8`
- UTF-16LE BOM（`FF FE`）→ `UTF16LE`
- UTF-16BE BOM（`FE FF`）→ `UTF16BE`
- **无 BOM 且 `auto_utf8` 且 `IsUTF8Bytes(str)` 为真** → `UTF8_NO_BOM`
- 否则**保持默认**（`CLyrics` 里默认是 `CodeType::ANSI`，即系统本地代码页）

也就是说：**没有 BOM 的 GBK 歌词靠"不是合法 UTF-8"来兜底成 ANSI**，在中文 Windows 上正确，在非中文区域设置下就是乱码。这是这套判定最脆的地方。

### 2.4 译文配对

有**三条**不同路径，容易看混：

1. **同行 ` / ` 分隔**（最常用）
   【源码确证】`DisposeLrc()`：
   ```cpp
   index = text_str.find(L" / ");
   if (index != npos) { lyric.translate = text_str.substr(index + 3); text_str = text_str.substr(0, index); m_translate = true; }
   ```
   注意注释："由于前面的 StringNormalize 操作，不可能出现 ` / ` 后面为空的情况"。

2. **相同（或接近）时间标签的相邻两行**
   【源码确证】`Lyric.cpp:CLyrics::CombineSameTimeLyric(int error = 0)`：
   ```cpp
   std::stable_sort(m_lyrics.begin(), m_lyrics.end());
   for (int i{}; i < size - 1; i++)
       if (m_lyrics[i+1].time_start_raw - m_lyrics[i].time_start_raw <= error) {
           m_lyrics[i].translate = m_lyrics[i+1].text;
           m_lyrics.erase(m_lyrics.begin() + i + 1);
           m_text_and_translatein_in_same_line = false;
       }
   ```
   `DisposeLrc()` 末尾固定调用 `CombineSameTimeLyric()`（**error=0，即只合并完全相同的时间标签**）。源码里有一段重要注释解释了为什么不在这里给默认误差：
   > "理由是歌词偏移量调整时若负偏移量导致歌词在时间 0 处堆积则现有代码会将它们拉开 10ms 间距存储，如果这里出现 10 及以上的参数会误合并"

3. **格式自带的翻译字段**
   - 网易云：`DisposeLrcNetease()` —— 因为下载回来的带翻译歌词是"两行相同时间标签、第一行内容空白"，它先把重复时间标签的两行**内容合并**再交给 `DisposeLrc()`：
     ```cpp
     if (!m_lyrics_str[i].compare(0, index, m_lyrics_str[i+1], 0, index)) {   // 时间标签前缀相同
         m_lyrics_str[i] += m_lyrics_str[i+1].substr(index);
         m_lyrics_str.erase(m_lyrics_str.begin() + i + 1);
     }
     ```
     还处理了"歌词行间缺 `\n`"的情况：按 `[` 主动把一行劈成两行。
   - QQ音乐：`DisposeLryic()` 把 `trans` 字段**用 `\r\n` 追加到 `lyric` 后面**，然后靠上面的路径 2 自动配对。
   - 括号翻译（`【】〖〗「」『』`）：`ExtractTranslationFromBrackets()`，是**用户手动触发**的编辑操作，不自动跑。

### 2.5 规范化：`NormalizeLyric()` —— 整个系统的时间轴真相

【源码确证】`Lyric.cpp:CLyrics::NormalizeLyric()`，逐段说明：

```cpp
// (1) 排序 + 应用偏移 + 去重叠
std::stable_sort(m_lyrics.begin(), m_lyrics.end());
int last{};
for (i...) {
    last = max(last, m_lyrics[i].time_start_raw + m_offset);
    m_lyrics[i].time_start = last;
    last += 10;                     // ← 硬编码 10ms 间隔
}
```

```cpp
// (2) 逐行 time_span：优先 raw，否则逐字段累加，再对下一行裁剪
if (!now.word_time.empty() && now.word_time.back() < 0)   // -1 是"匀速段未定"的哨兵
    now.word_time.back() = next.time_start - now.time_start - accumulate(其余段);
if (now.time_span_raw != 0) now.time_span = now.time_span_raw;
else if (!now.word_time.empty()) now.time_span = accumulate(word_time);
if (now.time_span == 0 || next.time_start - now.time_start < now.time_span)
    now.time_span = next.time_start - now.time_start;      // 夹住，禁止跨行重叠
```

```cpp
// (3) 最后一句的特殊处理
if (now.word_time.back() < 0) {
    if (now.word_time.size() >= 2) now.word_time.back() = *(now.word_time.end() - 2);  // 抄前一段
    else                           now.word_time.back() = 20000;                      // 20 秒
}
```

三个值得记住的常量：**10ms（行间距）**、**20000ms（末尾兜底）**、**6000000ms = 100 分钟（截断阈值）**。

【源码确证】`DeleteRedundantLyric()`：遇到 `time_start >= 6000000` 就**把这句和后面全部删掉**。这是给下载来的脏歌词（比如把 `[al:xxx]` 之类解析成超大时间）兜底。

【源码确证】`ParseLyricTimeTag` 的 `index > size - 9` 早退 —— 意思是**时间标签必须出现在离行尾至少 9 个字符之前**，否则不认。这挡住了"歌词文本里恰好有 `[` 和数字"的误判，但也意味着**极短的最后一句**（比如 `[00:01.00]啊`，共 11 字符，`index=0`，`size-9=2`，`0 > 2` 为假，通过）—— 实测边界还需注意 `[0:0.0]x` 这类极短标签会被拒。

---

## 3. 逐字高亮的渲染

### 3.1 进度是怎么算的

【源码确证】`Lyric.h / Lyric.cpp:CLyrics::GetLyricProgress(CPlayTime time, bool ignore_blank, bool blank2mark, std::function<int(const wstring&)> measure) const`

返回 **0~1000 的整数**。它是整条渲染链的核心。签名里那个 `measure` 回调是关键设计 —— 由**调用方**（拥有 DC / Graphics 的那一层）提供"这段文字有多宽"：

```cpp
// 主界面（GDI）
GetLyricProgress(time, false, false, [this](const wstring& s){ return GetTextExtent(s.c_str()).cx; })
// 桌面歌词（GDI+）
GetLyricProgress(time, ignore_blank, karaoke, [&](const wstring& s){
    Gdiplus::RectF bb;
    pGraphics->MeasureString(s.c_str(), -1, pFont, Gdiplus::RectF{},
        Gdiplus::StringFormat::GenericTypographic(), &bb, 0, 0);
    return static_cast<int>(bb.Width); })
```

算法（`Lyric.cpp:670-756`）：

1. 定位当前行 `now_index`（`GetLyricIndex`），可能按需对齐到非空行。
2. 若 `now_index < 0`（还在标题区）→ 用 `m_lyrics[0].time_start` 当分母，进度从 0 涨到标题结束。
3. 若在"进度符号"区（`donot_show_blank_lines` 打开且空白 > `LYRIC_BLANK_IGNORE_TIME` 时，会在空行前插一个音符符号，符号本身也走进度）。
4. **逐字**：
   ```cpp
   size_t i{}, split_num{ min(split.size(), word_time.size()) };   // 防越界
   while (i < split_num && lyric_current_time > now_lyric.word_time[i])
       lyric_current_time -= now_lyric.word_time[i++];
   if (i < split_num) {
       lyric_last_time = now_lyric.word_time[i];
       if (i == 0) lyric_word_size = measure(text.substr(0, split[i]));
       else {
           lyric_before_size += measure(text.substr(0, split[i-1]));
           lyric_word_size  = measure(text.substr(split[i-1], split[i]-split[i-1]));
       }
   } else return 1000;    // 逐字时间总和已过 → "已结束"
   ```
5. **换算成"整行像素进度"**：
   ```cpp
   int progress{ lyric_current_time * 1000 / max(lyric_last_time, 1) };
   if (lyric_line_size > 0)
       progress = (progress * lyric_word_size / 1000 + lyric_before_size) * 1000 / lyric_line_size;
   return min(progress, 1000);
   ```

**这就是"宽字符/比例字体下逐字高亮仍然准确"的诀窍**：字段时间只决定"当前处于第几段、段内百分比"，真正拿去渲染的百分比是**按测量出来的像素宽度**加权算出来的。对非等宽字体（中文+英文混排）也不会跑偏。

`measure` 的调用次数：**每帧 3~5 次 `GetTextExtent` / `MeasureString`，是对子串测量的**。工程量不大，但确实是每帧的开销。

**`progress == 1000` 有特殊语义**：源码注释明确写了
> "注意进度为 1000 时表示当前歌词'已结束'，不要进行高亮并应根据需要进行高亮取消操作，由于逐字歌词引入此状态可能维持一段时间"

所以整条链路上到处都能看到 `if (progress == 1000)` 的分支。

配套的还有 `GetLyricLrcProgress(CPlayTime)` —— 只按"行时间标签差"算的降级版，注释说"不支持逐字歌词，用于在无法测量文本宽度时使用"。

### 3.2 高亮到底怎么画 —— 两遍绘制 + 裁剪矩形

**核心在 `DrawCommon.cpp:CDrawCommon::DrawWindowText(rect, str, color1, color2, split, align, no_clip_area)`**（重载 2，`DrawCommon.cpp:110`）：

```cpp
text_size  = m_pDC->GetTextExtent(lpszString);
CRect text_rect  { CPoint{text_left, text_top}, text_size };                                  // 背景字
CRect text_f_rect{ CPoint{text_left, text_top}, CSize{ text_size.cx * split / 1000, text_size.cy } };  // 覆盖字
// （长文本时按 split 位置做水平滚动，见 §3.4）
m_pDC->SetTextColor(color2);
m_pDC->DrawText(lpszString, text_rect,   DT_SINGLELINE | DT_NOPREFIX);   // 画整行"未唱"色
if (color1 != color2 && split != 1000) {
    m_pDC->SetTextColor(color1);
    m_pDC->DrawText(lpszString, text_f_rect, DT_SINGLELINE | DT_NOPREFIX); // 画整行"已唱"色，但矩形只到进度处
}
```

**关键点：不是逐字 `TextOut`，是"整行文字用窄矩形画第二遍"**。字形的左半部分被 GDI 裁掉、右半部分显示高亮色，得到"正在唱某个字"的中间态。便宜、对任意字体有效、也不需要拆字。

桌面歌词走的是另一条等价路线（GDI+ 路径 + Region 裁剪）：

【源码确证】`LyricsWindow.cpp:CLyricsWindow::DrawHighlightLyrics(pGraphics, pPath, dstRect)`
```cpp
if (m_nHighlight <= 0 || m_nHighlight >= 1000) return;   // ≥1000 不画高亮
if (m_lyric_karaoke_disp) {
    Gdiplus::RectF CliptRect(dstRect);
    CliptRect.Width = CliptRect.Width * m_nHighlight / 1000;
    pRegion = new Gdiplus::Region(CliptRect);
    pGraphics->SetClip(pRegion, Gdiplus::CombineModeReplace);
}
if (m_pHighlightPen) pGraphics->DrawPath(m_pHighlightPen, pPath);          // 描边
Gdiplus::Brush* pBrush = CreateGradientBrush(m_HighlightGradientMode, m_HighlightColor1, m_HighlightColor2, dstRect);
pGraphics->FillPath(pBrush, pPath);                                        // 渐变填充
if (pRegion) { pGraphics->ResetClip(); delete pRegion; }
```

**文字在这里是先 `AddString` 进 `GraphicsPath`，再 `FillPath`。** 好处：可以同时做描边 + 渐变填充 + 阴影（阴影是同一个 path 偏移 `m_nShadowOffset` 再 `FillPath` 一次）。

### 3.3 每帧重绘还是定时器

**没有 per-frame 动画循环，是"UI 线程 + Sleep 轮询"**：

【源码确证】`MusicPlayerDlg.cpp:CMusicPlayerDlg::UiThreadFunc(LPVOID)`（`MusicPlayerDlg.cpp:4538`）
```cpp
while (true) {
    if (pPara->ui_thread_exit) break;
    CPlayer::GetInstance().CalculateSpectralDataPeak();
    // 获取当前播放进度（放进 UI 线程，和 UI 同步，让歌词和进度条更流畅）
    if (CPlayer::GetInstance().IsPlaying() && ...try_lock_for(10ms)) {
        CPlayer::GetInstance().GetPlayerCoreCurrentPosition();
        ...unlock();
    }
    if (fresh_cnt) { fresh_cnt--; pThis->m_pUI->DrawInfo(pPara->draw_reset); ... }
    // 迷你模式 / Cortana 歌词 / 桌面歌词都在这个循环里刷
    CPlayer::GetInstance().m_controls.UpdatePosition(...);
    pThis->m_fps_cnt++;
    Sleep(pThis->m_ui_refresh_interval);      // 默认 100ms
}
```

- 默认刷新间隔：`CommonData.h` → `int ui_refresh_interval{ 100 };`（**10 FPS**）。用户可在设置里改。
- 主窗口另有一个 `SetTimer(TIMER_ID, TIMER_ELAPSE, NULL)`，`Define.h: #define TIMER_ELAPSE 80`（80ms）—— 但那个 Timer 主要做窗口尺寸 / 首次启动 / 一秒计时，**主界面绘制在 UI 线程里**。
- **动态限帧**（`MusicPlayerDlg.cpp:2738`）：每秒统计一次实际 FPS，`if (m_fps > MAX_FPS + MARGIN) m_ui_refresh_interval++;` / 反之 `--`。也就是**帧率超标就自动加大 Sleep**。
- **桌面歌词是独立定时器**：`DesktopLyric.cpp:CDesktopLyric::Create()` → `SetTimer(TIMER_DESKTOP_LYRIC, 200, NULL)`。但它的 `OnTimer` 只做鼠标穿透和按钮 hover 判定，**真正的绘制是 UI 线程调 `ShowLyric()` → `Draw()`**。

### 3.4 长歌词的水平滚动

【源码确证】`DrawCommon.cpp:110-159`，`DrawWindowText` 重载 2 自带的逻辑（主界面用），同构的逻辑在 `LyricsWindow.cpp:DrawLyricText()` 里也有一份（桌面歌词用）：

```cpp
if (text_size.cx > rect.Width()) {
    if (text_rect.Width() - text_f_rect.Width() < rect.Width() / 2)      // 高亮位置快到右端
        text_rect.MoveToX(rect.left - (text_rect.Width() - rect.Width()));
    else if (text_f_rect.Width() > rect.Width() / 2)                     // 高亮位置已过半
        text_rect.MoveToX(rect.left - (text_f_rect.Width() - rect.Width() / 2));  // 让高亮点居中
    else
        text_rect.MoveToX(rect.left);                                    // 还没过半，左对齐
}
```

即：**高亮位置 < 半宽 → 左对齐；过半 → 高亮点钉在中间；剩余不足半宽 → 右对齐**。三段式，无缓动。

### 3.5 高亮之外的颜色过渡（"渐亮/渐暗"）

【源码确证】`CUIDrawer.cpp:DrawLyricTextMultiLine()` / `DrawLyricTextSingleLine()`
```cpp
int last_time_span = time - lyric_i.time_start;
int fade_percent = last_time_span / 8;    // 注释：除数越大则持续时间越长，10 则为 1 秒
COLORREF text_color = CColorConvert::GetGradientColor(m_colors.color_text_2, m_colors.color_text, fade_percent);
```
```cpp
// 上一句正在"取消高亮"
int last_time_span = time - (lyric_i.time_start + lyric_i.time_span);
int fade_percent = last_time_span / 20;   // 注释：2000 毫秒时为 100%
COLORREF text_color = CColorConvert::GetGradientColor(m_colors.color_text, m_colors.color_text_2, fade_percent);
```

**这是纯时间驱动的颜色插值**（`/8` 意味着约 8 秒到满亮；`/20` 约 20 秒到全暗）。不是 alpha 混合，是 `RGB` 线性插值。

---

## 4. 平滑滚动与同步

### 4.1 多行歌词的滚动（正主）

【源码确证】`CUIDrawer.cpp:CUIDrawer::DrawLyricTextMultiLine(CRect, Alignment, bool)`（`CUIDrawer.cpp:57`）

```cpp
int line_space     = theApp.m_lyric_setting_data.lyric_line_space;      // 默认 2
if (full_screen) line_space *= CONSTVAL::FULL_SCREEN_ZOOM_FACTOR;
int lyric_height   = GetLyricTextHeight() + line_space;                 // 单行高
int lyric_height2  = lyric_height * 2 + line_space;                     // 含翻译的行高

vector<CRect> rects;   // 为每一句歌词预先算一个矩形（首行是标题，所以 count+1）
for (int i{}; i < lyric_count; i++) {
    CRect arect{ lyric_area };
    arect.bottom = arect.top + (有翻译且显示翻译 ? lyric_height2 : lyric_height);
    rects.push_back(arect);
}

int center_pos  = (lyric_area.top + lyric_area.bottom) / 2;
CPlayTime time{ CPlayer::GetInstance().GetCurrentPosition() };
int lyric_index = ...GetLyricIndex(time);
int progress    = ...GetLyricProgress(time, false, false, [this](s){ return GetTextExtent(s).cx; });
int y_progress  = progress * (有翻译 ? lyric_height2 : lyric_height) / 1000;

int start_pos = center_pos - y_progress;
for (int i{ lyric_index - 1 }; i >= -1; i--)          // 往前累减每行高度
    start_pos -= (该行有翻译 ? lyric_height2 : lyric_height);

for (int i{ -1 }; i < rects.size() - 1; i++) {
    if (i == -1) rects[i+1].MoveToY(start_pos); else rects[i+1].MoveToY(rects[i].bottom);
    if (!(rects[i+1] & lyric_area).IsRectEmpty())     // 只画与可视区有交集的
        ...DrawWindowText(...);
}
```

**结论：不用 `SetScrollPos`，不做缓动（easing），不做行快照。** 滚动的"平滑"完全来自 `progress` 是连续变化的：

- 当前行**在整个行时长内持续**从 `center_pos` 往 `center_pos - lyric_height` 平移。
- 这带来一个副作用：**滚动速度 = 行高 / 行时长**，行长歌词滚得慢、行短歌词滚得快 → 观感上会"一顿一顿"。这是它和"行级 snap + tween"方案最大的差别（后者匀速但可能跳变）。
- 视口裁剪靠 `rects[i+1] & lyric_area` 的 `CRect` 求交，跳过不可见行 —— 这是 O(行数) 但每行只做一次交集判断，很便宜（尽管那个 for 循环仍然遍历所有行）。

【源码确证】`CUIDrawer.h:bool IsDrawMultiLine(int height) const` → `height >= GetLyricTextHeight() * 3.5`。歌词区域高度不足 **3.5 行**时自动切单行/双行模式。

### 4.2 双行模式

【源码确证】`CUIDrawer.cpp:DrawLyricTextSingleLine()` + `DrawLyricDoubleLine()`

```cpp
static int flag{};                       // 调用方传进来的静态变量，用来记忆状态
bool switch_flag{ flag > 5000 };
switch_flag ^= (flag % 5000) > progress; // 用 progress 回绕检测"这句唱完了"
flag = switch_flag ? 10000 + progress : progress;
```
`switch_flag` 为真时，当前歌词画在**下半行**、下一句画在上半行 —— 这就是"上一句往上飘走"的视觉效果。对齐方式为 `AUTO` 时用 **上左下右** 的卡拉 OK 式对齐（`up_align = LEFT; down_align = RIGHT;`）。

### 4.3 播放位置从哪来

【源码确证】`BassCore.cpp:CBassCore::GetCurPosition()`
```cpp
QWORD pos_bytes = BASS_ChannelGetPosition(m_musicStream, BASS_POS_BYTE);
double pos_sec  = BASS_ChannelBytes2Seconds(m_musicStream, pos_bytes);
int current_position = static_cast<int>(pos_sec * 1000);
if (current_position == -1000) current_position = 0;
GetMidiPosition();
return current_position;
```

- 用的是 **`BASS_ChannelGetPosition(m_musicStream, BASS_POS_BYTE)` + `BASS_ChannelBytes2Seconds`**，**没有用** `BASS_ChannelGetPositionEx`（全仓 grep 零命中）。
- 单位：**毫秒 int**。
- 同一个位置每帧只取一次，缓存进 `CPlayer::m_current_position`：

【源码确证】`Player.cpp:CPlayer::GetPlayerCoreCurrentPosition()`
```cpp
int current_position_int = m_pCore->GetCurPosition();
if (!IsPlaylistEmpty() && GetCurrentSongInfo().is_cue)
    current_position_int -= GetCurrentSongInfo().start_pos.toInt();   // CUE 单轨要减偏移
m_current_position.fromInt(current_position_int);
```

【源码确证】`Player.h`：`int GetCurrentPosition() const { return m_current_position.toInt(); }` —— **所有歌词绘制都读这个缓存值**，不会各自去打 BASS。这是好设计（BASS 调用有临界区 `CSingleLock sync(&m_critical, TRUE)`，密集调用会锁竞争）。

驱动时机见 §3.3 的 UI 线程循环。

### 4.4 提前/延后偏移

【源码确证】`MusicPlayerDlg.cpp`
```cpp
void CMusicPlayerDlg::OnLyricForward() { CPlayer::GetInstance().m_Lyrics.AdjustLyric(-500); }  // 提前 0.5s
void CMusicPlayerDlg::OnLyricDelay()   { CPlayer::GetInstance().m_Lyrics.AdjustLyric( 500); }  // 延后 0.5s
```

【源码确证】`Lyric.cpp:CLyrics::AdjustLyric(int offset)`
```cpp
if (m_lyrics.empty()) return;
m_offset += offset;
m_modified = true;
NormalizeLyric();       // 重算全部 time_start / time_span
```

**偏移量是累加的 `m_offset`，不是直接改时间标签**，只有保存时才展开：

【源码确证】`GetLyricsString()`：`if (m_offset == 0)` 直接返回原始行；否则调 `GetLyricsString2()`。
【源码确证】`GetLyricsString2()`：对每一句用 `a_lyric.time_start`（已含偏移）调 `toLyricTimeTag()` 重新生成 `[mm:ss.cc]`。
源码里还有一行注释说明这个决策：
```
// bool save_lyric_in_offset{};   //是否将歌词保存在offset标签中，还是保存在每个时间标签中   ← 在 CommonData.h 里被注释掉了
```
即：**曾经考虑过把偏移写进 `[offset:]` 标签，最后选了"展开写进每个时间标签"**。

自动保存策略：
【源码确证】`CommonData.h:LyricSettingData::LyricSavePolicy { LS_DO_NOT_SAVE, LS_AUTO_SAVE, LS_INQUIRY }`，默认从 ini 读到 **2 = LS_INQUIRY**（`MusicPlayerDlg.cpp:588` 默认值 2）。
【源码确证】`MusicPlayerDlg.cpp:CMusicPlayerDlg::DoLyricsAutoSave(bool no_inquiry)` 在切歌等时机被调用。

内嵌歌词的保存走 `CAudioTag::WriteAudioLyric()`（MP3/FLAC/MP4/WMA/WAV，见 `AudioTag.cpp:CAudioTag::IsFileTypeLyricWriteSupport`）。

---

## 5. 在线歌词下载

### 5.1 架构

【源码确证】`LyricDownloadCommon.h:class CLyricDownloadCommon` —— 纯虚基类：
```cpp
virtual std::wstring GetSearchUrl(const std::wstring& key_words, int result_count = 20) = 0;
virtual std::wstring GetAlbumCoverURL(const wstring& song_id) = 0;
virtual std::wstring GetOnlineUrl(const wstring& song_id) = 0;
virtual int  RequestSearch(const std::wstring& url, std::wstring& result) = 0;
virtual void DisposeSearchResult(vector<ItemInfo>&, const wstring&, int result_count = 30) = 0;
virtual bool DownloadLyric(const wstring& song_id, wstring& result, bool download_translate = true) = 0;
virtual bool DisposeLryic(wstring& lyric_str, bool download_translate) = 0;   // 注意拼写：DisposeLryic
struct ItemInfo { wstring id, title, artist, album; int duration{}; int track{}; };
static int SelectMatchedItem(...);
ItemInfo SearchSongAndGetMatched(...);
```
两个实现：`CNeteaseLyricDownload`、`CQQMusicLyricDownload`。工厂在
【源码确证】`MusicPlayer2.cpp:CMusicPlayerApp::InitLyricDownload()` 按 `lyric_download_service` 选一个 `make_unique`，通过 `GetLyricDownload()` 暴露。

### 5.2 网易云

【源码确证】`NeteaseLyricDownload.cpp`

| 用途 | URL | 方法 |
|---|---|---|
| 搜索 | `http://music.163.com/api/search/get/?s=%s&limit=%d&type=1&offset=0` | **POST**（`RequestSearch` 里调的 `HttpPost(url, result)`，无 body） |
| 歌词（无翻译） | `http://music.163.com/api/song/media?id=<id>` | GET |
| 歌词（带翻译） | `http://music.163.com/api/song/lyric?os=osx&id=<id>&lv=-1&kv=-1&tv=-1` | GET |
| 封面 | `http://music.163.com/api/song/detail/?id=<id>&ids=%5B<id>%5D&csrf_token=` | GET |
| 网页 | `http://music.163.com/#/song?id=<id>` | — |

**没有任何加密 / 签名 / host 白名单**，都是裸 HTTP（`http://` 而不是 `https://`）。搜索请求虽然是 POST 但参数全在 URL 里，body 为空。

解析用 `nlohmann/json`：
```cpp
json data = json::parse(search_result);
auto& songs = data["result"]["songs"];
item.id       = std::to_wstring(song_item.value("id", 0LL));
item.title    = CCommon::StrToUnicode(song_item.value("name", ""), CodeType::UTF8);
item.duration = song_item.value("duration", 0);      // ← 解析了，但后面没用（见 §5.5）
item.album    = song_item["album"].value("name", "");   // 艺术家用 '/' 连接
CInternetCommon::DeleteStrSlash(item.title);  // 删 `\"` 里的反斜杠
```

**歌词字符串是"从 JSON 里手抠出来的"** —— 这段代码很脆，值得单独看：
【源码确证】`NeteaseLyricDownload.cpp:CNeteaseLyricDownload::DisposeLryic()`
```cpp
size_t index1 = lyric_str.find('[');                     // 第一个 '[' 就是歌词开始
if (index1 == string::npos) return false;
lyric_str = lyric_str.substr(index1, lyric_str.size() - index1 - 13);   // ← 末尾硬减 13
if (!lyric_str.empty() && lyric_str.back() == L'\"') lyric_str.pop_back();
// 然后手写扫描，把 JSON 转义还原成真实字符：
//   "\r\n" 或 "\n\n" 四字符序列 → 真实 CRLF，并 erase 掉多余 2 字符
//   "\r" → CRLF
//   "\n" → CRLF
//   "\"" → 删掉反斜杠
return true;
```
`- 13` 是在砍掉 JSON 尾巴 `"},"klyric":{...` 之类的东西。**接口响应结构一变就废**，而且返回 `false` 也表示"这首歌没有歌词"（`{"nolyric":true}` 里没有 `[`，正好走这条）。

### 5.3 QQ音乐

【源码确证】`QQMusicLyricDownload.cpp`

| 用途 | URL |
|---|---|
| 搜索 | `https://c.y.qq.com/soso/fcgi-bin/client_search_cp?p=1&n=%d&w=%s&format=json` |
| 歌词 | `https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=%s&format=json&nobase64=1` |
| 封面 | `https://c.y.qq.com/v8/fcg-bin/fcg_play_single_song.fcg?songmid=%s&format=json` → `http://y.gtimg.cn/music/photo_new/T002R800x800M000<albumMid>.jpg` |
| 网页 | `https://y.qq.com/n/ryqq/songDetail/<songmid>` |

歌词请求**必须带 Referer 和 UA**（否则 403）：
```cpp
int rtn = CInternetCommon::HttpGet(lyric_url.GetString(), result,
    L"Referer: https://y.qq.com/\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
```
`nobase64=1` 让它返回明文 LRC 而不是 base64 —— **所以 QQ 这边拿到的就是普通 LRC，没有 QRC 逐字**。

结果解析：
```cpp
json res_json = json::parse(lyric_str);
lyric_str = CCommon::StrToUnicode(res_json.at("lyric").get<std::string>(), CodeType::UTF8);
if (download_translate) {
    std::wstring trans = CCommon::StrToUnicode(res_json.at("trans").get<std::string>(), CodeType::UTF8);
    if (!trans.empty()) { lyric_str += L"\r\n"; lyric_str += trans; }   // 靠相同时间标签自动配对
}
```
注意用的是 `.at()` 而不是 `.value()` —— **`trans` 字段不存在会抛异常，被 catch 后返回 false**，即"这首歌没有翻译"会被当成"下载失败"。

搜索结果的艺术家用 `;` 连接（网易用 `/`），`interval` 秒 × 1000 存成 ms，`cdIdx` 存 track。

### 5.4 搜索关键字怎么构造

【源码确证】`LyricDownloadCommon.cpp:CLyricDownloadCommon::SearchSongAndGetMatched()` 与 `LyricBatchDownloadDlg.cpp:ThreadFunc()`

```cpp
if (title.empty() || theApp.m_str_table.LoadText(L"TXT_EMPTY_TITLE") == title) {
    keyword = file_name;   keyword = keyword.substr(0, keyword.rfind(L'.'));   // 用文件名去扩展名
} else if (artist.empty() || ... TXT_EMPTY_ARTIST == artist) {
    keyword = title;
} else {
    keyword = artist + L' ' + title;      // "艺术家 标题"
}
wstring keyword_url = CInternetCommon::URLEncode(keyword);
```

**没有去括号处理**，没有 `feat.` 清理，没有中文繁简归一。URL 编码是自写的：
【源码确证】`InternetCommon.cpp:CInternetCommon::URLEncode()`
```cpp
str_utf8 = CCommon::UnicodeToStr(wstr, CodeType::UTF8_NO_BOM);
for (char ch : str_utf8) {
    if (ch == ' ') result.push_back(L'+');
    else if (isalnum) result.push_back(ch);
    else if (ch in "-_.!~*()") result.push_back(ch);
    else { swprintf_s(buff, L"%%%x", (unsigned char)ch); result += buff; }   // ← 注意 %x 不补零
}
```
**【源码坑】`L"%%%x"` 不补零**：字节 `0x0A` 会编码成 `%a` 而不是 `%0a`。对 `%` 后只截一位的服务器是错的。中文 UTF-8 三字节都是 0x80+，`%e4%b8%ad` 这种两位十六进制没问题，所以实际踩不到 ——但把中文之外的 Latin-1 字节（0x01~0x0F）传进去就有 bug。`'`（单引号）被注释掉了，所以会被编码成 `%27`。

### 5.5 匹配与排序

【源码确证】`LyricDownloadCommon.cpp:CLyricDownloadCommon::SelectMatchedItem()`

源码注释写了设计意图（写在函数头部）：
```
匹配度计算：
项目              权值
标题——标题         0.4
艺术家——艺术家     0.4
唱片集——唱片集     0.3
文件名——标题       0.3
文件名——艺术家     0.2      ← 注释如此
列表中的排序       0.05
时长              0.6      ← 注释如此
```

**但实际代码里时长（duration）根本没用上**，而且"文件名——艺术家"的系数写成了 **0.3**（不是注释里的 0.2）：

```cpp
weight = 0;
weight += CInternetCommon::StringSimilarDegree_LD(title,     down_list[i].title)  * 0.4;
weight += CInternetCommon::StringSimilarDegree_LD(artist,    down_list[i].artist) * 0.4;
weight += CInternetCommon::StringSimilarDegree_LD(album,     down_list[i].album)  * 0.3;
weight += CInternetCommon::StringSimilarDegree_LD(filename,  down_list[i].title)  * 0.3;
weight += CInternetCommon::StringSimilarDegree_LD(filename,  down_list[i].artist) * 0.3;   // ← 注释说 0.2
weight += ((1 - i * 0.02) * 0.05);   // 列表位置权值：第1项 1.0，之后每项 -0.02
...
if (max_weight < 0.3) max_index = -1;   // 阈值：低于 0.3 判定"没找到"
```

相似度是**带字符相似度的编辑距离**：
【源码确证】`InternetCommon.cpp:CInternetCommon::StringSimilarDegree_LD()`
```cpp
const int MAX_LENGTH = 256;
if (n <= 0 || n > MAX_LENGTH || m <= 0 || m > MAX_LENGTH || abs(n-m) > MAX_LENGTH) return 0;  // 过长不计算
vector<vector<double>> d(n + 1, vector<double>(m + 1));      // O(n*m) 的动态规划表
cost = 1 - CharacterSimilarDegree(ch1, ch2);                 // ← 替换代价不是 0/1，是"字符相似度"的补
d[i][j] = CCommon::Min3(d[i-1][j] + 1, d[i][j-1] + 1, d[i-1][j-1] + cost);
double ds = 1 - (double)d[n][m] / max(n, m);                 // 归一化到 0~1
```

【源码确证】`InternetCommon.cpp:CInternetCommon::CharacterSimilarDegree(wchar_t, wchar_t)`
- 完全相同 → 1.0
- ASCII 大小写 → 0.8
- **中文数字与阿拉伯数字**：`一↔1, 二↔2, ..., 九↔9, 零↔0` → 0.7
- 其他 → 0.0

这三个小设计（大小写 0.8、中文数字 0.7、替换代价连续化）是**专门为歌词/歌名匹配调的**，很实用。`MAX_LENGTH = 256` 是防 O(n²) 爆表。

**没有任何"时长比对"逻辑**（`ItemInfo::duration` 只在 UI 列表里显示、在 `ShowDownloadList()` 里格式化）。也就是说，**5 分钟的录音室版和 3 分钟的现场版会被判成同一个结果**，这是多版本歌曲匹配错误的主要来源。

### 5.6 本地歌词文件的查找与命名

【源码确证】`MusicPlayerCmdHelper.cpp:CMusicPlayerCmdHelper::SearchLyricFile(const SongInfo& song, bool fuzzy_match)`（`Player.cpp:SearchLyrics()` 调用）

```cpp
if (song.GetFileName().size() < 3) return wstring();       // 文件名太短直接放弃
bool find_org_name{ !song.is_cue && !COSUPlayerHelper::IsOsuFile(song.file_path) };
if (!find_org_name) {                                       // cue / osu：用"艺术家 - 标题.lrc"
    wstring ar_ti{ song.artist + L" - " + song.title + L".lrc" };
    CCommon::FileNameNormalize(ar_ti);
    lyric_path.SetFilePath(lyric_path.GetDir() + ar_ti);
}
// 搜索目录顺序：歌曲所在目录 → 用户配置的歌词文件夹
vector<wstring> path_list{ lyric_path.GetDir() };
if (CCommon::FolderExist(theApp.m_lyric_setting_data.AbsoluteLyricPath()))
    path_list.push_back(theApp.m_lyric_setting_data.AbsoluteLyricPath());

// 第一轮：完全匹配（遍历 {lrc, ksc, vtt} 三个扩展名，用 FileExist 快速查）
for (const wstring& pa : path_list) {
    lyric_path.SetFilePath(pa + lyric_path.GetFileName());
    for (const wstring& ext : CLyrics::m_surpported_lyric) {
        lyric_path.ReplaceFileExtension(ext.c_str());
        if (CCommon::FileExist(lyric_path.GetFilePath())) return lyric_path.GetFilePath();
    }
}
// 第二轮：模糊匹配（要枚举目录里所有文件，所以慢）
if (fuzzy_match) { ... }
```

模糊匹配的做法：**把歌名按 `-` 拆成若干关键字**，然后在目录里枚举所有歌词文件做匹配：
【源码确证】`MusicPlayerCmdHelper.cpp:isMatched` lambda
```cpp
CCommon::StringSplit(lyric_name, L'-', key_words);
if (fuzzy_match) {  // 部分匹配：任意一个关键字命中即可
    for (auto& kw : key_words) if (CCommon::StringNatchWholeWord(str, kw) != -1) return true;
    return false;
} else {            // 严格：所有关键字都要命中
    for (auto& kw : key_words) if (CCommon::StringNatchWholeWord(str, kw) == -1) return false;
    return true;
}
```
`SearchLyricFiles()`（复数版，给"关联歌词"对话框用）复用同一套逻辑。

**下载文件命名**：
【源码确证】`LyricDownloadDlg.cpp:GetLyricFileName() / GetSavedDir() / GetSavedPath()`
```cpp
bool save_to_lyric_folder = (!m_save_to_song_folder && CCommon::FolderExist(theApp.m_lyric_setting_data.AbsoluteLyricPath()));
if (m_song.is_cue || IsOsuFile() || save_to_lyric_folder) {
    lyric_name = CSongInfoHelper::GetDisplayStr(m_song, DF_ARTIST_TITLE);  // "艺术家 - 标题"
    CCommon::FileNameNormalize(lyric_name);
} else {
    lyric_name = CFilePathHelper(m_song.GetFileName()).ReplaceFileExtension(nullptr);  // 音频文件名（去扩展名）
}
```
保存目录：歌曲所在目录（默认）或用户在设置里指定的歌词文件夹。**扩展名固定 `.lrc`**（`GetSavedPath()` 硬编码）。

**【源码坑】**`GetSavedDir()` / `GetLyricFileName()` 里判存在用的是 `theApp.m_lyric_setting_data.AbsoluteLyricPath()`（每次把 ini 里的 `lyric_path` 转绝对路径），而 `CLyricBatchDownloadDlg` 直接用了 `AbsoluteLyricPath()`，两处一致；但 `m_lyric_dir` 成员（`LyricDownloadDlg.h` 里声明了）实际上没在这些函数里用 —— 属于历史残留。

### 5.7 保存流程（网易云的"翻译"特殊处理）

【源码确证】`LyricDownloadDlg.cpp:OnDownloadComplate(WPARAM, LPARAM)`
```cpp
if (!theApp.GetLyricDownload()->DisposeLryic(m_lyric_str, m_download_translate)) { 报错"这首歌没有歌词"; return; }
CLyricDownloadCommon::AddLyricTag(m_lyric_str, id, title, artist, album);   // 前置 [id:][ti:][ar:][al:]
if (wParam == 0) {                                  // 直接保存
    wstring saved_path = GetSavedPath();
    if (CCommon::FileExist(saved_path)) { 询问是否覆盖 }
    if (!SaveLyric(saved_path.c_str(), m_save_code)) return;
    if (m_download_translate) {
        auto lyric_type = (service == LDS_NETEASE) ? LY_LRC_NETEASE : LY_LRC;
        CLyrics lyrics{ saved_path, lyric_type };                              // ← 重新读回来解析
        lyrics.SaveLyric2(download_lyric_text_and_translation_in_same_line);   // ← 规范化后再写一遍
    }
    if (m_song == CPlayer::GetInstance().GetCurrentSongInfo())
        CPlayer::GetInstance().IniLyrics(saved_path);   // 正在播这首才刷新显示
}
```
**下载带翻译的歌词要写两次文件**：第一次落原始的（网易专用格式），第二次用 `CLyrics` 解析合并后按通用 LRC 落盘。`SaveLyric()` 的编码由 `m_save_code`（ANSI / UTF8）决定，通过 `CCommon::UnicodeToStr(str, code_type, &char_connot_convert)` 转换；**转换失败（有字符无法用 ANSI 表示）会弹框问用户是否改用 Unicode 编码**。

`AddLyricTag()` 的细节：`[id:]` 永远加；`[ti:]/[ar:]/[al:]` 只在原文里**没有**该标签**或者该标签是空的**（`find(L"[ti:]") != npos`）时才加，也就是不覆盖已有信息。

### 5.8 失败 / 超时 / 并发

**HTTP 层**：
【源码确证】`InternetCommon.cpp:CInternetCommon::SendHttpRequest(bool post, ...)`
```cpp
CInternetSession session;
if (custom_ua) session.SetOption(INTERNET_OPTION_USER_AGENT, (LPVOID)L"MuiscPlayer2" APP_VERSION, ...);
AfxParseURL(str_url.c_str(), dwServiceType, strServer, strObject, nPort);
if (AFX_INET_SERVICE_HTTP != dwServiceType && AFX_INET_SERVICE_HTTPS != dwServiceType) return FAILURE;
pConnection = session.GetHttpConnection(strServer, ... SECURE_CONNECT : NORMAL_CONNECT, nPort);
pFile = pConnection->OpenRequest(post ? _T("POST") : _T("GET"), strObject, NULL, 1, NULL, NULL, ...);
pFile->SendRequest(headers..., body...);
while (pFile->ReadString(data)) content += data;
result = CCommon::StrToUnicode(string{(const char*)content.GetString()}, CodeType::UTF8);
...
catch (CInternetException* e) {
    if (ERROR_INTERNET_TIMEOUT == dwErrorCode) return OUTTIME; else return FAILURE;
}
```
- 用 MFC 的 `CInternetSession` / `CHttpConnection` / `CHttpFile`（底层 WinINet）。
- 返回值枚举：`HttpResult { SUCCESS = 0, FAILURE = 1, OUTTIME = 2 }`。
- **没有任何 `INTERNET_OPTION_*TIMEOUT` 设置**，靠 WinINet 默认值（连接/接收默认约 30s + 重试）。全仓 grep `INTERNET_OPTION_CONNECT_TIMEOUT` / `_RECEIVE_TIMEOUT` 零命中。
- 结果**无条件按 UTF-8 解码**（`CodeType::UTF8`），所以服务器返回 GBK 就是乱码。
- `SECURE_REQUEST` 里带 `INTERNET_FLAG_IGNORE_CERT_CN_INVALID` —— 不校验证书 CN。
- 明文 `http://` 的网易接口在部分网络下会被运营商劫持/返回 302 登录页。
- 一个拼写错误：自定义 UA 是 `L"MuiscPlayer2"`（Mus**ic** → Muis**c** 写反了），全项目一致（两处），算是有意为之的"指纹"或者单纯没改。

**线程策略**：
【源码确证】`LyricDownloadDlg.h`，两个 `CWinThread*`：`m_pSearchThread` / `m_pDownThread`，各自对应 `LyricSearchThreadFunc` / `LyricDownloadThreadFunc`，通过网络线程 + `PostMessage(WM_SEARCH_COMPLATE / WM_DOWNLOAD_COMPLATE)` 回主线程。

【源码确证】`LyricDownloadDlg.cpp:LyricSearchThreadFunc()` 里有一段**作者自己写下的技术债说明**，非常值得引用：
```cpp
// 此处（以及大部分网络相关）有线程安全问题，HttpPost可能卡30s网络超时，要解决此问题
// CInternetSession的封装应当提供退出flag参数
// 此时如果歌词下载窗口关闭则pInfo会是野指针（比如关闭再打开此对话框会使得上面的检查无效）
```
这是"已知缺陷 + 已知修法"的完整记录。

**自动下载**：
【源码确证】`CommonData.h` → `GeneralSettingData::auto_download_lyric{ false }`（默认关）、`auto_download_only_tag_full{ true }`、`save_lyric_to_song_folder{ true }`、`download_lyric_text_and_translation_in_same_line{ true }`。
【源码确证】`MusicPlayerDlg.cpp:4414`
```cpp
bool download_lyric{ theApp.m_general_setting_data.auto_download_lyric
                     && CPlayer::GetInstance().m_Lyrics.IsEmpty()
                     && !midi_lyric
                     && !song_info_ori.NoOnlineLyric() };
```
`NoOnlineLyric` 是持久化在媒体库里的**负缓存标记**：搜不到就标记，下次不再重试（`SongInfo::SetNoOnlineLyric`）。这是必要的，否则每次播放都发一次网络请求。

**批量下载**：
【源码确证】`LyricBatchDownloadDlg.cpp:ThreadFunc()` —— **单线程串行**，一个 `AfxBeginThread`，`for` 遍历播放列表，每首歌独立地 搜索→匹配→下载→改名→保存，进度条 + 列表状态列。支持"跳过已存在歌词"（`pInfo->skip_exist`）。有 `theApp.m_batch_download_dialog_exit` 标志来中止（在长耗时的 HTTP 调用**之后**检查，注释说明了原因）。

---

## 6. 歌词编辑

### 6.1 控件：内嵌 Scintilla

【源码确证】`LyricEditDlg.cpp:OnInitDialog()` 附近：
```cpp
m_view = (CScintillaEditView*)RUNTIME_CLASS(CScintillaEditView)->CreateObject();
...
m_view->SetLexerLyric(theApp.m_app_setting_data.theme_color);
```
`CScintillaEditView : public CView`（`ScintillaEditView.h`），包了一堆 `SCI_*` 消息的 `SendMessage` 封装，包括 UTF-8 与宽字符的位置互转：
```cpp
static int CharactorPosToBytePos(int pos, const wchar_t* str, size_t size);  // 字符位置 → 字节位置(UTF8)
static int BytePosToCharactorPos(int pos, const char* str, size_t size);     // 反向
```
Scintilla 内部是 UTF-8 字节，界面层用 `wstring`，所以每次读写都要转位置 —— 这是内嵌 Scintilla 的必备功课。

Scintilla **是运行时动态加载的**（`theApp.IsScintillaLoaded()`，`MusicPlayerDlg.cpp:OnEditLyric()` 里没加载就报错退出），Scintilla 的头文件是随仓库一起带的副本（仓库根 `scintilla/` 目录，完整的 Scintilla 源码树）。

还有个贴心的 RAII 小工具：
```cpp
struct KeepCurrentLine {          // ScintillaEditView.h
    KeepCurrentLine(CScintillaEditView* view) : m_view(view) { current_line = m_view->GetFirstVisibleLine(); }
    ~KeepCurrentLine() { m_view->SetFirstVisibleLine(current_line); }
};
```
因为每次操作都是"整篇 `SetTextW()` 重设",不保存滚动位置光标就会跳回顶部。全文件里有 **20+ 处** `CScintillaEditView::KeepCurrentLine keep_cur_line(m_view);`。

### 6.2 语法高亮：自定义 Scintilla Lexer

【源码确证】`scintilla/lexers/LexLyric.cxx`（**作者自己写的词法分析器**，115 行）

- 注册：`LexerModule lmLyric(SCLEX_LYRIC, ColouriseLyricDoc, "Lyric");`
- 样式常量（`scintilla/include/SciLexer.h`）：
  ```c
  #define SCLEX_LYRIC 201
  #define SCE_LYRIC_DEFAULT 0
  #define SCE_LYRIC_TIMETAG 1
  #define SCE_LYRIC_TIME_TAG_KEYWORD 2
  #define SCE_LYRIC_TEXT 3
  #define SCE_LYRIC_SEPARATOR 4
  #define SCE_LYRIC_TRANSLATION 5
  ```
- 着色逻辑（`ColouriseMakeLine`）就是个逐字符状态机：
  ```cpp
  if (ch == '[')  { curStyle = SCE_LYRIC_TIMETAG; }
  if (ch == ':')  { if (keywords.InList(strBuff.substr(tag_start_pos+1, i-tag_start_pos-1).c_str()))
                        curStyle = SCE_LYRIC_TIME_TAG_KEYWORD; }     // ar/ti/al/by/id
  if (ch == ']')  { curStyle = SCE_LYRIC_TEXT; }
  if (i > 2 && lineBuffer[i-2]==' ' && lineBuffer[i-1]=='/' && lineBuffer[i]==' ')
                  { curStyle = SCE_LYRIC_TRANSLATION; }              // " / " 之后是翻译
  ```
- 行缓冲固定 `char lineBuffer[1024]`，超长行会被**截成多段分别着色**（`linePos >= sizeof(lineBuffer)-1` 时强制断行）。
- 关键词表在 `ScintillaEditView.cpp:SetLexerLyric()`：`SetKeywords(0, "ar ti al by id")`。
- 颜色映射：`SCE_LYRIC_TIMETAG/TIME_TAG_KEYWORD` → `theme_color.dark1_5`；`SEPARATOR` → `light1`；`TRANSLATION` → `dark2_5`；关键词额外加粗。
- 当前行高亮：`SCI_SETCARETLINEVISIBLE TRUE` + `SCI_SETCARETLINEBACK theme_color.light3`。

### 6.3 时间标签的插入 / 替换 / 删除

【源码确证】`LyricEditDlg.h:enum class TagOpreation { INSERT, REPLACE, DELETE_ };`
【源码确证】`LyricEditDlg.cpp:CLyricEditDlg::OpreateTag(TagOpreation operation)`

```cpp
if (m_lyric_type == CLyrics::LyricType::LY_KSC) return;      // KSC 不支持（单引号格式不同）
m_view->GetSel(start, end);
line_start = m_lyric_string.rfind(L"\r\n", start - 1);       // 向前找行首
line_start = (npos || start == 0) ? 0 : line_start + 2;
line_end   = m_lyric_string.find(L"\r\n", end);

CPlayTime time_tag{ CPlayer::GetInstance().GetCurrentPosition() };   // ← 取当前播放位置
wchar_t time_tag_str[16];
swprintf_s(time_tag_str, L"[%.2d:%.2d.%.2d]", time_tag.min, time_tag.sec, time_tag.msec / 10);   // ← msec/10

// 找当前行的标签边界（tag_end < tag_start 或 tag_end > line_end 都判为"没有标签"）
switch (operation) {
case INSERT:   m_lyric_string.insert(line_start, time_tag_str); break;                // 行首插入
case REPLACE:  if (无标签) insert(line_start, ...);
               else        replace(tag_start, tag_end-tag_start+1, time_tag_str, ...); break;
case DELETE_:  m_lyric_string.erase(tag_start, tag_end - tag_start + 1); break;
}
// 然后：整篇 SetTextW 回写 + 把光标移到下一行（INSERT/REPLACE）或本行行首（DELETE）
```

**快捷键（从 Tooltip 代码读出来的）**：`F8` 插入 / `F9` 替换 / `F10` 删除 / `Ctrl+S` 保存 / `Ctrl+P` 播放暂停 / `Ctrl+←` 快退 / `Ctrl+→` 快进 / `Ctrl+F` 查找 / `Ctrl+H` 替换 / `Ctrl+G` 定位到当前播放位置对应行。

**"边听边打轴"工作流**：播放 → `F9` 把光标所在行的时间标签替换成当前播放位置 → 光标自动跳到下一行。这就是整轴制作的交互。

### 6.4 其他编辑操作

都是"`CLyrics` 解析 → 改结构 → `GetLyricsString2()` 序列化 → 整篇回写"的模式：

| 命令 | 实现 |
|---|---|
| 合并相同时间标签 | `OnLryicMergeSameTimeTag()` → `lyrics.CombineSameTimeLyric()` |
| 交换原文/翻译 | `OnLyricSwapTextAndTranslation()` → `SwapTextAndTranslation()` |
| 时间标签提前一句 | `OnLyricTimeTagForward()` → `TimeTagForward()`（后一句的时间覆盖前一句） |
| 时间标签延后一句 | `OnLyricTimeTagDelay()` → `TimeTagDelay()`（反向遍历，前一句覆盖后一句） |
| 原文/翻译同行 | `OnLyricAndTranslationInSameLine()` → `GetLyricsString2(true)` |
| 原文/翻译分行 | `OnLyricAndTranslationInDifferentLine()` → `GetLyricsString2(false)` |
| 繁简转换 | `OnLeTranslateToSimplifiedChinese()` / `...TranditionalChinese()` → `CCommon::TranslateToSimplified/TranditionalChinese` |
| 定位播放位置 | `OnSeekToCurLine()` → `m_view->GetCurrentLineTextW()` → `CLyrics::ParseLyricTimeTag(...)` → `CPlayer::SeekTo(t.toInt())` |
| 查找/替换 | MFC 标准 `CFindReplaceDialog`（`WM_FINDREPLACE`） |
| 整体延时修正 | **不在编辑器里** —— 在播放界面的 `ID_LYRIC_FORWARD / ID_LYRIC_DELAY`（±500ms，见 §4.4） |

编辑器的编码处理：状态栏显示当前编码（`TXT_LYRIC_EDIT_UTF8NOBOM` 等），保存时 `SaveLyric()` 分两条路 —— 内嵌歌词走 `CAudioTag::WriteAudioLyric()`，外置走 `ofstream`。内嵌歌词保存时会 `CPlayer::ReOpen reopen(true)` 先独占重开文件。

一个细节：**命令是否可用依赖 `is_lrc`**（`OnInitMenu()` 里 `EnableMenuItem(..., is_lrc ? MF_ENABLED : MF_GRAYED)`）—— KSC / VTT 下这些时间标签操作全部灰掉，因为 `GetLyricsString2()` 对 KSC/VTT 是**重新生成**（KSC 会重写 `karaoke.add(...)`，VTT 会重写整个 cue 表 + 重新编号），改不起。

---

## 7. 其他显示面

### 7.1 桌面歌词 `CDesktopLyric`

【源码确证】`LyricsWindow.cpp:CLyricsWindow::Create(class, width, height)`
```cpp
DWORD dwStyle   = WS_POPUP | WS_VISIBLE | WS_THICKFRAME;
DWORD dwExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
// 默认宽度 = 桌面工作区宽度的 2/3，高 150（DPI 缩放），位置贴屏幕底部居中
CWnd::CreateEx(dwExStyle, lpszClassName, NULL, dwStyle, x, y, nWidth, nHeight, NULL, NULL);
```
- `WS_EX_LAYERED` + `UpdateLayeredWindow` 做**逐像素 alpha 透明**：
  ```cpp
  HBITMAP hBitmap = CreateDIBSection(m_hCacheDC, &bitmapinfo /*32bpp BI_RGB*/, 0, NULL, 0, 0);
  Gdiplus::Graphics* pGraphics = new Gdiplus::Graphics(m_hCacheDC);
  pGraphics->SetSmoothingMode(SmoothingModeAntiAlias);
  pGraphics->SetTextRenderingHint(TextRenderingHintAntiAlias);
  PreDrawLyric(pGraphics, m_pFont);   // 取歌词 / 进度
  DrawLyrics / DrawLyricsDoubleLine;
  AfterDrawLyric(pGraphics);          // 画工具条
  ::UpdateLayeredWindow(m_hWnd, hDC, NULL, &psize, m_hCacheDC, &DestPt, 0, &blendFunc32bpp /*AC_SRC_ALPHA*/, ULW_ALPHA);
  ```
- **`WS_EX_TOOLWINDOW`**：不出现在 Alt-Tab / 任务栏。
- **鼠标穿透**：`DesktopLyric.cpp:SetWindowStyle()`
  ```cpp
  if (m_bLocked) ModifyStyleEx(NULL, WS_EX_TRANSPARENT); else ModifyStyleEx(WS_EX_TRANSPARENT, NULL);
  if (m_bLocked || m_lyricBackgroundPenetrate) ModifyStyle(WS_THICKFRAME, NULL);
  else                                          ModifyStyle(NULL, WS_THICKFRAME);
  ```
  锁定时加 `WS_EX_TRANSPARENT`（穿透）+ 去掉 `WS_THICKFRAME`（不可缩放）。
- **"锁定但想解锁"的骚操作**（`DesktopLyric.cpp:OnTimer`）：一个 200ms 定时器持续用 `GetCursorPos` 检测鼠标是否落在解锁按钮矩形内，是就**临时摘掉 `WS_EX_TRANSPARENT`** 让按钮能点，否则再加回去。
- **拖动窗口**：`OnLButtonDown` 里 `PostMessage(WM_NCLBUTTONDOWN, HTCAPTION, ...)` 伪装成标题栏拖动；点击前先遍历工具栏按钮矩形排除掉。
- 工具条按钮：播放控制（上一曲/播放暂停/下一曲/停止）、锁定、双行、背景穿透、设置、**歌词提前/延后**、三套预设样式、关闭。鼠标悬停时才画（`if (m_lyricBackgroundPenetrate || m_bLocked ? m_bMouseInWindowRect : m_bHover)`），未锁定时背景铺一层 `alpha = m_bHover ? 80 : 1` 的白色，制造"鼠标移入才浮现"的效果。
- DPI 适配：`CLyricsWindow::Create(theApp.DPI(150))`。
- 三套预设样式的默认值硬编码在 `LoadDefaultStyle()` 里（绿色系/橙红系/紫蓝系，`RGB(55,138,23)` 之类）。
- 歌词文本绘制见 §3.2 的 `DrawHighlightLyrics`。字体/颜色/描边/阴影全部可配（`SetLyricsColor / SetLyricsBorder / SetHighlightColor / SetHighlightBorder / SetLyricsShadow / SetLyricsFont`）。
- 渐变画刷是自定义的（`CreateGradientBrush`）：**不用 GDI+ 自带的双色渐变，而是取 10 个插值点，用 `CDrawingManager::SmartMixColors` 算中间色再 `SetInterpolationColors`**。注释解释了原因：
  > "CDrawingManager::SmartMixColors 产生的渐变色更好，GDI+ 默认的渐变色有很多灰色"
  三色渐变是靠 `SetWrapMode(WrapModeTileFlipXY)` + 只加到半高实现的（注释："这里的三色渐变是靠环绕模式对映"）。

### 7.2 Cortana / Windows 搜索框歌词 `CCortanaLyric`

这是整套代码里最"硬核"的部分。

【源码确证】`CortanaLyric.cpp:CCortanaLyric::Init()`
```cpp
HWND hTaskBar = ::FindWindow(_T("Shell_TrayWnd"), NULL);            // 任务栏
if (CWinVersionHelper::IsWindows11OrLater()) {
    // Win11：搜索框是 SIBTrayButton（注意：注释说是 StartAllBack 实现的）
    m_hCortanaBar = ::FindWindowEx(hTaskBar, NULL, _T("SIBTrayButton"), NULL);
    // 用"客户区宽度 >= DPI(120)"来在多个 SIBTrayButton 里挑出真正的搜索框
    if (!isWindowSearchBox(m_hCortanaBar))
        m_hCortanaBar = ::FindWindowEx(hTaskBar, m_hCortanaBar, _T("SIBTrayButton"), NULL);
    ...
}
if (m_hCortanaBar == NULL) {
    // Win10：TrayDummySearchControl 里面有两个子窗口
    m_hCortanaBar     = ::FindWindowEx(hTaskBar, NULL, _T("TrayDummySearchControl"), NULL);
    m_cortana_hwnd    = ::FindWindowEx(m_hCortanaBar, NULL, _T("Button"), NULL);
    m_hCortanaStatic  = ::FindWindowEx(m_hCortanaBar, NULL, _T("Static"), NULL);
}
::GetWindowText(m_cortana_hwnd, buff, 31);   // 保存原始文本，退出时恢复
m_pDC = m_cortana_wnd->GetDC();              // ← 直接拿系统窗口的 DC
m_draw.Create(m_pDC, m_cortana_wnd->GetFont());
```

然后 `DrawInfo()`（由 UI 线程每帧调用）：
```cpp
// 模式 A（默认，Win11 强制）：直接往搜索框的 DC 上画
CDrawDoubleBuffer drawDoubleBuffer(m_pDC, m_cortana_rect);       // 双缓冲
m_draw.SetDC(drawDoubleBuffer.GetMemDC());
m_draw.FillRect(m_cortana_rect, m_colors.back_color);
if (cortana_show_spectrum) DrawSpectrum();
m_draw.DrawLyricTextMultiLine / DrawLyricTextSingleLine(...);    // 复用同一套歌词绘制
SetBeatAmp(spectrum_avg * 22000);                                // 让 Cortana 图标跟着节奏缩放
AlbumCoverEnable(cortana_show_album_cover); DrawAlbumCover(...);
// 模式 B（兼容模式，仅 Win10）：SetWindowText 改静态控件文本
pWnd->SetWindowText(str_disp.c_str()); pWnd->Invalidate();
```

配套：`SetCortanaBarOpaque(bool)` 给搜索框设置透明色；`CheckWindows10LightTheme()` 判深色模式；`ApplySearchBoxTransparentChanged()`。绘图内容会**盖住**系统原本的搜索框内容，程序退出时恢复原文本。

**【推测】**这种"往别的进程窗口 DC 上画"的做法依赖任务栏窗口类名（`Shell_TrayWnd` / `TrayDummySearchControl` / `SIBTrayButton`）和窗口树结构。Windows 更新、Explorer 重启、第三方任务栏增强工具都会让它失效 —— 源码里那个"宽度 >= 120 DPI"的启发式判断就是被逼出来的补丁。这也是它做"兼容模式"fallback 的原因。

### 7.3 任务栏缩略图歌词

**没有独立的"任务栏缩略图歌词"实现**。【源码确证】

`MusicPlayerDlg.cpp:CMusicPlayerDlg::TaskBarInit()` 用的是 `ITaskbarList3`：
```cpp
theApp.GetITaskbarList3()->ThumbBarAddButtons(m_hWnd, 3, m_thumbButton);       // 缩略图上的 3 个按钮
CRect thumbnail_rect = m_pUI->GetThumbnailClipArea();
theApp.GetITaskbarList3()->SetThumbnailClip(m_hWnd, thumbnail_rect);           // 只截取界面的某个区域作为缩略图
```
`CPlayerUIBase::GetThumbnailClipArea()` 返回的是**主界面里某个界面元素的矩形**（在 `CPlayerUIBase.cpp:162` 处、检测到绘图区尺寸变化或界面切换时才更新）。也就是说：**缩略图显示的是主界面的一部分，歌词是不是在里面取决于当前 UI 布局把歌词元素放在哪。** 没有额外的"在缩略图上单独画歌词"的代码。

`UpdateTaskBarProgress()` 另外用 `SetProgressState/SetProgressValue` 画进度条、用 `SetOverlayIcon` 画播放/暂停角标。

### 7.4 MIDI 歌词

【源码确证】`Player.h: wstring GetMidiLyric() const { return m_pCore->GetMidiInnerLyric(); }`，`BassCore.cpp:GetMidiPosition()`（`BASS_ChannelGetPosition(m_musicStream, BASS_POS_MIDI_TICK)`）。开启 `midi_use_inner_lyric` 时，歌词来源变成 MIDI 文件内部的歌词事件，**走的是和 `CLyrics` 完全平行的另一条路**（`CPlayerUIHelper::IsMidiLyric()` 在每个绘制入口都要判一次），因此 MIDI 播放时的时间标签操作（提前/延后）被禁用。

---

## 8. 坑与限制

### 8.1 【源码确证】的已知脆弱点

| # | 问题 | 位置 | 说明 |
|---|---|---|---|
| 1 | **`m_time_start` 去重叠强插 10ms** | `Lyric.cpp:NormalizeLyric()` `last += 10;` | 时间标签完全相同或负偏移堆积到 0 时，每行会被拉开 10ms。副作用：`CombineSameTimeLyric()` 的默认 `error=0` 是刻意保持的（源码有长注释解释）。 |
| 2 | **超出 100 分钟就截断** | `Lyric.cpp:DeleteRedundantLyric()` `if (time_start >= 6000000)` | **把这句之后的所有歌词全删掉**。正常歌曲无影响，但整轨 CUE / 长音频 / 有声书会被腰斩。只在 `LY_LRC_NETEASE` 路径调用。 |
| 3 | **无 BOM 的 GBK 靠"不是合法 UTF-8"兜底** | `Common.cpp:JudgeCodeType` | 非中文区域设置下 ANSI 歌词直接乱码。且 `IsUTF8Bytes` 对短文本的误判率不低。 |
| 4 | **HTTP 无超时设置** | `InternetCommon.cpp:SendHttpRequest` | 作者自己在 `LyricDownloadDlg.cpp:442` 写了注释："HttpPost 可能卡 30s 网络超时……此时如果歌词下载窗口关闭则 pInfo 会是野指针"。**这是源码里明确承认的线程安全 bug。** |
| 5 | **网易歌词字符串用字符偏移硬抠 JSON** | `NeteaseLyricDownload.cpp:DisposeLryic()` `substr(index1, size - index1 - 13)` | 那个 `13` 是数出来的魔数。接口响应格式一变就解析错。 |
| 6 | **手写 JSON 转义还原** | 同上 | 逐个字符扫描 `\r \n \"`，四字符序列 `\r\n` 的处理还有 `if (i < size - 3)` 边界条件。 |
| 7 | **`URLEncode` 的 `%x` 不补零** | `InternetCommon.cpp:URLEncode` `swprintf_s(buff, L"%%%x", ...)` | 0x01~0x0F 的字节会编码成 `%a` 而非 `%0a`。中文（≥0x80）踩不到，非 ASCII 单字节编码踩得到。 |
| 8 | **QQ音乐 `trans` 缺失被当失败** | `QQMusicLyricDownload.cpp:DisposeLryic()` 用 `.at("trans")` | 没有翻译字段时抛异常 → `catch` → `return false` → 界面报"下载失败"，而不是"无翻译"。 |
| 9 | **匹配权重与注释不符、时长未参与匹配** | `LyricDownloadCommon.cpp:SelectMatchedItem()` | 注释写"文件名——艺术家 0.2"实际是 0.3；注释写"时长 0.6"代码里**完全没用 duration**。→ 录音室版 / 现场版 / 翻唱版会误匹配。 |
| 10 | **`GetLyricIndex` 是 O(n) 线性扫描且每帧被调用多次** | `Lyric.cpp:GetLyricIndex()` | 主界面一帧内 `DrawLyricTextMultiLine` 调 1 次、`GetLyricProgress` 内部又调 1 次；单行模式再加 `GetLyric()` 里的。歌词 200 行时每帧几百次比较，不致命但不优雅。 |
| 11 | **`CPlayTime::toLyricTimeTag()` 只有两位厘秒** | `PlayTime.h` `swprintf_s(buff, L"[%.2d:%.2d.%.2d]", min, sec, msec / 10)` | 保存歌词必然丢失 10ms 精度。逐字歌词每保存一次就累积一次舍入误差。 |
| 12 | **`OpreateTag` 插入的标签同样只有两位厘秒** | `LyricEditDlg.cpp:42` `time_tag.msec / 10` | 手打轴的分辨率上限是 10ms。 |
| 13 | **`ParseLyricTimeTag` 的 `size - 9` 早退** | `Lyric.cpp:81` | 时间标签必须在离行尾 ≥9 字符处；极短行 / 畸形标签会被静默丢弃。 |
| 14 | **LRC 里 `" / "` 被无条件当作原文/译文分隔符** | `Lyric.cpp:DisposeLrc()` `text_str.find(L" / ")` | 歌词正文里出现 `" / "`（比如 "Rock / Pop"）会被误切。注释说"由于 StringNormalize 不可能出现后面为空" —— 依赖 `StringNormalize` 的行为。 |
| 15 | **长歌词行在 Scintilla 里按 1024 字节硬截** | `LexLyric.cxx:ColouriseLyricDoc` `lineBuffer[1024]` | 只是着色分段，不是数据丢失，但超长行的语法高亮会错位。 |
| 16 | **HTTP 结果无条件按 UTF-8 解码** | `InternetCommon.cpp:145` `StrToUnicode(..., CodeType::UTF8)` | 服务器换了编码就乱码。 |
| 17 | **`INTERNET_FLAG_IGNORE_CERT_CN_INVALID`** | `InternetCommon.h` `SECURE_REQUEST` | 不校验证书 CN。 |
| 18 | **网易用明文 http** | `NeteaseLyricDownload.cpp` | 运营商劫持 / 302 登录页会变成"解析错误"而不是明确报错。 |
| 19 | **批量下载串行 + 长耗时不可中断** | `LyricBatchDownloadDlg.cpp:ThreadFunc()` | 退出标志只在 HTTP 调用**之后**检查，用户点取消最多要等一次超时。 |
| 20 | **歌词文件搜索扩展名只有 3 个** | `Lyric.cpp:6` | 用户手动放一个 `.txt` 或 `.LRC`（大写）都找不到 —— `ReplaceFileExtension` 和 `FileExist` 都是精确匹配。**没有任何大小写不敏感的处理。** |

### 8.2 【推测】的观察

1. **滚动观感不均匀**（§4.1）：因为滚动位移直接绑定 `progress`，而行时长不一致 → 每行滚动速度不同。更稳的做法是"行级 snap + `animateTo` 缓动"。这是设计取舍，不是 bug，但对"平滑"这个体感目标有影响。
2. **`GetLyricProgress` 每帧 3~5 次文本测量**：`GetTextExtent` / `MeasureString` 在有 GDI+ 的情况下不算极便宜。缓存"该行各段的像素宽度"（按字体+字号为 key）能省掉绝大部分开销。目前没缓存。
3. **多版本歌曲匹配**：主要靠标题/艺术家的编辑距离 + 列表位置权重，没有时长、没有专辑年份、没有 `track` 号。对于"同一首歌的多个版本"是明确的弱项。
4. **`m_surpported_lyric` 里没有 `txt` / `LRC`**：如果用户在别处（比如 foobar2000 的歌词插件）生成了 `.txt` 歌词，本程序完全看不见。
5. **`CCortanaLyric` 依赖任务栏窗口类名**（§7.2）：Windows 版本更新 / Explorer 重启 / 第三方任务栏工具都可能让它失效，且失效时通常是"画到错误窗口上"这种很难查的症状。
6. **`m_lyric_download_dialog_exit` 是全局标志**（`theApp.m_lyric_download_dialog_exit`）：两个歌词下载对话框（单个 + 批量）各有一个全局标志，但都靠"网络调用返回后检查"。作者自己在注释里指出了这个模型的边界。
7. **桌面歌词的 200ms 定时器 + UI 线程 100ms 绘制双轨**：两者独立，桌面歌词实际刷新率由 UI 线程的 100ms 决定，那个 200ms 定时器只服务于鼠标穿透状态。这不算 bug，但读代码时容易误解。

### 8.3 用户在别处可能听过的说法，与源码的出入

- **"MusicPlayer2 支持 KRC / QRC 逐字"** —— ❌ 不对。全仓没有任何 KRC/QRC/YRC 解析代码。逐字来源只有扩展 LRC `<...>`、WebVTT、和 KSC 的时长数组。
- **"歌词下载会比对时长"** —— ❌ 不对。`ItemInfo::duration` 被解析出来了，但 `SelectMatchedItem()` 里没有用到。
- **"内嵌 SYLT（同步歌词）帧会被解析"** —— ❌ 不对。`taglib/synchronizedlyricsframe.h` 在源码树里存在（TagLib 库自带），但 `CAudioTag::GetAudioLyric()` 只调 `CTagLibHelper::Get*Lyric()`，读的是 USLT / `LYRICS` 这类**非同步**歌词帧，拿回来当普通文本走 `LyricsFromRowString()`。

---

## 9. 附：关键函数速查表

| 想做的事 | 去看 |
|---|---|
| 歌词类型枚举 / 扩展名表 | `MusicPlayer2/Lyric.h:CLyrics::LyricType`、`Lyric.cpp:6` |
| 时间标签扫描器 | `Lyric.cpp:CLyrics::ParseLyricTimeTag()` |
| LRC / 扩展 LRC 解析 | `Lyric.cpp:CLyrics::DisposeLrc()` |
| 网易格式预处理 | `Lyric.cpp:CLyrics::DisposeLrcNetease()` |
| KSC 解析 | `Lyric.cpp:CLyrics::DisposeKsc()` |
| WebVTT 解析 | `Lyric.cpp:CLyrics::DisposeWebVTT()` |
| 时间轴规范化（偏移/去重叠/补 span） | `Lyric.cpp:CLyrics::NormalizeLyric()` |
| 逐字进度计算 | `Lyric.cpp:CLyrics::GetLyricProgress()` |
| 译文配对 | `Lyric.cpp:CLyrics::CombineSameTimeLyric()` |
| 序列化（LRC/KSC/VTT 三套写回） | `Lyric.cpp:CLyrics::GetLyricsString2()` |
| 多行歌词滚动绘制 | `CUIDrawer.cpp:CUIDrawer::DrawLyricTextMultiLine()` |
| 单行/双行绘制 | `CUIDrawer.cpp:DrawLyricTextSingleLine()` / `DrawLyricDoubleLine()` |
| **卡拉OK 高亮的两遍绘制** | `DrawCommon.cpp:CDrawCommon::DrawWindowText()`（5 参重载） |
| 桌面歌词的 GDI+ 裁剪高亮 | `LyricsWindow.cpp:CLyricsWindow::DrawHighlightLyrics()` |
| 分层窗口 + UpdateLayeredWindow | `LyricsWindow.cpp:CLyricsWindow::Draw()` |
| 桌面歌词鼠标穿透 | `DesktopLyric.cpp:CDesktopLyric::SetWindowStyle()` / `OnTimer()` |
| 播放位置获取 | `BassCore.cpp:CBassCore::GetCurPosition()`、`Player.cpp:GetPlayerCoreCurrentPosition()` |
| UI 刷新循环 / 限帧 | `MusicPlayerDlg.cpp:CMusicPlayerDlg::UiThreadFunc()` |
| 歌词偏移 ±500ms | `MusicPlayerDlg.cpp:OnLyricForward()/OnLyricDelay()` → `Lyric.cpp:AdjustLyric()` |
| 歌词文件查找 | `MusicPlayerCmdHelper.cpp:SearchLyricFile()/SearchLyricFiles()` |
| 下载基类 / 匹配打分 | `LyricDownloadCommon.cpp:SelectMatchedItem()`、`SearchSongAndGetMatched()` |
| 相似度算法 | `InternetCommon.cpp:StringSimilarDegree_LD()`、`CharacterSimilarDegree()` |
| HTTP 封装 | `InternetCommon.cpp:SendHttpRequest()`、`GetURL()`、`URLEncode()` |
| 网易 / QQ API | `NeteaseLyricDownload.cpp`、`QQMusicLyricDownload.cpp` |
| Scintilla 集成 / LRC 语法高亮 | `ScintillaEditView.cpp:SetLexerLyric()`、`../scintilla/lexers/LexLyric.cxx` |
| 时间标签编辑 | `LyricEditDlg.cpp:CLyricEditDlg::OpreateTag()` |
| 搜索框歌词 | `CortanaLyric.cpp:CCortanaLyric::Init()/DrawInfo()` |

---

*报告基于 master 分支的静态源码阅读，未编译、未运行。所有代码片段为说明性的节选，省略了错误处理与无关分支。*
