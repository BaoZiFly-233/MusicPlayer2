# MusicPlayer2 音频播放内核 / 解码链路 / 音效 / 频谱 —— 源码级调查报告

- 调查对象：`zhongyang219/MusicPlayer2`，默认分支 `master`，源码根目录 `MusicPlayer2/`
- 调查时版本：`Define.h` → `APP_VERSION L"2.78"`，版权年 2025
- 取码方式：`gh api ... -H "Accept: application/vnd.github.raw"` 逐文件取到本地，未 clone
- 复核方式：报告写成后，对全部关键结论在本地完整副本 `D:\BoTapMusic\MusicPlayer2\` 上**逐行二次核对**，行号与内容完全一致。核过的点：`IniPlayerCore` 优先级（`Player.cpp:58-88`）、`BASS_Init`（`BassCore.cpp:69`）、`FREQ_TABLE`（`BassCore.h:112`）、`EQU_STYLE_TABLE`（`Define.h:117-128`）、`FFT_SAMPLE`/`SPECTRUM_COL`（`Define.h:109-110`）、插件解析循环（`BassCore.cpp:122-135`）、`CPlayer::SetPitch` 开区间（`Player.cpp:1423`）、`AlbumCoverResize`（`Player.cpp:2513-2532`）、`CalculateSpectralData/Peak`（`Player.cpp:688-736`）、`CFfmpegCore::SongIsOver`（`FfmpegCore.cpp:229-266`）、`DrawSpectrum` 宽度与高度换算（`CUIDrawer.cpp:376-449`）、`sizeof(fft_data)` 笔误（`FfmpegCore.cpp:374/377`、`MciCore.cpp:286`）、`Plugins/` 与 `Encoder/` 实际文件清单
- 标注约定：**【源码确证】**＝我实际读到了这段代码，附 `文件:函数`；**【推测】**＝由调用点/产物/文档反推，未读到直接实现

---

## 0. 一句话总览

MusicPlayer2 有一层干净的 `IPlayerCore` 抽象，底下挂了三个内核：**BASS（主力，静态链 bass.dll）**、**MCI（winmm，兜底/系统自带）**、**FFMPEG（第三方外挂 DLL，半成品）**。音效（10 段 DX8 参量均衡 + DX8 混响）走 BASS 原生 FX；变速变调依赖 `bass_fx.dll` 的 `BASS_FX_TempoCreate`；频谱是 **BASS 自己做 FFT**（`BASS_DATA_FFT1024`），项目只做「bin → 柱子」的映射和峰值衰减；UI 绘制被整个搬到了一条后台线程里。

---

## 1. 内核抽象 `IPlayerCore` —— 值得抄，但别照抄全部

### 1.1 接口清单

**【源码确证】** `MusicPlayer2/IPlayerCore.h:79-182` `class IPlayerCore`

纯虚接口，共 6 组：

| 组 | 方法 |
|---|---|
| 生命周期 | `InitCore()` / `UnInitCore()` |
| 打开播放 | `Open(const wchar_t*)` / `Close()` / `Play()` / `Pause()` / `Stop()` |
| 属性 | `GetAudioType()` / `GetChannels()` / `GetFReq()` / `GetBitrate()` / `GetSoundFontName()` |
| 进度 | `GetCurPosition()` / `GetSongLength()` / `SetCurPosition(int ms)` —— **全部以毫秒为单位，int** |
| 速度音调 | `SetVolume(int)` / `SetSpeed(float)` / `SetPitch(int 半音)` / `IsSpeedAvailable()` / `IsPitchAvailable()` |
| 状态 | `SongIsOver()` / `GetPlayingState()` / `GetErrorCode()` / `GetErrorInfo()` / `GetCoreType()` / `IsVolumeFadingOut()` |

另有三个「内核无关」的旁路能力，这个设计很实用：

- `GetAudioInfo(file_path, AudioInfo*, AudioTag*)` —— **注释明确要求「需要支持并发且不影响当前播放」**（`IPlayerCore.h:130-135`），所以每个内核都是**临时开一个独立流读元信息再释放**，不碰正在播的流。BASS 侧见 `CBassCore::GetAudioInfo`（`BassCore.cpp:526`，`BASS_StreamCreateFile` → 读属性 → `BASS_StreamFree`）。
- 编码能力：`EncodeAudio(...)` / `InitEncoder()` / `UnInitEncoder()` / `IsFreqConvertAvailable()` —— **把「格式转换」也算作内核能力**，这是这个抽象里最有争议也最实用的一笔。
- MIDI 专属：`IsMidi()` / `IsMidiConnotPlay()` / `GetMidiInfo()` / `GetMidiInnerLyric()` / `MidiNoLyric()` —— MIDI 概念直接漏到接口层了。
- 音效：`ApplyEqualizer(int channel, int gain)` / `SetReverb(int mix, int time)` / `ClearReverb()` / `GetFFTData(float fft_data[FFT_SAMPLE])` —— **注意这里泄露了实现常量 `FFT_SAMPLE`（=512，来自 `Define.h:109`）和「均衡器通道号 0~9」的硬编码假设**（注释原话：`channel为均衡器通道，取值为0~9，gain为增益，取值为-15~15`）。

配套类型：`enum PlayerCoreType { PT_BASS, PT_MCI, PT_FFMPEG }`（`IPlayerCore.h:12-17`）、`enum PlayingState { PS_STOPED, PS_PAUSED, PS_PLAYING }`（`:19-24`）、`struct MidiInfo`（`:3-10`）。

### 1.2 选择与切换

**【源码确证】** `Player.cpp:58-89` `CPlayer::IniPlayerCore()`

```cpp
if (theApp.m_play_setting_data.use_mci)
    m_pCore = new CMciCore();
else if (theApp.m_play_setting_data.use_ffmpeg)
    m_pCore = new CFfmpegCore();

// 判断MCI或FFMPEG内核是否加载成功
CDllLib* dll_lib = dynamic_cast<CDllLib*>(m_pCore);
if (dll_lib != nullptr && !dll_lib->IsSucceed()) { dll_lib->UnInit(); delete m_pCore; m_pCore = nullptr; }

if (m_pCore == nullptr) {
    m_pCore = new CBassCore();
    theApp.m_play_setting_data.use_mci = false;
    theApp.m_play_setting_data.use_ffmpeg = false;   // 回退写回配置
}
m_pCore->InitCore();
```

要点：
- **优先级 MCI > FFMPEG > BASS**（`else if` 链），BASS 是兜底，且**加载失败会静默回退并把配置改回 BASS**。
- **内核切换是「重建对象」，不是运行时热切**。配置项在 `CommonData.h:330` `bool use_mci{false}`（注释「是否使用MCI内核」）与 `:332` `bool use_ffmpeg{false}`。
- 但存在一条**运行时重建**路径：**【源码确证】** `Player.cpp` `CPlayer::ReIniPlayerCore(bool replay)` —— `MusicControl(CLOSE)` → `UnInitPlayerCore()` → `IniPlayerCore()` → `MusicControl(OPEN)` → `SeekTo(旧进度)` → 可选 `PLAY`，全程持有 `m_play_status_sync`（`try_lock_for(5000ms)`，注释：「系统从挂起中恢复可能很卡」）。触发者是设备变化（见 §9）和内核异常（`WM_RE_INIT_BASS_CONTINUE_PLAY`）。
- 还有一层**「换内核要不要重启程序」的问询**：**【源码确证】** `MusicPlayerDlg.cpp:1359-1360` 里 `ffmpeg_core_enable_WASAPI` / `..._exclusive_mode` 变化会置 `need_restart_player`。

### 1.3 是否值得抄

值得抄的部分：
1. **接口按「语义单位」而不是「后端单位」定义**：全用毫秒、全用 int、音调用半音。换后端时不用改 UI。
2. **`GetAudioInfo` 强制并发安全**这个约定写在注释里，非常值钱——媒体库扫描和正在播放会同时发生。
3. **用 `dynamic_cast<CDllLib*>` 做「动态加载型内核的健康检查」**，失败的自动降级，用户不会卡在一个打不开的内核上。

不值得照抄的部分：
1. `GetFFTData(float[FFT_SAMPLE])` 把 512 这个实现常量写进接口，FFmpeg 内核被迫也要凑出 512 个数（`FfmpegCore.cpp:372-379`）。
2. MIDI 的 5 个方法漏到接口上，MCI/FFmpeg 只能返回空/`false`。
3. `EncodeAudio` 的 `void* encode_para` + 注释里手写「这个格式对应这个 struct」——典型的 C 风格类型擦除，容易错（`IPlayerCore.h:143-158`）。
4. MCI 内核**不支持跨线程调用**（见 §3.6），这个限制被 `dynamic_cast`/`IsMciCore()` 特判散落在上层多处，抽象其实没兜住。

---

## 2. BASS 组件分工表

### 2.1 静态链接 vs 动态加载 —— 先分清

| DLL | 装载方式 | 依据 |
|---|---|---|
| `bass.dll` | **静态导入库**，进程启动即加载 | `MusicPlayer2.vcxproj:110/176` `AdditionalDependencies>bass.lib;Powrprof.lib`，x64 配置为 `bass_x64.lib`（`:141/212`）。`Define.h:29-34` 里那两行 `#pragma comment(lib,"bass.lib")` **是被注释掉的**。【源码确证】 |
| `bass_fx.dll` | 动态 `LoadLibrary` | `BassCore.cpp:100` `m_bass_fx_lib.Init(theApp.m_module_dir + L"bass_fx.dll")` |
| `bassenc.dll` / `bassmix.dll` | 动态，**延迟到「格式转换」首次使用时** | `BassCore.cpp:806-822` `CBassCore::InitEncoder()`；`FormatConvertDlg.cpp:431` 才调 |
| `basswma.dll`（编码侧） | 动态，同上 | `BassCore.cpp:816` |
| `bassmidi.dll` / `bassflac.dll` / `bass_aac.dll` / `bass_ape.dll` / `basscd.dll` / `basswma.dll`（解码侧） | 动态，**遍历 `Plugins\*.dll`** | `BassCore.cpp:104-111` |
| `ffmpeg_core.dll` | 动态 | `FfmpegCore.cpp:16` |
| `winmm.dll`（MCI） | 动态 | `MciCore.cpp:10` `CDllLib::Init(L"winmm.dll")` |

### 2.2 为什么要动态加载

**【源码确证】** `MusicPlayer2/DllLib.h` + `DllLib.cpp`（只有 30 行，是整个方案的核心）：

```cpp
class CDllLib {
public:
    void Init(const wstring& dll_path);   // LoadLibrary + GetFunction()
    void UnInit();
    bool IsSucceed();                     // m_dll_module != NULL && rtn
protected:
    virtual bool GetFunction() = 0;       // 子类各自 GetProcAddress
protected:
    HMODULE m_dll_module;  bool m_success{false};
};
```

```cpp
void CDllLib::Init(const wstring & dll_path) {
    m_dll_module = ::LoadLibrary(dll_path.c_str());
    bool rtn = false;
    if(m_dll_module != NULL) rtn = GetFunction();
    m_success = (m_dll_module != NULL && rtn);
}
```

动机（从代码结构可以直接读出来）：
1. **可选依赖不拖垮主程序**。`bass_fx.dll` 不在就无法变速变调，但程序照常跑（`CBassCore::IsPitchAvailable()` 直接 `return m_bass_fx_lib.IsSucceed()`，`BassCore.cpp:473`；`SetSpeed` 里也有降级分支，见 §3.4）。
2. **不静态依赖 GPL 不兼容或体积大的第三方**：`bassenc`/`bassmix`/编码器 exe 只在格式转换时才需要（`Encoder\` 目录：`bassenc.dll`、`bassmix.dll`、`lame.exe`、`oggenc.exe`、`flac.exe`）。
3. **插件可被用户自行增删**：`BASS_PluginLoad` 遍历目录，加载失败只是少一种格式，不崩。
4. 每个包装类都**独立声明自己用到的常量和结构体**（如 `BASSMidiLibrary.h` 里重新定义了 `BASS_MIDI_FONT`、`BASS_MIDI_MARK`、`BASS_POS_MIDI_TICK`、`BASS_ATTRIB_MIDI_PPQN`），因为 `bass.h` 里没有这些扩展定义。**这是「只取所需」的写法，好处是不用带全套头文件，坏处是常量值需要人工同步。**

### 2.3 逐个组件的具体用途

| 组件 | 在本项目里的具体职责 | 用到的 API | 依据 |
|---|---|---|---|
| **bass.dll** | 解码 + 输出 + FFT + DX8 音效宿主 | `BASS_Init`、`BASS_StreamCreateFile`、`BASS_StreamCreateURL`、`BASS_ChannelGetInfo/GetAttribute/GetLength/GetPosition/IsActive`、`BASS_ChannelPlay/Pause/Stop/SetPosition/SlideAttribute`、`BASS_ChannelSetFX/RemoveFX`、`BASS_ChannelSetSync`、`BASS_ChannelGetData`、`BASS_ChannelBytes2Seconds`/`Seconds2Bytes`、`BASS_PluginLoad/PluginGetInfo/PluginFree`、`BASS_ErrorGetCode`、`BASS_Stop/Free`、`BASS_GetDeviceInfo`、`BASS_GetVersion` | `BassCore.cpp` 全文；`MusicPlayer2.cpp:233` |
| **bass_fx.dll** | **只做一件事：变速不变调 + 变调** | `BASS_FX_TempoCreate(chan, BASS_FX_FREESOURCE)`、然后 `BASS_ATTRIB_TEMPO` / `BASS_ATTRIB_TEMPO_PITCH` | `BassFxLibrary.cpp:15`；`BassCore.cpp:341,444,464` |
| **bassmix.dll** | **两个用途**：① 格式转换时做**重采样**（用 mixer 的 `freq` 参数）；② 纯属配套 | `BASS_Mixer_StreamCreate(dest_freq, chans, BASS_MIXER_END \| BASS_STREAM_DECODE)`、`BASS_Mixer_StreamAddChannel` | `BassMixLibrary.cpp:17-18`；`BassCore.cpp:604-607`；`IsFreqConvertAvailable()` → `BassCore.cpp:831` |
| **bassenc.dll** | 格式转换的输出端，**把 PCM 喂给命令行编码器** | `BASS_Encode_Start`（unicode 包装成 `BASS_Encode_StartW(cmdline, BASS_UNICODE)`）、`BASS_Encode_Stop`、`BASS_Encode_IsActive`；flags `BASS_ENCODE_AUTOFREE`，WAV 时加 `BASS_ENCODE_PCM` | `BASSEncodeLibrary.h/.cpp`；`BassCore.cpp:681` |
| **bassmidi.dll** | MIDI 播放 + SF2 音色库 + **MIDI 内嵌歌词** | `BASS_MIDI_FontInit(path, BASS_UNICODE)`、`BASS_MIDI_StreamSetFonts`、`BASS_MIDI_FontGetInfo`、`BASS_MIDI_FontFree`、`BASS_MIDI_StreamGetEvent(…, MIDI_EVENT_TEMPO)`、`BASS_MIDI_StreamGetMark` | `BASSMidiLibrary.cpp:19-24`；`BassCore.cpp:156,167,319,327,332-336` |
| **basswma.dll** | **编码 WMA**（走 Windows Media Format，不是解码 WMA——解码 WMA 是由 Plugins 目录下的同名 dll 当插件加载的） | `BASS_WMA_EncodeOpenFile`（unicode 包装 `...W`）、`BASS_WMA_EncodeWrite`、`BASS_WMA_EncodeClose`、`BASS_WMA_EncodeSetTag` | `BASSWmaLibrary.cpp:17-20`；`BassCore.cpp:709` |
| **bassflac / bass_aac / bass_ape / basscd** | 纯解码插件，**代码里没有任何专属调用**，完全靠 `BASS_PluginLoad` 自动接管扩展名 | 无 | `BassCore.cpp:104-136` |
| **bassopus / basswv / bass_ac3 / bass_mpc / bass_spx / bass_tta / bassdsd / bassalac** | **仓库里没有这些 DLL**，但 Wiki《支持的音频格式》把它们列在表里，并说明「更多插件可以到 http://www.un4seen.com/ 下载，放到 `./Plugins` 目录即可」 | 无 | 见 §7 |

### 2.4 插件清单是怎么被发现的（这段设计挺聪明）

**【源码确证】** `BassCore.cpp:104-136` `CBassCore::InitCore()`

```cpp
plugin_dir = theApp.m_local_dir + L"Plugins\\";
CCommon::GetFiles(plugin_dir + L"*.dll", plugin_files);
for (const auto& plugin_file : plugin_files) {
    HPLUGIN handle = BASS_PluginLoad((plugin_dir + plugin_file).c_str(), 0);
    m_plugin_handles.push_back(handle);
    const BASS_PLUGININFO* plugin_info = BASS_PluginGetInfo(handle);
    if (plugin_info == nullptr) continue;
    format.file_name      = plugin_file;
    format.description    = CCommon::ASCIIToUnicode(plugin_info->formats->name);   // 插件自报家门
    format.extensions_list= CCommon::ASCIIToUnicode(plugin_info->formats->exts);
    // 解析 "*.flac;*.fla;" 这类字符串 -> vector，同时并进 m_all_surpported_extensions
    CAudioCommon::m_surpported_format.push_back(format);
    if (format.description == L"MIDI") { /* 单独初始化 bassmidi + 加载 sf2，见 §7 */ }
}
```

**所以「支持格式列表」是运行时由插件自报的**，UI 里的「帮助 → 支持的格式」直接渲染 `CAudioCommon::m_surpported_format`。我在报告里给的插件清单，来源是仓库 tree（`git/trees/master?recursive=1`，实到 6 个 dll）+ 上述代码，**这不是从 `LoadLibrary` 调用点推出来的，是仓库里实际存在的文件**。

解析循环里有一处 `wstring::substr` 用 `npos` 参与算术（`BassCore.cpp:126`，首次 `index` 为 `npos` 时 `index - last_index - 2` 会回绕成巨大值），靠 `substr` 内部截断才没崩。**「能用但属侥幸」的写法，抄的时候建议重写。**

---

## 3. 完整播放链路

### 3.1 从「点一首歌」到「出声」

**【源码确证】** 主线（BASS 内核）：

```
① UI 触发
   CMusicPlayerDlg / UiElement::* → CPlayer::MusicControl(Command::OPEN)     Player.cpp:498
② 打开并建流
   CPlayer::MusicControl(OPEN)                                              Player.cpp:507
     → CBassCore::Open(file_path)                                           BassCore.cpp:297
         ├─ 若已有流 → Close()（保证同时只有一个流）
         ├─ flags = BASS_SAMPLE_FLOAT; 若 bass_fx 可用 → |= BASS_STREAM_DECODE  BassCore.cpp:306-308
         ├─ BASS_StreamCreateFile(FALSE, path, 0, 0, flags)  或
         │  BASS_StreamCreateURL(path, 0, flags, NULL, NULL)   （URL 走 CCommon::IsURL）  :309-312
         ├─ BASS_ChannelGetInfo(&m_channel_info)                              :313
         ├─ BASS_ChannelGetAttribute(BASS_ATTRIB_BITRATE)                     :315
         ├─ m_is_midi = (GetAudioTypeByBassChannel(ctype) == AU_MIDI)         :317
         ├─ MIDI：BASS_MIDI_StreamSetFonts + PPQN/TEMPO/MARK + MidiEndSync     :318-338
         ├─ SetFXHandle()  // 建 10 个 PARAMEQ + 1 个 REVERB                  :339
         └─ m_musicStream = BASS_FX_TempoCreate(stream, BASS_FX_FREESOURCE)   :341
            否则 BASS_ChannelGetAttribute(BASS_ATTRIB_FREQ, &m_freq)         :343
③ 打补丁式恢复状态
   SetVolume() → SetSpeed(m_speed) → SetPitch(m_pitch)
   memset(m_spectral_data, 0)                                                Player.cpp:534-538
   if (m_equ_enable) SetAllEqualizer();   // 逐通道重放增益                   Player.cpp:540-541
   if (m_reverb_enable) SetReverb(mix,time) else ClearReverb()               Player.cpp:542-545
④ 真正出声
   CPlayer::MusicControl(Command::PLAY) → CBassCore::Play()                  BassCore.cpp:359
     ├─ 淡入开启：SetCurPosition(pos - fade_time/2)、BASS_ATTRIB_VOL=0
     │             BASS_ChannelPlay(stream, FALSE)
     │             BASS_ChannelSlideAttribute(VOL, vol, fade_time)
     └─ 否则：BASS_ChannelPlay(stream, FALSE)
⑤ 解耦到系统
   m_controls.UpdateControls(PlaybackStatus::Playing)  → MediaTransControlsImpl（SMTC）
```

**关键点：流和混音器怎么组合？**
- **正常播放时不使用 `BASS_Mixer_StreamCreate`**。播放流直接是 `BASS_StreamCreateFile(...)` 的产物（被 `BASS_FX_TempoCreate` 包了一层解码流）。
- **混音器只出现在一个地方：格式转换时的重采样。** `BassCore.cpp:599-614`：

```cpp
if (dest_freq > 0 && dest_freq != channel_info.freq) {
    hStreamOld = hStream;
    hStream = m_bass_mix_lib.BASS_Mixer_StreamCreate(dest_freq, channel_info.chans,
                                                     BASS_MIXER_END | BASS_STREAM_DECODE);
    if (hStream != 0) m_bass_mix_lib.BASS_Mixer_StreamAddChannel(hStream, hStreamOld, 0);
    else { hStream = hStreamOld; hStreamOld = 0; }
}
```

也就是说：**这个项目没有做「多流混音/交叉淡入淡出/无缝接歌」**，一次只有一个 `HSTREAM`。

### 3.2 多声道 / 多设备 / 独占模式

**【源码确证】** `BassCore.cpp:69-79` `BASS_Init`

```cpp
BASS_Init(
    theApp.m_output_devices[theApp.m_play_setting_data.device_selected].index,  // 播放设备
    44100,                          // 输出采样率 44100（常用值）
    BASS_DEVICE_CPSPEAKERS,         // 用 Windows 控制面板设置检测扬声器数量
    theApp.m_pMainWnd->m_hWnd,      // 程序窗口
    NULL                            // 类标识符
);
```

- **输出采样率硬编码 44100**，没有跟随源文件、也没有让用户选。
- 设备枚举：`BassCore.cpp:34-66` 从 `device_index = 1` 开始循环 `BASS_GetDeviceInfo`（0 是「默认设备」，不枚举，手工插在列表头，名称取自语言表 `TXT_OPT_PLAY_DEVICE_NAME_BASS_DEFAULT`），按**设备名字符串**与配置 `m_play_setting_data.output_device` 匹配得到 `device_selected`。
- **BASS 内核完全没有独占模式**。【源码确证】整个 `BassCore.cpp` 里搜不到 `BASS_CONFIG_DEV_EXCLUSIVE` / `BASS_CONFIG_WASAPI_PERSIST` 之类的调用（`bass.h` 里有这些宏定义，但项目没引）。独占模式**只有 FFmpeg 内核有**，走 FFmpeg 内核自己的 WASAPI（`ffmpeg_core_settings_set_use_WASAPI` / `..._set_enable_exclusive`）。
- `BASS_DEVICE_CPSPEAKERS` 这个 flag 是给多声道扬声器布局用的；除此之外**项目没有做任何多声道特殊处理**（只把 `m_channel_info.chans` 当元信息显示）。

### 3.3 播放位置怎么取

**【源码确证】** `BassCore.cpp:487-501` `CBassCore::GetCurPosition()`

```cpp
QWORD pos_bytes = BASS_ChannelGetPosition(m_musicStream, BASS_POS_BYTE);
double pos_sec  = BASS_ChannelBytes2Seconds(m_musicStream, pos_bytes);
int current_position = static_cast<int>(pos_sec * 1000);
if (current_position == -1000) current_position = 0;
GetMidiPosition();      // MIDI 额外算节拍数
```

`GetSongLength()`（`:503-513`）与 `SetCurPosition(int ms)`（`:515-524`）用同一套 bytes↔seconds 转换；静态副本 `GetBASSCurrentPosition` / `GetBASSSongLength` / `SetCurrentPosition(HSTREAM, int)`（`:948-974`）供格式转换复用。

**注意：所有位置 API 都是 `BASS_POS_BYTE`。** 用字节位置而不是 `BASS_POS_MIDI_TICK`（MIDI 除外，见 `:240`），因为字节位置统一、可按 `Seconds2Bytes` 反算。

### 3.4 播放结束怎么知道 —— **不是靠 `BASS_SYNC_END`，是轮询**

**【源码确证】** `BassCore.cpp:478-485` `CBassCore::SongIsOver()`

```cpp
DWORD state = BASS_ChannelIsActive(m_musicStream);
bool is_over{ (m_last_playing_state == BASS_ACTIVE_PLAYING && state == BASS_ACTIVE_STOPPED)
           || m_error_code == BASS_ERROR_ENDED };
m_last_playing_state = state;
return is_over && m_playing_state == PS_PLAYING && m_musicStream != 0;
```

即：**记住上一次 `BASS_ChannelIsActive` 的返回值，发现「上一帧在播、这一帧停了」就判定结束**，由上层定时器（主窗口 1 秒定时器链）反复调用。`m_last_playing_state` 初值是 `PLAYING_STATE_DEFAULT_VALUE = 99`（`BassCore.h:11`），保证第一次调用不会误判。

`BASS_SYNC_END` **只用在 MIDI 上**（`BassCore.cpp:336` `BASS_ChannelSetSync(m_musicStream, BASS_SYNC_END, 0, MidiEndSync, 0)`，作用仅仅是清空内嵌歌词缓存）。FFT 频谱也是轮询（UI 线程每帧调 `GetFFTData`）。

上层 `CPlayer::SongIsOver()`（`Player.cpp:643-653`）叠了一层：cue 或 MCI 内核时，额外用 `m_current_position >= m_song_length` 兜底。

### 3.5 淡入淡出

**【源码确证】** `BassCore.cpp:359-425`，纯靠 `BASS_ChannelSlideAttribute` + 一个 `SetTimer` 回调（`FADE_TIMER_ID = 1010`，`BassCore.h:117`）：

| 动作 | 实现 |
|---|---|
| **Play（淡入）** | `SetCurPosition(pos - fade_time/2)` 先回退半个淡入时长 → `BASS_ATTRIB_VOL = 0` → `BASS_ChannelPlay` → `BASS_ChannelSlideAttribute(VOL, m_volume/100, fade_time)` |
| **Pause（淡出）** | `BASS_ChannelSlideAttribute(VOL, 0, fade_time)` → `m_fading = true` → `SetTimer(hwnd, FADE_TIMER_ID, fade_time, lambda)`，定时器到点才 `BASS_ChannelPause` |
| **Stop（淡出）** | 同上，定时器到点 `BASS_ChannelStop` + `BASS_ChannelSetPosition(handle, 0, BASS_POS_BYTE)` |

`m_fading` 期间 `SetVolume()` **直接忽略**（`BassCore.cpp:429` `if (!m_fading)`），避免淡入过程中被用户拖音量破坏渐变。`IsVolumeFadingOut()` 供上层查状态。配置：`CommonData.h:322-323` `fade_effect{true}` / `fade_time{500}`（毫秒）。

### 3.6 变速变调（V2.78 新增能力）

**【源码确证】** `BassCore.cpp:437-466`

```cpp
void CBassCore::SetSpeed(float speed) {
    if (m_bass_fx_lib.IsSucceed()) {
        if (std::fabs(speed) < 0.01 || std::fabs(speed - 1) < 0.01
            || speed < MIN_PLAY_SPEED || speed > MAX_PLAY_SPEED) speed = 1;
        float tempo = (speed - 1) * 100;                       // 1.5 倍 → TEMPO = 50
        BASS_ChannelSetAttribute(m_musicStream, BASS_ATTRIB_TEMPO, tempo);
    } else {                                                    // 降级：改采样率 → 音调跟着变
        float freq;
        if (...) speed = 0;
        freq = m_freq * speed;
        BASS_ChannelSetAttribute(m_musicStream, BASS_ATTRIB_FREQ, freq);
    }
}

void CBassCore::SetPitch(int pitch) {
    if (m_bass_fx_lib.IsSucceed()) {
        if (pitch < MIN_PLAY_PITCH || pitch > MAX_PLAY_PITCH) pitch = 0;
        BASS_ChannelSetAttribute(m_musicStream, BASS_ATTRIB_TEMPO_PITCH, pitch);   // 单位：半音
    }
}
```

范围常量：`IPlayerCore.h:26-29` `MAX_PLAY_SPEED 4.0f` / `MIN_PLAY_SPEED 0.1f` / `MAX_PLAY_PITCH 12` / `MIN_PLAY_PITCH -12`。

为什么能「变速不变调」：`Open()` 里用 `BASS_STREAM_DECODE` 建解码流，再 `BASS_FX_TempoCreate` 包成播放流。**所以「bass_fx 不可用」时连时长/进度语义都会变**（`BASS_ATTRIB_FREQ` 只是改播放速率，`BASS_ChannelBytes2Seconds` 的换算基准不变但实际上不会被同步补偿）。

上层还有 `CPlayer::SetSpeed(float)` 二次夹取（`Player.cpp`）：`if (speed >= MIN_PLAY_SPEED && speed <= MAX_PLAY_SPEED)`，并同步 `m_controls.UpdateSpeed()` 给 SMTC。`CPlayer::SetPitch(int)` 用的是**开区间** `if (pitch > MIN_PLAY_PITCH && pitch < MAX_PLAY_PITCH)` —— **±12 会被静默丢弃**，而内核侧判定的是闭区间 `pitch < MIN || pitch > MAX`。这是一处轻微不一致，抄的时候注意。

MCI 内核：`SetSpeed`/`SetPitch` 空实现，`IsSpeedAvailable()`/`IsPitchAvailable()` 都 `return false`（`MciCore.cpp`）。
FFmpeg 内核：`SetSpeed` 有实现（`ffmpeg_core_set_speed`），`SetPitch` 空实现、`IsPitchAvailable() → false`。

### 3.7 MCI 内核（第三个内核，功能残缺但 MIDI 有独特价值）

**【源码确证】** `MciCore.cpp` 全文，本质是 `mciSendStringW` 字符串命令包一层：

```
open "<path>"        play "<path>"       pause "<path>"      stop "<path>"
seek "<path>" to N   setaudio "<path>" volume to V(0~1000)
status "<path>" length / position / tempo / bytespersec
```

- **音量映射**：`_itow_s(volume * 10, buff, 10)`，注释「设置音量100%时为1000」。
- **MIDI 特殊处理**：MCI 返回的长度/位置是**节拍数不是毫秒**，`GetCurPosition` 里 `position * m_midi_info.speed` 换算成毫秒，`SetCurPosition` 反向除回去。
- **音效/频谱全空**：`ApplyEqualizer`、`SetReverb`、`ClearReverb` 是空函数；`GetFFTData` 只 `memset(fft_data, 0, sizeof(fft_data))` —— **注意 `sizeof(fft_data)` 在数组形参上等于指针大小（4 或 8 字节），这是全项目反复出现的同一个笔误**（`MciCore.cpp`、`FfmpegCore.cpp:374/377`）。
- **不能跨线程**：这是硬伤。UI 线程里对 MCI 的处理是「发消息回主线程」——

**【源码确证】** `MusicPlayerDlg.cpp:4555-4561`（在 UI 绘制线程里）：

```cpp
if (CPlayer::GetInstance().IsPlaying() && CPlayer::GetInstance().GetPlayStatusMutex().try_lock_for(std::chrono::milliseconds(10))) {
    if (CPlayer::GetInstance().IsMciCore())
        pThis->SendMessage(WM_GET_MUSIC_CURRENT_POSITION);   // 由于MCI无法跨线程操作，因此在这里向主线程发送消息
    else
        CPlayer::GetInstance().GetPlayerCoreCurrentPosition();
    CPlayer::GetInstance().GetPlayStatusMutex().unlock();
}
```

Wiki 对 MCI 的官方评价（`doc/wiki/播放设置.md:61`）：「系统自带的播放内核，且已经多年不再维护，并存在一些BUG……**建议不要使用此内核**。此内核的一个好处是不需要第三方音色库文件 (*.sf2) 就可以播放 MIDI 音乐」。

---

## 4. 音效

### 4.1 均衡器：10 段 `BASS_FX_DX8_PARAMEQ`

**【源码确证】** `BassCore.cpp:244-255` 建句柄：

```cpp
void CBassCore::SetFXHandle() {
    for (int i{}; i < EQU_CH_NUM; i++)
        m_equ_handle[i] = BASS_ChannelSetFX(m_musicStream, BASS_FX_DX8_PARAMEQ, 1);   // priority = 1
    m_reverb_handle = BASS_ChannelSetFX(m_musicStream, BASS_FX_DX8_REVERB, 1);
}
```

**【源码确证】** `BassCore.h:110-112` 参数：

```cpp
int m_equ_handle[EQU_CH_NUM]{};    // EQU_CH_NUM = 10（Define.h:116）
const float FREQ_TABLE[EQU_CH_NUM]{ 80, 125, 250, 500, 1000, 1500, 2000, 4000, 8000, 1600 };
//                                                                                    ^^^^
```

> ⚠️ **发现一个明确的 bug：第 10 段中心频率写成了 `1600`，按数列规律和 UI 语义应为 `16000`。** 交叉证据：`FfmpegCore.cpp:649-674` `CFfmpegCore::GetEqChannelFreq` 的 `case 9: return 16000;`；`Define.h:117-128` 里 9 个 `EQU_STYLE_TABLE` 预设中第 10 列**全为 0**，所以「套预设」时听不出差异，但**「手动拉高第 10 段」在 BASS 内核下实际动的是 1600 Hz**。**【外部信息】** 据维护侧线索，此问题对应上游 issue #905「16K频点错误」（我未独立核对 issue 原文，仅记录编号供你查证）。抄这个设计时务必修正。

**【源码确证】** `BassCore.cpp:872-884` `CBassCore::ApplyEqualizer`：

```cpp
if (channel < 0 || channel >= EQU_CH_NUM) return;
if (gain < -15) gain = -15;
if (gain > 15)  gain = 15;
BASS_DX8_PARAMEQ parameq;
parameq.fBandwidth = 30;                        // Q 值用「带宽(倍频程)」表示，固定 30？见下
parameq.fCenter    = FREQ_TABLE[channel];       // 中心频率（Hz）
parameq.fGain      = static_cast<float>(gain);  // 增益（dB），-15 ~ +15
BASS_FXSetParameters(m_equ_handle[channel], &parameq);
```

`bass.h:940-944` 的结构定义（**源码确证**）：

```c
typedef struct { float fCenter; float fBandwidth; float fGain; } BASS_DX8_PARAMEQ;
```

各字段的官方取值范围在 `bass.h` 里**没写注释**（只有 `BASS_DX8_REVERB` 有），所以我把 `fBandwidth = 30` 单独说明：【推测】DirectShow `IEqualizer` 的 `fBandwidth` 单位是**倍频程（octave），合法范围 1.0 ~ 36.0**。写 30 接近「一个频段覆盖整个音频范围」的极端值，实际听感上**会让 10 个滤波器严重互相重叠**。BASS 的 DX8 参数检查只校验 `fGain` 范围，因此这个值不会报错，但音质上是可疑的。**如果你想抄，建议从 1.0~2.0 倍频程开始试，而不是 30。**

注意 `fCenter` / `fBandwidth` / `fGain` 这三个**短名字是 BASS 官方头文件里就有的**（不是项目自己起的别名），所以代码能编过。

**UI 滑块映射（这是个「反着来」的细节）** —— **【源码确证】** `EqualizerDlg.cpp`：

```cpp
m_sliders[i].SetRange(-15, 15, TRUE);
m_sliders[i].SetPos(-CPlayer::GetInstance().GeEqualizer(i));   // 取负！滑块越往上值越小
...
int gain{ -m_sliders[i].GetPos() };                            // 再取负回来
CPlayer::GetInstance().SetEqualizer(i, gain);
```

原因在 `EqualizerDlg.cpp` 的初始化里：Win32 垂直滑块（`TBS_VERT`）**位置值往下增大**，所以「往上拖 = 增益变大」需要取负。这个技巧值得记下来。

**9 个预设 + 自定义**，数值抄在 `Define.h:117-127`：

```cpp
const int EQU_STYLE_TABLE[9][EQU_CH_NUM] {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },        //无
    { 4, 3, 3, 2, 2, 1, 0, -1, -2, -2 },     //古典
    { 3, 2, 0, -1, -2, -2, -1, 0, 2, 3 },    //流行
    { 2, 1, 0, -1, -1, 1, 3, 5, 3 },         //爵士  ← 只有 9 个数，第 10 段隐式为 0
    { -2, 0, 2, 4, -1, -1, 0, 0, 2, 3 },     //摇滚
    { 1, 0, 0, 1, 2, 1, -1, -2, -2, -2 },    //柔和
    { 4, 6, 6, -2, -1, 0, 0, 0, 0, 0 },      //重低音
    { -5, -5, -3, -2, -2, 0, 0, 0, 0, 0 },   //消除低音
    { 0, 0, 0, 0, 0, -1, -3, -5, -5, -4 }    //弱化高音
};
```

> 「爵士」那行少了一个数（C++ 会用 0 补齐，不报错）。**抄的时候补上。**

**增益值存在 Player 层而不是 BASS 层**：`CPlayer::GeEqualizer(channel)` 直接返回 `m_equalizer_gain[channel]`（`Player.cpp:2315-2323`），注释掉的旧实现才是 `BASS_FXGetParameters` 反查。这是因为**换歌时旧 FX 句柄已销毁，必须由上层重放一遍**（`Player.cpp:540-541` `if (m_equ_enable) SetAllEqualizer();`）。

### 4.2 混响：`BASS_FX_DX8_REVERB`

**【源码确证】** `BassCore.cpp:886-905`：

```cpp
void CBassCore::SetReverb(int mix, int time) {
    BASS_DX8_REVERB parareverb;
    parareverb.fInGain = 0;
    // 旧实现（线性）：fReverbMix = mix / 100 * 96 - 96;
    parareverb.fReverbMix = static_cast<float>(std::pow(static_cast<double>(mix) / 100, 0.1) * 96 - 96);
    parareverb.fReverbTime = static_cast<float>(time * 10);        // UI 单位 10ms → 毫秒
    parareverb.fHighFreqRTRatio = 0.001f;
    BASS_FXSetParameters(m_reverb_handle, &parareverb);
}

void CBassCore::ClearReverb() {
    parareverb.fInGain = 0; parareverb.fReverbMix = -96;
    parareverb.fReverbTime = 0.001f; parareverb.fHighFreqRTRatio = 0.001f;
    BASS_FXSetParameters(m_reverb_handle, &parareverb);
}
```

**参数语义（抄自 `bass.h:946-951`，官方注释）：**

```c
typedef struct {
    float fInGain;           // [-96.0, 0.0]       default 0.0 dB
    float fReverbMix;        // [-96.0, 0.0]       default 0.0 dB   （-96 = 无混响）
    float fReverbTime;       // [0.001, 3000.0]    default 1000.0 ms
    float fHighFreqRTRatio;  // [0.001, 0.999]     default 0.001
} BASS_DX8_REVERB;
```

**UI → DSP 的映射（这是全项目最值得抄的一个数学细节）：**

| UI | 范围 | → DSP |
|---|---|---|
| 混响强度 `mix` | 0~100（整数百分比） | `fReverbMix = (mix/100)^0.1 * 96 - 96` |
| 混响时间 `time` | 1~300（**单位 10ms**） | `fReverbTime = time * 10`（变成 ms，1~3000ms） |

`^0.1` 这个指数是**为了把线性滑块映射到对数感知的 dB 值上**——因为 `fReverbMix` 是 dB，人耳对 dB 是指数感知的。旧的线性写法 `mix/100*96-96` 会让滑块前 90% 几乎听不出变化。**这个 `pow(x, 0.1)` 补偿非常实用，强烈建议抄。**

UI 侧（**【源码确证】** `ReverbDlg.cpp:90-93`）：
```cpp
m_reverb_mix_slider.SetRange(0, 100);   m_reverb_mix_slider.SetPos(...GetReverbMix());
m_reverb_time_slider.SetRange(1, 300);  m_reverb_time_slider.SetPos(...GetReverbTime());
```
显示格式：`swprintf_s(buff, L"%d%%", mix)` 与 `L"%.2fs"`（`time / 100`）。

`CPlayer::EnableReverb(true)` 里会把 mix/time 夹到合法区间再下发（`Player.cpp:2350-2367`）。开/关时 `SetAllEqualizer()` / `ClearReverb()` 也是「换歌后重放」模式。

**FFmpeg / MCI 内核：`SetReverb` / `ClearReverb` 都是空函数**，Wiki 明确写了「均衡器——混响功能在 ffmpeg 内核下暂时无法使用」。

### 4.3 音量：只有软件音量

**【源码确证】** `Player.cpp:669-686` `CPlayer::SetVolume()`：

```cpp
int volume = m_volume;
volume = volume * theApp.m_nc_setting_data.volume_map / 100;   // 全局音量映射（默认 100）
m_pCore->SetVolume(volume);
SendMessage(hwnd, WM_VOLUME_CHANGED, 0, 0);
```

→ `CBassCore::SetVolume(int vol)`：`BASS_ChannelSetAttribute(m_musicStream, BASS_ATTRIB_VOL, vol / 100.0f)`。

- **不碰系统音量（不做 WASAPI 会话音量、不模拟音量键）**。`volume_map`（`CommonData.h:406`，注释「如果将此值从100改为60，则当音量设置为最大（100%）时的音量大小为原来的60%」）是一个纯粹的全局上限衰减器 —— **给「我想限制最大音量」这个需求留的口子**。
- MCI 音量是 `setaudio volume to (volume*10)`，量程 0~1000。
- FFmpeg 音量走 `ffmpeg_core_set_volume`（在没打开文件时写入 settings，`FfmpegCore.cpp:191-201`）。

### 4.4 其他音效

**【源码确证】** 全项目搜 `BASS_ChannelSetFX` 只有两处，`BASS_FX_*` 常量只用到 `BASS_FX_DX8_PARAMEQ` 和 `BASS_FX_DX8_REVERB`。

**没有**：压缩器/限幅器、环绕声、合唱、回声/延迟、3D 定位、响度归一化（ReplayGain）、淡入淡出以外的交叉淡化。**这是一个「音效很克制」的播放器** —— 对想抄的人来说是好消息，需要实现的 DSP 面很小。

---

## 5. 频谱分析

### 5.1 FFT 是 BASS 做的，项目不做 FFT

**【源码确证】** `BassCore.cpp:907-910`：

```cpp
void CBassCore::GetFFTData(float fft_data[FFT_SAMPLE]) {
    BASS_ChannelGetData(m_musicStream, fft_data, BASS_DATA_FFT1024);
}
```

- **采样点数：`BASS_DATA_FFT1024` = 1024 点 FFT**（`bass.h:614`）。
- 但目标缓冲只有 **`FFT_SAMPLE = 512` 个 float**（`Define.h:109`）。
- BASS 的契约是「N 点 FFT 输出 N/2 个 bin」（1024 → 512 个），**所以 512 正好装得下，不溢出，但这个配对关系在代码里没有任何注释说明，也没有 `BASS_DATA_FFT_NYQUIST`（不返回 Nyquist bin）**。
- 项目自己**不写 FFT**，只做「bin → 柱子」映射（下一节）。
- **没有窗函数选择**：既没加 `BASS_DATA_FFT_NOWINDOW`（默认加 Hanning 窗），也没加 `BASS_DATA_FFT_REMOVEDC`（不预先去直流）。
- **没有分声道**：没加 `BASS_DATA_FFT_INDIVIDUAL`，所以是 BASS 自动把各声道**合并**后的频谱。

**【源码确证】** `Player.cpp:688-708` `CPlayer::CalculateSpectralData()`：

```cpp
if (... && ((GetBassHandle() && m_playing != 0 && m_current_position.toInt() < m_song_length.toInt() - 500)
    || m_pCore->GetCoreType() == PT_FFMPEG) && m_pCore->GetPlayingState() != PS_STOPED)
{
    int scale = (m_pCore->GetCoreType() == PT_FFMPEG ? 100 : 60);   // 两个内核量纲不同，分别定标
    m_pCore->GetFFTData(m_fft);
    for (int i{}; i < FFT_SAMPLE; i++) m_fft[i] = std::abs(m_fft[i]);   // FFT 输出是带符号的，取绝对值
    if (theApp.m_app_setting_data.use_old_style_specturm)
        CSpectralDataHelper::SpectralDataMapOld(m_fft, m_spectral_data, scale);
    else
        m_spectrum_data_helper.SpectralDataMap(m_fft, m_spectral_data, scale);
} else {
    memset(m_spectral_data, 0, sizeof(m_spectral_data));
}
```

两个值得抄的细节：
1. **`std::abs(fft[i])`** —— BASS 的 FFT 输出是**实部/虚部交替或带符号的幅度**，必须取绝对值，否则负值会让柱子高度算成负数。`CUIDrawer` 里还有一层防御（`if (spetral_height < 0 || IsError()) spetral_height = 0`）。
2. **歌曲最后 500ms 不取频谱**（`m_current_position < m_song_length - 500`），**因为到末尾时 `BASS_ChannelGetData` 会失败/返回垃圾**。这是个真实踩过的坑，注释里写明了。

### 5.2 频段 → 柱子的映射（对数刻度，且低频段特殊处理）

**【源码确证】** `Define.h:109-110`：
```c
#define FFT_SAMPLE 512          // 频谱分析采样点数
#define SPECTRUM_COL (FFT_SAMPLE / 4)   // 频谱分析柱形的条数（必须为2的整数次方且小于或等于FFT_SAMPLE）
```
→ **`SPECTRUM_COL = 128` 根柱子**（数据层），UI 层可以再压缩显示成 4/8/16/32/64/128 根。

**【源码确证】** `SpectralDataHelper.cpp:4-25` —— 构造时建一张「bin → 柱子」映射表：

```cpp
const int LINEAR_SAMPLE_COUNT{ FFT_SAMPLE * 110 / 256 };   // = 220

CSpectralDataHelper::CSpectralDataHelper() {
    // 前 220 个 bin：线性的，每两个 bin 合成一根柱子
    for (int i{}; i < LINEAR_SAMPLE_COUNT; i++) {
        int m = i / 2;
        spectrum_map[i] = m;
        if (m >= 0 && m < SPECTRUM_COL) spectrum_map_count[m]++;
    }
    // 后面的 bin：对数映射（把 bin 序号的 log 按比例投到柱子上）
    for (int i{ LINEAR_SAMPLE_COUNT }; i < FFT_SAMPLE; i++) {
        int m = static_cast<int>(std::log(i) / std::log(FFT_SAMPLE) * SPECTRUM_COL);
        if (m >= 0 && m < SPECTRUM_COL) { spectrum_map[i] = m; spectrum_map_count[m]++; }
    }
}
```

**这是全篇最值得抄的频谱设计。** 它做的是**分段混合刻度**：

- **前 220 个 bin 用线性映射**（每个柱子 2 个 bin）→ 低频分辨率高。为什么？因为 `log` 映射在低频处会把大量 bin 挤到第 0 根柱子上，导致低频柱子「虚高且毫无细节」。人耳对低频最敏感，所以这里宁可牺牲高频分辨率。
- **220 号之后的 bin 用 `log(i)/log(512)` 对数映射** → 高频铺开。
- **预计算 `spectrum_map[]` 和 `spectrum_map_count[]`，运行时不调用 `log()`** —— 每帧省下 512 次对数运算，且避免了浮点不确定性。**对 UI 帧率是实打实的好处。**

**【源码确证】** `SpectralDataHelper.cpp:43-57` `SpectralDataMap`：

```cpp
memset(spectral_data, 0, sizeof(float) * SPECTRUM_COL);
for (int i{}; i < FFT_SAMPLE; i++)
    spectral_data[spectrum_map[i]] += fft_data[i];          // 求和（不是取最大值）
for (int i{}; i < SPECTRUM_COL; i++) {
    spectral_data[i] = spectral_data[i] / spectrum_map_count[i];   // 除以该柱子的 bin 数 → 平均值
    spectral_data[i] = std::sqrtf(spectral_data[i]);               // 开方，压缩不同频率的量级差
    spectral_data[i] *= scale;                                     // scale: BASS=60, FFMPEG=100
}
```

- **是平均值，不是最大值。** 官方注释解释了原因：「如果改用最大值，低频部分的频谱会显得过高」。
- **`sqrtf` 是为了压缩动态范围**，注释：「对每个频谱柱形的值取平方根，以减少不同频率频谱值的差异」。**这是替代 dB 对数刻度的廉价做法**，值得抄。
- 旧写法 `SpectralDataMapOld`（`:27-41`）是纯线性 `i / (FFT_SAMPLE / SPECTRUM_COL)`（即每 4 个 bin 一根柱子），由配置 `use_old_style_specturm` 切换（`CommonData.h:241`，默认 `false`）。**新旧两套并存，用户可回退** —— 这个做法本身也值得学。

**【源码确证】** `SpectralDataHelper.cpp:59-70` `CalculateCompressedSpectralData`：把 128 根数据柱子**再求平均**压成 UI 需要的更少柱子（`COL_MIN = SPECTRUM_COL / col_num * index`，`COL_MAX = SPECTRUM_COL / col_num * (index+1)`）。**注意：这个方法在 `master` 里没有被任何地方调用**（我全库 grep 过），是遗留 API。

### 5.3 平滑 / 衰减（峰值条）

**没有对柱子高度本身做平滑/指数衰减** —— 柱子高度 = 当前帧的瞬时值，这是刻意的（跟节拍同步，不能糊）。

**但顶端的「峰值帽」有衰减**，而且**衰减速度跟帧率绑定**。**【源码确证】** `Player.cpp:711-736` `CPlayer::CalculateSpectralDataPeak()`：

```cpp
static int fall_count[SPECTRUM_COL];
for (int i{}; i < SPECTRUM_COL; i++) {
    if (m_spectral_data[i] > m_spectral_peak[i]) {
        m_spectral_peak[i] = m_spectral_data[i];   // 更高就顶上去
        fall_count[i] = 0;
    } else if (m_spectral_data[i] < m_spectral_peak[i]) {
        float fall_distance = 0;
        if (theApp.m_fps > 0)
            fall_distance = fall_count[i] * (7.002355f / theApp.m_fps - 0.042824f);
        if (fall_distance < 0) fall_distance = 0;
        m_spectral_peak[i] -= fall_distance;       // 逐渐下降
        fall_count[i]++;
    }
}
```

**那两个魔数 `7.002355f` / `0.042824f` 是「按帧率补偿的线性下落公式」**：

```
每帧下落量 = 帧数 × (7.002355 / fps − 0.042824)
```

含义：**目标是「不管 fps 是多少，峰值下落速度恒定」**。系数的来源可以用两点拟合反推：代入 fps=60 得每帧约 `7.002355/60 − 0.042824 = 0.07388`，代入 fps=30 得 `0.19058`。【推测】原始意图是拟合「每帧下落 `a/fps + b`」使下落相对时间恒定（`7.002355/fps` 是主项，`-0.042824` 是零阶修正，避免高帧率时下落过快）。**这是从「经典 Winamp 频谱衰减」那套系数逐帧反推出来的结果，属于经验值 —— 你可以直接抄这两个数，也可以换成你自己的。**

**替代做法**（更干净）：用 `std::chrono` 按真实经过时间计算下落量，而不是依赖 fps 估算。但必须承认项目这个做法**在没有可靠 delta-time 的老框架里是有效的**。

### 5.4 多少根柱子

**【源码确证】** UI 层可选，`CUIDrawer.h` `enum SpectrumCol { SC_AUTO, SC_128, SC_64, SC_32, SC_16, SC_8, SC_4 }`；
**【源码确证】** `CUIDrawer.cpp:327-388` `CUIDrawer::DrawSpectrum`：

```cpp
case SC_AUTO:   // 根据矩形宽度自动选择
    if      (rect.Width() < DPI(20))  cols = 4;
    else if (rect.Width() < DPI(52))  cols = 8;
    else if (rect.Width() < DPI(120)) cols = 16;
    else if (rect.Width() < DPI(240)) cols = 32;
    else if (rect.Width() < DPI(480)) cols = 64;
    else                              cols = 128;
...
double gap_width_double{ max_width * 256.0 / (cols * 672.0) };   // 间隙 = 宽度的 256/(672*cols) 倍
if (theApp.m_ui_data.full_screen && !m_for_cortana_lyric)
    gap_width_double *= CONSTVAL::FULL_SCREEN_ZOOM_FACTOR;
int gap_width{ static_cast<int>(gap_width_double + 0.5) };
int width = (max_width - (cols - 1) * gap_width) / (cols - 1);   // ⚠️ 除以 (cols-1) 而不是 cols
```

> ⚠️ `width` 用 `(cols - 1)` 做除数、`gap_width` 用 `(cols - 1)` 参与扣减，数学上是把「最后一个间隙」也算进去了，导致实际总宽略大于 `max_width`。**这是能跑但对不齐的写法**，抄的时候用 `cols` 更正确。

**取数（`CUIDrawer.cpp:414-433`）—— 关键：直接跨步采样，不做插值**：

```cpp
index = low_freq_in_center
    ? (i < cols/2 ? (-i + cols/2) * 2 - 1 : (i - cols/2) * 2)   // 低频放中间：从中心向两边展开
    : i;
if (index >= cols) index = cols;
float spetral_data = CPlayer::GetInstance().GetSpectralData()[index * (SPECTRUM_COL / cols)];
float peak_data    = CPlayer::GetInstance().GetSpectralPeakData()[index * (SPECTRUM_COL / cols)];
```

`index * (SPECTRUM_COL / cols)` 是**等间隔抽样**：128 根数据柱子 → 显示 4 根时步长 32，→ 显示 64 根时步长 2，→ 显示 128 根时步长 1。**不是求和也不是平均，就是直接挑一根。** （因为 128 根数据柱子已经是「按感知均匀分布」的了，再平均反而糊。）

**高度换算（`CUIDrawer.cpp:436-449`）**：

```cpp
if (sprctrum_height <= 0) sprctrum_height = theApp.m_app_setting_data.sprctrum_height;   // 默认 100（%）
int spetral_height = static_cast<int>(spetral_data * rects[0].Height() / 30 * sprctrum_height / 100);
int peak_height    = static_cast<int>(peak_data    * rects[0].Height() / 30 * sprctrum_height / 100);
...
int peak_rect_height = max(theApp.DPIRound(1.1), gap_width / 2);
spetral_height += peak_rect_height;   // 至少和顶端矩形一样高
peak_height    += peak_rect_height;
rect_tmp.top = rect_tmp.bottom - spetral_height;
if (rect_tmp.top < rects[0].top) rect_tmp.top = rects[0].top;   // 限幅
FillRect(rect_tmp, color, true);
```

**`/30` 这个除数就是「满量程」**：`spectral_data` 经过 `sqrt × 60` 之后典型峰值约 30，所以 `/30` 就是把它归一化到 0~1。**这个 30 和 §5.2 里的 `scale = 60` 是配套的两个魔数** —— 改 `scale` 就必须改 `/30`。

**倒影（`CUIDrawer.cpp:393-394, 450-457`）**：倒影占整体高度的下 1/3（`rc_spectrum_top.bottom = rect.top + rect.Height() * 2/3`），用 `FillAlphaRect(rc_invert, color, 96, true)` 画，**alpha = 96（0~255）**。

**迷你频谱（播放列表里正在播放曲目前面那个小图标）** —— **【源码确证】** `CPlayerUIBase.cpp:2478-2520` `CPlayerUIBase::DrawMiniSpectrum`：
- `COLS = 4`，`spectrum_unit_width = DPI(4)`，`col_width = DPI(2)`，`gap_width = 2`；
- **只取中间一段频谱**：`COL_MIN = SPECTRUM_COL/4`（=32），`COL_MAX = SPECTRUM_COL*3/4`（=96），把 64 根柱子分 4 组求和 → 避开两端的极端低频/高频。

**节拍指示器（`<<<<` 那个）** —— **【源码确证】** `CPlayerUIBase.cpp:1082-1102` `CPlayerUIBase::DrawBeatIndicator`：
```cpp
if (IsMidi()) progress = (GetMidiInfo().midi_position % 4 + 1) * 250;   // 用 MIDI 节拍
else          progress = (time.sec % 4 * 1000 + time.msec) / 4;         // 每 4 秒一个循环
m_draw.DrawWindowText(rect, _T("<<<<"), color_text, color_text2, progress);   // 用双色渐变文字实现「进度」
```
**注意这是「时间驱动的假节拍」，不是音频节拍检测（beat detection）。** 没有 onset detection、没有 BPM 估计。抄的时候不要期待它跟鼓点对齐。

---

## 6. UI 卡顿的规避

### 6.1 V2.70 那两条更新日志对应的实现

**【源码确证】** `doc/update_log.md:288-289`（V2.70）：
> * 如果专辑封面尺寸过大则将其缩小，以解决界面卡顿的问题。
> * 将UI绘图放到后台线程中处理，以解决UI绘图耗时过长导致消息阻塞的问题。

#### (a) 绘图搬到后台线程

**【源码确证】** `MusicPlayerDlg.h:155-167`：

```cpp
CWinThread* m_uiThread;                              // 主界面绘图的线程
static UINT UiThreadFunc(LPVOID lpParam);            // 主界面绘图的线程函数
struct UIThreadPara {
    bool draw_reset{ false };                // 主界面绘图需要重置
    bool ui_thread_exit{ false };            // 指示线程退出
    bool ui_force_refresh{ false };          // 指示主界面强制重绘
    bool search_box_force_refresh{ false };  // 指示搜索框界面强制重绘
    bool is_active_window{ false };          // 指示当前窗口是否为激活窗口
    bool is_completely_covered{ false };     // 指示当前激活的窗口是否完全覆盖主界面
};
UIThreadPara m_ui_thread_para{};
std::atomic<int> m_ui_refresh_interval{};    // 界面实际刷新时间（毫秒）
```

启动：**【源码确证】** `MusicPlayerDlg.cpp:2564` `m_uiThread = AfxBeginThread(UiThreadFunc, (LPVOID)&m_ui_thread_para);`

线程主体：**【源码确证】** `MusicPlayerDlg.cpp:4538-4613` `CMusicPlayerDlg::UiThreadFunc`：

```cpp
int fresh_cnt{};
pThis->m_ui_refresh_interval = theApp.m_app_setting_data.ui_refresh_interval;
while (true) {
    if (pPara->ui_thread_exit) break;

    CPlayer::GetInstance().CalculateSpectralDataPeak();      // 峰值衰减在 UI 线程里算

    if (CPlayer::GetInstance().IsPlaying() && ...GetPlayStatusMutex().try_lock_for(10ms)) {
        if (IsMciCore()) pThis->SendMessage(WM_GET_MUSIC_CURRENT_POSITION);
        else             CPlayer::GetInstance().GetPlayerCoreCurrentPosition();
        ...unlock();
    }

    if (pThis->IsWindowVisible() && !pThis->IsIconic()
        && (IsPlaying() || pPara->is_active_window || pPara->draw_reset
            || pPara->ui_force_refresh || CPlayer::GetInstance().m_loading || theApp.IsMeidaLibUpdating())
        && (!pPara->is_completely_covered || theApp.m_nc_setting_data.always_on_top))
        fresh_cnt = 2;                                       // 连刷两帧
    if (fresh_cnt) { fresh_cnt--; pThis->m_pUI->DrawInfo(pPara->draw_reset);
                     pPara->draw_reset = false; pPara->ui_force_refresh = false; }

    if (IsWindow(pThis->m_miniModeDlg.GetSafeHwnd())) pThis->m_miniModeDlg.DrawInfo();   // 迷你模式
    if (cortana_info_enable) { ... m_cortana_lyric.DrawInfo(); }                          // Cortana 搜索框歌词
    ... m_desktop_lyric.ShowLyric();                                                      // 桌面歌词
    CPlayer::GetInstance().m_controls.UpdatePosition(GetCurrentPosition());                // SMTC 进度
    pThis->m_fps_cnt++;
    Sleep(pThis->m_ui_refresh_interval);
}
```

**抄的时候最该学的三点：**
1. **一个后台线程把「主界面 + 迷你模式 + Cortana 歌词 + 桌面歌词 + SMTC 进度」全包了**，主消息循环只剩事件处理。这就是「解决消息阻塞」的全部秘密。
2. **一个 `fresh_cnt = 2` 的「连刷两帧」机制**：状态变化（`draw_reset` / `ui_force_refresh`）时刷两帧，避免单帧渲染时序问题导致视觉残留。
3. **空闲时降载**：`!IsWindowVisible() || IsIconic()` 或「窗口未激活且没在播放」时**根本不刷**（`MusicPlayerDlg.cpp:4569` 注释：「窗口最小化、隐藏，以及窗口未激活并且未播放时不刷新界面，以降低CPU利用率」）；`is_completely_covered`（被别的窗口完全盖住）也跳过，除非开了置顶。

**双缓冲**：**【源码确证】** `DrawCommon.h` `class CDrawDoubleBuffer`（RAII，构造建 memDC + CompatibleBitmap，析构时 `BitBlt` 回屏并清理）：
```cpp
CDrawDoubleBuffer(CDC* pDC, CRect rect, CRgn* draw_rgn = nullptr) {
    m_memDC.CreateCompatibleDC(NULL);
    if (m_pDC != nullptr) {
        m_memBitmap.CreateCompatibleBitmap(pDC, rect.Width(), rect.Height());
        m_pOldBit = m_memDC.SelectObject(&m_memBitmap);
    }
}
~CDrawDoubleBuffer() {
    if (m_pDC != nullptr) {
        if (m_draw_rgn != nullptr) m_pDC->SelectClipRgn(m_draw_rgn);
        m_pDC->BitBlt(m_rect.left, m_rect.top, m_rect.Width(), m_rect.Height(), &m_memDC, 0, 0, SRCCOPY);
        m_memDC.SelectObject(m_pOldBit); m_memBitmap.DeleteObject();
    }
    m_memDC.DeleteDC();
}
```
用于 `CPlayerUIBase::DrawInfo`（**【源码确证】** `CPlayerUIBase.cpp:43-55`）：
```cpp
//双缓冲绘图
{
    CDC* pDC = m_pDC;
    if (m_skip_next_frame) pDC = nullptr;   // 「跳过下一帧」时把 DC 置空，等于整帧不画
    m_skip_next_frame = false;
    CDrawDoubleBuffer drawDoubleBuffer(pDC, m_draw_rect, draw_rgn);
    m_draw.SetDC(drawDoubleBuffer.GetMemDC());
    ...
}
```
`m_pDC` 是主窗口的 DC，**因此整个绘制是在 UI 线程里操作一个来自主线程的 `CDC*`** —— 这不是严格的线程安全做法，靠的是「主线程此时不画」这个约定。

#### (b) 专辑封面过大就缩小

**【源码确证】** `Player.cpp:2513-2530` `CPlayer::AlbumCoverResize()`：

```cpp
void CPlayer::AlbumCoverResize() {
    m_album_cover_info.GetInfo(m_album_cover);
    m_album_cover_info.size_exceed = false;
    if (!m_album_cover.IsNull() && theApp.m_nc_setting_data.max_album_cover_size > 0) {
        CSize image_size{ m_album_cover.GetWidth(), m_album_cover.GetHeight() };
        if (max(image_size.cx, image_size.cy) > theApp.m_nc_setting_data.max_album_cover_size) {
            wstring temp_img_path{ CCommon::GetTemplatePath() + ALBUM_COVER_TEMP_NAME };
            CDrawCommon::ImageResize(m_album_cover, temp_img_path, theApp.m_nc_setting_data.max_album_cover_size, IT_PNG);
            m_album_cover.Destroy();
            m_album_cover.Load(temp_img_path.c_str());
            m_album_cover_info.size_exceed = true;
        }
    }
    ... (后续生成模糊背景等)
}
```

- 配置项：`CommonData.h:416` `int max_album_cover_size{ 800 };`，落盘在 `[config] max_album_cover_size`（`MusicPlayerDlg.cpp:477` 写 / `:685` 读，默认 800）。
- **注意实现方式：缩放到临时 PNG 文件再重新 Load**（而不是在内存里 `StretchBlt` 到新 `CImage`）。`Player.cpp:2470-2481` 里那段**注释掉的旧实现**正是内存版：
  ```cpp
  ////如果专辑封面过大，则将其缩小，以提高性能
  //if (!m_album_cover.IsNull() && (m_album_cover.GetWidth() > 800 || m_album_cover.GetHeight() > 800))
  //{
  //    CSize image_size(...); CCommon::SizeZoom(image_size, 800);
  //    CImage img_temp;
  //    if (CDrawCommon::BitmapStretch(&m_album_cover, &img_temp, image_size)) m_album_cover = img_temp;
  //}
  ```
  【推测】改成走临时文件，很可能是为了**避开 `BitmapStretch` 在 GDI+ 上的质量/兼容问题**，代价是一次磁盘 IO（在切歌时发生，不在渲染循环里，可接受）。**抄的时候优先试内存版，不行再用文件版。**
- 同样的模式还用于**高斯模糊背景**（**【源码确证】** `Player.cpp:2486-2511` `CPlayer::AlbumCoverGaussBlur`）：先把封面 `CCommon::SizeZoom(image_size, 300)`（长边缩到 300）再模糊，注释写明「将图片缩小以减小高斯模糊的计算量」，模糊半径 `gauss_blur_radius/10`（默认 `CommonData.h:251` `gauss_blur_radius{60}` → sigma 6.0）。
- 另一处相关的：`PropertyAlbumCoverDlg.cpp:644-654` 写入封面时也做同样的缩小，输出 `IT_JPG`。

### 6.2 频谱 / 歌词怎么和绘制线程解耦

**没有用「脏矩形 + 只刷频谱区域」的精细方案**，用的是**整帧重绘 + 后台线程 + 帧率自适应**三件套：

| 数据 | 生产方 | 消费方 | 解耦方式 |
|---|---|---|---|
| **频谱原始 FFT** | `CPlayer::CalculateSpectralData()`（在主线程定时器里调） | `CUIDrawer::DrawSpectrum`（UI 线程） | 成员数组 `float m_fft[512]` / `m_spectral_data[128]`，**无锁读写**（`Player.h:121-124`） |
| **频谱峰值** | `CPlayer::CalculateSpectralDataPeak()`（**在 UI 线程内部**） | 同上 | 同上。**这一项特意放在 UI 线程里算**，就是为了避免「算峰值」和「画峰值」之间出现撕裂 |
| **播放进度** | `CPlayer::GetPlayerCoreCurrentPosition()`（**在 UI 线程里，带 `m_play_status_sync` 锁，10ms 超时**） | 进度条 / 歌词时间轴 | 写进 `m_current_position` 后 UI 线程自己读。注释说明了动机：「和UI同步，使得当界面刷新时间间隔设置得比较小时歌词和进度条看起来更加流畅」 |
| **歌词文本** | `CLyrics` 对象（切歌时换） | `CUIDrawer::DrawLyricTextMultiLine` / `DrawLyricTextSingleLine` | 直接共享 `m_Lyrics`，按当前时间查行 |
| **专辑封面** | `CPlayer::SearchAlbumCover()` | 绘制 | `CCriticalSection m_album_cover_sync`（`Player.h:246`），`GetAlbumCover()` 加锁返回引用 |
| **播放列表初始化** | `CPlayer::IniPlaylistThreadFunc` | 主 UI | `ThreadInfo m_thread_info` + `WM_POST_..._COMPLATE` 消息 |
| **全局播放状态锁** | — | — | `std::timed_mutex m_play_status_sync`（`Player.h:247`），注释：「更改播放状态时加锁，请使用 `GetPlayStatusMutex`」。用 `try_lock_for` 而不是 `lock`，**避免 UI 线程被慢操作卡死** |

**`WM_MAIN_WINDOW_ACTIVATED` / `WM_SET_UI_FORCE_FRESH_FLAG` 这类消息**用于通知 UI 线程外部状态变了（`Define.h:141` 注释：「通知主窗口设置UI强制刷新标志 `m_ui_thread_para.ui_force_refresh`」）。

### 6.3 刷新间隔可配置 + 实测帧率自适应

**【源码确证】** `Define.h:100-104`：
```c
#define UI_INTERVAL_DEFAULT 50      // 界面刷新时间的默认时间间隔（毫秒）
#define MIN_UI_INTERVAL 2           // 界面刷新时间间隔最小值
#define MAX_UI_INTERVAL 300         // 界面刷新时间间隔最大值
#define UI_INTERVAL_STEP 10         // 调整界面刷新时间间隔的步长
```
**【源码确证】** `Define.h:202-203`：
```c
#define MAX_FPS 90                  // UI的最大帧率
#define FPS_LIMIT_MARGIN 10         // 限制帧率上下浮动范围
```
配置项：`CommonData.h:256` `int ui_refresh_interval{ 100 };`，读回时越界则回落默认（`MusicPlayerDlg.cpp:658-660`）。V2.78 更新日志第 25 条：「界面刷新时间间隔的最小值调整为 2 毫秒」。

**「2ms」是怎么被用的**：不是靠 `SetTimer`（`MusicPlayerDlg.cpp:2356` 那行 `//SetTimer(TIMER_ID, ...)` 已被注释掉），而是靠 UI 线程末尾的 `Sleep(pThis->m_ui_refresh_interval)`。

**实测帧率 → 自适应调节（这个闭环很巧妙）** —— **【源码确证】** `MusicPlayerDlg.cpp:2734-2742`（在 1 秒定时器里）：

```cpp
//每隔一秒保存一次统计的帧率
theApp.m_fps = m_fps_cnt;
m_fps_cnt = 0;

//限制帧率
if (theApp.m_fps > MAX_FPS + FPS_LIMIT_MARGIN)                     // > 100 fps
    m_ui_refresh_interval++;
if (m_ui_refresh_interval > theApp.m_app_setting_data.ui_refresh_interval
    && theApp.m_fps < MAX_FPS - FPS_LIMIT_MARGIN)                  // < 80 fps
    m_ui_refresh_interval--;
```

即：**实际帧率超 100 就拉长 Sleep（省 CPU），低于 80 就缩短 Sleep（往目标帧率靠），但永远不会超过用户设定的间隔**。`m_fps_cnt` 在 UI 线程每轮 `++`（`MusicPlayerDlg.cpp:4608`）。

**【源码确证】** 帧率还以 `%(FPS)` 公式变量暴露给自定义界面（`PlayerFormulaHelper.cpp`，`VariableNameMap` 里的 `{L"FPS", PlayerVariable::FPS}` 和 `{L"UiRefreshInterval", ...}`），并在 UI 上画出来（`CPlayerUIBase.cpp:1858` `str_info.Format(_T("%dFPS"), theApp.m_fps)`）。**这属于调试期遗留下来的「性能 HUD」，但非常有用。**

**注意 `theApp.m_fps` 还被频谱峰值衰减公式消费**（§5.3），所以帧率自适应会**间接改变频谱峰值下落手感** —— 这是两条看似无关的代码之间的隐藏耦合，抄的时候要么解耦（用真实 delta-time），要么接受它。

---

## 7. 格式支持矩阵

### 7.1 BASS 内核：基础格式 + 插件发现

**【源码确证】** `BassCore.cpp:82-97`：
```cpp
CAudioCommon::m_surpported_format.clear();
SupportedFormat format;
format.description = theApp.m_str_table.LoadText(L"TXT_FILE_TYPE_BASE");
format.extensions.insert(format.extensions.end(),
    theApp.m_nc_setting_data.default_file_type.begin(), ...end());   // 从配置读
for (const auto& f : theApp.m_nc_setting_data.default_file_type) { ... 拼 "*.ext;" ... }
CAudioCommon::m_surpported_format.push_back(format);
CAudioCommon::m_all_surpported_extensions = format.extensions;
```

`default_file_type` 的默认值 —— **【源码确证】** `MusicPlayerDlg.cpp:698`：
```cpp
vector<wstring>{ L"mp3", L"wma", L"wav", L"flac", L"ogg", L"oga", L"m4a", L"mp4",
                 L"cue", L"mp2", L"mp1", L"aif", L"aiff", L"asf" }
```
**用户可以往 `config.ini` 的 `[config] default_file_type` 里加扩展名**（Wiki《支持的音频格式》也这么说），加完之后这些文件才会被拖进播放列表。

然后是插件自报（§2.4），**仓库里实际带的插件**（`Plugins/` 目录，来自 git tree）：

| 插件 DLL | 覆盖格式（Wiki 表 + 命名） |
|---|---|
| `bassflac.dll` | flac |
| `basswma.dll` | wma / asf（解码侧） |
| `bassmidi.dll` | mid / midi / rmi / kar |
| `bass_aac.dll` | aac |
| `bass_ape.dll` | ape / mac |
| `basscd.dll` | cda（CD Audio） |

**Wiki 里列了但仓库里没有的插件**（按 Wiki 表里的「BASS插件」列）：`bassalac.dll`(m4a)、`bassdsd.dll`(dff/dsf)、`bassopus.dll`(opus)、`basswv.dll`(wv)、`bass_ac3.dll`(ac3)、`bass_mpc.dll`(mpc/mp+/mpp)、`bass_spx.dll`(spx)、`bass_tta.dll`(tta)。**【源码确证】** `doc/wiki/主菜单.md:382`：「此列表显示的内容会取决于加载的 BASS 插件……更多插件可以到 http://www.un4seen.com/ 下载。下载后将dll文件放到 `./Plugins` 目录下，重新启动播放器即可。」

另外 TAK 格式：**【源码确证】** `doc/wiki/支持的音频格式.md` 注：「由于 tak 格式官方解码器只提供了 32 位版本，因此仅 32 位的 MusicPlayer2 能够播放 tak 格式音频。」

### 7.2 `.cue` 扩展名到 `AudioType` 的映射（判类型用两套逻辑）

**【源码确证】** `AudioCommon.cpp` `CAudioCommon::GetAudioTypeByFileExtension` —— 按**扩展名**判（用于播放列表/媒体库阶段，此时还没开流）：
```
mp3|mp2|mp1→AU_MP3   wma|asf→AU_WMA_ASF   ogg|oga→AU_OGG   m4a|mp4→AU_MP4
aac→AU_AAC   flac|fla→AU_FLAC   cue→AU_CUE   ape|mac→AU_APE
mid|midi|rmi|kar→AU_MIDI   aif|aiff→AU_AIFF   wav→AU_WAV
mpc|mp+|mpp→AU_MPC   dff|dsf→AU_DSD   opus→AU_OPUS   wv→AU_WV
spx→AU_SPX   tta→AU_TTA   其他→AU_OTHER
```

**【源码确证】** `AudioCommon.cpp:755-806` `GetAudioTypeByBassChannel` —— 按 **BASS 的 `ctype` 数值**判（用于流已打开后）。用的是**硬编码 magic number**而不是 `bass.h` 的宏（因为部分插件类型 `bass.h` 里没有）：
```
0x10003/0x10004/0x10005 → AU_MP3      BASS_CTYPE_STREAM_WAV(+PCM/FLOAT) → AU_WAV
0x10300 / 0x10301       → AU_WMA_ASF  BASS_CTYPE_STREAM_AIFF / _OGG → AIFF / OGG
0x10b01 → AU_MP4   0x10b00 → AU_AAC   0x10900/0x10901 → AU_FLAC   0x10d00 → AU_MIDI
0x10700 → AU_APE   0x10a00 → AU_MPC   0x11700 → AU_DSD   0x11200 → AU_OPUS   0x10500 → AU_WV
```
另有 `CAudioCommon::GetBASSChannelDescription(DWORD ctype)`（`:668` 起）把 ctype 转成显示字符串（`"MP3"`/`"FLAC"`/`"MIDI"`…），用于「音频类型」列。

**判 MIDI 靠的是 ctype**（`BassCore.cpp:317`）：`m_is_midi = (GetAudioTypeByBassChannel(m_channel_info.ctype) == AudioType::AU_MIDI)`。

### 7.3 MIDI 和 sf2 音色库

**【源码确证】** `BassCore.cpp:138-174`（初始化阶段）：

```cpp
if (format.description == L"MIDI") {
    m_bass_midi_lib.Init(plugin_dir + plugin_file);       // 加载 bassmidi.dll
    m_sfont_name = ...LoadText(L"UI_TXT_SF2_NAME_NONE");
    m_sfont.font = 0;
    if (m_bass_midi_lib.IsSucceed()) {
        wstring sf2_path = theApp.m_play_setting_data.sf2_path;
        if (!CCommon::FileExist(sf2_path)) {              // 配置的路径无效 → 找 Plugins\soundfont\*.sf2
            vector<wstring> sf2s;
            CCommon::GetFiles(plugin_dir + L"soundfont\\*.sf2", sf2s);
            if (!sf2s.empty()) sf2_path = plugin_dir + L"soundfont\\" + sf2s[0];
        }
        if (CCommon::FileExist(sf2_path)) {
            m_sfont.font = m_bass_midi_lib.BASS_MIDI_FontInit(sf2_path.c_str(), BASS_UNICODE);
            ... BASS_MIDI_FontGetInfo → m_sfont_name
            m_sfont.preset = -1;   // -1 = 全部 preset
            m_sfont.bank   = 0;
        }
    }
}
```

**要点：**
- **`bassmidi.dll` 不内置，是 `Plugins` 目录下的插件**（但代码特别识别它：靠 `plugin_info->formats->name == "MIDI"` 这个**字符串相等**来认，比较脆）。
- **sf2 加载时机是 `InitCore()`，即启动/重建内核时**。所以 Wiki 说「更改此项后需要重新启动播放器才能生效」。
- **sf2 查找顺序**：配置的 `sf2_path` → `Plugins\soundfont\*.sf2` 的第一个。
- **没有 sf2 就不能播 MIDI**：`CBassCore::IsMidiConnotPlay()` = `m_is_midi && m_sfont.font == 0`（`BassCore.cpp:841-844`）；上层 `CPlayer::ConnotPlayWarning()` 弹 `WM_CONNOT_PLAY_WARNING` → `MusicPlayerDlg.cpp` `OnConnotPlayWarning` 弹框「MSG_NO_MIDI_SF2_WARNING」，用户选取消可以关掉这个警告（`no_sf2_warning`）。格式转换时也会因为没 sf2 直接返回错误码 `CONVERT_ERROR_MIDI_NO_SF2 (-4)`（`BassCore.cpp:585-596`）。
- **每开一个 MIDI 都要重新绑音色库**：`BassCore.cpp:318-319` `if (m_bass_midi_lib.IsSucceed() && m_is_midi && m_sfont.font != 0) BASS_MIDI_StreamSetFonts(m_musicStream, &m_sfont, 1);`
- **MIDI 元信息**（`BassCore.cpp:322-338`）：
  ```cpp
  BASS_ChannelGetAttribute(m_musicStream, BASS_ATTRIB_MIDI_PPQN, &m_midi_info.ppqn);
  m_midi_info.midi_length = BASS_ChannelGetLength(m_musicStream, BASS_POS_MIDI_TICK) / ppqn;
  m_midi_info.tempo = BASS_MIDI_StreamGetEvent(m_musicStream, 0, MIDI_EVENT_TEMPO);
  m_midi_info.speed = 60000000 / m_midi_info.tempo;       // BPM
  ```
  **节拍数 = `(BASS_ChannelGetPosition(stream, BASS_POS_MIDI_TICK) + ppqn/4) / ppqn`**（`BassCore.cpp:240`，注释：「+ (m_midi_info.ppqn / 4) 的目的是修正显示的节拍不准确的问题」—— **这是一个实测出来的补偿量**）。
- **MIDI 内嵌歌词**（这是这个项目的一个亮点）：
  ```cpp
  if (BASS_MIDI_StreamGetMark(m_musicStream, BASS_MIDI_MARK_LYRIC, 0, &mark))         // 优先歌词轨
      BASS_ChannelSetSync(m_musicStream, BASS_SYNC_MIDI_MARK, BASS_MIDI_MARK_LYRIC, MidiLyricSync, (void*)BASS_MIDI_MARK_LYRIC);
  else if (BASS_MIDI_StreamGetMark(m_musicStream, BASS_MIDI_MARK_TEXT, 20, &mark))   // 退而求其次：文本轨超过 20 条
      BASS_ChannelSetSync(m_musicStream, BASS_SYNC_MIDI_MARK, BASS_MIDI_MARK_TEXT, MidiLyricSync, (void*)BASS_MIDI_MARK_TEXT);
  BASS_ChannelSetSync(m_musicStream, BASS_SYNC_END, 0, MidiEndSync, 0);
  ```
  回调 `MidiLyricSync`（`BassCore.cpp:204-227`）按 karaoke 标记解析：`'@'` 开头跳过（info）、`'\\'` 清屏、`'/'` 换行。
  从 MIDI 取标题：`BASS_MIDI_StreamGetMark(hStream, BASS_MIDI_MARK_TRACK, 0, &mark)`（`BassCore.cpp:555`）。

### 7.4 cue 分轨怎么实现

**核心思想：cue 只做「元信息 + 起止时间」的解析，播放时仍然播整个音频文件，靠 seek 跳过非本轨部分、靠上层计时做「结束」判定。**

**【源码确证】** `CueFile.h` `class CCueFile`：`MoveToSongList(vector<SongInfo>&)` / `GetAnalysisResult()` / `GetTrackInfo(audio_path, track)` / `Save(...)` / `GetCuePropertyMap()` / `GetTrackPropertyMap(...)`；私有 `DoAnalysis()` / `GetCommand()` / `FindAllProperty()`。

**【源码确证】** `CueFile.cpp:189` 起 `CCueFile::DoAnalysis()`：
- 先解析 `FILE` 之前的部分（cue 头）：`TITLE`→album、`REM GENRE`、`REM DATE`、`REM COMMENT`、`PERFORMER`→album_artist/artist、`REM DISCNUMBER`、`REM TOTALDISCS`，并设 `song_info_common.is_cue = true`。
- 然后双循环：外层找 `FILE `（可多个音频文件），内层找 `TRACK `（受下一个 `FILE` 位置限制）。
- 每轨取 `TITLE`、`PERFORMER`、`INDEX 00` / `INDEX 01`（`CueTime2Time(字符串)` 解析成 `CPlayTime`）。
  ```cpp
  song_info.start_pos = time_index01;
  // 上一个 TRACK 的结束位置 = 本轨的 INDEX 00（若有）否则 INDEX 01
  if (!m_result.empty() && ...) {
      if (!time_index00.isZero()) m_result.back().end_pos = time_index00;
      else                        m_result.back().end_pos = time_index01;
  }
  ```
- 编码识别存在 `CodeType m_code_type`，V2.70 修过「无法正常读取 UTF8 格式的 cue 文件…新增对 UTF16-LE-BOM 编码格式的 cue 文件的支持」。
- 支持「内嵌 cue 分轨」（V2.7x 更新日志第 235 行「新增内嵌cue分轨的支持」）和「cue 音轨保存到媒体库」（`:154`）。

**播放侧（cue 的关键就这几行）** —— **【源码确证】** `Player.cpp:2025-2045` `CPlayer::SeekTo(int position)`：

```cpp
void CPlayer::SeekTo(int position) {
    ...
    if (m_playlist[m_index].is_cue)
        position += m_playlist[m_index].start_pos.toInt();     // 相对进度 → 文件的绝对位置
    m_pCore->SetCurPosition(position);
    ...
}
```

**【源码确证】** `Player.cpp:655-666` `GetPlayerCoreCurrentPosition()`：
```cpp
int current_position_int = m_pCore->GetCurPosition();
if (!IsPlaylistEmpty() && GetCurrentSongInfo().is_cue)
    current_position_int -= GetCurrentSongInfo().start_pos.toInt();   // 绝对位置 → 相对进度
m_current_position.fromInt(current_position_int);
```

**【源码确证】** `Player.cpp:527-528`（`Command::OPEN`）：
```cpp
if (cur_song.is_cue) m_song_length = cur_song.length();      // 用 cue 算出来的本轨长度
else                 m_song_length = m_pCore->GetSongLength();// 用内核给的文件长度
```

**结束判定靠上层兜底** —— **【源码确证】** `Player.cpp:643-653` `CPlayer::SongIsOver()`：
```cpp
if (m_pCore->SongIsOver()) return true;
if (GetCurrentSongInfo().is_cue || IsMciCore())
    return (m_playing == PS_PLAYING && m_current_position >= m_song_length && m_current_position.toInt() != 0);
```
**因为内核只知道整个文件的长度，不知道「本轨到哪儿结束」。**

**【源码确证】** `Player.cpp:578-582`（`Command::STOP` 的特殊处理）：
```cpp
if (GetCurrentSongInfo().is_cue && GetCurrentSongInfo().start_pos > 0) {
    SeekTo(0);            // cue 音轨「停止」= 回到本轨开头并暂停（而不是停到文件开头）
    m_pCore->Pause();
} else m_pCore->Stop();
```

**格式转换也复用了 cue 的起止时间** —— **【源码确证】** `FormatConvertDlg.cpp:525-536`：
```cpp
int start_pos = 0, end_pos = 0;
if (song_info.is_cue) { start_pos = song_info.start_pos.toInt(); end_pos = song_info.end_pos.toInt(); }
... EncodeAudio(file_path, out_path, format, para, freq, callback, start_pos, end_pos)
```
→ `CBassCore::EncodeAudio`（`BassCore.cpp:622-626`）先 `SetCurrentPosition(hStream, start_pos)`，然后在循环里用 `cue_length = end_pos - start_pos` 计算百分比，`percent == 100` 就 break（`:761-769`）。

---

## 8. BASS 的授权与替代线索（**这一节对你评估「不用 BASS」最关键**）

### 8.1 授权：代码和文档里几乎没有

**【源码确证】** 全库搜索结论：

| 位置 | 内容 |
|---|---|
| `doc/wiki/支持的音频格式.md:3` | 「音频解码功能基于 BASS 音频库 ([www.un4seen.com](http://www.un4seen.com/))。」 —— **只给 URL，没有一个字提授权** |
| `doc/wiki/主菜单.md:382` | 「更多插件可以到 http://www.un4seen.com/ 下载。下载后将dll文件放到 `./Plugins` 目录下，重新启动播放器即可。」 |
| `MusicPlayer2/bass.h:1-6` | `/* BASS 2.4 C/C++ header file  Copyright (c) 1999-2021 Un4seen Developments Ltd.  See the BASS.CHM file for more detailed documentation */` —— **只有版权声明，没有 license 声明** |
| 仓库根 / `About` 对话框 | 顶层 `LICENSE` 是 **GPL-3.0**（覆盖 MusicPlayer2 自己的代码）。**我没有在仓库里找到任何关于 BASS 商业授权条款、许可证文本或购买说明的文件。** |

**【推测 / 外部常识】** BASS 的实际授权模式是「**非商业用途免费，商业用途需向 un4seen 购买 license**」——但**这一点在 MusicPlayer2 的仓库里没有任何文档或代码痕迹**。如果你要写「造播放器前必读」，这一条必须自己去看 un4seen 官网确认，**不能引用这个项目当作依据**。

### 8.2 DLL 分发方式（这个很重要）

| DLL | 是否在仓库里 | 依据 |
|---|---|---|
| `bass.dll` | ❌ **不在仓库**（只有 `Debug/bass.dll`、`x64/Debug/bass.dll` 这种**构建产物残留**，被误提交了） | git tree 里只有 `Debug/bass.dll`、`x64/Debug/bass.dll`，**没有 `MusicPlayer2/bass.dll` 或 Release 目录下的** |
| `bass.lib` / `bass_x64.lib` | ✅ 在 `MusicPlayer2/` 下 | `MusicPlayer2/bass.lib`、`MusicPlayer2/bass_x64.lib` |
| `bass_fx.dll` | ❌ 不在仓库（只有 `Debug/bass_fx.dll`、`x64/Debug/bass_fx.dll`） | 同上 |
| `Plugins/*.dll`（6 个） | ✅ 在仓库 | `MusicPlayer2/Plugins/` |
| `Encoder/{bassenc,bassmix}.dll` + `{lame,oggenc,flac}.exe` | ✅ 在仓库 | `MusicPlayer2/Encoder/` |
| `ffmpeg_core.dll` | ❌ 不在仓库，**要求用户单独下载** | `FfmpegCore.cpp:16`；Wiki《播放设置》给了下载链接 |
| `tag.dll` | ❌（只有 Debug/Release 残留） | 与 taglib 同理 |

**【源码确证】** 运行时兜底检查：`MusicPlayer2.cpp:232-242`：
```cpp
//检查bass.dll的版本是否和API的版本匹配
WORD dll_version{ HIWORD(BASS_GetVersion()) };
if (dll_version != BASSVERSION) {                       // BASSVERSION = 0x204（bass.h:47）
    ... MSG_BASS_VERSION_WARNING ...
    if (AfxMessageBox(info, MB_ICONWARNING | MB_OKCANCEL) == IDCANCEL) return FALSE;   // 用户可取消 → 不启动
}
```
**这就是「bass.dll 缺失/版本不符」时的实际表现：弹框警告，用户点取消就直接退出程序**（Wiki《程序文件说明》也说「没有它，MusicPlayer2 将无法启动」）。

**关键结论：程序把核心 DLL 作为「随包分发但不在版本库」的外部依赖。** 想抄这个模式的话，`bass.dll` 这种「免费但不开源」的组件，通常做法就是把预编译 dll 放进 Release 压缩包而不是 git 仓库。

### 8.3 FFmpeg 内核走到什么程度 —— **半成品，而且是「套壳」**

这是全篇最需要说清楚的一点：

**它没有链接 libavcodec，也没有调用 ffmpeg.exe 命令行。它调用的是一个第三方 DLL 的扁平 C 接口。**

**【源码确证】** `FfmpegCore.cpp:13-21`：
```cpp
CFfmpegCore::CFfmpegCore() {
    handle = nullptr; err = 0;
    Init(L"ffmpeg_core.dll");                 // ← 关键
    if (!IsSucceed()) { theApp.WriteLog(...LoadText(L"LOG_FFMPEG_INIT_FAILED")); }
}
```

**【源码确证】** `FfmpegCore.h:11-73` —— 整个接口是**一堆 C 函数指针 typedef**，`typedef struct MusicHandle MusicHandle;` 这种**不完整类型**说明实现完全在 DLL 里：
```c
ffmpeg_core_open / _open2 / _open3          ffmpeg_core_play / _pause / _seek
ffmpeg_core_set_volume / _set_speed / _set_equalizer_channel
ffmpeg_core_get_cur_position / _song_is_over / _get_song_length
ffmpeg_core_get_channels / _get_freq / _get_bits / _get_bitrate
ffmpeg_core_get_metadata / _get_fft_data
ffmpeg_core_init_settings / _settings_set_volume / _settings_set_speed
ffmpeg_core_settings_set_cache_length / _max_retry_count / _url_retry_interval
ffmpeg_core_settings_set_use_WASAPI / _enable_exclusive / _max_wait_time
ffmpeg_core_get_audio_devices / _is_wasapi_supported / _version / _version_str
ffmpeg_core_dump_library_version / _dump_ffmpeg_configuration
```
`GetFunction()`（`FfmpegCore.cpp:411-531`）用 `GetProcAddress` 取了 **50+ 个**函数指针，并在末尾**逐个 `rtn &= (... != NULL)` 全量校验**——**任何一个缺失，整个内核判定为加载失败**（然后 `IniPlayerCore` 会回退到 BASS）。

**上游是谁（我实际查证了）：**
```console
$ gh api /repos/lifegpc/ffmpeg_core --jq '{description,language,license:.license.spdx_id,stars,pushed}'
{"description":"A music player core which use ffmpeg and SDL","language":"C",
 "license":"GPL-3.0","stars":49,"pushed":"2024-05-21T11:37:50Z"}

$ gh api /repos/lifegpc/ffmpeg_core/releases --jq '.[] | "\(.tag_name) \(.published_at) \([.assets[].name]|join(","))"'
v1.0.0.1  2022-04-10  ffmpeg_core.v1.0.0.1.x64.7z
v1.0.0.0  2022-02-23  ffmpeg_core.v1.0.0.0.x64.7z, ffmpeg_core.v1.0.0.0.x86.7z
```
→ **上游是 `lifegpc/ffmpeg_core`（C 语言 + FFmpeg + SDL，GPL-3.0，49 stars），最后一版 `v1.0.0.1` 发布于 2022-04，仓库最后 push 是 2024-05。也就是说：MusicPlayer2 的「第二内核」建立在一个 2022 年就基本停更的第三方 DLL 上。**（**【源码确证】** 版本门限：`FfmpegCore.cpp:519` `if (version > FFMPEG_CORE_VERSION(1,0,0,0))` 才去取 WASAPI 相关函数，说明作者适配的是 1.0.0.0 之后加了 WASAPI 的版本。）

**功能完成度（逐个函数看）：**

| 能力 | 状态 | 依据 |
|---|---|---|
| 播放/暂停/定位/音量/变速 | ✅ | `Play()` `Pause()` `Stop()`（= `Close()`）`SetCurPosition()` `SetVolume()` `SetSpeed()` |
| **变调** | ❌ | `FfmpegCore.cpp:215-217` `void CFfmpegCore::SetPitch(int pitch) { }` 空函数；`IsPitchAvailable() → false` |
| **混响** | ❌ | `FfmpegCore.cpp:366-370` `SetReverb` / `ClearReverb` 都是空函数 |
| **均衡器** | ⚠️ 部分 | `ApplyEqualizer` 有实现，但要先把 0~9 通道号转成中心频率 `GetEqChannelFreq()`（**10 档：80/125/250/500/1000/1500/2000/4000/8000/16000**，注意第 10 档是 16000，反证了 §4.1 里 BASS 侧 `1600` 是笔误） |
| **频谱** | ✅ 但量纲不同 | `GetFFTData` 调 `ffmpeg_core_get_fft_data(handle, fft_data, FFT_SAMPLE)`，上层用 `scale = 100`（BASS 是 60） |
| **格式转换（编码）** | ❌ | `FfmpegCore.cpp:691-708`：`EncodeAudio → false`、`InitEncoder → false`、`IsFreqConvertAvailable → false` |
| MIDI | ❌ | `IsMidi → false`、`GetMidiInnerLyric → L""` |
| 元信息/标签 | ✅ | 直接读 FFmpeg 的 metadata key：`title`/`artist`/`album`/`comment`（fallback `description`）/`genre`/`date`/`track` |
| WASAPI / 独占 | ✅ **仅此内核有** | `EnableWASAPI` / `EnableExclusiveMode` / `IsWASAPISupported` |
| 网络流缓存/重试 | ✅ | `cache_length`(默认15)、`max_retry_count`(3)、`url_retry_interval`(5)、`max_wait_time`(3000) |

**支持的格式（硬编码列表，`FfmpegCore.cpp:30-67`）** —— **这是「FFmpeg 内核比 BASS 强」的核心卖点**：
```
基础: mp3 wma wav m4a ogg oga flac ape mp2 mp1 opus cda aif aiff cue mp4 mkv mka m2ts
3gp 3g2 mj2 psp m4b ism ismv isma f4v | aa(Audible) | aac | ac3 | alac | asf
als | amr | ape mac | aptx | atrac | dst | dca(DTS) | flac fla | flv live_flv kux
gsm | mp1 mp2 mp3 mp4 | m4a mp4 | opus | pcm | spx | tak | tta | ogg oga
wv | mpc mp+ mpp | wma | mov | avi | cda
+ 用户自定扩展名（config: user_defined_type_ffmpeg）
```
**结论：FFmpeg 内核是「能播但功能残缺」的半成品 —— 格式覆盖远超 BASS（含视频容器 mkv/mov/avi/m2ts/flv），但音效、变调、格式转换、MIDI 全部缺失，且依赖一个停更的外部 DLL。**

Wiki 也直言不讳（**【源码确证】** `doc/wiki/播放设置.md:73-77`）：
> * 「均衡器」——「混响」功能在 ffmpeg 内核下暂时无法使用。
> * 转换格式功能在 ffmpeg 内核下暂时无法使用。
> * 在 bass 内核下使用「播放控制」菜单下的「加速」、「减速」功能时会导致音调发生变化，而 ffmpeg 内核下就不会。
> * 频谱分析的显示效果略有区别。

### 8.4 对「不用 BASS」的可行性判断

**【推测】** 基于以上源码事实，我的判断：

1. **解码/输出的活儿，FFmpeg 完全能替**。这个项目的 FFmpeg 路径已经证明：`avformat_open_input` + 编解码 + SDL/WASAPI 输出这套组合能覆盖 BASS 的全部格式（且更多）。**真正的替代品是「libavcodec + SDL/miniaudio/WASAPI」，而不是 ffmpeg.exe 命令行。**
2. **最难替的是「变速不变调」和「DX8 音效」这两块。** BASS 在这里提供的是 `BASS_FX_TempoCreate`（时域伸缩 + 变调）和 `BASS_FX_DX8_PARAMEQ`/`BASS_FX_DX8_REVERB`（现成的均衡/混响 DSP）。用 FFmpeg 阵营替代需要：
   - 变速变调：FFmpeg 的 `atempo`/`rubberband` 滤镜，或 libsoundtouch / Signalsmith Stretch；
   - 均衡：自己写 biquad peaking EQ（10 个级联），或 FFmpeg 的 `equalizer`/`anequalizer` 滤镜（但要接在播放链上，不能在解码侧离线做）；
   - 混响：FFmpeg 有 `afir`/`aecho`，但**没有 Freeverb/DX8 那种现成的「mix/time」双参数混响**，`BASS_DX8_REVERB` 的 4 参数组合需要自己用 Freeverb 或 Schroeder 实现，并像 §4.2 那样写 dB 映射。
3. **FFT 频谱反而是最好替的**：`BASS_ChannelGetData(..., BASS_DATA_FFT1024)` = 1024 点 Hanning 窗 FFT。任何 FFT 库（FFTW/kissfft/pffft）+ 一个 Hanning 窗都能做到，**前提是你得自己抽音频样本**——而 BASS 的便利在于它能从「正在播放的流」里直接抽，不需要你插一个 tap。
4. **注意 `IPlayerCore` 里 `GetFFTData(float[512])` 这个签名本身就是 BASS 的形状**。用 FFmpeg 阵营时，最好定义成 `GetFFTData(float* out, int count, int sample_rate)` 之类，别继承这个 512 的硬编码。
5. 如果只是为了「不依赖 BASS」，`IPlayerCore` 这套抽象是**可以直接拿来用的**：BASS/MCI/FFMPEG 三个实现已经证明了接口能容纳三种完全不同的后端，包括一个「什么都不会」的后端（MCI 的音效全是空实现）。

---

## 9. 坑与已知问题

### 9.1 FFmpeg 内核「播放结束后卡住」（V2.78 修过，#820 #822）

**【源码确证】** 更新日志：`doc/update_log.md:73`「修正使用FFMPEG内核时，部分音频文件在播放结束后卡住的问题 #820 #822」（V2.78，2025/12/27）。

代码里对应的修复痕迹 —— `FfmpegCore.cpp` 里 **`SongIsOver()` 有两套判定 + 一个 seek 保护**：

```cpp
// FfmpegCore.cpp:229-266  CFfmpegCore::SongIsOver()
bool song_is_over = ffmpeg_core_song_is_over(handle);
if (song_is_over) return true;

auto playing_state = GetPlayingState();
//如果正在播放但是播放进度没有变化，则认为已播放结束
if (playing_state == PS_PLAYING) {
    static int last_position = 0;
    static int position_no_changed_count = 0;       //播放进度没有变化计数
    int position = GetCurPosition();
    int length = GetSongLength();
    bool is_at_end = false;
    if (length > 0 && position > 0 && (double)position / (double)length > 0.98) is_at_end = true;
    if (position == last_position && is_at_end) position_no_changed_count++;
    else                                        position_no_changed_count = 0;
    //播放进度没有变化超过一定时间，则认为播放结束。防止因为系统卡顿等原因导致播放进度没有变化被误认为播放结束
    if (position_no_changed_count > 30) return true;
    last_position = position;
}
```

```cpp
// FfmpegCore.cpp:284-291  CFfmpegCore::SetCurPosition()
void CFfmpegCore::SetCurPosition(int position) {
    if (IsSucceed() && handle && GetSongLength() != 0) {    // 时长为0(获取失败)时seek(0)会卡死
        int re = ffmpeg_core_seek(handle, (int64_t)position * 1000);
        ...
    }
}
```

**三条可直接抄的经验：**
1. **依赖 `ffmpeg_core_song_is_over()` 这个单一信号不够** —— 有些文件它永远返回 false，于是「播完了但 UI 一直显示在播」，这就是「卡住」。
2. **兜底判定 = 「进度值连续 30 次不变，且已过 98% 时长」**。`> 0.98` 这个阈值是必需的，否则开头缓冲卡顿也会被误判为结束（注释明确写了这层顾虑）。
3. **`GetSongLength() == 0` 时绝对不能 seek** —— 注释「时长为0(获取失败)时seek(0)会卡死」。这是**很典型的一类死锁**：后端拿不到时长，`seek(0)` 就阻塞在等数据上。任何播放器都该有这个保护。

**⚠️ 但这两个实现本身有两个缺陷，抄的时候要改：**
- `static int last_position` / `static int position_no_changed_count` **是函数级 static，不是成员变量**。多实例或换歌后状态不重置 —— 换歌时如果新歌进度恰好从相同值开始，计数会延续。
- `position_no_changed_count > 30` 的「30」**没有跟 `ui_refresh_interval` 关联**，所以「30 次」对应的真实时长随刷新间隔在 60ms~9s 之间浮动。

### 9.2 多设备 / 蓝牙切换

**【源码确证】** `CDevicesManager.cpp` + `CMMNotificationClient.cpp`：

```cpp
// CDevicesManager.cpp:41-52
HRESULT CDevicesManager::InitializeDeviceEnumerator() {
    if (!pEnum) {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        client = new CMMNotificationClient(pEnum, this);
        pEnum->RegisterEndpointNotificationCallback(client);
    }
    return hr;
}

// CDevicesManager.cpp:60-67
void CDevicesManager::DefaultMultimediaDeviceChanged() {
    TRACE("PostMessage: WM_RE_INIT_BASS_CONTINUE_PLAY\n");
    PostMessage(theApp.m_pMainWnd->GetSafeHwnd(), WM_RE_INIT_BASS_CONTINUE_PLAY,
                theApp.m_play_setting_data.stop_when_play_device_changed, 0);
}
```
```cpp
// CMMNotificationClient::OnDefaultDeviceChanged(...)  →  filter eRender/eMultimedia  →  manager->DefaultMultimediaDeviceChanged()
```
```cpp
// MusicPlayerDlg.cpp  OnReInitBassContinuePlay
if (GetPlayerCore() != nullptr && GetPlayerCore()->GetCoreType() == PlayerCoreType::PT_BASS) {
    bool stop_play = (wParam != 0);
    CPlayer::GetInstance().ReIniPlayerCore(!stop_play);   // 重建内核，成功则续播
}
```

**注意几点：**
- **源文件头是 `DefaultAudioChanger` 的 GPL 头**（`CDevicesManager.cpp:1-15`，`Copyright (c) 2011 Sergiu Giurgiu`）—— 从别的开源项目搬过来的，抄的时候注意署名。
- **只有 BASS 内核走这条路**（`GetCoreType() == PT_BASS` 才处理）。FFmpeg 内核的设备变化靠它自己的 `max_wait_time` / cache 设置，**项目里没有等价的处理**。
- **`stop_when_play_device_changed`（`CommonData.h:327`，默认 `false`）** 是 `wParam`：0 = 「换设备后继续播」，1 = 「换设备就停」。对应更新日志第 221 行「新增播放设备变化时自动切换播放设备的功能」和第 301 行「修正播放设备发生变化时无法继续播放的问题」。
- **蓝牙耳机**：`端点是 WASAPI 的 `eRender`，蓝牙耳机的连接/断开就是默认设备变化 → 走同一条路。【推测】蓝牙切换时会有明显的重连延迟，而 `ReIniPlayerCore` 里那句注释「系统从挂起中恢复可能很卡」+ `try_lock_for(5000ms)` 说明作者确实遇到过这种慢场景。
- 还有一条**电源事件**路径：`MusicPlayerDlg.cpp` 里有 `ON_WM_POWERBROADCAST` + `DeviceNotifyCallbackRoutine` / `HPOWERNOTIFY RegistrationHandle`（`MusicPlayerDlg.h:179-180`），用于挂起/恢复。**恢复后的续播也靠 `ReIniPlayerCore`。**
- **显示变化**：`ON_WM_DISPLAYCHANGE` → `GetScreenInfo()` + 退出全屏（`MusicPlayerDlg.cpp` `OnDisplaychange`）。

### 9.3 采样率不一致时的重采样

**BASS 内核：完全交给 BASS。**
- 输出固定 44100（`BassCore.cpp:71`），源文件任何采样率都由 BASS 内部重采样，项目代码零参与。
- 唯一显式的重采样发生在**格式转换**时（用 `BASS_Mixer_StreamCreate(dest_freq, ...)`，见 §3.1）。
- **项目不暴露任何重采样质量设置**（没有 `BASS_CONFIG_SRC` / `BASS_ATTRIB_SRC` 的调用）。
- **MIDI 的 `BASS_ATTRIB_MIDI_PPQN` 是节拍分辨率，跟采样率无关**，别混。

**FFmpeg 内核**：由 `ffmpeg_core.dll` 内部处理（**这是 DLL 内部实现，不在本仓库，我读不到**【推测】）。

### 9.4 内存占用

**【源码确证】** 更新日志第 302 行（V2.6x 某版）：「稍微减少了程序的内存占用。」—— **没有更详细的说明。**

代码里能找到的所有「省内存」措施：

| 措施 | 位置 | 说明 |
|---|---|---|
| 专辑封面超过 `max_album_cover_size`（默认 800px）就缩小 | `Player.cpp:2513-2530` `AlbumCoverResize()` | **主要目的是省 GDI+ 绘制时间，顺带省内存** |
| 高斯模糊前先把封面长边缩到 300 | `Player.cpp:2498-2500` `CPlayer::AlbumCoverGaussBlur()` | 注释：「将图片缩小以减小高斯模糊的计算量」。**背景模糊这张图是常驻的，所以这 300px 上限直接决定了背景图内存** |
| 频谱用预计算映射表避免每帧 `log()` | `SpectralDataHelper.cpp:6-25` | 省 CPU 不是省内存 |
| 切歌时 `m_album_cover.Destroy()` + 重建 | `Player.cpp:2444-2445` | 避免两张封面同时存在 |
| 播放列表初始化放线程 + 分批 | `CPlayer::IniPlaylistThreadFunc` / `ThreadInfo` | 配合 `MAX_SONG_NUM 99999`、`ADD_TO_PLAYLIST_MAX_SIZE 20` 等上限 |
| 媒体库扫描走 `GetAudioInfo(files, ...)` 批量 + `MediaLibRefreshMode`（最小/按修改时间/强制全刷） | `IPlayerCore.h` 的 `GetAudioInfo` 注释、`AudioCommon.h` 的 `MediaLibRefreshMode` | **媒体库是内存大头**（`song_data.dat`），分级刷新是主要手段 |

**没有**：内存池、流式解码缓冲上限控制、封面缓存 LRU。**【源码确证】** 搜不到 `LRU`/`cache` 相关的封面缓存实现（只有 `bass` 自己的网络流 cache）。

### 9.5 其他我在阅读中发现的坑（都标了源码位置）

| # | 问题 | 位置 | 影响 |
|---|---|---|---|
| 1 | **EQ 第 10 段中心频率 `1600` 应为 `16000`**（上游 issue #905「16K频点错误」，编号来自维护侧线索，未独立核对原文） | `BassCore.h:112` `FREQ_TABLE[...]` | 高频 EQ 实际作用在 1.6kHz |
| 2 | **`EQU_STYLE_TABLE` 的「爵士」行只有 9 个数** | `Define.h:121` | C++ 补 0，能跑但意图不明 |
| 3 | **`fBandwidth = 30` 倍频程过大** | `BassCore.cpp:879` | 10 个滤波器严重重叠，音质可疑（见 §4.1） |
| 4 | `sizeof(数组形参)` 清不干净 | `MciCore.cpp` `GetFFTData`、`FfmpegCore.cpp:374/377` | 只清 4/8 字节；FFmpeg 侧靠紧接着的 API 覆盖才没出事 |
| 5 | `CPlayer::SetPitch` 用开区间，±12 被丢弃 | `Player.cpp` `SetPitch` vs `IPlayerCore.h:28-29` 定义闭区间 | 用户设到极值无效 |
| 6 | `DrawSpectrum` 宽度算错（用 `cols-1` 当除数） | `CUIDrawler.cpp:380` | 频谱总宽略超容器 |
| 7 | 插件扩展名解析用 `npos` 参与算术 | `BassCore.cpp:126` | 靠 `substr` 截断侥幸不崩 |
| 8 | 靠 `plugin_info->formats->name == "MIDI"` 字符串判 bassmidi | `BassCore.cpp:139` | 插件更新改名就失效；**且只读 `formats[0]`，一个插件多格式时会漏** |
| 9 | FFmpeg 的 `SongIsOver` 兜底用函数级 `static` | `FfmpegCore.cpp:239-240` | 换歌不重置，可能提前判定结束（见 §9.1） |
| 10 | `CDrawDoubleBuffer` 在 UI 线程操作主线程的 `CDC*` | `CPlayerUIBase.cpp:49` | 依赖「主线程此刻不画」的约定，非严格线程安全 |
| 11 | `bass.dll` 版本不符只弹警告、用户点取消即退出 | `MusicPlayer2.cpp:235-242` | 无自动降级/无友好提示 |
| 12 | `Debug/`、`Release/`、`x64/` 下的 `bass.dll`/`tag.dll` 被提交进仓库 | git tree | 仓库卫生问题，也污染了「DLL 从哪来」的判断 |
| 13 | 播放位置/时长全用 `int` 毫秒 | `IPlayerCore.h:125-127` | 约 24.8 天溢出，且 2^31 ms 上限；对音乐播放够用，但别抄到长音频/有声书场景 |

---

## 10. 给「造播放器」的最终提炼

**必须抄的 6 条：**
1. `IPlayerCore` 这种「按语义单位定义接口、后端可替换、加载失败自动降级」的抽象（§1.3 列出该改的 4 点）。
2. `CDllLib`（30 行）：`LoadLibrary` + 虚 `GetFunction()` + `IsSucceed()`，让可选依赖变成可选。
3. 频谱的**分段混合刻度**（低频线性、高频对数）+ **预计算映射表**（`SpectralDataHelper` 构造函数）。
4. 混响的 **`pow(mix/100, 0.1)`** 对数补偿，以及「UI 值 → DSP 参数的显式映射表」。
5. **UI 绘制独立线程** + `Sleep(interval)` + 「实测 fps 超上限就拉长间隔、低于下限就缩短」的闭环。
6. **`GetSongLength() == 0` 时禁止 seek**、**「进度不变 + 已过 98%」兜底判定播放结束**。

**必须避开的 5 条：**
1. 别把 `FFT_SAMPLE` / 均衡器通道数这类实现常量写进接口签名。
2. 别用「函数级 `static` 变量」做播放结束计数状态。
3. 别让「帧率」参与音频相关的数学（峰值衰减公式里的 fps 依赖），用真实 delta-time。
4. 别只靠音频后端的单一结束信号（BASS 靠 `ChannelIsActive` 前后对比、FFmpeg 靠一个 bool），一定要有超时兜底。
5. 别让「字符串相等」当插件类型判据。

**关于 BASS 的最终答案：** 它在这个项目里承担的是「解码 + 输出 + FFT + 现成 DSP + 变速变调 + MIDI/SF2」这一整包。**能替的部分（解码/输出/FFT）好替；难替的是 `BASS_FX_TempoCreate`（变速不变调）和 DX8 EQ/混响这三个现成 DSP。** 而 MusicPlayer2 自己的 FFmpeg 路径恰恰证明了这一点——它能播更多格式，但变调、混响、格式转换三个功能全是空的。如果你不用 BASS，请把预算留给**时域伸缩（SoundTouch/libsamplerate+变调）**和**自研 biquad EQ + Freeverb 混响**这两块。

---

*报告完。所有【源码确证】的文件均已通过 `gh api .../contents/... -H "Accept: application/vnd.github.raw"` 取到本地 `D:\BoTapMusic\_mp2\src\` 下核对，行号对应当前 master。*
