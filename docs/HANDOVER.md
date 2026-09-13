# 交接文档

当前在线浏览、原生皮肤、音频缓存、自动歌词、波点账号/歌单及酷狗每日会员领取见 [在线音乐开发说明](ONLINE_MUSIC.md)。下文保留早期音源接入记录；其中独立搜索/登录窗口、歌词未启用和波点仅支持匿名接口的描述已由当前实现替代。

这份文档说明本分支相对上游 MusicPlayer2 做了哪些改动，供接手的人（或 Agent）快速上手。

**基点**：上游最后提交 `328af4cc`。本分支在其之上新增 16 次提交。

```bash
git log --oneline 328af4cc..HEAD      # 看全部改动提交
git diff --stat 328af4cc..HEAD        # 看改动规模
```

---

## 一、整体思路

目标是给一个本地音乐播放器加上**在线音源**（酷狗概念版、波点音乐），让它能搜在线歌、直接试听。

核心设计决策是**虚拟路径**：

在线歌曲在 `SongInfo::file_path` 里存的**不是**真实网络地址，而是一个虚拟地址：

```
kugou://B3A52A7A958BF0AED0EBFBA2E9A818B7?aaid=32100650
bodian://78932517
```

真正播放时才由音源层换成 http 地址。这样做的好处是：

- 播放列表、曲库、收藏、最近播放这些**现有功能全都不用改** —— 它们只需要一个字符串作标识
- 播放地址是有时效的，每次播放现取，不存在列表里就不用担心过期
- 地址只在真正要播的那一刻解析，没登录/没权限时能给出准确原因

代价是需要在几个关键位置放行这类路径（见第三节）。

---

## 二、新增的文件

### 音源框架

| 文件 | 职责 |
|---|---|
| `OnlineSource.h` / `.cpp` | 音源抽象层。`online::IOnlineSource` 接口 + `online::CSourceRegistry` 注册表 |

`IOnlineSource` 的接口：

```cpp
std::wstring GetScheme();                                  // "kugou" / "bodian"
std::wstring GetDisplayName();                             // 显示名
bool Search(keyword, page, std::vector<Track>& result);     // 搜索
std::wstring ResolvePlayUrl(const std::wstring& virtual_path);  // 虚拟路径 -> 真实地址
bool GetLyric(const std::wstring& virtual_path, Lyric& result); // 歌词（未启用）
std::wstring GetLastError();                                // 失败原因，直接给用户看
```

`CSourceRegistry` 是关键，其他代码靠它判断一个路径是不是在线曲目：

```cpp
online::CSourceRegistry::Instance().IsVirtualPath(path)     // 是不是 kugou:// 这类
online::CSourceRegistry::Instance().ResolvePlayUrl(path)    // 换成真实地址
online::CSourceRegistry::Instance().FindByScheme(L"kugou")  // 按 scheme 找音源
```

### 两个音源

| 文件 | 职责 |
|---|---|
| `KugouSource.h` / `.cpp` | 酷狗概念版：搜索、取播放地址、扫码登录、设备注册 |
| `KugouCrypto.h` / `.cpp` | 酷狗的签名和加密（MD5 签名、AES、RSA、Base64） |
| `KugouLoginDlg.h` / `.cpp` | 扫码登录窗口（显示二维码、轮询扫码状态） |
| `BodianSource.h` / `.cpp` | 波点音乐：搜索、取播放地址 |

### 界面

| 文件 | 职责 |
|---|---|
| `OnlineMusicDlg.h` / `.cpp` | 在线音乐搜索对话框（选音源、搜关键词、双击播放） |

### 第三方

| 文件 | 说明 |
|---|---|
| `qrcodegen/qrcodegen.{hpp,cpp}` | 二维码生成库，nayuki/QR-Code-generator，MIT，未做任何修改（本项目里给它单独关了预编译头） |

### 工具脚本

| 文件 | 用途 |
|---|---|
| `scripts/check-bom.ps1` | 检查/修复源文件编码（**改代码前务必先看第六节**） |
| `scripts/kg_register_dev.js` | 设备注册的独立验证脚本，零依赖，`node scripts/kg_register_dev.js` |

### 文档

`docs/research/` 下是前期调研资料（接口调研、同类实现分析、MusicPlayer2 源码梳理），仅供参考，不影响构建。

---

## 三、对上游原有文件的改动

**这一节是接手时最需要看的**，因为这些是"侵入"上游代码的地方，将来合并上游更新时容易冲突。

### `Player.cpp` — 播放链路（关键）

改动 1：`MusicControl()` 开头的文件存在性检查，放行虚拟路径。

```cpp
// 原：
if (!CCommon::IsURL(GetCurrentFilePath()) && !CCommon::FileExist(GetCurrentFilePath()))

// 改：
const wstring& cur_path = GetCurrentFilePath();
bool is_online = online::CSourceRegistry::IsVirtualPath(cur_path);
if (!CCommon::IsURL(cur_path) && !is_online && !CCommon::FileExist(cur_path))
```

不改这里的话，在线曲目会被当成"文件不存在"直接拦掉。

改动 2：`OPEN` 分支里把虚拟路径换成真实地址。

```cpp
wstring play_path = online::CSourceRegistry::Instance().ResolvePlayUrl(cur_song.file_path);
if (play_path.empty())
{
    // 解析失败（没版权/要会员/接口变了）。
    // 千万不要拿空路径去调播放核心 —— 见下面 BassCore 那条，会崩。
    m_error_state = ES_FILE_CANNOT_BE_OPEN;
    m_file_opend = true;
    PostMessage(theApp.m_pMainWnd->m_hWnd, WM_MUSIC_STREAM_OPENED, 0, 0);
    m_controls.UpdateControls(PlaybackStatus::Closed);
    return;
}
m_pCore->Open(play_path.c_str());
```

### `Playlist.cpp` — 播放列表读写（关键，共 4 处）

播放列表文件（`.playlist` / `.m3u` / `.wpl` / `.ttpl`）的解析里，各有两处需要放行虚拟路径。

**坑 1**：不能让虚拟路径走"相对路径转绝对路径"：

```cpp
bool is_url = CCommon::IsURL(item.file_path);
bool is_online = online::CSourceRegistry::IsVirtualPath(item.file_path);
if (!is_url && !is_online)      // 在线曲目不做这个转换
    item.file_path = CCommon::RelativePathToAbsolutePath(...);
```

不改的话，`bodian://78932517` 会被拼成 `C:\...\playlist\bodian://78932517`。

**坑 2**：解析末尾的合法性检查会把虚拟路径丢掉：

```cpp
// 原：
if (is_url || CCommon::IsPath(item.file_path))
// 改：
if (is_url || CCommon::IsPath(item.file_path) || online::CSourceRegistry::IsVirtualPath(item.file_path))
```

不改的话，保存进播放列表的在线歌曲**读出来时会被静默丢弃**，列表变空、双击毫无反应。这个症状很难查，因为保存看起来是成功的。

### `BassCore.cpp` — 加了一个句柄判空

```cpp
m_musicStream = BASS_StreamCreateURL(...) 或 BASS_StreamCreateFile(...);

// 新增：创建失败时句柄为 0，下面的查询接口不接受无效句柄，会直接崩
if (m_musicStream == 0)
{
    m_channel_info = {};
    m_bitrate = 0;
    m_is_midi = false;
    return;
}
```

这是**上游本来就存在的问题**（本地文件打不开时同样会崩），在线播放把它暴露得更频繁。

### `MusicPlayerDlg.h` / `.cpp` — 菜单入口与播放触发

新增成员：

```cpp
SongInfo m_online_song;
bool m_has_online_song{ false };
afx_msg LRESULT OnPlayOnlineSong(WPARAM wParam, LPARAM lParam);   // 消息映射 WM_PLAY_ONLINE_SONG
```

`OnCommand` 里新增 `case ID_ONLINE_MUSIC`：先（未登录时）弹登录窗口，再开搜索对话框。双击结果后**不直接播放**，而是记下曲目 + `PostMessage`：

```cpp
COnlineMusicDlg dlg;
if (dlg.DoModal() == IDOK && dlg.HasSelection())
{
    m_online_song = dlg.GetSelectedSong();
    m_has_online_song = true;
    PostMessage(WM_PLAY_ONLINE_SONG, 0, 0);   // 等对话框彻底销毁后再播
}
```

**为什么绕这一圈**：在模态对话框还没完全退出时去改主窗口的播放列表，会让通用控件状态错乱，实测崩在 `comctl32.dll`。投递消息能把播放推迟到对话框销毁之后。

### `Define.h`

```cpp
#define WM_PLAY_ONLINE_SONG (WM_USER+144)
```

### `MenuMgr.cpp`

`MainToolMenu` 里加了一行菜单项：

```cpp
menu.AppendItem(EX_ID(ID_ONLINE_MUSIC), IconMgr::IconType::IT_Online);
```

### `MusicPlayer2.cpp` — 启动初始化和自检入口

`InitInstance()` 里在配置目录确定之后：

```cpp
online::InitOnlineSources();                        // 注册音源
static_cast<kugou::CKugouSource*>(kugou_source)->LoadIdentity(m_config_dir);  // 载入设备身份和账号
```

**必须在配置目录确定之后**，因为设备身份和账号要存到那里。

另外加了一个命令行自检开关 `--test-source`（详见第五节）。

### 资源文件

- `resource.h`：新增 `IDD_KUGOU_LOGIN_DIALOG`、`IDC_KUGOU_*`、`IDD_ONLINE_MUSIC_DIALOG`、`IDC_ONLINE_*`、`ID_ONLINE_MUSIC`
- `MusicPlayer2.rc`：新增两个对话框模板（登录窗口、搜索窗口），**文件是 UTF-16LE 编码**
- `language/*.ini`（4 个语言）：新增菜单和对话框用的文本
- `MusicPlayer2.vcxproj`：新增上述源文件；给 `qrcodegen.cpp` 单独设了 `<PrecompiledHeader>NotUsing</PrecompiledHeader>`（第三方库不该被迫加 stdafx.h）；链接库加了 `crypt32.lib`

---

## 四、两个音源的接口要点

### 酷狗概念版

**身份常量**（和标准版完全不通用，token 也不通用）：

| 项 | 值 |
|---|---|
| appid | `3116` |
| clientver | `11440` |
| Android 签名 salt | `LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` |
| Web 签名 salt（登录用） | `NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt` |
| signKey salt | `185672dd44712f60bb1736df5a377e82` |

**签名算法**：`MD5(salt + 排序后的 k=v 拼接 + body + salt)`。Android 版和 Web 版只差 salt，共用一个实现（`SignatureWithSalt`）。

**设备身份**：`guid` 随机生成一次后固定 → `CalculateMid(guid)` 得到 `mid` → 向 `/risk/v2/r_register_dev` 注册换 `dfid`。都持久化在 `%APPDATA%\MusicPlayer2\kugou.ini`。**不要每次换**，否则会被当成新设备、更容易触发风控。

**搜索接口**返回的字段是 **PascalCase**（`FileHash`、`MixSongID`、`OriSongName`、`SingerName`），不是小写。

**未登录时 `userid=0` 也必须带上**，否则报 `152 Parameter Error`。

**登录接口**（`login-user.kugou.com`）有个容易踩的坑：除了平台自己的参数，**必须带上客户端公共参数**（`dfid`/`mid`/`uuid`/`appid`/`clientver`/`clienttime`/`userid`），少一个就报 `20010` 签名校验失败。`clienttime` 尤其不能省（否则 `20006`）。

**设备注册**最隐蔽的一个坑（我在这上面卡了三轮）：

> 概念版公钥的模数最高位是 1，X.509 里 DER 会给正数补一个 `0x00`，所以 INTEGER 长度是 **129** 不是 128。直接把整块当模数用，CNG 会照单全收、加密也不报错，但产出的是**废密文**，服务端只回 `rsa failure`。

现在的做法是在 `KugouCrypto.cpp` 里**硬编码** `BCRYPT_RSAPUBLIC_BLOB` 字节，不再运行时解析 X.509。注释里留了校验值：

```
模数 SHA1 = f0c8cd9790d89910b4695e63e1959b5992940a6f
模数前 16 字节 = c40a2d0da76511f3bb1cc2bbd3afbd8b
```

另外记两点，都是我当时判断错的方向：
- CNG 输出的**就是标准大端序**（和 OpenSSL 一致），不要反转字节序
- RSA 加密的块类型是 `00 02`（PS 随机非零）；`00 01 | FF...FF` 是签名用的

### 波点音乐

- 域名 `bd-api.kuwo.cn`，**没有签名**，但请求头要求严格（`plat`/`channel`/`brand`/`devid`/`ver`/`appuid`/`api-ver`/`net`/`user-agent` 都得带对）
- 用 WinHTTP 发请求（项目原本的 `CInternetCommon::HttpGet` 不好控制请求头）
- 搜索返回 `data.resultList`，歌曲号字段是 **`musicRid`**，值是 `"MUSIC_228908"` 这种，用之前要**去掉 `MUSIC_` 前缀**
- 付费曲返回 `20018`，下架/地区限制返回 `20012`，两种情况给了不同的提示文案
- 播放地址优先用 `audioHttpsUrl`

---

## 五、构建与调试

### 环境

- Visual Studio Build Tools 2022（MSVC v143）+ Windows SDK，需要 **MFC** 组件
- 本机装在 `D:\VSBuildTools`

```powershell
& 'D:\VSBuildTools\MSBuild\Current\Bin\MSBuild.exe' MusicPlayer2.sln `
    /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
```

**目前只在 x86 下编译通过。** x64 会报 `LNK1104: 无法打开文件 mfc140u.lib` —— 因为 `lib/x64` 里只有 ANSI 版 MFC，缺 Unicode 版。要上 x64 得先补装对应组件。这是已知技术债。

### 运行

产物在 `Release\MusicPlayer2.exe`。运行前需要有：

- `bass.dll`、`bass_fx.dll`
- `Plugins\*.dll`、`Encoder\*.dll`
- `skins\`、`language\`
- `SciLexer.dll`、`tag.dll`

配置和日志都在 `%APPDATA%\MusicPlayer2\`。

### 自检开关

```powershell
MusicPlayer2.exe --test-source
```

会依次调用每个音源做搜索和取播放地址，结果写到 `%APPDATA%\MusicPlayer2\source_test.log`（UTF-8）。**改完音源相关代码，用这个验证最快**，不用点界面。

它同时会验证酷狗的设备注册和登录二维码接口。

### 独立验证脚本

设备注册出问题时，可以先用 `node scripts/kg_register_dev.js` 判断是接口变了还是 C++ 写错了 —— 两者是同一套算法。

---

## 六、容易踩的坑

### 1. 源文件必须带 UTF-8 BOM（最高频）

丢了 BOM，MSVC 会按 GBK(936) 去解 UTF-8 的中文注释，然后报出一堆莫名其妙的语法错误（`C3872`、`C3688`、`C2001` 之类），**不会**提示你编码问题。

改完代码先跑：

```powershell
pwsh -File scripts/check-bom.ps1        # 检查改动过的文件
pwsh -File scripts/check-bom.ps1 -Fix   # 自动补 BOM
```

注意：仓库里有 **38 个上游文件本来就没有 BOM**（纯 ASCII 或 GBK 编码），它们能正常编译，**不要动**。所以脚本默认只检查相对 git 有改动的文件。

**不要用 `Get-Content` + `Set-Content` 改这些文件** —— 我这么干过，把 `MusicPlayer2.cpp` 的首字节写成了 `0x0A`、中文注释全坏，只能从 git 恢复。用 `[System.IO.File]::ReadAllText` / `WriteAllText`，或者用编辑器工具。

### 2. 行尾

`core.autocrlf=true`，仓库里大部分文件是 LF（有些是 CRLF）。如果整体改动了行尾，`git diff` 会显示几千行无关变化。提交前看一眼 `git diff --stat`，规模不对劲就是行尾问题。

### 3. `MusicPlayer2.rc` 是 UTF-16LE

用 UTF-8 工具写它会坏。读的时候指定 `[System.Text.Encoding]::Unicode`。

### 4. 不要拿空路径调播放核心

`BassCore.cpp` 里虽然加了判空，但在 `OPEN` 分支解析失败时**仍然不要**去调 `m_pCore->Open(L"")`。原来的崩法是：句柄为 0 → `BASS_ChannelGetInfo(0, ...)` → 访问违例。

### 5. 模态对话框里不要动播放列表

见第三节 `MusicPlayerDlg.cpp` 的说明。会崩在 `comctl32.dll`，而且崩点固定、看起来像控件问题，其实是用错时机。

### 6. 项目里唯一的 `EndDialog()`

搜索对话框里那个 `EndDialog()` 是新增的，上游其他对话框都靠 MFC 默认行为。上游的模式是"对话框只负责收集/浏览，活由主窗口干"，改动时尽量跟着这个风格。

---

## 七、验证状态

### 已实测通过

| 项目 | 怎么验证的 |
|---|---|
| 编译 | 0 错误，产物 9MB |
| 程序启动 | 内存约 49MB，标题正常 |
| 酷狗搜索 | 搜「晴天」返回 30 首 |
| 酷狗登录二维码 | 能取到；二维码内容做了独立解码验证，确认可扫 |
| 酷狗设备注册 | 拿到真实 `dfid`；连续测 8 次全部成功 |
| 波点搜索 | 搜「晴天」返回 30 首 |
| 波点播放 | 搜到 → 双击 → 取地址 → **实际出声**（2743KB 有效 MP3） |
| 界面 | 两个对话框都能正常打开、控件完备 |
| 编码规范 | `check-bom.ps1` 通过 |

### 未验证

**酷狗歌曲的实际播放。** 这需要登录一个真实的酷狗账号，我没有。链路上每一环（搜索 → 扫码登录 → 设备注册 → 取地址）都单独验证过，但"登录后能不能拿到完整播放地址并出声"这一步没跑通过。

未登录时酷狗只给 60 秒试听，接口返回 `priv_status=0`、`fail_process` 含 `pkg`/`buy`，这是权限限制不是签名问题。

### 已知限制

- 登录 token 没有做自动续期，失效要重新扫码
- 音质按 `flac → 320 → 128` 依次降级尝试，取到哪个用哪个
- 播放地址不缓存（有时效），每次播放现取
- 酷狗那边有说法：注册拿到的 `dfid` 偏"网页版"，带上它调其它接口可能过一段时间掉登录。所以程序只在取播放地址时用它，没有写进长期凭据
- x64 缺 Unicode MFC，暂不可用

---

## 八、如果继续做

按优先级：

1. **验证酷狗登录后的播放** —— 用自己的账号扫码试一次，确认能否拿到完整地址。这是唯一还悬着的一环。
2. **歌词** —— `IOnlineSource::GetLyric` 接口留好了（酷狗歌词接口的实现也在 `KugouSource.cpp` 里），但没有接进播放器的歌词显示。
3. **账号界面** —— 目前没有"退出登录"的入口，登录状态只能在 `kugou.ini` 里看。
4. **x64 支持** —— 补装 x64 Unicode MFC 组件即可。
5. **音源扩展** —— 加新音源只需实现 `IOnlineSource` 并在 `InitOnlineSources()` 里注册，其他代码不用改。

### 改动时注意保持的风格

- 注释写"为什么"而不是"是什么"
- 面向用户报错要说清原因（是没登录、要会员、还是接口挂了），别笼统说"失败"
- 跟随项目原有写法（MFC、`CString`/`wstring` 混用处照旧），别夹带个人习惯
