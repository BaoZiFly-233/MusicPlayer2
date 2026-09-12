# 造播放器前必读：从 MusicPlayer2 提取的工程实现经验

> 调研对象：[`zhongyang219/MusicPlayer2`](https://github.com/zhongyang219/MusicPlayer2)（C++ / MFC + Win32 / GPL-3.0 / 6663★）
> 调研方式：`gh api` 读远端源码 + README + Wiki + 更新日志 + Issue 历史；**同时**把 master 完整副本取到本地 `D:\BoTapMusic\MusicPlayer2\` 逐行核对。
> 本地副本 HEAD：`328af4cc02e4afd463a79943e2182fe57e6441e9`（master，最后 push 2026-09-10），程序版本 2.78。
> 四份更细的专题报告（每个结论都带「文件:函数」出处，共约 4400 行的原始调查记录）：
> - `_mp2/reports/report-engine.md` —— 音频内核 / BASS / 音效 / 频谱（1377 行）
> - `_mp2/reports/report-lyrics.md` —— 歌词系统（1136 行）
> - `_mp2/reports/report-medialib.md` —— 曲库 / 媒体库（1207 行）
> - `_mp2/reports/report-tags.md` —— 标签 / 封面（含 commit 级考古，约 1350 行）
>
> **取证方法上的一个坑（留给你复现时避雷）**：`gh api ... | Out-File` 会把 LF 归一化成 CRLF，破坏字节保真（例如 `SongInfo.h` 会从 7909 字节变成 8107 字节，用 git tree 的 `size` 字段一校验就露馅）；`gh api` 也没有 `--output` 参数。要拿保真字节得走 cmd 层重定向：`cmd /c 'gh api "<url>" -H "Accept: application/vnd.github.raw" > "file"'`。**本报告的所有行号引用以本地 clone 的原始字节为准。**

## 证据分级约定

| 标记 | 含义 |
|---|---|
| **【源码确证】** | 在源码里读到，附「文件:函数」，可复核 |
| **【文档确证】** | 来自 README / Wiki / 更新日志 / Issue 原文 |
| **【外部确认】** | 来自项目之外（官网、第三方仓库），附链接 |
| **【推测】** | 我的判断，没有直接证据 |

**一个必须先说的坑**：`Documents/Introduction.md` 现在只有一句话——

> **帮助文档已经移至 [Wiki页面](https://github.com/zhongyang219/MusicPlayer2/wiki)。**

所以「功能全貌」必须从 **Wiki（34 个页面）+ README + `Documents/update_log.md`（440 行，覆盖 V2.56→V2.78）** 一起拼。

---

# 1. 功能全貌

## 1.1 总清单（按功能域分类，标注 MVP 建议）

「核心必备」= 不做就不算一个能用的播放器；「锦上添花」= 有加分，可后置。

### A. 播放控制 —— **全部核心必备**

| 功能 | 说明 | 分级 |
|---|---|---|
| 播放/暂停/停止/上一曲/下一曲 | `CPlayer::MusicControl(Command)` | 核心 |
| 进度跳转 / 快进快退 5s | `Command::FF` / `REW` | 核心 |
| 音量（含全局鼠标滚轮调节） | 软件音量，`BASS_ATTRIB_VOL` | 核心 |
| 循环模式：顺序 / 列表循环 / 单曲循环 / 随机 / **无序播放** | 无序 = 洗牌队列可向前回溯（V2.60 加的） | 核心 |
| 记住上次播放位置、上次曲目、上次列表 | 开关可关（V2.78 加的「切换列表时从头播放」） | 核心 |
| 播放速度 / 变调（不变调变速） | 依赖 `bass_fx.dll`，V2.78 才做完整 | 锦上添花 |
| AB 重复 | A/B 两点循环 | 锦上添花 |
| 声音淡入淡出 | `BASS_ChannelSlideAttribute` | 锦上添花 |
| 播放设备选择 + 设备变化自动跟随 | 仅 BASS 内核 | 锦上添花 |
| cue 分轨播放（含内嵌 cue） | 自解析 cue，一段一段播 | **中文用户核心** |
| MIDI 播放 + sf2 音色库 | `bassmidi.dll` | 锦上添花 |
| 网络 URL / m3u 流播放 | `BASS_StreamCreateURL`、`IsURL()` 分支 | 核心（对你尤其重要） |
| osu! 安装目录歌曲 | 读 `osu!.db`，属于垂直场景 | 可砍 |

### B. 歌词 —— **核心必备（这是它的招牌）**

| 功能 | 分级 |
|---|---|
| LRC 显示（单行 / 双行 / 多行自适应） | 核心 |
| 卡拉OK 逐字高亮（扩展 LRC `<mm:ss.xx>` / KSC / WebVTT） | 核心 |
| 歌词翻译双行显示（原文 ` / ` 译文） | 核心 |
| 歌词进度提前/延后 0.5s | 核心 |
| 关联本地歌词（手动指定） | 核心 |
| 歌词编辑（Scintilla + 自定义词法高亮 + 时间标签快捷键） | 核心 |
| 歌词繁简转换 | 锦上添花 |
| 在线歌词下载（网易云 / QQ音乐，可切换源） | 核心 |
| 批量下载（按播放列表） | 锦上添花 |
| 桌面歌词（分层窗口 + 鼠标穿透 + 锁定 + 渐变高亮） | 锦上添花（但口碑极好） |
| 内嵌歌词读写（ID3 USLT / FLAC LYRICS / MP4 / WMA / WAV） | 锦上添花 |
| Windows10 任务栏搜索框歌词 | 锦上添花（且极度脆弱，见 §4） |
| 歌词不显示空白行时用 `♪♪♪` 占位 | 细节亮点 |
| 无歌词时显示歌曲信息 | 细节亮点 |

### C. 曲库 / 媒体库 —— 「小巧」定位下可以砍一半

| 功能 | 分级 |
|---|---|
| 文件夹模式（打开文件夹即播，含子文件夹开关） | 核心 |
| 播放列表（自建 / 重命名 / 删除 / 另存 m3u-m3u8 / 拖拽排序） | 核心 |
| 自动扫描媒体库目录 + 增量更新 + 启动时自动更新 | 核心 |
| 按 艺术家 / 唱片集 / 流派 / 年份 分类浏览 | 核心 |
| 所有曲目 / 最近播放 / 文件夹浏览树 | 核心 |
| 我喜欢的音乐（红心） | 核心 |
| 播放次数 / 累计听歌时间 / 歌曲分级（1-5 星） | 锦上添花 |
| 按 文件类型 / 比特率 / 分级 分类（默认隐藏，可开） | 锦上添花 |
| 播放列表「修复错误的文件路径」 | 锦上添花 |
| 同一首歌多版本合并（V2.78 新增，Beta） | 锦上添花 |
| last.fm scrobbling | 可砍 |
| 拼音全拼 / 首字母搜索 | **中文用户核心** |
| 媒体库统计对话框 | 可砍 |

### D. 标签与封面

| 功能 | 分级 |
|---|---|
| 标签读取（标题/艺术家/专辑/年份/音轨/碟号/流派/注释/专辑艺术家） | 核心 |
| 标签**写入**（V2.71 才加，覆盖 15+ 格式） | 核心（用户很在意） |
| 内嵌封面读取 / 写入 / 删除 | 核心 |
| 外部封面图片（同目录 `cover.*` / `封面文件夹` / 绝对路径） | 核心 |
| 在线封面下载（网易云 + QQ音乐） | 锦上添花 |
| 批量标签编辑、从文件名获取标签、从歌词获取标签 | 锦上添花 |
| 高级标签信息（原始 `PropertyMap` 全字段） | 锦上添花 |
| 文件重命名（模板化，V2.71） | 锦上添花 |
| 内嵌歌词读写 | 锦上添花 |

### E. 音效与可视化

| 功能 | 分级 |
|---|---|
| 10 段均衡器（±15dB，9 个预设） | 锦上添花 |
| 混响（mix / time 双参数） | 可砍 |
| 频谱分析（4~128 柱可调、倒影、峰值帽、低频居中） | 核心（视觉招牌） |
| 节拍指示器 `beatIndicator` | 锦上添花 |

### F. 界面

| 功能 | 分级 |
|---|---|
| **XML 自定义界面**（`skins/*.xml` + 87KB 的 `skin.xsd`）+ 运行时切换界面 | 这是它「小巧又百变」的根本，见 §7 |
| 12+ 套内置界面布局（`Ctrl+U` 切换，`Ctrl+数字` 直达） | 锦上添花 |
| 深色/浅色模式（跟随系统） | 核心 |
| 主题色（跟随系统强调色 / 自定义） | 核心 |
| 迷你模式（3 种布局，可自定义 XML） | 锦上添花 |
| 全屏模式（元素放大 1.5 倍） | 锦上添花 |
| 背景：专辑封面 / 桌面壁纸 / 指定图片 + **高斯模糊** + 不透明度 | 锦上添花 |
| 浮动播放列表 / 停靠播放列表 | 锦上添花 |
| 自绘标题栏 / 菜单栏 / 状态栏 | 锦上添花 |
| 总是置顶 / 迷你模式多显示器 | 锦上添花 |
| 多语言（中/英/繁）| 锦上添花 |

### G. 系统集成

| 功能 | 分级 |
|---|---|
| 全局快捷键 | 核心 |
| 系统媒体控件 SMTC（多媒体键 / 锁屏 / 蓝牙耳机） | **核心**（现代播放器必须有） |
| 任务栏缩略图按钮（播放/暂停/上一曲/下一曲） | 核心 |
| 任务栏进度条 | 锦上添花 |
| 通知区图标（含深浅色自适应） | 核心 |
| 文件关联 / 创建快捷方式（含迷你模式快捷方式） | 锦上添花 |
| 开机自启动 | 锦上添花 |
| 崩溃时生成 dump + 调用栈对话框 | **核心**（工程必备） |
| 检查更新（GitHub / Gitee 双源） | 锦上添花 |
| 便携模式（`global_cfg.ini` 里的 `portable_mode`） | 核心（好设计） |

### H. 其他

格式转换（WAV/MP3/WMA/OGG/FLAC，走 `Encoder\` 里的 lame/oggenc/flac.exe）—— 可砍，但它是 `bassenc` 的唯一用途。

## 1.2 「小巧」的真相

Wiki《程序文件说明》列出了程序目录的内容：`MusicPlayer2.exe`、`bass.dll`、`bass_fx.dll`、`tag.dll`、`SciLexer.dll`、`Plugins\*.dll`（6 个）、`Encoder\`（4 个 exe + 2 dll）、`skins\*.xml`、`playlist\`、`language\`、`config.ini`、`global_cfg.ini`、`song_data.dat`、`recent_list.dat`、`user_ui.dat`、`error.log`、`default_background.jpg`。

**它「小巧」不是因为功能少，而是因为：DLL 都在旁边、界面全在 XML 里、没有 .NET/Electron 运行时。** 这套结构值得学：主程序小 + 旁挂资源。

---

# 2. 音频播放链路

## 2.1 内核抽象：`IPlayerCore` —— **值得抄，但要改三处**

【源码确证】`MusicPlayer2/IPlayerCore.h` 定义了三后端统一接口：

```cpp
enum PlayerCoreType { PT_BASS, PT_MCI, PT_FFMPEG };

class IPlayerCore {
    virtual void InitCore() / UnInitCore() = 0;
    virtual void Open(const wchar_t* file_path) = 0;
    virtual void Play() / Pause() / Stop() / Close() = 0;
    virtual void SetVolume(int volume) = 0;
    virtual void SetSpeed(float speed) = 0;      // 1.0 = 原速，范围 [0.1, 4.0]
    virtual void SetPitch(int pitch) = 0;        // 半音为单位，[-12, +12]
    virtual bool IsSpeedAvailable() / IsPitchAvailable() = 0;   // ← 能力查询
    virtual bool SongIsOver() = 0;
    virtual int GetCurPosition() / GetSongLength() = 0;         // 毫秒
    virtual void SetCurPosition(int position) = 0;
    virtual void GetAudioInfo(const wchar_t*, AudioInfo*, AudioTag*) = 0;  // 注释明确要求「支持并发且不影响当前播放」
    virtual void ApplyEqualizer(int channel, int gain) = 0;     // 0~9 通道，-15~+15 dB
    virtual void SetReverb(int mix, int time) / ClearReverb() = 0;
    virtual void GetFFTData(float fft_data[FFT_SAMPLE]) = 0;
    virtual bool IsMidi() / GetMidiInnerLyric() / GetMidiInfo() ... = 0;
    virtual bool EncodeAudio(...) = 0;           // 格式转换
};
```

**三个优点：**
1. **接口按语义单位定义**（毫秒、半音、dB），没有泄漏任何一家后端的细节。
2. **`IsSpeedAvailable()` / `IsPitchAvailable()` / `IsFreqConvertAvailable()` 这类能力查询**，让「这个后端做不到」变成正常返回值而不是崩溃。三个后端（BASS / MCI / FFmpeg）都实现了它，其中 MCI 的音效全是空函数 —— **接口能容纳一个「什么都不会」的后端，说明抽象是够住的**。
3. **加载失败自动降级**：`CPlayer::IniPlayerCore()` 按 **MCI > FFMPEG > BASS** 的优先级尝试，`dynamic_cast<CDllLib*>` 检查 DLL 加载结果，失败就回退 BASS 并把配置改回去。

**三处必须改（否则会绑死后端）：**
1. `GetFFTData(float fft_data[FFT_SAMPLE])` —— **把实现常量 512 写进了接口签名**。应该改成 `GetFFTData(float* out, int count, int sample_rate)`。
2. MIDI 的 5 个方法（`IsMidi` / `GetMidiInfo` / `GetMidiInnerLyric` / `IsMidiConnotPlay` / `MidiNoLyric`）直接泄漏到通用接口上，绝大多数后端都是 `return false` / 空串。
3. `EncodeAudio(..., void* encode_para, ...)` 用 `void*` 做类型擦除 —— 格式转换这种「编解码器专属」的能力，不该放在播放接口里，应该独立成一个 `IAudioEncoder`。

## 2.2 BASS 组件分工表

**先说一个容易搞错的事实：`bass.dll` 是静态导入的**（`MusicPlayer2.vcxproj` 里有 `bass.lib;bass_x64.lib`；`Define.h` 里的 `#pragma comment(lib,"bass.lib")` 是被注释掉的），**其余全是动态 `LoadLibrary`**。【源码确证】

| 组件 | 在本项目里的职责 | 关键 API | 仓库里有吗 |
|---|---|---|---|
| `bass.dll` | 解码 + 输出 + FFT + DX8 音效宿主 | `BASS_Init`、`BASS_StreamCreateFile/URL`、`BASS_Channel*`、`BASS_ChannelSetFX`、`BASS_ChannelSetSync`、`BASS_ChannelGetData`、`BASS_PluginLoad/PluginGetInfo` | ❌ 只有 `Debug/` 残留 |
| `bass_fx.dll` | **只干一件事：变速不变调 / 变调** | `BASS_FX_TempoCreate(chan, BASS_FX_FREESOURCE)` → `BASS_ATTRIB_TEMPO` / `BASS_ATTRIB_TEMPO_PITCH` | ❌ 只有 `Debug/` 残留 |
| `bassmix.dll` | **只用在格式转换时的重采样** | `BASS_Mixer_StreamCreate(dest_freq, chans, BASS_MIXER_END\|BASS_STREAM_DECODE)` + `BASS_Mixer_StreamAddChannel` | ✅ `Encoder/` |
| `bassenc.dll` | 格式转换的输出端，把 PCM 喂给命令行编码器 | `BASS_Encode_StartW(cmdline, BASS_ENCODE_AUTOFREE\|BASS_ENCODE_PCM)`、`BASS_Encode_Stop` | ✅ `Encoder/` |
| `bassmidi.dll` | MIDI 播放 + SF2 音色库 + **读 MIDI 内嵌歌词** | `BASS_MIDI_FontInit/StreamSetFonts/FontGetInfo`、`BASS_MIDI_StreamGetEvent(TEMPO)`、`BASS_MIDI_StreamGetMark` | ✅ `Plugins/` |
| `basswma.dll` | **编码** WMA（解码 WMA 是 Plugins 下同名 dll 当插件） | `BASS_WMA_EncodeOpenFileW`、`BASS_WMA_EncodeWrite/Close/SetTag` | ✅ 两处都有 |
| `bassflac` / `bass_aac` / `bass_ape` / `basscd` | 纯解码插件，**代码里零专属调用** | 靠 `BASS_PluginLoad` 自动接管 | ✅ `Plugins/` |
| `bassopus` / `basswv` / `bass_ac3` / `bass_mpc` / `bass_spx` / `bass_tta` / `bassdsd` / `bassalac` | 纯解码插件 | 同上 | ❌ 用户自行下载 |
| `ffmpeg_core.dll` | 第二内核（见 §2.5） | 50+ 个扁平 C 函数指针 | ❌ 用户自行下载 |
| `tag.dll` | TagLib 封装（标签/封面） | —— | ❌ 只有残留 |

### 为什么全部动态加载 —— `CDllLib`（30 行，最值得抄的设计之一）

【源码确证】`MusicPlayer2/DllLib.h` + `.cpp`：

```cpp
class CDllLib {
public:
    void Init(const wstring& dll_path);   // LoadLibrary + 调虚函数 GetFunction()
    void UnInit();
    bool IsSucceed();                     // m_dll_module != NULL && GetFunction() 全成功
protected:
    virtual bool GetFunction() = 0;       // 子类各自 GetProcAddress
    HMODULE m_dll_module;  bool m_success{ false };
};
```

动机（从代码结构直接读得出）：
1. **可选依赖不拖垮主程序**。`bass_fx.dll` 不在 → 变速降级成改采样率（会变调）、变调直接不可用（`IsPitchAvailable()` 返回 `m_bass_fx_lib.IsSucceed()`），但程序照跑。
2. **不用的东西不进内存**。`bassenc` / `bassmix` / 编码器 exe 只在点「格式转换」时才初始化。
3. **插件可被用户自行增删**，加载失败只是少一种格式。

### 插件清单是「运行时自报」的 —— 这段设计很聪明

【源码确证】`BassCore.cpp:104-136` `CBassCore::InitCore()`：

```cpp
plugin_dir = theApp.m_local_dir + L"Plugins\\";
CCommon::GetFiles(plugin_dir + L"*.dll", plugin_files);
for (const auto& plugin_file : plugin_files) {
    HPLUGIN handle = BASS_PluginLoad((plugin_dir + plugin_file).c_str(), 0);
    m_plugin_handles.push_back(handle);
    const BASS_PLUGININFO* info = BASS_PluginGetInfo(handle);
    if (info == nullptr) continue;
    format.description      = CCommon::ASCIIToUnicode(info->formats->name);
    format.extensions_list  = CCommon::ASCIIToUnicode(info->formats->exts);   // "*.flac;*.fla;"
    CAudioCommon::m_surpported_format.push_back(format);
}
```

「帮助 → 支持的格式」对话框直接渲染 `CAudioCommon::m_surpported_format`。**用户丢一个新 dll 进 `Plugins\` 就多一种格式，不需要改代码、不需要改 UI。**

> 两处该改的：① 靠 `info->formats->name == "MIDI"` 字符串判 bassmidi，插件改名就失效，且只读了 `formats[0]`（一个插件多格式时会漏）；② 解析扩展名字符串时 `npos` 参与了算术（`BassCore.cpp:126`），靠 `substr` 内部截断才没崩。

### 支持的音频格式矩阵（BASS 内核）

| 格式 | 扩展名 | 标签读 | 标签写 | 封面读 | 封面写 | 靠哪个组件 |
|---|---|:-:|:-:|:-:|:-:|---|
| MPEG 音频 | mp1, mp2, mp3 | ✔ | ✔ | ✔ | ✔ | bass 核心 |
| FLAC | flac | ✔ | ✔ | ✔ | ✔ | `bassflac.dll` |
| WMA | wma | ✔ | ✔ | ✔ | ✔ | `basswma.dll` |
| ASF | asf | ✔ | ✔ | ✔ | ✔ | bass 核心 |
| WAV | wav | ✔ | ✔ | ✔ | ✔ | bass 核心 |
| OGG Vorbis | ogg, oga | ✔ | ✔ | ✔ | ✔ | bass 核心 |
| ALAC | m4a | ✔ | ✔ | ✔ | ✔ | `bassalac.dll` |
| Monkey's Audio | ape | ✔ | ✔ | ✔ | ✔ | `bass_ape.dll` |
| AAC | aac | ✔ | ✗ | ✗ | ✗ | `bass_aac.dll` |
| AIFF | aif, aiff | ✔ | ✔ | ✔ | ✔ | bass 核心 |
| CD Audio | cda | ✗ | ✗ | ✗ | ✗ | `basscd.dll` |
| DSD | dff, dsf | ✗ | ✗ | ✗ | ✗ | `bassdsd.dll` |
| MIDI | mid, midi, rmi, kar | ✗ | ✗ | ✗ | ✗ | `bassmidi.dll` + sf2 |
| Opus | opus | ✔ | ✔ | ✔ | ✔ | `bassopus.dll` |
| WavPack | wv | ✔ | ✔ | ✔ | ✔ | `basswv.dll` |
| AC-3 | ac3 | ✗ | ✗ | ✗ | ✗ | `bass_ac3.dll` |
| Musepack | mpc, mp+, mpp | ✔ | ✔ | ✔ | ✔ | `bass_mpc.dll` |
| Speex | spx | ✔ | ✔ | ✔ | ✔ | `bass_spx.dll` |
| TTA | tta | ✔ | ✔ | ✔ | ✔ | `bass_tta.dll` |
| TAK | tak | ✗ | ✗ | ✗ | ✗ | 仅 32 位版（官方解码器只有 x86） |

【文档确证】Wiki《支持的音频格式》，并注明「上表仅针对 BASS 内核」。FFmpeg 内核的格式表更宽（约 40 组，含 mkv/mov/avi/m2ts/flv 等视频容器），见 §2.5。

## 2.3 播放链路：从「点一首歌」到「出声」

【源码确证】主线（BASS 内核），函数名串起来：

```
① UI 事件  →  CPlayer::MusicControl(Command::OPEN)                     Player.cpp:498
② CPlayer::MusicControl(OPEN)                                          Player.cpp:507
     → CBassCore::Open(file_path)                                      BassCore.cpp:297
         ├─ 已有流则先 Close()                              （同时只允许一个流）
         ├─ flags = BASS_SAMPLE_FLOAT；bass_fx 可用时 |= BASS_STREAM_DECODE
         ├─ CCommon::IsURL(path) ? BASS_StreamCreateURL : BASS_StreamCreateFile
         ├─ BASS_ChannelGetInfo / BASS_ChannelGetAttribute(BASS_ATTRIB_BITRATE)
         ├─ MIDI 分支：BASS_MIDI_StreamSetFonts + PPQN/TEMPO/MARK + BASS_SYNC_END
         ├─ SetFXHandle()    // 建 10 个 BASS_FX_DX8_PARAMEQ + 1 个 BASS_FX_DX8_REVERB
         └─ m_musicStream = BASS_FX_TempoCreate(stream, BASS_FX_FREESOURCE)
③ 打补丁恢复状态（换歌后 FX 句柄全没了，必须重放一遍）    Player.cpp:534-545
     SetVolume() → SetSpeed(m_speed) → SetPitch(m_pitch)
     memset(m_spectral_data, 0)
     if (m_equ_enable) SetAllEqualizer()
     if (m_reverb_enable) SetReverb(mix, time) else ClearReverb()
④ CPlayer::MusicControl(Command::PLAY) → CBassCore::Play()              BassCore.cpp:359
     淡入开启：BASS_ATTRIB_VOL=0 → BASS_ChannelPlay() → BASS_ChannelSlideAttribute(VOL, vol, fade_time)
⑤ m_controls.UpdateControls(PlaybackStatus::Playing)  →  SMTC
```

**四个反直觉的实测结论：**

1. **播放结束不靠 `BASS_SYNC_END`，是轮询。**【源码确证】`BassCore.cpp:478`：
   ```cpp
   bool CBassCore::SongIsOver() {
       // 记住上一帧的 BASS_ChannelIsActive 结果，上一帧 PLAYING、本帧 STOPPED 就算结束
       bool is_over{ (m_last_playing_state == BASS_ACTIVE_PLAYING && state == BASS_ACTIVE_STOPPED) };
   }
   ```
   由主定时器（`TIMER_ELAPSE 80`ms）每 80ms 检查一次。**`BASS_SYNC_END` 只用在一个地方：MIDI 播放结束时清歌词。** 理由（【推测】）：同步回调在 BASS 自己的线程里触发，跨线程操作 UI 太麻烦。
   → **抄的时候要加超时兜底**：这个项目 FFmpeg 内核就是因为结束信号不可靠，到 V2.78 还在修「播放结束后卡住」（issue #820 #822）。

2. **正常播放完全不用 mixer，只有一个 `HSTREAM`。** 没有交叉淡入淡出、没有无缝接歌。`bassmix` 只用在格式转换的重采样。
   → 如果你要做「切歌无感」，这一块它没给答案。

3. **`BASS_Init` 的输出采样率硬编码 44100**，不跟随源文件、不让用户选。**BASS 内核也没有独占模式**（`BASS_CONFIG_DEV_EXCLUSIVE` 一次都没出现）。独占只有 FFmpeg 内核的 WASAPI 有。
   → 结果就是 issue #799「小尾巴使用 wasapi 独占模式，不能正常播放」。**这是真实的用户痛点，你要做得比它好。**

4. **播放位置全用 `BASS_POS_BYTE` + `BASS_ChannelBytes2Seconds`**，单位毫秒 `int`。每帧只取一次缓存进 `CPlayer::m_current_position`，所有歌词/进度条读缓存，不各自去打 BASS（BASS 调用有临界区，密集调用会锁竞争）。**这个缓存设计要抄。**
   > 注意 `int` 毫秒上限约 24.8 天 —— 音乐够用，有声书/长音频要换 `int64`。

## 2.4 为什么它选 BASS，而不是自己写解码

从代码结构能读出的理由（**【推测】**，源码里没有明写决策过程）：

1. **一站式**：解码 + 输出 + 重采样 + FFT + DX8 均衡/混响 + 变速变调 + MIDI/SF2，一个 `bass.dll` 全给。自己写等于把 6 个库的集成工作全做一遍。
2. **插件是二进制 drop-in**：新增格式只是往 `Plugins\` 丢一个 dll，不改代码（§2.2）。
3. **格式覆盖广且授权门槛低**（对个人开发者）：非商业免费。
4. **API 极其稳定**：`bass.h` 版权行是 `1999-2021`，20 多年接口没大改，`BASS_StreamCreateFile` 这种调用二十年不变。

**代价（也是它今天的问题）：**
- **商业授权要钱**（见 §2.6）。
- **DLL 闭源，出了问题只能等上游**。issue #923「BASS内核出现歌曲被删除的bug」、#953「播放32位深的FLAC音乐，软件直接卡死」、#976「拖入m4a文件软件直接卡死」—— 这类崩溃它自己修不了。
- **BASS 的 FFT/音效是「黑盒取样」**，想插自己的 DSP（比如 ReplayGain、音量均衡）没有位置。

## 2.5 ⭐ 不用 BASS 的替代方案（Windows）

### 先看它自己怎么做的 —— FFmpeg 内核的真相

**它没有链接 libavcodec，也没有调用 `ffmpeg.exe` 命令行。它套壳了一个第三方 DLL。**

【源码确证】`FfmpegCore.cpp:16`：`Init(L"ffmpeg_core.dll")`
【外部确认】上游是 [`lifegpc/ffmpeg_core`](https://github.com/lifegpc/ffmpeg_core) —— C + FFmpeg + SDL，GPL-3.0，49★，**最后一个 release `v1.0.0.1` 停在 2022-04，仓库最后 push 2024-05**。

这个套壳内核的完成度（逐个函数看）：

| 能力 | 状态 | 依据 |
|---|---|---|
| 播放/暂停/定位/音量/变速 | ✅ | 有实现 |
| **变调** | ❌ 空函数 `SetPitch(int){}`，`IsPitchAvailable()→false` | `FfmpegCore.cpp:215` |
| **混响** | ❌ `SetReverb` / `ClearReverb` 都是空的 | `FfmpegCore.cpp:366-370` |
| 均衡器 | ⚠️ 有，且它的 `GetEqChannelFreq()` 第 10 档是 **16000**（反证 BASS 侧写成 1600 是笔误） | `FfmpegCore.cpp:649` |
| 频谱 | ✅ `ffmpeg_core_get_fft_data(handle, data, FFT_SAMPLE)` | 量纲不同，`scale=100`（BASS 是 60） |
| **格式转换** | ❌ `EncodeAudio → false`、`InitEncoder → false` | `FfmpegCore.cpp:691-708` |
| MIDI | ❌ 全不支持 | —— |
| 元信息/标签 | ✅ 直接读 FFmpeg metadata key | —— |
| **WASAPI / 独占模式** | ✅ **只有这个内核有** | `ffmpeg_core_settings_set_use_WASAPI` / `_enable_exclusive` |
| **网络流缓存/重试** | ✅ `cache_length`(15) / `max_retry_count`(3) / `url_retry_interval`(5) / `max_wait_time`(3000) | 对在线音源很有参考价值 |

**结论：它证明了「FFmpeg 路线能播得比 BASS 更多」，但也证明了「DSP 三件套（变速不变调 / EQ / 混响）才是 BASS 的真实护城河」——它自己那条路走了三年，这三样一个都没补上。**

### 替代方案对比（Windows）

| 方案 | 解码覆盖 | 授权 | 体积 | 变速不变调 | EQ/混响 | 在线流 | 判断 |
|---|---|---|---|---|---|---|---|
| **BASS** | 极广（+20 插件） | 非商业免费，**商业要买** | `bass.dll` ~200KB + 插件 | ✅ 现成 `bass_fx` | ✅ 现成 DX8 | ✅ `StreamCreateURL` | 最省事，但要钱 |
| **FFmpeg（libavformat/libavcodec + 自选输出）** | **最广**（含视频容器） | LGPL 2.1+ / GPL（取决编译选项） | `avcodec+avformat+swresample` 约 15~30MB（可裁剪到 ~8MB） | ❌ 需 `atempo`/`rubberband` 滤镜或 SoundTouch | ❌ 需自研 biquad + Freeverb | ✅ 原生支持 http/hls | **最主流，功能够，工程量大** |
| **Windows Media Foundation**（系统自带） | 中（mp3/aac/wma/wav，flac 看版本，ape/opus 不行） | **免费，无第三方依赖** | **0（系统 DLL）** | ❌ 有 `IMFPMediaPlayer::SetRate` 但会变调 | ❌ 需自研（可用系统 `MFTransform`） | ⚠️ 能做但麻烦 | 体积最优，格式覆盖是硬伤 |
| **各格式独立库**（dr_flac / dr_mp3 / stb_vorbis / minimp4 / opusfile…） | 看你集几个 | 多为 public domain / MIT | 每个几十~几百 KB，合计 <2MB | ❌ | ❌ | ❌ | **体积冠军，但要自己集 6~10 个库** |
| **Rust 生态：Symphonia + cpal + rubato** | 广（mp3/aac/flac/ogg/wav/alac…） | MPL-2.0 / MIT | 静态链接进 exe，约 2~5MB | ⚠️ `rubberband` crate / SoundTouch 绑定 | ❌ 自研 | ⚠️ 需自己做 HTTP + 缓存 | **如果你用 Rust 写，这是最优解** |
| **libmpg123 / libFLAC / libvorbis 等「一家一个库」** | 窄 | LGPL/BSD 混合 | 中等 | ❌ | ❌ | ❌ | 不推荐，集成成本 ≫ 收益 |

**推荐的组合（按你的「小巧 + 在线音源」定位）：**

- **主力：FFmpeg（解码）+ miniaudio/WASAPI（输出）+ 自研 DSP**
  - 用 FFmpeg 而不是 MF：因为你要**在线音源**，FFmpeg 对 http/hls/各种奇怪容器/重定向/断线重连的支持是决定性的（它自己的 FFmpeg 内核就带了 `max_retry_count` / `url_retry_interval` / `cache_length`）。
  - FFmpeg 可以裁剪：`--disable-everything --enable-decoder=... --enable-demuxer=...` 能压到 8MB 左右。
  - **注意 GPL 传染**：如果你用 `--enable-gpl`（比如为了 libmp3lame 编码），你的程序必须 GPL。纯 LGPL 配置（只用解码器）可以闭源。**这个决定要在写第一行代码前定下来。**

- **如果想极致小体积：miniaudio（单头文件，public domain/MIT）已经内置了 WAV/FLAC/MP3 解码器 + WASAPI/DirectSound/WinMM 输出**，再配 dr_libs 系列补 ogg/opus。**不足 1MB 就能跑起「本地常见格式」，但 AAC/m4a 和网络流要另想办法。** 这是「千千静听那种小巧」最贴近的实现路径。

- **MF 值得作为兜底**：不需要任何第三方依赖就能播 mp3/aac/wma/wav，可以作为「FFmpeg 没加载成功时的降级路径」—— 和它「MCI > FFMPEG > BASS」的降级思路一样，但换成 MF 更实用。

### BASS 的授权说明（【外部确认】）

- 官网明确：**BASS 非商业用途免费**（个人、不从中获利，含广告也算获利）；商业用途需要买 license，**[Shareware licence €125](https://www.un4seen.com/bass.html)**，另有 Commercial licence（按平台/产品数计价）。各 Add-on（BASSASIO $40 起等）**单独计价**。
- 【源码确证】**MusicPlayer2 仓库里关于授权一个字都没有** —— Wiki《支持的音频格式》只写「基于 BASS 音频库 (www.un4seen.com)」并给了个 URL；`bass.h` 只有 Copyright 行。仓库根 `LICENSE` 是 MusicPlayer2 自己的 GPL-3.0。**所以「BASS 商业要钱」这个结论不能引用这个项目当依据，得看官网。**
- **`bass.dll` / `bass_fx.dll` 并不在 git 仓库里**（只有 `Debug/`、`x64/Debug/` 下有构建残留，属于误提交）。它的做法是：**预编译 DLL 放进 Release 压缩包，不进版本库。** 这是「免费但不开源」组件的常规处理方式，值得照做。
- 运行时兜底【源码确证】`MusicPlayer2.cpp:232-242`：检查 `HIWORD(BASS_GetVersion()) == BASSVERSION`，不符就弹警告框；**用户点「取消」直接不启动程序**。这个体验不好，你要做得更友好。

---

# 3. 歌词系统

> 完整版见 `_mp2/reports/report-lyrics.md`（1136 行，含每个函数的关键代码片段）。这里给结论与我建议你怎么抄。

## 3.1 支持哪些格式（**先纠正一个常见误传**）

【源码确证】`Lyric.cpp:6`：

```cpp
const vector<wstring> CLyrics::m_surpported_lyric{ L"lrc", L"ksc", L"vtt" };   // 注意源码里 m_surpported 就拼错了
enum class LyricType { LY_AUTO, LY_LRC, LY_LRC_NETEASE, LY_KSC, LY_VTT };
```

| 格式 | 支持 | 依据 |
|---|---|---|
| **LRC**（标准，含压缩多标签 `[..][..]`、`[mm:ss:xx]` 冒号毫秒） | ✅ 主力 | `DisposeLrc()` |
| **扩展 LRC / 逐字**（`[00:12.34]<00:12.34>逐<00:12.78>字`） | ✅ | `DisposeLrc()`，注释写「按 ESLyric 0.5.x 解析」 |
| **网易云 LRC 变体**（相同时间标签的两行做翻译） | ✅ 特殊处理 | `DisposeLrcNetease()` |
| **KSC（酷狗）**（`karaoke.add('00:12.340','00:15.120','逐[字]歌[词]','120,340,...')`） | ✅ | `DisposeKsc()` |
| **WebVTT** | ✅ | `DisposeWebVTT()` |
| 内嵌歌词（ID3 **USLT** / FLAC `LYRICS` / MP4 `----:com.apple.iTunes:Lyrics` / ASF） | ✅ 但不解析同步帧 | `CAudioTag::GetAudioLyric()` |
| **KRC（酷狗加密）** | ❌ **全仓零命中** | grep `KRC` 无有效结果 |
| **QRC / YRC（QQ音乐逐字）** | ❌ **全仓零命中** | grep `QRC`/`YRC` 零结果 |
| **TRC（天天动听）** | ❌ 零命中 | —— |
| **SYLT 同步歌词帧** | ❌ 不解析 | `taglib/synchronizedlyricsframe.h` 只是 TagLib 自带文件 |

**逐字歌词只有三个来源**：扩展 LRC 的 `<mm:ss.xx>`、WebVTT 的 `<hh:mm:ss.mmm>`、KSC 的时长数组。QQ音乐走的是**明文 LRC** 接口（不是 QRC）。

> ⚠️ 你要做得比它好：**KRC / QRC 是国内两大平台的逐字格式，用户是有期待的（issue #919 甚至有人在要 TTML）。** 但这个项目证明了「不做 KRC/QRC 也能活」，如果你要做，那是差异化优势。

## 3.2 解析：数据结构与规范化

【源码确证】`Lyric.h:CLyrics::Lyric`：

```cpp
struct Lyric {
    int time_start_raw{};   // 行开始时间：初始化时写入，之后只读
    int time_span_raw{};    // 行持续时间：同上
    int time_start{};       // 行开始时间：偏移量即时应用
    int time_span{};        // 行持续时间：同上
    wstring text;           // 歌词文本
    wstring translate;      // 翻译
    vector<int> split;      // 逐字歌词对 text 的分割位置（字符下标）
    vector<int> word_time;  // 每段持续时间（毫秒）
};
```

### ⭐ 最值得抄的设计：「双份存储 + 整体重算」

- `_raw` 字段是文件原值，**永不变**；
- `time_start/time_span` 是叠加用户偏移后的「生效值」；
- 用户按「歌词提前/延后 0.5s」只改一个累加的 `m_offset`，然后整体重跑 `NormalizeLyric()`；
- **额外好处**：`m_lyrics_str` 单独保存「未拆时间标签的原始行」，偏移为 0 时**逐字节还原用户文件**，不污染。

这套模式在任何「用户可调 + 需要写回原文件」的场景都能用。

### `NormalizeLyric()` —— 整个系统的时间轴真相

【源码确证】做了三件事：

```cpp
// (1) 排序 + 应用偏移 + 去重叠
std::stable_sort(...);
int last{};
for (...) { last = max(last, time_start_raw + m_offset); time_start = last; last += 10; }   // ← 硬编码 10ms 间隔

// (2) 逐行 time_span：优先 raw，否则累加 word_time，再被下一行裁剪
if (time_span_raw != 0) time_span = time_span_raw;
else if (!word_time.empty()) time_span = accumulate(word_time);
if (time_span == 0 || next.time_start - time_start < time_span)
    time_span = next.time_start - time_start;      // 夹住，禁止跨行重叠

// (3) 最后一句 duration 未定时：抄前一段，或默认 20 秒
```

**没有正则**，时间标签解析是手写的 `find_first_of` + `substr` + `_wtoi`（`ParseLyricTimeTag`）—— 括号字符参数化，所以同一个函数能处理 `[...]`、`<...>`、KSC 的 `'...'`。对歌词这种短行是正确选择：不会踩正则回溯，也不吃 `<regex>` 的编译成本。

**译文配对有三条路径**：① 同行 ` / ` 分隔（最常用）；② 相同或接近时间标签的相邻两行（`CombineSameTimeLyric(error)`，`DisposeLrc` 末尾固定用 `error=0`）；③ 格式自带字段（网易云 / QQ音乐）。

## 3.3 ⭐⭐ 逐字高亮：**它不是逐字绘制的**

这是全篇最值得学的一招。**同一个字符串画两遍，第二遍用一个窄矩形裁剪。**

【源码确证】`DrawCommon.cpp:CDrawCommon::DrawWindowText(rect, str, color1, color2, split, ...)`：

```cpp
text_size   = m_pDC->GetTextExtent(lpszString);
CRect text_rect  { CPoint{text_left, text_top}, text_size };                                    // 整行
CRect text_f_rect{ CPoint{text_left, text_top}, CSize{ text_size.cx * split / 1000, text_size.cy } };  // 只到进度处

m_pDC->SetTextColor(color2);
m_pDC->DrawText(lpszString, text_rect,   DT_SINGLELINE | DT_NOPREFIX);   // ① 整行画"未唱"色
if (color1 != color2 && split != 1000) {
    m_pDC->SetTextColor(color1);
    m_pDC->DrawText(lpszString, text_f_rect, DT_SINGLELINE | DT_NOPREFIX);  // ② 整行画"已唱"色，矩形只到进度
}
```

**优点**：不需要拆字、对任意字体有效、不需要 alpha 合成、不需要逐字 `TextOut`。字形被 GDI 天然裁开，得到「正在唱某个字」的中间态。**便宜到可以直接每帧做。**

桌面歌词走的是等价路线（GDI+）：

【源码确证】`LyricsWindow.cpp:CLyricsWindow::DrawHighlightLyrics()`：
```cpp
if (m_nHighlight <= 0 || m_nHighlight >= 1000) return;      // ≥1000 不画高亮
if (m_lyric_karaoke_disp) {
    Gdiplus::RectF CliptRect(dstRect);
    CliptRect.Width = CliptRect.Width * m_nHighlight / 1000;
    pRegion = new Gdiplus::Region(CliptRect);
    pGraphics->SetClip(pRegion, Gdiplus::CombineModeReplace);
}
if (m_pHighlightPen) pGraphics->DrawPath(m_pHighlightPen, pPath);   // 描边
pGraphics->FillPath(CreateGradientBrush(...), pPath);              // 渐变填充
```
文字先 `AddString` 进 `GraphicsPath`，再 `FillPath` —— 这样能同时做描边 + 渐变 + 阴影（阴影 = 同一个 path 偏移再 `FillPath` 一次）。

## 3.4 ⭐⭐ 进度计算：用「像素宽度」加权，不是「字数」

`GetLyricProgress` 返回 **0~1000 的整数**，签名里带一个测量回调：

【源码确证】`Lyric.cpp:670`：

```cpp
int CLyrics::GetLyricProgress(CPlayTime time, bool ignore_blank, bool blank2mark,
                              std::function<int(const wstring&)> measure) const
```

调用方注入自己环境的测量函数：

```cpp
// 主界面（GDI）
GetLyricProgress(time, false, false, [this](const wstring& s){ return GetTextExtent(s.c_str()).cx; })
// 桌面歌词（GDI+）
GetLyricProgress(time, ig, kara, [&](const wstring& s){
    Gdiplus::RectF bb;
    pGraphics->MeasureString(s.c_str(), -1, pFont, Gdiplus::RectF{},
        Gdiplus::StringFormat::GenericTypographic(), &bb, 0, 0);
    return static_cast<int>(bb.Width); })
```

算法：
1. 逐字歌词：字段时间只用来确定「当前第几段、段内百分比」；
2. 然后**按 `measure()` 出的真实像素宽度重算整行百分比**：
   ```cpp
   int progress{ lyric_current_time * 1000 / max(lyric_last_time, 1) };
   if (lyric_line_size > 0)
       progress = (progress * lyric_word_size / 1000 + lyric_before_size) * 1000 / lyric_line_size;
   ```
3. `progress == 1000` 有特殊语义：**「这句已结束，不要高亮」**（源码注释明写），因为逐字歌词引入这个状态后可能维持一段时间。

**这就是「比例字体下中英混排也不跑偏」的诀窍。** 解析层因此完全不知道 DC 存在 —— 依赖注入用得漂亮。

> 代价：**每帧 3~5 次子串文本测量，且没有缓存**。抄的时候建议按「字体 + 字号 + 子串」做一层 LRU 缓存。

## 3.5 平滑滚动与同步：**不用缓动，靠进度连续**

【源码确证】`CUIDrawer.cpp:CUIDrawer::DrawLyricTextMultiLine()`：

```cpp
int line_space    = theApp.m_lyric_setting_data.lyric_line_space;
int lyric_height  = GetLyricTextHeight() + line_space;
int lyric_height2 = lyric_height * 2 + line_space;          // 带翻译的行高
int center_pos    = (lyric_area.top + lyric_area.bottom) / 2;
int lyric_index   = ...GetLyricIndex(time);
int progress      = ...GetLyricProgress(time, false, false, measure);
int y_progress    = progress * (有翻译 ? lyric_height2 : lyric_height) / 1000;

int start_pos = center_pos - y_progress;                    // 当前行正在穿过中线
for (int i{ lyric_index - 1 }; i >= -1; i--)                // 往前累减每行高度
    start_pos -= 该行高度;
// 然后从 start_pos 开始逐行铺下去，只画与可视区有交集的
```

**结论：没有 `SetScrollPos`，没有缓动，没有行快照。** 滚动的「平滑」完全来自 `progress` 是连续变化的 —— 当前行在**整个行时长内持续**从中线往上平移一个行高。

- ✅ 优点：实现极简、永远和音频位置精确同步、不会累积漂移。
- ⚠️ 副作用：**滚动速度 = 行高 / 行时长** → 长句滚得慢、短句滚得快，观感上会「一顿一顿」。issue #944 有人直接问「能否实现 Apple Music 那种歌词高帧滚动效果」。
- **替代方案**（我的建议）：行级 snap + 短 tween（比如 200ms 缓动到目标行），匀速但可能有跳变 —— 这是取舍。想要 Apple Music 那种效果，得做「行内逐字 + 行间弹簧插值」，比它这套复杂一档。

**歌词区域高度自适应单行/双行/多行**：【源码确证】`CUIDrawer::IsDrawMultiLine(h) → h >= GetLyricTextHeight() * 3.5`。低于 3.5 行自动降级。

**双行模式的「上飘」技巧**：【源码确证】用一个静态 `flag` 变量 + `progress` 回绕检测这句唱完：
```cpp
static int flag{};
bool switch_flag{ flag > 5000 };
switch_flag ^= (flag % 5000) > progress;      // 检测 progress 回绕 = 换句
flag = switch_flag ? 10000 + progress : progress;
```
`switch_flag` 为真时当前句画下半行、下一句画上半行 —— 这就是「上一句往上飘走」的视觉。

**同步数据来源**：
- 【源码确证】`BassCore.cpp:GetCurPosition()`：`BASS_ChannelGetPosition(stream, BASS_POS_BYTE)` + `BASS_ChannelBytes2Seconds`，**没用 `GetPositionEx`**。
- 位置由 UI 线程每帧取一次，缓存进 `CPlayer::m_current_position`，`Player.cpp:GetPlayerCoreCurrentPosition()` 里对 cue 单轨减 `start_pos`。
- 【源码确证】偏移调整：`OnLyricForward() → AdjustLyric(-500)` / `OnLyricDelay() → AdjustLyric(+500)`，**只改 `m_offset` 不碰时间标签**，保存时才展开成 `[mm:ss.cc]`。
- 【源码确证】自动保存策略三选一：`LS_DO_NOT_SAVE` / `LS_AUTO_SAVE` / **`LS_INQUIRY`（默认）**，在切歌前触发。

## 3.6 在线歌词下载

### 架构

【源码确证】`LyricDownloadCommon.h:class CLyricDownloadCommon` —— 纯虚基类：

```cpp
virtual std::wstring GetSearchUrl(const std::wstring& key_words, int result_count = 20) = 0;
virtual std::wstring GetAlbumCoverURL(const wstring& song_id) = 0;
virtual std::wstring GetOnlineUrl(const wstring& song_id) = 0;
virtual int  RequestSearch(const std::wstring& url, std::wstring& result) = 0;
virtual void DisposeSearchResult(vector<ItemInfo>&, const wstring&, int result_count = 30) = 0;
virtual bool DownloadLyric(const wstring& song_id, wstring& result, bool download_translate = true) = 0;
virtual bool DisposeLryic(wstring& lyric_str, bool download_translate) = 0;   // 源码里就拼错了
struct ItemInfo { wstring id, title, artist, album; int duration{}; int track{}; };
```

两个实现：`CNeteaseLyricDownload`、`CQQMusicLyricDownload`。工厂在 `MusicPlayer2.cpp:InitLyricDownload()` 按配置 `lyric_download_service` 选一个。

### 接口（【源码确证】`NeteaseLyricDownload.cpp`）

| 用途 | URL | 方法 |
|---|---|---|
| 搜索 | `http://music.163.com/api/search/get/?s=%s&limit=%d&type=1&offset=0` | POST（body 为空） |
| 歌词（无翻译） | `http://music.163.com/api/song/media?id=<id>` | GET |
| 歌词（带翻译） | `http://music.163.com/api/song/lyric?os=osx&id=<id>&lv=-1&kv=-1&tv=-1` | GET |
| 封面 | `http://music.163.com/api/song/detail/?id=<id>&ids=%5B<id>%5D&csrf_token=` | GET |

**没有任何加密 / 签名 / host 白名单，全是明文 `http://`。** 解析用 `nlohmann/json`。

QQ音乐（【源码确证】`QQMusicLyricDownload.cpp`）走 `nobase64=1`，**拿回来的是明文 LRC**，`trans` 字段用 `\r\n` 追加到 `lyric` 后面靠「相同时间标签」自动配对。

### 搜索关键字与匹配

【源码确证】关键字构造（`SearchSongAndGetMatched()`）：
```cpp
if (标题为空)      keyword = 文件名去掉扩展名;
else if (艺术家为空) keyword = 标题;
else                keyword = 艺术家 + L' ' + 标题;
wstring url = CInternetCommon::URLEncode(keyword);
```
**没有去括号、没有清 `feat.`、没有繁简归一。**

【源码确证】匹配打分（`SelectMatchedItem()`），**这是重点**：

```cpp
weight  = StringSimilarDegree_LD(标题,   候选标题)   * 0.4;
weight += StringSimilarDegree_LD(艺术家, 候选艺术家) * 0.4;
weight += StringSimilarDegree_LD(专辑,   候选专辑)   * 0.3;
weight += StringSimilarDegree_LD(文件名, 候选标题)   * 0.3;
weight += StringSimilarDegree_LD(文件名, 候选艺术家) * 0.3;   // 注释写 0.2，代码是 0.3
weight += ((1 - i * 0.02) * 0.05);                          // 列表位置：第 1 项 1.0，之后每项 -0.02
if (max_weight < 0.3) max_index = -1;                       // 阈值：低于 0.3 判"没找到"
```

相似度是**带字符相似度的编辑距离**（`StringSimilarDegree_LD`）：

```cpp
cost = 1 - CharacterSimilarDegree(ch1, ch2);     // 替换代价不是 0/1
d[i][j] = Min3(d[i-1][j] + 1, d[i][j-1] + 1, d[i-1][j-1] + cost);
double ds = 1 - (double)d[n][m] / max(n, m);
// CharacterSimilarDegree: 完全相同 1.0 / ASCII 大小写 0.8 / 中文数字↔阿拉伯数字 0.7 / 其他 0.0
// MAX_LENGTH = 256 挡住 O(n²) 爆表
```

**这三档字符相似度是专门为歌名匹配调的**（大小写 0.8、「一↔1、二↔2…零↔0」0.7），几乎零成本、效果明显 —— **值得直接抄**。

### ⭐ 最大的坑：**时长完全没参与匹配**

`ItemInfo::duration` 被解析出来了，但 **`SelectMatchedItem()` 里一次都没用到**（只在 UI 列表里显示）。注释头部明明写着「时长——0.6」的权重。

→ **后果：5 分钟的录音室版和 3 分钟的现场版会判成同一个结果。** 这是「多版本歌曲匹配错误」的主要来源（对应 issue #911「歌词搜索与结果不符」、#940「下载专辑封面和歌词失败」）。

**这是投入产出比最高的修复**：给时长加一项「±5 秒内得满分、差得越多扣得越多」，能立刻改善体验。

### 其他网络层的坑（都要避开）

| 坑 | 位置 | 说明 |
|---|---|---|
| **HTTP 没有任何超时设置** | `InternetCommon.cpp:SendHttpRequest` | 只设了 User-Agent，超时判定靠 `catch` 里的 `ERROR_INTERNET_TIMEOUT`。**作者自己在 `LyricDownloadDlg.cpp:442` 写注释承认了线程安全 bug**：「HttpPost 可能卡 30s 网络超时……此时如果歌词下载窗口关闭则 pInfo 会是野指针」 |
| URI 编码 `%x` 不补零 | `InternetCommon.cpp:URLEncode` | 字节 0x0A 会编成 `%a` 而非 `%0a`。中文（≥0x80）踩不到，Latin-1 单字节踩得到 |
| 结果无条件按 UTF-8 解码 | `InternetCommon.cpp:145` | 服务器换编码就乱码 |
| 不校验证书 CN | `InternetCommon.h` `INTERNET_FLAG_IGNORE_CERT_CN_INVALID` | —— |
| 网易用明文 http | `NeteaseLyricDownload.cpp` | 运营商劫持 / 302 登录页会变成「解析错误」而不是明确报错 |
| **接口变了就挂** | issue #881「歌词封面和歌词下载源失效了~」 | 网易云加强防护后接口就失效了。**它的应对方式是「加第二个源（QQ音乐）」，这是正确的产品设计 —— 音源/歌词源必须可插拔。** |
| 批量下载不可中断 | `LyricBatchDownloadDlg.cpp:ThreadFunc()` | 退出标志只在 HTTP 调用**之后**检查，点取消最多要等一次超时 |
| 歌词扩展名只有 3 个 | `Lyric.cpp:6` | 用户手动放 `.txt` 或大写 `.LRC` 都找不到，**没有大小写不敏感处理** |

**给新项目的底线**：显式超时（连接/读各设一个）+ 取消令牌（`std::stop_token` 或 `shared_ptr<atomic_bool>`）+ 结果只回投给仍存活的接收者 + 至少两个可切换的歌词源。

## 3.7 歌词编辑

- 【源码确证】**编辑器是内嵌的 Scintilla**（仓库根有完整 `scintilla/` 副本，需要 `SciLexer.dll`），并且**自己写了一个 Scintilla 词法分析器** `Lyric/LexLyric.cxx` —— 给 `[...]` 时间标签、` / ` 分隔符、翻译部分上不同颜色。
- 快捷键：`F8` 插入当前播放时间标签、`F9` 替换光标处时间标签、`Ctrl+Delete` 删除、`Ctrl+S` 保存。
- 批量操作：合并相同时间标签的歌词、交换原文与翻译、时间标签整体错位一句、繁简转换、从括号提取翻译。
- **时间标签精度只有两位厘秒（10ms）**：`CPlayTime::toLyricTimeTag()` 输出 `[%.2d:%.2d.%.2d]`，`msec/10`。**每保存一次就累积一次舍入误差** —— 逐字歌词尤其明显。你要用三位毫秒。

## 3.8 其他显示面

| 面 | 实现 | 判断 |
|---|---|---|
| **桌面歌词** | `UpdateLayeredWindow` 分层窗口 + `SetWindowLong(WS_EX_TRANSPARENT)` 鼠标穿透 + 锁定模式 + GDI+ 渐变/描边/阴影 + 3 套预设配色 | ✅ 值得做，用户口碑极好（issue 里反复被拿出来夸） |
| **Cortana / Win10 搜索框歌词** | `FindWindowEx("Shell_TrayWnd")` 拿到系统搜索框 HWND 后**直接往它的 DC 上画** | ⚠️ **硬核但极其脆弱**。依赖任务栏窗口类名，Explorer 重启 / 第三方任务栏工具就失效，且失效时表现为「画到错误窗口上」这种极难查的症状。**别抄。** |
| 任务栏缩略图 | `ITaskbarList3::SetThumbnailClip` 截主界面某块矩形 | 顺带做就行 |
| MIDI 内嵌歌词 | `BASS_MIDI_StreamGetMark` + `BASS_ChannelSetSync(BASS_SYNC_MIDI_MARK)` | 边缘功能 |

> **一个必须澄清的说法**：「任务栏缩略图歌词」并不存在 —— 那只是缩略图裁剪区域恰好包含歌词区域。真正独立的是 Cortana 搜索框那条路。

---

# 4. 曲库管理

> 完整版见 `_mp2/reports/report-medialib.md`（1207 行）。这里给结论。

## 4.1 ⭐ 存储：**没有数据库。是一个 `unordered_map` + MFC `CArchive` 手写序列化**

【源码确证】`SongDataManager.h`：

```cpp
class CSongDataManager {
    using SongDataMap = std::unordered_map<SongKey, SongInfo>;   // SongKey 的核心是文件绝对路径
    SongDataMap m_song_data;
    std::atomic<bool> m_song_data_modified{ false };
    CString m_data_version;
    mutable std::shared_mutex m_shared_mutex;   // 遍历/查找加读锁，添加/删除加写锁
};
```

【源码确证】`SongDataManager.cpp:SaveSongData()` —— **手写字段顺序**，版本号是字符串 `"2.781"`：

```cpp
CFile file;
file.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite);   // ← 直接截断覆盖，无原子性、无备份
std::shared_lock<std::shared_mutex> readLock(m_shared_mutex);
CArchive ar(&file, CArchive::store);
ar << CString(_T("2.781"));                       // 数据版本
ar << static_cast<int>(m_song_data.size());       // 条目数
for (const auto& [key, song] : m_song_data) {
    ar << CString(key.path.c_str())
       << song.start_pos.toInt() << song.end_pos.toInt() << song.bitrate
       << CString(song.title.c_str()) << CString(song.artist.c_str())
       << CString(song.album.c_str()) << CString(song.get_year().c_str())
       << CString(song.comment.c_str()) << CString(song.genre.c_str())
       << song.genre_idx << song.track << song.tag_type
       << CString(song.song_id_netease...) << CString(song.song_id_qq_music...)
       << song.listen_time << song.info_acquired << song.is_cue
       << song.flags << song.last_played_time << ...;
}
```

**代价（明确结论）：**
- ❌ **没有任何查询能力**。「最近播放」「播放次数 > N」「按流派筛选」全都得把整个 map 拉进内存自己算。
- ❌ **每次保存全量重写**，几万首时退出程序会有明显卡顿；**崩溃/断电就整个曲库报废**（`CFile::modeCreate` 直接截断原文件，没有 temp + rename）。
- ❌ **版本兼容靠字符串比较** —— 字段加一个就得想办法兼容。
- ❌ **文件里混着「标签元数据」和「用户统计」两类数据**，前者随时可以从文件重建、后者不能丢。**这两类应该分开存**：元数据用可重建的缓存（SQLite / 甚至每次重扫），统计用一个小而稳的文件。

**我的判断：你绝对不要抄这个。** 用 **SQLite**（单文件、零配置、有索引、有事务、崩溃安全、`INSERT OR REPLACE` 天然去重）。曲库稍微上千首，「按艺术家分组」「按播放次数排序」「模糊搜索」这些用 SQL 是一行，用内存 map 是一整屏代码。**SQLite 不是一个「重」依赖 —— 它就是一个 .c 文件 + 一个 .h，跟 stb 一样丢进项目就能编。**

**唯一值得抄的是「按文件绝对路径做 key」这一点**（配合 `modified_time` 做增量，见下）。

## 4.2 一首歌存了哪些字段

【源码确证】`SongInfo.h`，主要字段：

- **标识**：`file_path`（key）、`is_cue`、`start_pos`/`end_pos`（cue 分轨的起止）、`modified_time`（增量判断用）、`info_acquired`、`ChannelInfoAcquired()`、`tag_type`、`flags`（位标志）
- **标签**：`title` / `artist` / `album` / `album_artist` / `genre` / `genre_idx` / `year`（`get_year()` 字符串）+ `comment` / `track` / `total_tracks` / `disc_num` / `total_discs`
- **音频信息**：`bitrate` / `freq` / `channels` / `length()`
- **统计**：`listen_time`（累计听歌时间）/ `last_played_time` / `rating`（1-5）/ 收藏标志（在 `flags` 里）
- **在线关联**：`song_id_netease`（`int64`）/ `song_id_qq_music`（字符串）

**多值字段怎么存**：`artist` 是**一个字符串**，用 `" / ; & 、"` 这几个字符分隔，读的时候靠 `SongInfo::GetArtistList(vector<wstring>&)` 现拆。为了不误拆本来含这些字符的艺术家名，设置里有个「艺术家识别例外」列表。【文档确证】《媒体库设置》

> 这是典型的「先简单后补丁」。多值字段用分隔符存，必然要面对「分隔符出现在内容里」的问题，然后就要加例外表。**新项目直接用关联表（SQLite 里 `song_artist(song_id, artist_id)`）更省事。**

## 4.3 扫描流程与增量更新

【源码确证】调用链：

```
CMusicPlayerApp::StartUpdateMediaLib(bool force)                       MusicPlayer2.cpp:637
  → if (!m_media_lib_updating) { m_media_lib_updating = true;
      m_media_lib_update_thread = AfxBeginThread([](){                 // ← 一个后台线程
          if (remove_file_not_exist_when_update) { CleanUpRecentFolders(); CleanUpSongData(); }
          CMusicPlayerCmdHelper::UpdateMediaLib();                     MusicPlayerCmdHelper.cpp:597
          theApp.m_media_lib_updating = false;
          CUiMyFavouriteItemMgr::Instance().UpdateMyFavourite();       // ← 回主线程前先把 UI 缓存刷一遍
          CUiFolderExploreMgr::Instance().UpdateFolders();
          CUiAllTracksMgr::Instance().UpdateAllTracks();
          CUiMediaLibItemMgr::Instance().Init();
      });
    }
```

**增量判断靠「文件最后修改时间」**（【源码确证】`AudioCommon.cpp:GetAudioInfo`）：

```cpp
unsigned __int64 modified_time{};
if (!CCommon::GetFileLastModified(song_info.file_path, modified_time))
    continue;                                             // 文件不存在 → 跳过
if (refresh_mode != MR_FOECE_FULL
    && song_info.modified_time == modified_time
    && !need_get_info)                                    // need_get_info = 没拿到过信息 / 通道信息缺失
    continue;                                             // ← 三者都满足才跳过
song_info.modified_time = modified_time;
// 然后：TagLib 读标签 → 失败才降级到播放内核 GetAudioInfo → 填充 bitrate/freq/channels/length
```

【源码确证】`AudioCommon.h:56` 有**三级**刷新模式（名字里的 `FOECE` 是源码的拼写错误）：

```cpp
enum MediaLibRefreshMode {
    MR_MIN_REQUIRED,        // 仅获取不存在于媒体库的条目（最小化文件读取，最快但不保证最新）
    MR_FILE_MODIFICATION,   // 重新获取修改时间与媒体库记录不同的条目（需要读修改时间，略耗时）
    MR_FOECE_FULL           // 强制重新获取所有条目
};
```

**这个三级设计很值得抄**：`IniPlayList()` 默认用 `MR_MIN_REQUIRED`（加载播放列表时只补库里没有的歌，最快），媒体库更新用 `MR_FILE_MODIFICATION`，用户手点「强制重新加载」才 `MR_FOECE_FULL`。**同一套扫描逻辑，三种成本档位，由调用方按场景选。** 读修改时间用的是 `GetFileAttributesEx` 而不是 `FindFirstFile`（源码注释说这样省约 1/3 开销）。

**线程模型**：单线程扫描（`AfxBeginThread` 一个 worker），`m_media_lib_updating` 做「已经在跑就别再开一个」的互斥，退出时 `WaitForSingleObject(thread, 1000)` **最多等 1 秒就放弃等待**（【源码确证】`MusicPlayer2.cpp:327`）。

**进度反馈**：`CAudioCommon::GetAudioInfo(..., int& process_percent, ...)` 通过引用回传百分比，UI 在状态栏显示（状态栏平时隐藏，更新时才出现）。

> ⚠️ 两处改进点：
> 1. **单线程扫描在大曲库上就是慢**。TagLib 读标签是 IO + 解析密集，应该用线程池（`std::thread::hardware_concurrency()` 个 worker），但要注意「同时写 `m_song_data` 的锁粒度」和「读同一个旋转盘时的寻道劣化」——**建议是「并行读文件、串行写库」**。
> 2. **cue 的增量判断只认 cue 文件自己的时间**，音频文件变了但 cue 没变就不会重读（`AudioCommon.cpp:365 GetCueTracks`），这是个已知偏差。

## 4.4 统计数据的存放与写盘

| 数据 | 存在哪 | 写盘时机 | 依据 |
|---|---|---|---|
| 播放次数 | **不存次数**。只存 `listen_time`（累计秒数）；界面上的「累计次数」是**推算值**：`times = listen_time / length * 1000`（`CListenTimeStatisticsDlg.cpp:104`） | 每秒定时器 `CPlayer::AddListenTime(1)` 累加 | 【源码确证】`Player.cpp:AddListenTime` / `MediaLibHelper.cpp` |
| 「我喜欢」判定 | 每次 `Contains()` 都是 `std::find` 线性查找 → 构建「所有曲目」时退化成 **O(N×F)**（F = 收藏数） | 点赞时立即写 `favourite.playlist` | 【源码确证】`CUiMyFavouriteItemMgr.cpp` |
| 最近播放 | 每个 `SongInfo` 的 `last_played_time`；「最近播放列表」另有 `recent_list.dat` | 切歌时更新 | 【源码确证】`CRecentList` |
| 我喜欢的音乐 | **就是一个叫「我喜欢的音乐」的特殊播放列表文件**（`playlist/` 目录下），不是字段 | 点赞时立刻写播放列表文件 | 【文档确证】《媒体库》 |
| 歌曲分级 rating | `SongInfo.rating`（1-5，0 = 未分级），**双写：音频标签 + 内存曲库** | 由 `SongDataManager` 统一落盘 | 【源码确证】`TagLibHelper.cpp:GetId3v2Rating/WriteId3v2Rating`（对应 MP3 的 `POPM` 帧、FLAC 的 `RATING` 标签） |
| 累计听歌时间 | `SongInfo.listen_time` + 「播放时间统计」对话框的数据 | 退出时批量保存 `song_data.dat` | 【源码确证】`CListenTimeStatisticsDlg.cpp` |

**【更正一处常见误读】`is_favourite` 字段在序列化时是被注释掉的**（`SongDataManager.cpp:SaveSongData` 里 `//<< song_data.second.is_favourite`）—— 也就是说**「我喜欢的音乐」根本不存在曲库文件里，它就是一个普通播放列表文件** `playlist\favourite.playlist`（纯文本，点赞时立即写盘）。这种「不为一个功能引入新概念」的做法很好，但代价是 §4.4 末尾说的去重问题。

**写盘时机**（【源码确证】）：**退出时一次 + 主定时器每 600×80ms ≈ 48 秒**，如果有修改才写。也就是说**异常退出会丢最多 48 秒的统计**，而且 48 秒后的那次保存会把「残缺的数据」固化到文件里。

> **「我喜欢的音乐是一个播放列表」这个设计值得学**：不引入新概念、可以直接导出 m3u、用户能看懂。但代价是 issue #759「媒体库选中歌曲中添加喜欢列表时，会重复加入，未做判断」——**播放列表必须有去重语义**（或者至少有个「导入时去重」的选项）。

## 4.5 搜索与拼音

【源码确证】`AbstractListElement::QuickSearch(key_word)` —— **朴素线性扫描**：

```cpp
searched = !key_word.empty();
search_result.clear();
for (int i = 0; i < GetRowCount(); i++)
    if (IsItemMatchKeyWord(i, key_word))
        search_result.push_back(i);          // 命中行号存进 vector

bool IsItemMatchKeyWord(int row, const std::wstring& kw) {
    for (int i = 0; i < GetColumnCount(); i++) {
        std::wstring text = GetItemText(row, i);     // ← 每次都要取文本
        if (!text.empty() && theApp.m_chinese_pingyin_res.IsStringMatchWithPingyin(kw, text))
            return true;
    }
    return false;
}
```

拼音匹配在 `ChinesePingyinRes.cpp`（只有 2.3KB，是一张内置的拼音表 + 逐字符匹配），支持**全拼**和**首字母**（V2.77 加的）。

**每次输入都全量重扫**，没有前缀树、没有索引、没有防抖。几千首还行，几万首会卡在按键上。

> **另有一处确证的实现 bug**（【源码确证】`ListSearchCache.cpp:16`）：
> ```cpp
> bool CListSearchCache::reload() {
>     if (!m_list_cache.reload())      // ← 没有花括号，也没有 return
>
>     m_ui_searched_playing_index = -1;   // ← 这一行是 if 的「空语句体」之后的代码，实际无条件执行
>     ...
>     return true;                        // ← 永远返回 true，调用方拿不到「没有变化」这个信号
> }
> ```
> `CListCache::reload()` 的语义是「版本没变就返回 false」；这里漏了 `return`，导致**「没有变化就跳过重算」的优化完全失效**，每次调用都白做一遍。**功能不出错，性能白花钱 —— 这类 bug 最难发现**，因为没有任何可见症状。写单元测试覆盖「调用 N 次只应重算 1 次」就能抓住。

> **你该做的**：输入防抖（150ms）+ 在 SQLite 里对 `title/artist/album` 建 FTS5 索引（或退一步：启动时把「拼音全拼 + 首字母」预计算成额外列）。搜索是用户最高频的操作，值得单独优化。

## 4.6 ⭐ 列表流畅度：**它没有做虚拟化**

这是本次调查最需要纠正的认知。答案很明确：**没有真正的虚拟化。**

【源码确证】`AbstractListElement.cpp:592`：

```cpp
void UiElement::AbstractListElement::CalculateItemRects() {
    item_rects.resize(GetRowCount());                    // ← 每帧 resize 到总行数
    for (size_t i{}; i < item_rects.size(); i++) {       // ← 每帧填满每一行
        int start_y = -scroll_offset + rect.top + i * ItemHeight();
        item_rects[i] = CRect{ rect.left, start_y, rect.right, start_y + ItemHeight() };
    }
}
```

无条件每帧调用（`AbstractListElement::Draw()` 第 265 行），然后绘制循环：

```cpp
int displayed_row_index{};
for (int i{}; i < GetRowCount(); i++)          // ← 上界是「总行数」，不是「可见行数」
{
    if (!IsRowDisplayed(i)) continue;
    CRect rect_item{ item_rects[displayed_row_index] };
    rect_item &= m_scroll_area_rect;
    if (!(rect_item & rect).IsRectEmpty())     // ← 真正的可见性判断在这里（求交）
        ... 背景 / 高亮 / 图标 / 每列文本 ...
    displayed_row_index++;
}
```

**它做到了**：不可见行不产生任何 GDI 绘制调用（没有画笔、没有排版、没有 `DrawText`）。
**它没做到**：不可见行**仍然每帧参与 O(N) 循环、O(N) 矩形重建、O(N) 虚函数调用和分支**；没有「第一个可见行」的 O(1)/O(log N) 定位；滚动时无法增量更新。

**放大问题的地方：**

1. **搜索态下 `IsRowDisplayed` 是线性查找**（`CCommon::IsItemInVector` = 手写 for 循环）→ 每帧 **O(N × M)**（N = 总行数，M = 命中数）。**这是「搜索时列表发卡」的直接原因。**
2. **`GetRowCount()` 在 `for` 条件里被反复调用**；对树控件而言 `TreeElement::GetRowCount()` **每次调用都完整遍历整棵树**（`TreeElement.cpp:60`）→ **文件夹浏览树每帧 O(N²)**。目录多了就明显掉帧。
3. **`GetDisplayedIndexByPoint` 是线性扫描**，挂在**每个鼠标移动消息**上（`AbstractListElement.cpp:814`，被 `MouseMove` 调用）→ 鼠标划过大列表时每帧 O(N)。

> ⚠️ **一个很有意思的对照**：同一个项目里的**老 MFC 列表**（`CListCtrlEx`，`.rc` 里 8 处）用的是 **`LVS_OWNERDATA`（虚拟列表）**，**反而是真虚拟化**。也就是说：**它自己手搓的新列表控件，性能还不如它替换掉的 Win32 控件。** 这是「重新发明轮子」最典型的翻车方式，值得记住。

**你该做的（不要抄它）：**
- 固定行高时：`first = scroll_offset / row_height`，**只画 `[first, first + visible_rows + 1]`**，O(可见行数)。
- 搜索命中集合改成 `std::vector<bool>` 或「排序后的区间」，`IsRowDisplayed` 变 O(1)；更好的做法是搜索时直接生成一个「显示索引数组」，绘制时按它取。
- 行的矩形**不需要存数组**，`y = top + (i - first) * row_height` 现算。
- 命中测试用同一个公式反算，O(1)，不要线性扫。
- 现代框架（WPF `VirtualizingStackPanel` / Avalonia `VirtualizingStackPanel` / egui `show_rows`）**默认就给你虚拟化**，这一整块问题会自然消失。

## 4.7 ⭐ 三种模式怎么统一到同一批 UI 元素

这是它架构上最漂亮的一处（也是「小巧」的来源）：

1. **统一的支点是 `ListItem` 这个值类型**。【源码确证】`ListItem` 用 `{ LT_FOLDER, LT_PLAYLIST, LT_MEDIA_LIB }` + 一个 name 描述「当前在播什么列表」。
2. **播放侧统一**：`CPlayer::IniPlayList(bool play, MediaLibRefreshMode, SongKey)` 一个函数吃三种模式 —— 内部按 `ListItem` 的类型分别取「文件夹里的文件」「播放列表文件」「媒体库查询结果」，最后都变成同一个 `vector<SongInfo>`。
3. **展示侧统一**：`AbstractListElement` 一个基类派生全部列表：

```cpp
virtual std::wstring GetItemText(int row, int col) = 0;
virtual int GetRowCount() = 0;
virtual int GetColumnCount() = 0;
virtual int GetColumnWidth(int col, int total_width) = 0;
virtual IconMgr::IconType GetIcon(int row);
virtual bool IsHighlightRow(int row);
virtual bool IsMultipleSelectionEnable();
virtual void QuickSearch(const std::wstring& key_word);
virtual int GetHoverButtonCount(int row);         // 鼠标指向行时显示几个按钮
virtual void OnHoverButtonClicked(int btn_index, int row);
```

于是「所有曲目」「我喜欢的音乐」「最近播放」「媒体库项目」「文件夹树」「播放列表」全是同一个基类的派生，**共用一整套滚动/选中/悬停按钮/右键菜单/快速搜索逻辑**。

**这一条直接照搬。** 无论你用 WPF/Avalonia/egui，「一个列表抽象基类（或 trait/interface）+ N 个数据源适配器」都是对的。

---

# 5. 标签与封面

## 5.1 读写哪些字段

【源码确证】`TagLibHelper.cpp` 用 TagLib 的 `Tag` 通用接口 + `PropertyMap`：

```cpp
static void SongInfoToTag(const SongInfo& song, Tag* tag) {
    tag->setTitle(...); tag->setArtist(...); tag->setAlbum(...); tag->setGenre(...);
    tag->setTrack(...); tag->setComment(...); tag->setYear(...);
}
```

**具体映射（按容器分派）**：

| 数据 | MP3 | FLAC | MP4/M4A | ASF/WMA | APE | WAV |
|---|---|---|---|---|---|---|
| 封面 | `ID3v2::AttachedPictureFrame`（FrontCover） | `FLAC::Picture` | `covr` atom | `WM/Picture` | `COVER ART (FRONT)` | ID3v2 |
| 内嵌歌词 | `ID3v2::UnsynchronizedLyricsFrame`（**USLT**） | `LYRICS` | `----:com.apple.iTunes:Lyrics` | `LYRICS` | —— | ID3v2 |
| 评级 | `ID3v2::PopularimeterFrame`（**POPM**） | `RATING` | —— | —— | —— | —— |
| cue | —— | —— | —— | —— | `CUESHEET` | —— |
| 专辑艺术家 | `ALBUMARTIST` | `ALBUMARTIST` | —— | —— | —— | —— |

【源码确证】`TagLibHelper.cpp:34-47` 把这些 key 定义成宏：
```cpp
#define STR_MP4_COVER_TAG "covr"
#define STR_ASF_COVER_TAG "WM/Picture"
#define STR_APE_COVER_TAG "COVER ART (FRONT)"
#define STR_MP4_LYRICS_TAG "----:com.apple.iTunes:Lyrics"
#define STR_ID3V2_LYRIC_TAG "USLT"
#define STR_FLAC_LYRIC_TAG "LYRICS"
#define STR_ASF_LYRIC_TAG "LYRICS"
#define STR_APE_CUE_TAG "CUESHEET"
#define STR_ID3V2_RATEING_TAG "POPM"
#define STR_FLAC_RATING_TAG "RATING"
```

「高级标签信息」标签页就是把 `tag->properties()` 整个 `PropertyMap` 铺出来（`GetTagPropertyMap()`）。

## 5.2 ⭐ 编码问题：一个必须知道的 TagLib 坑

**TagLib 把所有「非 Unicode 编码」的字符串一律当作 Latin-1 处理。** 所以 GBK 写的中文标签，TagLib 会给你一串 Latin-1 的乱码字符。

【源码确证】`TagLibHelper.cpp:49-60` —— 这是全篇第二值得抄的小技巧：

```cpp
//将taglib中的字符串转换成std::wstring类型。
//由于taglib将所有非unicode编码全部作为Latin编码处理，因此无法正确处理本地代码页
//这里将Latin编码的字符串按本地代码页处理
static std::wstring TagStringToWstring(const String& str, bool to_local) {
    std::wstring result;
    if (to_local && str.isLatin1())
        result = CCommon::StrToUnicode(str.to8Bit(), CodeType::ANSI);   // ← 关键：按 CP_ACP / GBK 重新解释字节
    else
        result = str.toWString();
    return result;
}
```

**所有标签读取都过这个函数**（`TagToSongInfo` 里 6 个字段、`GetTagPropertyMap` 里所有 value 都传 `to_local=true`）。

其他地方：
- **ID3v1 vs ID3v2 的优先级**：`getMpegPropertyMap()` 的顺序是 **ID3v2 → APE → ID3v1**，但 `GetTagPropertyMap()` 里对同名 key 是**先到先得**（`if (iter == property_map.end()) property_map[key] = value; else if (iter->second.empty()) ...`）。所以实际优先级是 **ID3v2 > APE > ID3v1**。【源码确证】`TagLibHelper.cpp:506-514`
- **ID3v1 是 Latin-1 的宿命**：ID3v1 规范就是 Latin-1，中文标签在 v1 里本来就是非标做法，只能按本地代码页猜。**这就是「ID3v1 中文乱码」的根本原因，不是它能修的 bug。**
- **genre 是数字时查表**：`if (IsStringNumber(song_info.genre, num)) song_info.genre = CAudioCommon::GetGenre(num);` —— 标准的 ID3v1 genre 索引表。
- **写入 ID3v2 版本可选**：`GetWriteId3v2Version() → ID3v2::Version::v3` 或 `v4`，**默认 v3**.【文档确证】Wiki 解释了原因：「某些软件对 ID3v2.4 的支持不够完整……例如使用 ID3v2.4 时，通过 MusicPlayer2 向音频文件写入专辑封面后，Windows 资源管理器可能无法正常显示」。**这个默认值一定要跟着抄 —— v3 是兼容性最好的选择。**

## 5.3 封面：来源优先级、缩放与缓存

【源码确证】`Player.cpp:2438 CPlayer::SearchAlbumCover()`：

```
① 内嵌封面（ID3v2 APIC / FLAC Picture / MP4 covr / ASF WM/Picture）
     条件：!use_out_image || use_inner_image_first
② 外部图片文件（同目录 cover.* / AlbumCover.*；或「封面文件夹」；支持绝对路径）
     条件：① 拿不到
③ 在线下载（网易云 / QQ音乐）
     条件：开关打开 + 上面的都拿不到
④ 显示默认背景图 / 黑胶唱片图
```

**大图性能问题怎么解决** —— 【源码确证】`Player.cpp:2513 CPlayer::AlbumCoverResize()`：

```cpp
if (!m_album_cover.IsNull() && max_album_cover_size > 0) {
    CSize image_size{ m_album_cover.GetWidth(), m_album_cover.GetHeight() };
    if (max(image_size.cx, image_size.cy) > max_album_cover_size) {     // 默认 800
        wstring temp_img_path{ CCommon::GetTemplatePath() + ALBUM_COVER_TEMP_NAME };
        CDrawCommon::ImageResize(m_album_cover, temp_img_path, max_album_cover_size, IT_PNG);
        m_album_cover.Destroy();
        m_album_cover.Load(temp_img_path.c_str());
        m_album_cover_info.size_exceed = true;
    }
}
```

**「超过 800px 就缩到 800px 再画」** —— 这是 V2.70 修「界面卡顿」的那一条。**任何时候都不要把原始大图直接丢给每帧的绘制。**

**封面处理的「三段式」**（【源码确证】`AudioTag.cpp:199`）：**读出字节 → 写到 `%TEMP%` 下一个无扩展名的文件 → 让 GDI+ 自己嗅探格式再 `Load`**。没有扩展名是故意的 —— 逼 GDI+ 靠魔数判断，绕开「MIME 字段经常是错的」这个问题。**这个技巧值得抄。**

同一个思路也用在高斯模糊背景上（`AlbumCoverGaussBlur()`）：**先缩到长边 300 再模糊**（`SizeZoom(image_size, 300)`），把 O(w·h·r) 的计算量砍掉一个数量级。**模糊半径和图像尺寸是乘积关系，先降采样再模糊是免费的巨大加速。**

> **更好的做法**：做一个**磁盘缩略图缓存**（比如 `%LOCALAPPDATA%\App\covercache\<hash>-256.jpg`），列表里的小图用 64px 缓存、详情页用 512px 缓存。**它没有做磁盘缓存**，所以每次启动/每次切歌都要重新解码原图 —— 这是你可以明显改进的点。

**GDI vs GDI+ 的取舍**：【文档确证】Wiki 明确说「专辑封面使用 GDI+ 绘图」这个选项**大多数情况下效果不明显，帧率低就别开**。原因就是 GDI+ 的 `Bitmap` 绘制走 CPU + 软件合成，比 GDI `StretchBlt` 慢很多。

## 5.4 ⭐⭐ 这里最容易踩的坑（按严重度排序）

| # | 坑 | 依据 | 怎么避 |
|---|---|---|---|
| 1 | **TagLib 把 GBK 当 Latin-1 → 中文全乱码** | `TagStringToWstring` 的实现与注释 | 所有标签字符串都过 `isLatin1()` 检测 + `MultiByteToWideChar(CP_ACP)` 回退。**新项目用 Rust 的话，lofty / audiotags 也有类似问题，务必先用中文 GBK 标签的 mp3 实测。** 另外这个启发式**会误伤真 Latin-1 文本**（`Björk` / `Motörhead` 在中文代码页下会乱码）—— 这是有意识的取舍，你要知道它的边界。 |
| 2 | **写标签会把用户的文件改坏** —— 原地改，失败没有回滚 | `MPEG::File::save(tags, File::StripOthers, version)` | 大厂做法：写临时文件 + `rename` 原子替换；至少要保证「写之前备份」或「写失败不动原文件」。**这个项目没有做，所以 issue 里有「在软件内修改曲目信息，Windows 资源管理器里会显示乱码」（#939）这类投诉。** |
| 3 | **字段重复写入**（V2.77 修过 `ALBUMARTIST` / `DISCNUMBER` 重复） | `WriteOtherProperties` 里 `properties["ALBUMARTIST"].clear();` —— **先 clear 再 set** | TagLib 的 `PropertyMap` 是 `StringList`，`.append()` 是**追加**；`file.properties()` 已经含旧值，所以每保存一次就多一条。**这个 bug 活了 3 年**（2020-09 引入 → 2023-11 才修）。症状是渐进式的，跑一遍测试根本发现不了。**必须先 clear 再 append。** |
| 4 | **Unicode 字符变成问号**（V2.73 修过「转换成 mp3 后一些 Unicode 字符会变成问号」） | `CCommon::UnicodeToStr(str, code_type, &char_connot_convert)` | **编码转换必须检查「有字符转换不了」的情况**。它的做法是：转换失败就弹框问用户是否改用 Unicode 编码保存。**你要做的是「永不静默丢字符」。** |
| 5 | **`File::StripOthers` 会静默删掉用户的 ID3v1** | `WriteMpegTag` —— `tags` 里只放 ID3v2，代码里 `tags |= MPEG::File::ID3v1` 那两行**被注释掉了** | 需要兼容老设备的用户**保存一次标签就丢数据，界面无任何提示**。写标签前要先问「我要保留哪些标签类型」，把 `StripOthers` 换成显式的类型集合。 |
| 6 | **`WriteM4aTag` 漏了一行** | `TagLibHelper.cpp:1546` —— 全文件 `WriteOtherProperties` 被调了 12 次，**唯独漏了 M4A** | 结果：编辑 M4A 的专辑艺术家 / 碟号不会被保存。**这种「12 个分支漏 1 个」的错误必须有测试兜住**：写一个「每种格式都写一遍全部字段再读回来比对」的表驱动测试。 |
| 7 | **`switch` 里注释掉代码却不写 `break`** | `AudioTag.cpp:351` / `:392` —— `case AU_OPUS:` 后面跟一行注释，直接贯穿到 `case AU_WV:` | 对 `.opus` 写标签实际走的是 `WavPack::File`。目前被 `IsFileTypeTagWriteSupport` 挡在 UI 外，是定时炸弹。**开 `-Wimplicit-fallthrough`（C++17 `[[fallthrough]]`）能自动抓这类问题。** |
| 8 | **ID3v2.4 兼容性差** | 【文档确证】Wiki 明说 Windows 资源管理器读不到 v2.4 写的封面 | 默认写 v2.3。 |
| 9 | **长路径 / 长文件名** | issue #792「更新后文件重命名功能出问题」+ 贡献者注释 | 重命名模板要用 `%(Artist)` 的**第一个艺术家**而不是全部，否则「超过 10 人的艺术家」会让路径超过 260 字符。**它的应对是主动截断文件名（`CheckFilePathLength`）而不是突破 MAX_PATH 限制 —— 这是个务实的选择。** |
| 10 | **内嵌封面可能是 BMP / 非标准 MIME / 封面封底搞混** | V2.70 修过「无法获取 bmp 格式专辑封面」；`GetPicType()` 只认 jpeg/png/gif/bmp；`GetId3v2AlbumCover` 只取 `frameList.front()` 而不挑 `FrontCover` 类型 | 别假设封面是 JPEG。**MIME 字段经常是错的，应该靠魔数嗅探而不是靠 MIME 字符串**；一个有多个 APIC 帧（封面+封底+艺人照）时要显式挑 `PictureType == FrontCover`。 |
| 11 | **超大图片直接把 UI 拖死** | V2.70 的修复 + `AlbumCoverResize` | 见 §5.3。 |
| 12 | **`is_cue` 分轨的标签是「虚拟」的** | `SongDataManager` 里 cue 轨的标签存在曲库而不是文件里 | cue 分轨的编辑要写回 cue 文件或只写曲库，**绝对不能去改音频文件**。 |
| 13 | **网络盘 / 只读介质** | 错误处理路径 | 写标签前检查可写性，失败给明确提示而不是静默。 |
| 14 | **换库后丢掉的信息，只能靠取巧捞回来** | 旧实现 `AudioTagOld.cpp:435 GetSpecifiedId3V2Tag` 是**真的在读帧的 encoding byte**（`switch (tag_contents[tag_index+10])`：0→ANSI、1/2→UTF16、3→UTF8），比新实现准确；它还按魔数（`jpg_head`/`png_head`/`"BM"`）嗅探封面格式。TagLib 把这些抽象掉了，只能用 `isLatin1()` 猜 + 用扩展名拼 MIME | **这是换库必然要付的税。** 换库前先列出「哪些信息在新库里拿不到」，能自己补的就自己补 |

### ⭐ 一个必抄的解法：`CPlayer::ReOpen` —— 「改正在播放的文件」的标准姿势

改标签时文件正被播放器打开着，怎么办？【源码确证】`Player.h:563` 给了一个 RAII 守卫：

```cpp
// 用于在执行某些操作时，播放器需要关闭当前播放的歌曲，操作完成后再次打开
// 使用后需要先检查 IsLockSuccess，如果返回 false（极小概率）那么此次操作应当放弃并让出主线程，在主线程等待会死锁
struct ReOpen {
    ReOpen(bool reopen) : m_reopen{ reopen } {
        if (m_reopen && !m_instance.m_loading
            && m_instance.GetPlayStatusMutex().try_lock_for(std::chrono::milliseconds(1000)))   // ← 超时而不是死等
        {
            lock_success = true;
            current_position = m_instance.GetCurrentPosition();
            is_playing       = m_instance.IsPlaying();
            current_song     = m_instance.GetCurrentSongInfo();
            m_instance.MusicControl(Command::CLOSE);      // 关掉流，释放文件句柄
        }
    }
    ~ReOpen() {
        if (lock_success) {
            m_instance.MusicControl(Command::OPEN);       // 重新打开
            m_instance.SeekTo(current_position);          // 恢复位置
            if (is_playing) m_instance.MusicControl(Command::PLAY);
            m_instance.GetPlayStatusMutex().unlock();
        }
    }
    bool IsLockSuccess() { return !m_reopen || lock_success; }   // false 时调用方应放弃本次操作
private:
    int current_position{};  SongInfo current_song;  bool is_playing{};
    bool m_reopen{};  bool lock_success{};
};
```

**四个设计要点：**
1. **RAII**：构造关流、析构必然恢复（异常路径也安全）。
2. **`try_lock_for(1000ms)` 而不是死等** —— 注释明确写了「在主线程等待会死锁」。
3. **`IsLockSuccess()` 让调用方有机会放弃** —— 「拿不到锁」是个正常的、可预期的结果，不是错误。
4. **保存并恢复播放位置和播放状态** —— 用户感觉不到流被关过。

调用点只有一处：`PropertyDlg.cpp:164`（属性对话框）。**如果你要让用户编辑正在播放的文件标签，这就是标准答案。**

---

# 6. UI 组织方式

## 6.1 先说清楚它到底是什么

- **MFC 对话框程序**（`CDialog` + 消息映射 + `.rc` 资源）。README 明确要求 VS 安装时勾选 MFC。
- 【源码确证】主窗口 `CMusicPlayerDlg : CMainDialogBase : CDialog`（`MusicPlayer2/MusicPlayerDlg.cpp`，291KB / 6892 行 —— **这是一个应该被拆开的文件**）。
- 【源码确证】窗口结构：
  - 主窗口 `CMusicPlayerDlg`
  - ├─ 左侧播放列表（`CPlayListCtrl`）、路径栏、工具栏（`CPlayerToolBar`）
  - ├─ 右侧「界面区」：一个 `CStatic` 子控件（`m_ui_static_ctrl`），**整个可视化界面都是在这一个控件上自绘的**
  - ├─ 迷你模式 `CMiniModeDlg`、浮动播放列表 `CFloatPlaylistDlg`
  - └─ 各种对话框：`CMediaLibDlg`（媒体库）、`CMediaLibTabDlg` 派生的一堆标签页、`CPropertyDlg`、`COptionsDlg`、`CLyricEditDlg`…

## 6.2 界面区：**XML 驱动的自绘 UI 引擎**（核心设计）

这是它「小巧又百变」的根本。`CPlayerUIBase` 是界面基类，`CUserUi` 是 XML 派生的实现：

【源码确证】`UserUi.h`：
```cpp
class CUserUi : public CPlayerUIBase {
    std::shared_ptr<UiElement::Element> m_root_default;    // 同一份 XML 可以有三套布局
    std::shared_ptr<UiElement::Element> m_root_ui_big;
    std::shared_ptr<UiElement::Element> m_root_ui_narrow;
    std::shared_ptr<UiElement::Element> m_root_ui_small;
    static std::shared_ptr<UiElement::Element> BuildUiElementFromXmlNode(tinyxml2::XMLElement*, CPlayerUIBase*);
};
```

【源码确证】`UIElement/ElementFactory.cpp` —— **`name` → 元素的工厂**，40+ 种：

```cpp
if      (name == "verticalLayout")        element = make_shared<Layout>(Vertical);
else if (name == "horizontalLayout")      element = make_shared<Layout>(Horizontal);
else if (name == "stackElement")          element = make_shared<StackElement>();
else if (name == "rectangle")             element = make_shared<Rectangle>();
else if (name == "button")                element = make_shared<Button>();
else if (name == "text")                  element = make_shared<Text>();
else if (name == "albumCover")            element = make_shared<AlbumCover>();
else if (name == "spectrum")              element = make_shared<Spectrum>();
else if (name == "trackInfo")             element = make_shared<TrackInfo>();
else if (name == "progressBar")           element = make_shared<ProgressBar>();
else if (name == "lyrics")                element = make_shared<Lyrics>();
else if (name == "volume")                element = make_shared<Volume>();
else if (name == "playlist")              element = make_shared<Playlist>();
else if (name == "navigationBar")         element = make_shared<NavigationBar>();
else if (name == "panel")                 element = make_shared<Panel>();
else if (name == "rating")                element = make_shared<RatingElement>();
else if (name == "searchBox")             element = make_shared<SearchBox>();
else if (name == "scrollArea")            element = make_shared<ScrollArea>();
/* ... 还有 recentPlayedList / mediaLibItemList / myFavouriteList / allTracksList /
   mediaLibFolder / medialibFolderExplore / elementSwitcher / icon / slider / comboBox ... */
```

【源码确证】皮肤文件：`MusicPlayer2/res/skins/ui1.xml`、`ui2.xml`（内置资源）、`MusicPlayer2/skins/*.xml`（用户可改，`skins/` 目录启动时自动扫描），外加一个 **87KB 的 `skin.xsd`**（完整的界面定义规范）。

【文档确证】Wiki《用户自定义界面》里给了一个完整例子：

```xml
<root name="测试界面">
  <ui type="big">
    <verticalLayout margin="4">
      <trackInfo height="24"/>
      <horizontalLayout>
        <verticalLayout>
          <albumCover square="true" margin="20" />
          <text type="title" style="scroll" height="24" alignment="center" font_size="10"/>
          <spectrum height="48" width="280" draw_reflex="true"/>
        </verticalLayout>
        <lyrics margin="4"/>
      </horizontalLayout>
      <horizontalLayout margin="4" height="40">
        <button key="previous" width="32" height="32" bigIcon="true" margin="2"/>
        <button key="playPause" width="32" height="32" bigIcon="true" margin="2"/>
        <progressBar show_play_time="true"/>
      </horizontalLayout>
    </verticalLayout>
  </ui>
  <ui type="narrow"> ... </ui>       <!-- 窄布局 -->
  <ui type="small">  ... </ui>       <!-- 小布局（窗口高度不足时自动切换） -->
</root>
```

## 6.3 布局引擎：手写的 Flexbox

【源码确证】`UIElement/Layout.cpp` —— 三百多行，实现了简化版 flex：

- 属性：`margin-*`、`x`/`y`、`width`/`height`（**支持百分比字符串**）、`min/max_width`/`min/max_height`、`proportion`（比例分配）、`hide_width`/`hide_height`（小于该尺寸就隐藏）
- 算法（水平为例）：
  1. 第一遍：算出所有**固定宽度**元素的宽度和 + 边距总和；
  2. 若有多余空间且**所有子元素都是固定宽度** → **整体居中**；
  3. 否则按 `proportion` 分配剩余空间，**逐个检查 min/max 是否冲突**，冲突就把该元素「升格为固定宽度」然后 `continue` 重算 —— 一个朴素但正确的不动点迭代；
  4. 第二遍：从左到右累加 `left/right`。
- 【源码确证】`Element::Value` 支持百分比：
  ```cpp
  struct Value { int value; bool is_percentage; bool is_vertical; Element* owner;
                 int GetValue(CRect parent_rect) const; };
  ```
  并且**百分比是相对父元素算的**（`owner` 指针），这就是 `height="60%"`（皮肤里的频谱）能工作的原因。

**这个布局引擎是现代 UI 框架白送的**（WPF `Grid`/`StackPanel`、Avalonia、egui 的 `ui.horizontal`…），**没必要抄**。要抄的是「**同一份界面定义，按尺寸切换三套布局（big/narrow/small）**」这个思路 —— 这比「响应式断点」更直白，而且用户在 XML 里就能改。

## 6.4 列表控件用了什么

**三层并存**（历史包袱）：

1. **老的 MFC `CListCtrl` 派生**：`ListCtrlEx`、`TreeCtrlEx`、`PlayListCtrl`、`CSelectPlaylist`、`CMiniModeDlg` 里的列表…（`LVS_OWNERDRAWFIXED` 自绘）。
2. **新的自绘列表元素**：`UiElement::AbstractListElement` 及其派生（`Playlist`、`AllTracksList`、`MyFavouriteList`、`RecentPlayedList`、`MediaLibItemList`、`MediaLibFolder`、`MediaLibPlaylist`、`TracksList`、`TreeElement`/`FolderExploreTree`）—— **完全不走 Win32 控件，纯 `CDC` 绘制**。
3. **可复用的 `ListElement`**：一个「列 + 行数据」的通用自绘列表（供对话/面板用）。

【文档确证】V2.76 新增自绘播放列表，V2.77 才补上「多选 / Ctrl+A 全选 / 删除后按 Delete 键」这些本该有的行为 —— **自绘控件的隐性成本：你会把 Win32 控件免费给你的东西重新实现一遍（选中、多选、键盘导航、无障碍、IME、拖放、高 DPI、触摸板手势）。** V2.77 还在修「触摸板手势时列表滚动过快」「触摸板手势时音量调整过快」。

## 6.5 换肤/主题机制

| 机制 | 实现 |
|---|---|
| **主题色** | 【文档确证】跟随系统强调色（`DwmGetColorizationColor` 之类，Wiki 说「支持 Windows Vista 及以上」）或自定义；影响主界面、迷你模式、搜索框、所有列表控件。相关消息 `WM_COLOR_SELECTED`。【源码确证】`CPlayerUIBase::m_colors` + `UIColors` 结构体统一取色 |
| **深色/浅色** | 【源码确证】`theApp.m_app_setting_data.dark_mode`，影响背景 alpha 计算（`ALPHA_CHG(transparency)/3` vs `/2`）、图标选择、通知区图标自适应 |
| **圆角风格** | 【文档确证】`button_round_corners`，Vista/Win7/Win11 默认开，Win8/8.1/10 默认关。**【文档确证】Wiki 明确说画圆角矩形会明显增加 CPU 占用、降低帧率** |
| **皮肤** | 就是 §6.2 的 XML 文件，`skins/` 目录 + 内置 res。**换肤 = 换布局，不是换图片** |
| **图标** | 【源码确证】`IconMgr`：`IconType`（几十种）× `IconStyle`（彩色/单色描边深浅）× `IconSize`（DPI 16/32/…），从资源里按需取 `HICON` |
| **背景** | 专辑封面 / 桌面壁纸 / 指定图片 + `GaussBlur` 高斯模糊 + 不透明度 + 开关 |
| **DPI** | 【源码确证】`theApp.DPI(x)` 全局缩放函数，所有自绘尺寸都过它 |

## 6.6 绘制管线：后台线程 + 双缓冲

【源码确证】`MusicPlayerDlg.cpp:4538 CMusicPlayerDlg::UiThreadFunc()`：

```cpp
while (true) {
    if (pPara->ui_thread_exit) break;
    CPlayer::GetInstance().CalculateSpectralDataPeak();          // 频谱峰值衰减在 UI 线程算
    if (IsPlaying() && GetPlayStatusMutex().try_lock_for(10ms)) {
        GetPlayerCoreCurrentPosition();                          // 每帧只取一次播放位置
        unlock();
    }
    if (IsWindowVisible() && !IsIconic()
        && (IsPlaying() || is_active_window || draw_reset || ui_force_refresh
            || m_loading || theApp.IsMeidaLibUpdating())
        && (!is_completely_covered || always_on_top))
        fresh_cnt = 2;                                           // ← 状态变化时连刷两帧
    if (fresh_cnt) { fresh_cnt--; m_pUI->DrawInfo(draw_reset); draw_reset = false; ui_force_refresh = false; }

    if (IsWindow(m_miniModeDlg.GetSafeHwnd())) m_miniModeDlg.DrawInfo();
    if (cortana_info_enable) m_cortana_lyric.DrawInfo();
    m_desktop_lyric.ShowLyric();
    m_controls.UpdatePosition(GetCurrentPosition());              // SMTC 进度
    m_fps_cnt++;
    Sleep(m_ui_refresh_interval);                                 // 默认 50ms
}
```

**一个后台线程包揽了「主界面 + 迷你模式 + Cortana 歌词 + 桌面歌词 + SMTC 进度」**，主消息循环只剩事件处理 —— 这就是 V2.70「解决 UI 绘图耗时过长导致消息阻塞」的全部秘密。

**四件值得抄的事：**
1. **双缓冲 RAII**：`CDrawDoubleBuffer`（`DrawCommon.h`），构造建 memDC + CompatibleBitmap，析构时 `BitBlt` 回屏。`CPlayerUIBase::DrawInfo` 里用它。还有「跳过下一帧」技巧：`if (m_skip_next_frame) pDC = nullptr;` —— 把 DC 置空等于整帧不画。
2. **空闲降载**：窗口不可见/最小化/未激活且未播放/被完全遮挡 → **根本不刷**。
3. **fps 自适应闭环**：【源码确证】每秒统计实测 fps，`if (fps > MAX_FPS + MARGIN) m_ui_refresh_interval++;` / `if (fps < MAX_FPS - MARGIN) m_ui_refresh_interval--;`（`MAX_FPS 90`、`FPS_LIMIT_MARGIN 10`、间隔范围 `[2, 300]`ms、默认 50ms）—— **自动在帧率和 CPU 之间找平衡**，用户不用管。
4. **连刷两帧**（`fresh_cnt = 2`）—— 状态变化时刷两帧，避免单帧渲染时序问题导致视觉残留。

## 6.7 ⭐ 关于「帧率低」的诚实结论

FAQ 里作者自己写得很直白：

> 目前 MusicPlayer2 的界面渲染使用的是 GDI 和 GDI+，界面帧率低是由于 GDI 绘图的效率限制，由于本人能力有限，这个问题目前还没有很好的解决办法。GDI 绘图使用的是 CPU，无法使用 GPU 加速。窗口面积越大，绘制一帧画面的时间就会越长，帧率就会越低。

给出的缓解手段（都要用户手动去做）：调小刷新间隔（默认 50ms → 20ms，但 CPU 上升）、缩小窗口、关掉背景、关掉 GDI+ 画封面、关掉圆角。

issue #752（19 条评论，全库最高）就是在讨论这个：用户说滚动条「跳动」而不「滑动」、按钮悬停有延迟；作者的回复是「打算以后改成 Direct2D 渲染」（至今没做）。

**给你的判断：这是「用 Win32 + GDI 手搓 UI」这条路的天花板。** 如果你在意动画流畅度，一开始就别走这条路。

## 6.8 ⭐ 换成现代 UI 框架（WPF / Avalonia / egui）：什么能抄、什么必须改

### 可以照搬（与框架无关的设计）

| 设计 | 为什么 |
|---|---|
| **`IPlayerUI` 接口 + 多套界面实现** | 界面与播放逻辑解耦。它对「同一份数据，N 种呈现」的抽象是对的 |
| **「一份界面定义 → big/narrow/small 三套布局」** | 比响应式断点直白，用户在配置里就能改。WPF 里可以用 `DataTrigger` + 多个 `DataTemplate`；Avalonia 同理；egui 里就是 `if width > X { ... } else { ... }` |
| **元素工厂 + 声明式描述** | `name → Element` 的映射，本质就是「声明式 UI」。现代框架给你了，但**「让用户能改布局」这个产品决策**值得保留（可以用 JSON/TOML，也可以直接暴露成配置文件） |
| **列表抽象基类 + N 个数据源适配器** | 见 §4.7，直接照搬 |
| **绘制与逻辑分离（UI 独立线程/独立刷新节奏）** | 现代框架里对应「渲染线程 + 虚拟化」，但要自己做「空闲降载」和「fps 自适应」 |
| **双缓冲 + 只重绘变化区域** | WPF/Avalonia 自动做；egui 是立即模式，每帧全画（但 GPU 加速，成本可控） |
| **频谱/歌词都从「缓存的位置」读，不各自打音频后端** | 与框架无关的好习惯 |
| **`IconMgr` 的「图标类型 × 风格 × 尺寸」三维查找** | 好设计，和框架无关 |
| **`theApp.DPI(x)` 的统一缩放入口** | 现代框架有 DIP/逻辑像素，天然解决；但仍要处理「1px 线在高 DPI 下变糊」 |
| **皮肤 XML + XSD** | 如果你要做可换肤，**保留这个思路，但换格式**（XSD 87KB 太重了） |

### 必须改（GDI/Win32 特有，不要带过去）

| 它这么做 | 你该怎么做 |
|---|---|
| 手写 flexbox 布局引擎（300 行） | 用框架的布局系统（WPF `Grid`/`DockPanel`、Avalonia `Panel`、egui `ui.horizontal`） |
| 手写虚拟化（而且没做对，见 §4.6） | 用 `VirtualizingStackPanel` / `ListView`（Avalonia）/ `egui_extras::TableBuilder`，**默认就有** |
| 手写列表选中/多选/键盘导航/拖放/IME/触摸板 | 用框架控件。**这是自绘控件最大的隐性成本** |
| `CDC` + `TextOut` + `BitBlt` + 手算文本宽度 | 用框架的文本布局（`FormattedText` / `TextLayout`）。`GetTextExtent` 那套「每帧 3~5 次测量」的问题自然消失 |
| `SetTimer` + `Sleep` 轮询刷新 | 用框架的渲染循环；音频位置用**音频回调线程推事件**或高精度定时器读，别在主线程轮询 |
| 自绘标题栏 + `WS_CAPTION` 增删 + `remove_titlebar_top_frame` 这种 hack | 用框架的无边框窗口/自定义 chrome 支持（Avalonia `ExtendClientAreaToDecorationsHint`） |
| 桌面歌词用 `UpdateLayeredWindow` | 框架的透明置顶窗口（Avalonia `TransparencyLevelHint="Transparent"` + `Topmost`）；鼠标穿透仍要平台互操作 `WS_EX_TRANSPARENT` |
| GDI 符号图标（`IconMgr` 从资源取 `HICON`） | 用矢量图标（Path/几何图形），高 DPI 下天然清晰。**它 issue 里反复出现「125%/150% 缩放下图标不清晰」** |
| 一个 291KB / 6892 行的 `MusicPlayerDlg.cpp` | 按功能拆文件/拆组件。**这是它最大的可维护性问题** |
| 手写 `CArchive` 序列化 + INI 配置 | 用 SQLite（数据）+ JSON/TOML（配置） |

---

# 7. 音效与频谱

## 7.1 均衡器：10 段 `BASS_FX_DX8_PARAMEQ`

【源码确证】`BassCore.cpp:244 SetFXHandle()`：
```cpp
for (int i{}; i < EQU_CH_NUM; i++)                                       // EQU_CH_NUM = 10
    m_equ_handle[i] = BASS_ChannelSetFX(m_musicStream, BASS_FX_DX8_PARAMEQ, 1);
m_reverb_handle = BASS_ChannelSetFX(m_musicStream, BASS_FX_DX8_REVERB, 1);
```

【源码确证】`BassCore.cpp:872 ApplyEqualizer()`：
```cpp
if (gain < -15) gain = -15;
if (gain > 15)  gain = 15;
BASS_DX8_PARAMEQ parameq;
parameq.fBandwidth = 30;                     // ← 倍频程，合法范围 1.0~36.0，30 接近极端值
parameq.fCenter    = FREQ_TABLE[channel];
parameq.fGain      = static_cast<float>(gain);
BASS_FXSetParameters(m_equ_handle[channel], &parameq);
```

**`FREQ_TABLE` 有一个明确的 bug**（【源码确证】`BassCore.h:112`）：
```cpp
const float FREQ_TABLE[EQU_CH_NUM]{ 80, 125, 250, 500, 1000, 1500, 2000, 4000, 8000, 1600 };
//                                                                                  ^^^^ 应为 16000
```
交叉证据：FFmpeg 内核的 `GetEqChannelFreq()` 第 10 档是 **16000**；9 个预设里第 10 列全是 0（所以套预设听不出问题，但**手动拉高第 10 段实际动的是 1600Hz**）。对应 issue #905「音效设定->均衡器->16K频点错误」。

**`fBandwidth = 30` 也可疑**：10 个滤波器带宽各 30 倍频程 = 严重重叠。**建议从 1.0~2.0 开始试。**

**9 个预设**（【源码确证】`Define.h:117`，其中「爵士」那行只有 9 个数，C++ 补 0）：
```
无        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
古典      { 4, 3, 3, 2, 2, 1, 0, -1, -2, -2 }
流行      { 3, 2, 0, -1, -2, -2, -1, 0, 2, 3 }
爵士      { 2, 1, 0, -1, -1, 1, 3, 5, 3 }        ← 只有 9 个
摇滚      { -2, 0, 2, 4, -1, -1, 0, 0, 2, 3 }
柔和      { 1, 0, 0, 1, 2, 1, -1, -2, -2, -2 }
重低音    { 4, 6, 6, -2, -1, 0, 0, 0, 0, 0 }
消除低音  { -5, -5, -3, -2, -2, 0, 0, 0, 0, 0 }
弱化高音  { 0, 0, 0, 0, 0, -1, -3, -5, -5, -4 }
```

**一个 Win32 UI 小技巧**：【源码确证】`EqualizerDlg.cpp` 里垂直滑块（`TBS_VERT`）的位置值是**往下增大**的，所以「往上拖 = 增益变大」要取负：
```cpp
m_sliders[i].SetRange(-15, 15, TRUE);
m_sliders[i].SetPos(-CPlayer::GetInstance().GeEqualizer(i));   // 存的时候取负
int gain{ -m_sliders[i].GetPos() };                            // 读的时候再取负回来
```

**增益值存在 Player 层而不是 BASS 层**：因为**换歌时旧 FX 句柄全销毁了，必须由上层重放一遍**（`Player.cpp:540` `if (m_equ_enable) SetAllEqualizer();`）。**这是任何「效果器挂在流上」的设计都要面对的：状态归上层所有。**

## 7.2 混响：`BASS_FX_DX8_REVERB`

【源码确证】`BassCore.cpp:888`：
```cpp
BASS_DX8_REVERB parareverb;
parareverb.fReverbMix        = pow(mix / 100.0f, 0.1f) * 96 - 96;   // ← 对数补偿，值得抄
parareverb.fReverbTime       = time * 10;                            // UI 的 1~300 → 10~3000 ms
parareverb.fHighFreqRTRatio  = 0.001f;
BASS_FXSetParameters(m_reverb_handle, &parareverb);
// ClearReverb: fReverbMix = -96, fReverbTime = 0.001
```

**`pow(mix/100, 0.1) * 96 - 96` 这一条最值得抄**：BASS 的 `fReverbMix` 单位是 dB（-96 = 无声，0 = 全湿），而 UI 滑块是线性的 0~100。直接线性映射会让前 90% 的行程几乎听不出变化。用 `pow(x, 0.1)` 做感知补偿（近似对数），把「人耳感知的均匀」映射到「dB 的均匀」。

**这个模式通用**：任何「UI 线性值 → DSP 对数/dB 参数」的映射都该这么写（音量也是）。

## 7.3 频谱分析

### FFT 是 BASS 做的，项目只做「bin → 柱子」映射

【源码确证】`BassCore.cpp:907`：
```cpp
void CBassCore::GetFFTData(float fft_data[FFT_SAMPLE]) {
    BASS_ChannelGetData(m_musicStream, fft_data, BASS_DATA_FFT1024);
}
```

- **1024 点 FFT，输出 512 个 bin**（正好装进 `FFT_SAMPLE = 512` 的缓冲）。
- **没有窗函数选择**（用 BASS 默认的 Hanning）、**没有去直流**、**没有分声道**（各声道自动合并）。
- **项目自己不写 FFT。**

【源码确证】`Player.cpp:688 CalculateSpectralData()`：
```cpp
if (... && ((GetBassHandle() && m_current_position < m_song_length - 500) || IsFfmpegCore())
    && GetPlayingState() != PS_STOPED) {
    int scale = (m_pCore->GetCoreType() == PT_FFMPEG ? 100 : 60);   // 两个内核量纲不同，分别定标
    m_pCore->GetFFTData(m_fft);
    for (int i{}; i < FFT_SAMPLE; i++) m_fft[i] = std::abs(m_fft[i]);   // ← 必须取绝对值
    SpectralDataMap(m_fft, m_spectral_data, scale);
} else {
    memset(m_spectral_data, 0, sizeof(m_spectral_data));
}
```
> 两个细节：① **FFT 输出是带符号的，必须 `abs`**；② **歌曲最后 500ms 不取频谱**，因为到末尾 `BASS_ChannelGetData` 会失败/返回垃圾（源码注释写明，是真踩过的坑）。

### ⭐⭐ 频段 → 柱子：分段混合刻度 + 预计算映射表

**这是全篇最值得抄的 DSP 工程技巧**（【源码确证】`SpectralDataHelper.cpp:4-25`）：

```cpp
const int LINEAR_SAMPLE_COUNT{ FFT_SAMPLE * 110 / 256 };      // = 220

CSpectralDataHelper::CSpectralDataHelper() {
    // 前 220 个 bin：线性，每 2 个 bin 合成一根柱子
    for (int i{}; i < LINEAR_SAMPLE_COUNT; i++) {
        int m = i / 2;
        spectrum_map[i] = m;
        if (m >= 0 && m < SPECTRUM_COL) spectrum_map_count[m]++;
    }
    // 后面的 bin：对数映射
    for (int i{ LINEAR_SAMPLE_COUNT }; i < FFT_SAMPLE; i++) {
        int m = static_cast<int>(std::log(i) / std::log(FFT_SAMPLE) * SPECTRUM_COL);
        if (m >= 0 && m < SPECTRUM_COL) { spectrum_map[i] = m; spectrum_map_count[m]++; }
    }
}
```

**为什么这么设计：**
- **低频用线性**：`log` 映射会把大量低频 bin 全挤到第 0 根柱子，导致低频柱子「虚高且毫无细节」。而人耳对低频最敏感，宁可牺牲高频分辨率。
- **高频用对数**：符合人耳的等响特性，高频铺开。
- **预计算映射表**：运行时不调 `log()` —— **每帧省 512 次对数运算**，还避免了浮点不确定性。对 UI 帧率是实打实的好处。

**柱子取值**（【源码确证】`SpectralDataMap`）：
```cpp
for (i) spectral_data[spectrum_map[i]] += fft_data[i];               // 求和
for (i) {
    spectral_data[i] /= spectrum_map_count[i];                       // 除以 bin 数 → 平均值
    spectral_data[i] = std::sqrtf(spectral_data[i]);                 // 开方，压缩动态范围
    spectral_data[i] *= scale;                                       // BASS=60 / FFMPEG=100
}
```
- **是平均值不是最大值**：源码注释解释「如果改用最大值，低频部分的频谱会显得过高」。
- **`sqrtf` 是替代 dB 对数刻度的廉价做法**：注释「对每个频谱柱形的值取平方根，以减少不同频率频谱值的差异」。**比 `log10` 便宜一个数量级，视觉上差不多。**
- 新旧两套并存，配置 `use_old_style_specturm` 可回退 —— **「保留旧实现作为回退选项」这个做法本身也值得学**。

**柱子数量**：【源码确证】`Define.h:110` `SPECTRUM_COL = FFT_SAMPLE / 4 = 128`（数据层），UI 层可再压缩成 128/64/32/16/8/4 根（`SpectrumCol` 枚举）。`SC_AUTO` 按矩形宽度自动选（`<20px→4 根，<52→8，<120→16，<240→32，<480→64，否则 128`）。

### 峰值帽衰减：**与帧率强耦合的魔数**

【源码确证】`Player.cpp:711 CalculateSpectralDataPeak()`：
```cpp
static int fall_count[SPECTRUM_COL];
for (i) {
    if (m_spectral_data[i] > m_spectral_peak[i]) { m_spectral_peak[i] = m_spectral_data[i]; fall_count[i] = 0; }
    else if (m_spectral_data[i] < m_spectral_peak[i]) {
        float fall_distance = 0;
        if (theApp.m_fps > 0)
            fall_distance = fall_count[i] * (7.002355f / theApp.m_fps - 0.042824f);
        if (fall_distance < 0) fall_distance = 0;
        m_spectral_peak[i] -= fall_distance;
        fall_count[i]++;
    }
}
```

**意图**：让「峰值下落速度」与 fps 无关（不管 20fps 还是 90fps，一秒内下落的量相同）。`7.002355/fps` 是主项，`-0.042824` 是零阶修正。

**⚠️ 但它依赖 `theApp.m_fps`** —— 如果 fps 估算错了（窗口被遮挡时根本不刷，fps 会掉到 0），衰减就失控。**你该改用真实 delta-time**：`peak -= fall_speed * dt`，`fall_speed` 单位是「单位/秒」。

> 柱子高度本身**没有做平滑/衰减**，用的是瞬时值 —— 这是刻意的（要跟节拍同步，不能糊）。只有峰值帽有衰减。

### 怎么避免频谱刷新拖累 UI

它其实**没有特别为频谱做什么**，靠的是整体架构：

1. **频谱计算跑在 UI 线程里**（每帧一次 `GetFFTData` + 映射），但成本已经被压到很低（1024 点 FFT 由 BASS 用汇编做，映射是查表）。
2. **UI 绘制在独立线程 + 双缓冲**，频谱只是绘制内容的一部分。
3. **空闲/最小化/被遮挡时不刷**。
4. **fps 自适应闭环**：帧率超标自动加大间隔。
5. **可关闭**：`show_spectrum` 开关，`Spectrum::IsShown()` 直接返回 false 时连布局都不参与。

**你该做的（现代架构）：**
- FFT 放到**音频线程或独立分析线程**（音频回调里 tap 一段样本 → 送到分析线程做 FFT → 结果放进无锁双缓冲/`triple buffer`），UI 只读最新一帧结果。**这样 UI 永远不被 FFT 阻塞。**
- 用 `kissfft` / `pffft` / `rustfft` + Hanning 窗。BASS 的便利只在于「能从正在播放的流里直接抽样本」，自己接 FFmpeg/cpal 时要记得插一个 tap。
- 保留它的三段精华：**分段混合刻度 + 预计算映射表 + `sqrt` 压缩**。

## 7.4 这些 DSP 经验对「不用 BASS」的方案还适用吗

| 经验 | 是否适用 | 说明 |
|---|---|---|
| 分段混合刻度 + 预计算映射表 | ✅ **完全适用** | 纯数学，与后端无关 |
| `sqrt` 压缩动态范围 | ✅ 完全适用 | —— |
| 平均值而非最大值 | ✅ 完全适用 | —— |
| 峰值帽的「帧率补偿」思路 | ✅ 思路适用，**实现要改成 delta-time** | 别抄那两个魔数 |
| 最后 500ms 不取频谱 | ⚠️ BASS 特有 | 是 BASS `ChannelGetData` 的行为；别的后端要自己测 |
| `std::abs(fft[i])` | ✅ 适用 | 取决于你的 FFT 库输出格式 |
| 10 段 biquad 参数均衡 | ⚠️ **要自研** | 级联 10 个 peaking EQ biquad，或 FFmpeg 的 `equalizer` 滤镜。**`fBandwidth=30` 这个值千万别抄，从 1.0 开始试** |
| `pow(mix/100, 0.1)` 的 dB 对数补偿 | ✅ **完全适用** | 任何「线性 UI → dB 参数」都该这么做 |
| `fReverbTime = time * 10` 的单位换算 | ⚠️ 是 DX8 的约定 | 自研 Freeverb 时参数含义不同 |
| 换歌后重放全部效果参数 | ✅ **完全适用** | 效果器挂在流上就必然面对 |
| EQ 增益存在播放器层而非后端层 | ✅ 完全适用 | 同上 |
| 变速不变调 | ❌ **必须自研** | SoundTouch / Signalsmith Stretch / FFmpeg `rubberband`/`atempo`。**这是 BASS 最难替的一块** |

---

# 8. 值得警惕的设计

数据来源：`gh issue list -R zhongyang219/MusicPlayer2 --limit 200 --state all` + 更新日志 + 几个高评论 issue 的完整讨论串。

## 8.1 用户抱怨最多的问题（按主题聚类）

| 主题 | 代表 issue | 次数 |
|---|---|---|
| **界面帧率低 / 滚动不跟手 / 卡顿** | #752（19 评论）、#769「fps 5 是正常现象吗」、#920「全屏后歌词滚动卡顿」、#801「拖动新文件进入播放列表时会卡顿回闪」、#909「播放时加入新歌，会导致短暂卡顿」 | **最集中的一类** |
| **歌词/封面下载失效或不匹配** | #881、#898、#893、#816、#940、#900、#911、#788、#795、#730 | 第二多 |
| **界面/布局不满意** | #957「界面4改的一坨」、#754、#760「歌手被砍头」、#875「正在播放的歌曲能高亮显示」、#944「能否实现 apple music 那种歌词高帧滚动」 | 很多 |
| **缺少最小化到托盘** | #767、#823、#921、#964 | 反复被要（已有「最小化到通知区」，但用户要的是托盘行为） |
| **崩溃 / 卡死** | #976（拖 m4a 卡死）、#953（32bit FLAC 卡死）、#789（创建列表闪退）、#802（退出时弹「遇到不适当的参数」） | 少数但严重 |
| **高 DPI 显示问题** | #830「高分辨率屏幕时字体过小」、#98（125%/150% 图标不清晰）、#774（桌面歌词中文显示不全） | 反复 |
| **音量均衡（ReplayGain）** | #935、#932 | 反复被要，从未实现 |
| **网络盘 / SMB / Navidrome** | #764、#945、#904、#739 | 反复被要 |
| **标签写坏 / 乱码** | #939、#760、#888 | 少数但严重 |

## 8.2 作者反复修的问题（= 设计缺陷的信号）

| 问题 | 修了几次 | 根因 |
|---|---|---|
| **GDI 句柄泄漏** | V2.63「修正一个由于GDI句柄泄漏导致程序运行一段时间崩溃的问题」、V2.66「窗口大小改变时会导致GDI句柄泄漏」、V2.70「修正句柄泄漏的问题」 | **手写 GDI 绘制的必然代价**：每个 `CreateFont`/`CreateCompatibleBitmap`/`GetDC` 都要配对释放。**每次加新界面元素都可能再漏一次。** |
| **任务栏/Windows11 相关** | V2.77「修正Windows11任务栏卡死的问题」、#780「会导致windows资源管理器占用异常高」、#963「Win11 自带搜索图标变色」、#890「播放器给系统提供的媒体信息有误」 | Cortana 搜索框硬改系统窗口 + SMTC 集成，都是**依赖系统内部实现**的脆弱功能 |
| **FFmpeg 内核播完不跳下一曲** | #820、#822，V2.78 才修 | **结束信号不可靠**（见 §2.3），靠轮询 + 双判定兜底。这是所有播放器都会遇到的问题，它走了三年才补上兜底 |
| **标签字段重复** | V2.77 修 `ALBUMARTIST`/`DISCNUMBER` 重复 | TagLib `PropertyMap` 的 `insert` 语义不是 `set` |
| **Unicode 字符变问号** | V2.73 修「转换成 mp3 后一些 Unicode 字符会变成问号」 | 编码转换没检查失败 |
| **标题栏白边** | V2.77 加 `remove_titlebar_top_frame`，**但作者自己说「还会闪烁，目前不知道怎么解决」，所以默认关闭且没放进 UI** | 自绘标题栏 + Win10/11 DWM 的边界交互，是个无底洞 |
| **无法自动下载歌词** | #889「不会自动下载歌词」、#846「最新Actions版程序启动时自动播放失效」 | 「首次失败就标记为无歌词」的缓存策略与用户预期不符 |
| **播放列表/媒体库不刷新** | #938、#847、#845、#843、#759 | 缓存失效策略没有统一切口 |
| **多显示器窗口位置** | V2.78 修「主窗口在副显示器最大化后，最小化再点任务栏按钮会显示到主显示器」、#859、#804、V2.73「迷你模式多显示器的支持」 | Win32 多显示器是老大难 |

## 8.3 ⭐「看起来很美好但实际是坑」的功能

### ① 自动歌词/封面匹配 —— **最大的坑**

- `SelectMatchedItem()` **完全没用时长**（§3.6）。→ 录音室版 / 现场版 / 翻唱版必然误匹配。
- 搜索关键字只是 `艺术家 + 空格 + 标题`，**没有去括号、没有清 `feat.`、没有繁简归一**。
- 网易云接口是明文 http、无签名，**上游一改就全挂**（issue #881 一整串）。作者的应对是「加 QQ音乐作为第二个源」—— **这是正确的产品决策，但代价是设置里多一个选择器，而且用户不知道什么时候该切。**
- 更隐蔽的：**「首次失败就标记为无歌词，下次不再请求」**（【文档确证】《常规设置》）—— 用户看到的现象是「它再也不自动下载了」，而不知道该去哪重置。

**你该做的：**
- 匹配必须用**时长**（±5s 打分）+ 专辑 + 标题 + 艺术家的加权。
- 关键词要归一化（去括号内容、去 `feat.`/`ft.`、繁简归一、大小写）。
- **必须有两个以上可切换的源**，且失败要能重试（提供「重试」按钮和「清除失败标记」的入口）。
- **不要把「失败」缓存成永久状态。**

### ② Cortana / Win10 搜索框歌词

`FindWindowEx("Shell_TrayWnd")` 拿系统搜索框 HWND 直接往上画。**依赖任务栏内部结构**，Explorer 重启/系统更新/第三方任务栏工具都会让它失效，失效时表现为「画到错误窗口上」这种极难查的症状。Win11 直接不支持（#841「win11 无法在搜索框播放」）。**别抄。**

### ③ 自绘列表控件

V2.76 才做出来，然后 V2.77 一口气修了「迷你模式播放列表无法双击播放」「stackElement 切换后未显示的按钮还能被点击」「播放列表菜单的全部选择/取消/反选对自绘列表无效」「从播放列表删除曲目后按 Delete 键还能删除」「触摸板手势滚动过快」……**自绘控件是一张长期账单**。

### ④ XML 自定义界面

产品上非常成功（用户真的会自己改皮肤，issue 里有人贴自己的皮肤的），但：
- 需要维护一个 **87KB 的 XSD**；
- 属性名会写错（`ckick_to_switch` 被拼错，作者自己说「某次重构导致的」）；
- 加新元素要同时改：工厂 + XSD + Wiki 文档 + 每套内置皮肤；
- 用户改错了 XML 没有好的报错。

**要做的话：格式简单化（JSON/TOML 就够）、属性名加严格校验、给一份「改坏了怎么恢复」的说明。**

### ⑤ 播放列表的「下一首播放」语义

贡献者 lrisora 在 #759 里说得很清楚：

> 原因是 MusicPlayer2 从最初开始就没有使用播放队列的概念……也就是没有默认使用一个自动调整内容的列表作为播放实体（我很喜欢这一点，播放内容总是已知可控的）

**这是一个刻意的设计取舍。** 代价是「下一首播放」只对**已存在于当前播放列表**的曲目有效 —— 用户从媒体库点「下一首播放」会发现是灰的。**现代播放器（Spotify/网易云）都有独立播放队列，这是用户已经习惯的心智模型。** 你要提前决定：**播放列表 = 播放实体，还是 播放队列 ≠ 播放列表**。选后者对用户更友好，但内部要维护两套状态。

### ⑥ 「无序播放」持久化

issue #779：用户想要「保存未播放完的无序播放列表」，贡献者解释了为什么难做：「列表是会变动的（这包括直接修改 playlist 文件），协调曲目的增减与其随机性很有难度，存储在哪里也是问题」。**这是「随机播放」这个看似简单的功能背后的真实复杂度** —— 洗牌队列必须和列表变更保持一致。

## 8.4 大曲库（几万首）下的性能问题

| 点 | 依据 | 后果 |
|---|---|---|
| **列表不做虚拟化** | §4.6，`CalculateItemRects` 每帧 `resize(总行数)` + 绘制循环遍历总行数 | 1 万行 = 每帧 1 万次矩形计算。默认 50ms 刷新 ≈ 20FPS → **每秒 200 万次矩形写入** |
| **`GetRowCount()` 在循环条件里反复调** | 对树控件是 O(N) | **文件夹浏览树每帧 O(N²)** |
| **搜索态 `IsRowDisplayed` 是线性查找** | `CCommon::IsItemInVector` | 每帧 **O(N × 命中数)**，搜索时明显发卡 |
| **命中测试线性扫描且挂在鼠标移动上** | `GetDisplayedIndexByPoint` | 鼠标划过大列表时每帧 O(N) |
| **`ClassifyMedia()` 被过度调用** | 每次打开媒体库/切标签都**全库重分组 + 重排序 + 深拷贝 `SongInfo`**；而它在「左栏选中项变化」时也会被调（`UIElement/TracksList.cpp:24`），`CUiMediaLibItemMgr::Init()` 里连调 7 次 | **「键盘上下键翻艺术家」= 每按一次一次全库分类。** 几万首时打开媒体库明显卡（对应 V2.70「修正打开媒体库对话框时卡顿的问题」） |
| **「我喜欢」判定是 O(N×F) 线性查找** | `CUiMyFavouriteItemMgr::Contains` 用 `std::find` | 构建「所有曲目」列表时按曲目数 × 收藏数退化成平方级 |
| **`listSearchCache` 的「无变化即跳过」优化失效** | `ListSearchCache.cpp:16` 漏 `return`（见 §4.5） | 每次调用都白重算一遍，无可见症状 |
| **四个 `C*Mgr` 缓存的锁是错的** | `CUiMediaLibItemMgr` / `CUISongListMgr` / `CUiMyFavouriteItemMgr` / `CUiAllTracksMgr` 的 `Update/Init` **用 `shared_lock` 却写数据**，而所有读取方法**完全不加锁** | UI 跑在独立线程（`MusicPlayerDlg.cpp:4538`），读写之间没有任何互斥 → **TOCTOU 悬垂引用**。`CUiFolderExploreMgr` 更彻底：完全无锁、返回内部 `vector` 引用、还在工作线程里 `while(!IsPlayerCoreInited()) Sleep(20)` 忙等 |
| **`GetItemText` 每次调用都重新构造 wstring** | `MediaLibItemList::GetItemText` 等 | 每帧每行每列一次 |
| **V2.70 修过「歌词过多时会导致加载播放列表卡顿」** | 更新日志 | 说明它历史上踩过「一次性加载过多数据」的坑 |
| **`song_data.dat` 全量重写** | §4.1 | 几万首时退出程序明显变慢，且崩溃会毁库 |
| **读取失败后仍然允许写回 —— 这是最危险的一条** | `LoadSongData` 的 `catch` **只写日志、不提示用户、不设置任何标志**；而退出路径（`MusicPlayer2.cpp:329`）**无条件**调用 `SaveSongData()` | **曲库一旦损坏（格式变了/被截断/磁盘错误），启动时静默读失败 → 内存里是一个空库 → 退出时把空库写回去 → 用户的数据永久丢失。** 必须做三件事：① 写临时文件 + 原子替换；② 保留 `.bak`；③ **加载失败后置「禁止保存」标志**，绝不用不完整的数据覆盖 |
| **最多 99999 首的硬上限** | `Define.h` `MAX_SONG_NUM 99999` | 够用，但说明它是按「不会太多」设计的 |
| **扫描是单线程** | §4.3 | 几万首首次扫描很慢 |

**它自己承认的缓解手段**（V2.70）：把 UI 绘图移到后台线程、封面超过 800px 就缩小、「稍微减少了程序的内存占用」。

**你该做的**：真正的虚拟化（O(可见行)）+ SQLite 索引（不要全库分组）+ 增量/并行扫描 + 封面磁盘缓存。这四条基本就能覆盖几万首的场景。

## 8.5 从 issue 学到的产品经验

1. **「最小化到托盘」被要了 4 次**（#767/#823/#921/#964）。作者实现的是「最小化到通知区」，但用户想要的是「启动时最小化到托盘」+「关闭时最小化到托盘」的完整行为。**这是个 10 分钟的功能，但漏了会被反复要。**
2. **「音量均衡 / ReplayGain」被要了 2 次**，从未实现。**这是有声书/古典乐用户的核心需求**（同一张专辑里不同曲目音量差很多）。用 FFmpeg 的 `loudnorm` 离线扫描 + 播放时 `volume` 补偿很容易做，是差异化优势。
3. **多版本歌曲（同一首歌的多个版本）是个真需求**：V2.78 才加「合并正在播放列表中同一首歌曲的不同版本（Beta）」，判定条件是「标题、艺术家、唱片集都相同」。**这也说明「同一首歌」的判定是个需要认真设计的问题**（它用的是文件的物理路径做唯一键，但用户认知里的「同一首歌」是标签级别的）。
4. **网络库 / Subsonic / Navidrome 被要了 4 次**（#739/#904/#906/#945）。**加上你要做在线音源，这一块的设计要提前留出口子。**
5. **#965「微软商店发现恶意套壳加广告收费盗版应用」** —— 开源免费软件的宿命，选许可证时想清楚（它选了 GPL-3.0，所以套壳是违法的，但维权成本高）。**你要做商业产品的话，许可证不能选 GPL。**

---

# 9. 可执行结论

## 9.1 MVP 清单（核心必备）

**目标：一个能日常用起来的本地 + 在线播放器。**

### 第一层：播放器能不能用（第 1~3 周）

1. 音频输出 + 解码（FFmpeg 或 miniaudio，见 §9.3）
2. 打开文件 / 打开文件夹 / 拖入文件
3. 播放、暂停、停止、上一曲、下一曲、进度跳转、音量
4. 播放列表（增删排序、拖拽）+ 顺序/循环/单曲/随机
5. 播放位置记忆（上次曲目 + 上次进度）
6. 命令行参数 / 文件关联（双击播放）
7. **崩溃处理 + 日志**（别等到最后做）

### 第二层：能不能当日常播放器（第 4~8 周）

8. **歌词：LRC 解析 + 逐字高亮 + 多行滚动 + 翻译**（§3.2/3.3/3.4/3.5 —— 全部可直接照搬思路）
9. **标签读取 + 封面读取**（lofty / TagLib / audiotags），**必须用 GBK 标签的 mp3 实测**
10. **曲库：SQLite + 文件夹扫描 + 增量更新**（§4.1 的反面教材 + §4.3 的增量思路）
11. 搜索（标题/艺术家/专辑 + 拼音首字母）
12. **SMTC / 系统媒体控件**（多媒体键、锁屏、蓝牙耳机）
13. 通知区图标 + 最小化到托盘
14. 深色/浅色 + 系统主题色
15. 全局快捷键（至少：播放/暂停、上一曲、下一曲、音量）

### 第三层：在线音源（第 9~14 周）

16. **音源层抽象**（见 §9.2，这是你相对它的核心差异）
17. 搜索 + 播放（http 流，带缓存和重试 —— 参考 `ffmpeg_core` 的 `cache_length`/`max_retry_count`/`url_retry_interval`）
18. 在线歌词/封面获取（**至少两个源 + 用时长校验匹配**，见 §8.3①）
19. 播放队列（在线音源会强制你面对「播放列表 vs 队列」的问题，见 §8.3⑤）

### 第四层：锦上添花（按用户反馈排）

20. 歌词编辑 / 桌面歌词
21. 频谱分析（它的三招可以直接抄）
22. 均衡器（自研 biquad）
23. 标签写入（**先做备份/原子写**，见 §5.4）
24. 迷你模式
25. 可换肤（**别用 XML+XSD，用 JSON**）
26. 格式转换、AB 重复、变速变调、MIDI、cue 分轨

### 明确可以砍掉的

- Cortana/Win10 搜索框歌词（脆弱、Win11 不支持）
- last.fm scrobbling
- 格式转换（除非目标用户有需求）
- MIDI + sf2
- osu! 支持
- 多语言（除非要分发）
- 12 套内置界面（**先做 1 套，把换肤机制做好**）

## 9.2 ⭐ 建议的技术架构分层

因为我加在线音源，**「音源层」必须是第一等公民**。MusicPlayer2 最大的结构局限就是「一切皆本地文件路径」—— 它的 `SongKey` 是**文件绝对路径**，它没有「一个不落地的曲目」这个概念。

```
┌─────────────────────────────────────────────────────────────┐
│  UI 层（WPF / Avalonia / egui）                             │
│  · 视图与逻辑分离，列表用框架的虚拟化                        │
│  · 一个列表抽象基类 + N 个数据源适配器（抄 §4.7）            │
│  · 一份布局定义 → 多套尺寸布局（抄 §6.2 的思路）             │
│  · 空闲降载 + 刷新节奏自适应（抄 §6.6）                      │
└───────────────┬─────────────────────────────────────────────┘
                │ 只读「播放状态快照」（位置/状态/当前曲目），不直接调音频后端
┌───────────────┴─────────────────────────────────────────────┐
│  播放器 / 播放编排层（Playback Orchestrator）                │
│  · 播放队列（独立于播放列表！）                              │
│  · 循环模式、下一首、历史回溯、无缝接歌（做不做先留口子）     │
│  · 「播放位置缓存」：每帧只取一次（抄 §2.3④）                │
│  · 播放结束判定：信号 + 超时兜底双判定（抄 §2.3① + 反例）     │
│  · 效果器状态归这一层所有，换曲后重放（抄 §7.1）             │
└───────────────┬─────────────────────────────────────────────┘
                │ 统一接口：open(url|path) → 解码流
┌───────────────┴─────────────────────────────────────────────┐
│  音频后端层（AudioBackend trait/interface）                  │
│  · 抄 IPlayerCore 的语义（毫秒/半音/dB/能力查询），           │
│    但把 FFT 签名改成 (float*, int count, int sample_rate)、  │
│    把 MIDI/编码拆出去，不要 void* 类型擦除                    │
│  · 实现：FfmpegBackend（主）/ MediaFoundationBackend（兜底）  │
│  · 播放位置、时长、跳转、音量、结束判定                       │
└───────────────┬─────────────────────────────────────────────┘
                │
┌───────────────┴─────────────────────────────────────────────┐
│  DSP / 分析层（独立线程）                                    │
│  · biquad EQ 级联（自研，10 段 peaking）                     │
│  · 混响（Freeverb/Schroeder，mix 用 pow(x,0.1) 映射）        │
│  · 变速变调（SoundTouch / Signalsmith Stretch）              │
│  · FFT 分析线程 → triple buffer → UI 只读最新帧              │
│    分段混合刻度 + 预计算映射表 + sqrt 压缩（抄 §7.3）         │
│  · ReplayGain / 音量均衡（差异化优势，见 §8.5②）             │
└───────────────┬─────────────────────────────────────────────┘
                │
┌───────────────┴─────────────────────────────────────────────┐
│  ⭐ 音源层（Source Providers）—— 你的核心差异，抽象要早做     │
│                                                              │
│  trait MusicSource {                                         │
│      fn id(&self) -> SourceId;                               │
│      fn search(&self, q: &Query) -> Vec<SearchResult>;       │
│      fn tracks_in(&self, item: &ItemId) -> Vec<Track>;       │
│      fn resolve_stream(&self, t: &Track) -> StreamHandle;    │<-- 关键
│      fn lyrics(&self, t: &Track) -> Option<Lyrics>;          │<-- 关键
│      fn cover(&self, t: &Track) -> Option<ImageRef>;         │<-- 关键
│  }                                                           │
│                                                              │
│  实现：LocalFileSource / 各在线平台 Source / SubsonicSource   │
│                                                              │
│  ★ Track 必须有「来源」+「源内 ID」+「可选的本地路径」        │
│    —— 不要用文件路径当唯一键（这是它的根本局限）              │
│  ★ StreamHandle 统一「本地文件 / http url / 需要解密的流」    │
│  ★ 每个 source 的搜索/流解析/歌词都要能独立失败并降级         │
│  ★ 缓存层：搜索结果、流 url（带过期时间）、歌词、封面          │
└───────────────┬─────────────────────────────────────────────┘
                │
┌───────────────┴─────────────────────────────────────────────┐
│  数据层                                                      │
│  · SQLite：曲库元数据（可重建）+ 统计（不可丢）→ 分两张表     │
│    或分两个库：cache.db（可删）+ user.db（要备份）            │
│  · 播放列表：SQLite 表（不要一个列表一个文件 —— 见 §4.1 的代价）│
│  · 配置：JSON/TOML（不要手写 INI 解析器）                     │
│  · 封面缩略图磁盘缓存（hash 命名）—— 它没做，你补上           │
│  · 歌词：文件（沿用 lrc/ksc/vtt + 内嵌），路径可配置          │
└─────────────────────────────────────────────────────────────┘
```

**几条关键的架构约束（都是我上面论证过的）：**

1. **曲目的唯一键不能是文件路径。** 用 `(source_id, source_track_id)`；本地源可以是路径哈希。这是「加在线音源」的第一前提。
2. **音源层必须能「不落地」地提供音频**（直接给 URL 或流），所以解码层要吃 URL 而不只是文件路径。
3. **歌词和封面要按「源」分派**，本地源读文件/内嵌，在线源走 API。**匹配算法共用一套（含时长校验）**。
4. **统计和元数据分开存**：元数据可重建（删了重扫），统计不能丢（要备份）。它把两者混在一个 `song_data.dat` 里是错的。
5. **DSP 和分析跑在自己的线程**，UI 只读快照。别学它「频谱在 UI 线程里算」。
6. **播放队列独立于播放列表。** 这是用户心智模型，也是在线音源的自然要求。

## 9.3 什么必须自己写 / 用什么现成库 / 什么能砍

### 必须自己写（没有现成的好方案，或者抄它的思路最省事）

| 项 | 说明 | 可抄的部分 |
|---|---|---|
| **歌词解析 + 时间轴规范化** | 没有通用的跨语言库覆盖 LRC/扩展LRC/KSC/VTT + 翻译配对 + 偏移 | **§3.2 的数据结构 + §3.5 的滚动算法几乎可逐行翻译** |
| **逐字高亮的进度计算** | 同上 | **§3.4 的「像素宽度加权 + 测量回调注入」直接照搬** |
| **逐字高亮的绘制** | 每家的绘制 API 不同 | **「同一行画两遍 + 窄矩形裁剪」的思路通用**（egui 用 `painter.with_clip_rect`，WPF 用 `Clip`） |
| **音源层抽象 + 各平台的 search/resolve/lyrics** | 业务逻辑，没有通用库 | 参考 §3.6 的 `CLyricDownloadCommon` 基类设计（源可插拔） |
| **匹配算法（带时长校验）** | 同上 | **抄它的字符相似度三档（大小写 0.8 / 中文数字 0.7），但必须补上时长** |
| **歌词/封面/曲库的缓存失效策略** | 业务逻辑 | —— |
| **拼音搜索** | 中文场景必需 | 有现成库（`pinyin` crate / `pypinyin` / 各语言的拼音表），别自己写表 |
| **可换肤的布局系统**（如果你要做） | 现代框架没有「让用户改布局」的现成机制 | 抄「声明式元素 + 工厂 + 多尺寸布局」，但用 JSON |

### 用现成库

| 项 | 选型 |
|---|---|
| **解码 + 解复用** | FFmpeg（`ffmpeg-next` / `rsmpeg` / 直接 C API）。**注意 GPL 传染**：纯 LGPL 配置可以闭源，开 `--enable-gpl` 就必须 GPL |
| **音频输出** | `cpal`（Rust）/ WASAPI 直连 / **miniaudio**（C，单头文件，同时含解码器） |
| **重采样** | `rubato`（Rust）/ libsoxr / swresample |
| **变速不变调** | `rubberband` / SoundTouch / Signalsmith Stretch（**这是最难的一块，务必用库**） |
| **FFT** | `rustfft` / kissfft / pffft（**别自己写**） |
| **标签读写** | Rust：`lofty`（最全）/ `audiotags`；C++：**TagLib**（它的选择，成熟） |
| **数据库** | **SQLite**（`rusqlite` / `sqlx` / 直接 C）。**别用自研二进制格式** |
| **配置** | `serde` + TOML/JSON |
| **HTTP** | `reqwest` / libcurl。**必须有超时 + 重试 + 取消** |
| **JSON** | `serde_json`（它用的是 nlohmann/json，C++ 下也好用） |
| **UI** | WPF / Avalonia（C#）/ egui / iced / Tauri（Rust）。**列表虚拟化必须用框架自带的** |
| **崩溃上报** | `sentry` / `crashpad` / Windows 的 `MiniDumpWriteDump`（它自己实现的 `crashtool.cpp`） |
| **SMTC 集成** | Windows 的 `ISystemMediaTransportControls`（C# 有 `SystemMediaTransportControls`；Rust 用 `windows` crate） |

### 可以砍掉

Cortana 搜索框歌词、last.fm、格式转换、MIDI/sf2、osu!、AB 重复、歌曲分级、播放列表「修复错误的文件路径」、多语言（初期）、12 套内置界面、任务栏缩略图按钮（SMTC 已经给了系统级的控制）。

## 9.4 ⭐ 从零开始的实现顺序（我的建议）

**原则：每一步都交付一个「能跑、能用、能给别人看」的东西。** 不要先搭框架。

### 阶段 0：地基（1~2 天，但千万别跳过）

1. 选语言 + UI 框架 + **确定 FFmpeg 的编译选项（决定 GPL 与否）**
2. 建好「崩溃日志 + 结构化日志」，`RUST_BACKTRACE` / dump 能落地
3. 定好配置格式和存放位置（含便携模式）

> 它 V2.65 才加崩溃 dump，V2.63/V2.66/V2.70 反复修 GDI 句柄泄漏 —— **这些「晚做的代价」你要提前付。**

### 阶段 1：出声（第 1 周）

4. 用 FFmpeg 打开一个 mp3 → 解码 → WASAPI/cpal 输出。**先命令行版，别碰 UI。**
5. 加播放/暂停/停止/跳转/音量
6. 加「播完自动下一曲」（**一开始就做「信号 + 超时兜底」双判定**，别像它一样三年后才补）
7. 加「每帧只取一次位置并缓存」

**交付物**：命令行能播一个文件夹。

### 阶段 2：能看的界面（第 2~3 周）

8. 主窗口：播放控制 + 进度条 + 音量 + 当前曲目信息
9. 播放列表（用框架的虚拟化列表）—— **先只做「文件夹模式」**
10. 深色/浅色 + 主题色
11. 拖放文件、文件关联、命令行参数

**交付物**：一个能用的极简播放器。**这时候就该给身边的人试用。**

### 阶段 3：曲库（第 4~5 周）

12. **SQLite 表设计**（曲目 / 艺术家 / 专辑 / 播放列表 / 统计，分 cache 和 user）
13. TagLib/lofty 读标签 + 封面 —— **立刻用 GBK 标签的 mp3 + 各种格式的怪文件测一遍**（§5.4 的坑）
14. 文件夹扫描 + 增量更新（按 `mtime`）+ 进度反馈
15. 三种视图：所有曲目 / 艺术家 / 专辑
16. 搜索（标题/艺术家/专辑 + 拼音首字母）+ 防抖

**交付物**：能管理几千首歌的曲库。**测一下 3 万首时的启动和滚动。**

### 阶段 4：歌词（第 6~7 周）

17. LRC 解析 + 时间轴规范化（**照抄 §3.2 的双份存储 + Normalize 思路**）
18. 多行滚动显示（照抄 §3.5 的 `center_pos - y_progress` 算法）
19. 卡拉OK 逐字高亮（扩展 LRC + 照抄 §3.3 的两遍绘制 + §3.4 的像素加权）
20. 翻译配对 + 提前/延后 0.5s
21. 在线歌词下载（**至少两个源 + 时长校验匹配 + 显式超时 + 可取消**）

**交付物**：歌词体验对标它 —— 这是用户最在意的功能，做到这一步你的播放器就「能用」了。

### 阶段 5：系统集成（第 8 周）

22. **SMTC**（多媒体键 / 锁屏 / 蓝牙耳机 —— 现代用户的硬需求）
23. 通知区图标 + 最小化到托盘（**issue 里被要了 4 次，别漏**）
24. 全局快捷键
25. 任务栏缩略图按钮 / 进度（可选）
26. 播放位置记忆 + 上次列表恢复

**交付物**：可以设为默认播放器，长期使用。

### 阶段 6：在线音源（第 9~12 周）

27. **音源层抽象先落地**（`MusicSource` trait + `Track { source_id, source_track_id, ... }`）
28. 第一个在线源的搜索 + 流解析 + 播放（**HTTP 带缓存/重试/超时**）
29. 在线歌词/封面接进已有的歌词/封面管线（复用匹配算法）
30. **播放队列**（独立于播放列表）
31. 缓存策略（搜索结果 / 流 url 过期 / 歌词 / 封面 → SQLite + 磁盘）

**交付物**：本地 + 在线统一体验。**这是你的核心差异，也是架构最容易翻车的地方 —— 所以音源抽象要在这个阶段之前就想好（阶段 3 设计数据库时就要给 `source_id` 留列）。**

### 阶段 7：打磨（第 13 周起，持续）

32. 频谱分析（照抄 §7.3 的三招）
33. 均衡器（自研 biquad，**别抄 `fBandwidth=30`**）
34. 歌词编辑 / 桌面歌词
35. 标签写入（**先做原子写 + 备份**）
36. 迷你模式 / 可换肤
37. 音量均衡 ReplayGain（**它的用户要了 2 次没给，这是你的机会**）

### 三个「顺序上的忠告」

1. **音频链路的「结束判定」和「位置缓存」一开始就要做对。** 它在这上面栽了三年（FFmpeg 内核 #820/#822 到 V2.78 才修）。
2. **数据库表设计在加在线音源之前就要留好 `source_id`。** 它整个项目都假设「曲目 = 本地文件路径」，加在线源时无处下手。
3. **UI 框架的选择决定你的天花板。** 它的 GDI 路线导致「帧率低、滚动不跟手」成为 19 条评论的头号 issue，作者自己说「打算以后改成 Direct2D」但一直没做。**别重蹈。**

---

## 附录 A：可直接复用的关键代码位置索引

（路径相对 `MusicPlayer2/`，行号对应 master `328af4cc`）

| 想抄的东西 | 去看 |
|---|---|
| 播放后端抽象接口 | `IPlayerCore.h` |
| DLL 动态加载 + 降级（30 行） | `DllLib.h` / `DllLib.cpp` |
| 插件扫描 + 格式自报 | `BassCore.cpp:104` `InitCore()` |
| BASS 流创建全流程 | `BassCore.cpp:297` `Open()` |
| 播放结束轮询判定 | `BassCore.cpp:478` `SongIsOver()` |
| 音量淡入淡出 | `BassCore.cpp:359` `Play()` |
| 变速变调 | `BassCore.cpp:341/444/464` |
| 10 段 EQ + 混响参数 | `BassCore.cpp:244/872/888`、`BassCore.h:112` |
| 歌词数据结构 | `Lyric.h:CLyrics::Lyric` |
| 时间标签手写扫描器 | `Lyric.cpp:73` `ParseLyricTimeTag()` |
| 歌词时间轴规范化 | `Lyric.cpp` `NormalizeLyric()` |
| ⭐ 逐字进度（像素加权） | `Lyric.cpp:670` `GetLyricProgress()` |
| ⭐ 逐字高亮绘制（两遍+裁剪） | `DrawCommon.cpp:110` `DrawWindowText()` |
| ⭐ 多行歌词平滑滚动 | `CUIDrawer.cpp:57` `DrawLyricTextMultiLine()` |
| 桌面歌词高亮（GDI+ Region） | `LyricsWindow.cpp:242` `DrawLyricText()` |
| 歌词下载源抽象基类 | `LyricDownloadCommon.h` |
| ⭐ 歌名相似度（含中文数字） | `InternetCommon.cpp` `StringSimilarDegree_LD()` / `CharacterSimilarDegree()` |
| ⭐ 匹配打分（**注意缺时长**） | `LyricDownloadCommon.cpp` `SelectMatchedItem()` |
| 曲库容器与锁 | `SongDataManager.h` |
| 曲库序列化（**反面教材**） | `SongDataManager.cpp` `SaveSongData()` |
| ⭐ 增量扫描（靠 mtime） | `AudioCommon.cpp:526` `GetAudioInfo()` |
| 媒体库扫描线程 | `MusicPlayer2.cpp:637` `StartUpdateMediaLib()` |
| ⭐ ListItem 三模式统一 | `Player.cpp:147` `IniPlayList()` |
| ⭐ 列表抽象基类 | `UIElement/AbstractListElement.h` |
| 列表绘制（**反面教材**） | `UIElement/AbstractListElement.cpp:12/592` |
| ⭐ XML → 元素工厂 | `UIElement/ElementFactory.cpp` |
| 手写 flex 布局 | `UIElement/Layout.cpp` |
| ⭐ UI 绘制线程 + fps 自适应 | `MusicPlayerDlg.cpp:4538` `UiThreadFunc()` |
| 双缓冲 RAII | `DrawCommon.h` `CDrawDoubleBuffer` |
| ⭐ 频谱分段映射表 | `SpectralDataHelper.cpp:4` 构造函数 |
| 频谱定标/sqrt/绝对值 | `SpectralDataHelper.cpp:43`、`Player.cpp:688` |
| 峰值帽衰减（**fps 耦合，要改**） | `Player.cpp:711` `CalculateSpectralDataPeak()` |
| 频谱绘制 | `CUIDrawer.cpp:327/390` `DrawSpectrum()` |
| ⭐ GBK 标签容错 | `TagLibHelper.cpp:52` `TagStringToWstring()` |
| ⭐ `ReOpen` RAII（改正在播放的文件） | `Player.h:563` |
| 封面缩放防卡顿 | `Player.cpp:2513` `AlbumCoverResize()` |
| 封面「读→临时文件→Load」三段式 | `AudioTag.cpp:199` |
| 封面查找优先级 | `Player.cpp:2438` `SearchAlbumCover()` |
| 三级刷新模式 | `AudioCommon.h:56` `MediaLibRefreshMode` |
| 常用常量（帧率/间隔/FFT/EQ） | `Define.h:87-203` |

## 附录 B：本报告未能确证的部分

1. **MusicPlayer2 的 `Documents/Introduction.md` 已是空壳**，功能清单是从 Wiki + README + 更新日志重建的。可能有个别功能遗漏。
2. **BASS 的授权条款在项目仓库里没有任何文本**。「非商业免费 / 商业需付费」来自 un4seen 官网，**不能引用本项目当依据**。
3. **FFmpeg 内核 DLL（`ffmpeg_core.dll`）的内部实现不在本仓库**，其重采样细节、缓存策略均无法源码确证，报告中相关内容已标【推测】。
4. **issue #905「16K频点错误」的原文我没有逐字核对**，但 `FREQ_TABLE` 末项 `1600` 与 FFmpeg 侧 `GetEqChannelFreq()` 的 `16000` 冲突是源码确证的。
5. **「几万首」的实际性能数字我没有实测**（没有构建环境），所有性能结论都基于源码复杂度分析（O(N) 每帧循环、O(N²) 树控件、O(N×M) 搜索态）。
6. 上游仓库仍在活跃（最后 push 2026-09-10），本报告基于 `328af4cc` 这一版；后续版本可能有变化。
7. **涉及「实际有多卡」「并发缺陷实际触发的概率」的判断是静态阅读的推论，没有运行时验证**（本机没有 MSVC + MFC 构建环境，跑不起来）。所有复杂度结论（每帧 O(N)、树控件 O(N²)、搜索 O(N×M)、分类 O(N log N) × 调用次数）都是从源码结构推导的，**结构可信，体感需要你用大曲库实测后才好定稿**。
8. 曲库数据文件的实际路径有两条：`%APPDATA%\MusicPlayer2\song_data.dat`（默认 `portable_mode = false`）或程序 exe 同目录（便携模式）。报告里为简洁只写了文件名。

---

*报告完。三条专题调查报告（含逐函数代码片段）在 `_mp2/reports/` 下。*
