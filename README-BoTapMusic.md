# BoTapMusic

基于 [MusicPlayer2](https://github.com/zhongyang219/MusicPlayer2) 的 Windows 桌面音乐播放器，在本地播放、歌词、频谱和均衡器之外，提供酷狗概念版与波点音乐的在线浏览和播放。

## 在线音乐

在默认界面 1、2 中选择「在线音乐」导航，或点击工具栏的地球图标；Groove Music 界面使用左侧在线音乐入口。菜单「工具 → 在线音乐搜索」也可切换到同一页面。页面使用播放器原生皮肤组件，沿用背景、主题、字体和播放控制条。

- **发现与搜索**：查看热搜，双击热搜发起搜索；输入歌名或歌手后按 Enter，双击歌曲播放。
- **推荐与榜单**：酷狗榜单、波点排行榜及分类可直接浏览；波点提供匿名推荐，酷狗个人推荐需要登录。
- **云歌单**：查看酷狗账号歌单、波点精选歌单及登录后的喜欢/创建/收藏歌单。更多菜单支持按关键词搜索波点歌单、打开歌单编号和完整导入本地。
- **本地歌单**：保存在线或本地歌曲，自动跳过重复项。播放与加入队列使用播放器现有播放引擎，可继续浏览其他页面。
- **导入与导出**：工具栏文件夹图标导入歌单，保存图标导出选中歌曲；未选择时导出当前已加载的歌曲。支持 M3U8、BoTapMusic JSON、原生 `.playlist`，导入还兼容 M3U、WPL、TTPL。
- **账号与歌词**：按所选音源扫码登录，账号页显示用户名与会员状态；在线歌词和封面自动获取并缓存。普通 LRC 整句高亮，带逐词时间的歌词才逐词填色，滚动歌词和桌面歌词均适用。
- **下载与缓存**：播放页工具栏的“下载”按钮保存当前在线歌曲，更多菜单可批量保存。下载将歌曲信息、歌词和封面内嵌到音频文件；平台未提供的内容会显示缺失状态。默认预缓存下一首，上限 1 GiB；可查看状态、开关或清理缓存。同名文件自动编号，缓存清理不删除手动下载。
- **签到领会员**：酷狗账号页的更多菜单可领取当天会员、检查到账或开启自动签到。自动签到默认关闭，仅在播放器运行期间检查；发放条件和有效期以平台结果为准。波点暂未接入签到。

列表支持 Ctrl / Shift 多选、Ctrl+A 全选和右键操作。底部箭头用于返回、刷新、加载更多。窄窗口使用页面下拉框节省空间；窗口高度不足时，请拉高窗口并收起右侧播放列表。其他皮肤可通过 `<onlineMusic/>` 元素接入。

BoTapMusic JSON 歌单可完整保存虚拟地址、歌曲信息和 CUE 时间信息。导出前写入临时文件，成功后替换目标；读取无效 JSON 时拒绝整份导入。云歌单只有完整读取成功后才写入本地。

## 构建与运行

需要 Visual Studio C++ 生成工具（MSVC v143、MFC）及 Windows SDK。

```powershell
& "你的MSBuild路径\MSBuild.exe" MusicPlayer2.sln /p:Configuration=Release /p:Platform=x86
```

编译产物为 `Release/MusicPlayer2.exe`，运行目录需要 BASS 相关库、`Plugins`、`Encoder`、`SciLexer.dll`、`tag.dll`。将 `MusicPlayer2/skins` 复制到运行目录的 `skins`，并复制 `MusicPlayer2/default_background.jpg`。已验证构建目标为 Release x86。

## 验证

```powershell
MusicPlayer2.exe --test-online            # 歌单格式与异步页面状态回归
MusicPlayer2.exe --test-online --network  # 增加公共音源接口检查
MusicPlayer2.exe --test-online --playback # 解析临时歌单第一首酷狗歌曲并静默解码
MusicPlayer2.exe --test-online --media-cache # 在临时目录验证完整下载、歌词、缓存命中及保存
MusicPlayer2.exe --render-online-preview # 使用合成歌曲导出原生皮肤预览
MusicPlayer2.exe --test-source           # 搜索与播放地址解析诊断
```

在线检查及预览命令以退出码 0 表示通过，非 0 表示失败。日志写入配置目录的 `online_test.log`、`online_playback_test.log`、`online_media_test.log`；皮肤预览在 `ui-preview`。导出预览不代替实际窗口交互验证；静默解码不验证扬声器输出。扫码确认、个人云歌单与会员播放需要对应账号验证，在线自检不会提交签到领取。

## 音源与歌单说明

在线歌曲保存为 `kugou://...` 或 `bodian://...` 虚拟地址，播放时优先使用完整缓存，否则解析有时效的音频地址。普通播放器不一定识别这些虚拟地址；导出歌单不包含音频文件。需要音频文件时使用「保存所选歌曲到文件夹」。

歌曲是否可完整播放由平台、曲目和账号权限决定。酷狗登录状态保存在配置目录的 `kugou.ini`，失效后需重新扫码；不要分享这个文件。波点登录凭据在本机加密保存；扫码未返回凭据时，可从更多菜单选择自己的 `uid` / `token` JSON 文件导入。音质按 `flac → 320 → 128` 尝试。

下一首预缓存保存在配置目录的 `online_cache`，单曲上限 256 MiB。当前歌曲播放不会触发音频缓存下载；后台提前下载实际队列中的下一首，支持顺序、列表循环、随机和无序播放。队列变化会取消旧预缓存，关闭开关不影响手动下载。歌词缓存独立于音频缓存开关。

## 代码导航

| 文件 | 职责 |
|---|---|
| `OnlineSource.*`、`OnlineJson.h` | 音源抽象、浏览数据和字段适配 |
| `OnlineHttp.h` | HTTPS 请求、超时和响应大小限制 |
| `KugouSource.*`、`KugouCrypto.*` | 酷狗接口、签名、账号和设备身份 |
| `BodianSource.*` | 波点浏览、歌单、账号、播放权限和歌词 |
| `OnlineMediaCache.*` | 下一首预缓存、歌词后台任务、手动下载及容量清理 |
| `OnlineDailyRewards.*` | 酷狗当天会员记录、领取和每日去重 |
| `OnlineMusicModel.*` | 页面状态、后台请求、歌单与账号操作 |
| `UIElement/OnlineMusic.*` | 原生皮肤导航、搜索、列表和详情 |
| `Playlist.*` | 播放列表格式及安全写入 |

结构、固定版本参考和采用边界见 [在线音乐开发说明](docs/ONLINE_MUSIC.md)。早期音源实现与兼容性记录见 [工程记录](docs/HANDOVER.md)。接口参考 KuGouMusicApi、PyBodian 和 lmplayer，保留平台权限校验。

源文件要求 UTF-8 BOM，资源文件保持原有 UTF-16 编码。修改后执行：

```powershell
pwsh -File scripts/check-bom.ps1
```

脚本默认检查有改动的文件；`-Fix` 用于为已确认采用 UTF-8 的文件补 BOM。不要批量转换上游未修改的文件。

## 许可

沿用 MusicPlayer2 的 **GPL-3.0**，见 [LICENSE](LICENSE)。使用在线服务时应遵守对应平台的条款。
