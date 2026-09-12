# BoTapMusic

基于 [MusicPlayer2](https://github.com/zhongyang219/MusicPlayer2) 改的桌面音乐播放器，在原有本地播放能力之上加了**在线音源**：可以在软件里直接搜索并试听酷狗概念版和波点音乐上的歌曲。

本地播放那部分——歌词、频谱、均衡器、格式支持、界面布局——都是原项目的能力，没有改动。

## 加了什么

**在线音乐搜索**：菜单「工具 → 在线音乐搜索」

- 左上角选音源（酷狗概念版 / 波点音乐），输入关键词搜索
- 双击搜索结果即可加入播放列表播放
- 搜索在后台线程执行，网络慢不会卡界面

**登录酷狗**：打开「在线音乐搜索」时，如果还没登录会先弹出登录窗显示二维码，用酷狗App扫一下确认即可。登录后账号信息保存在配置目录，下次不用再扫。二维码过期可以点「刷新二维码」。

**播放失败时会说明原因**，比如「这首歌需要购买或开通会员才能完整播放」，而不是笼统地报一个播放失败。

## 怎么构建

需要 Visual Studio 的 C++ 生成工具（MSVC v143）和 Windows SDK。

```powershell
# 在仓库根目录
& "你的MSBuild路径\MSBuild.exe" MusicPlayer2.sln /p:Configuration=Release /p:Platform=x86
```

编译产物在 `Release\MusicPlayer2.exe`。运行前需要把 BASS 相关库放到一起：

- `bass.dll`、`bass_fx.dll`（在 `Debug/` 里）
- `Plugins\*.dll`（从 `MusicPlayer2\Plugins\` 复制）
- `Encoder\*.dll`（从 `MusicPlayer2\Encoder\` 复制）
- `SciLexer.dll`、`tag.dll`

目前只在 **x86（32 位）** 下编译通过。x64 需要补装 x64 版的 Unicode MFC 库，本机环境里没有，所以没试过。

## 已知限制

这些限制是接口本身的，不是程序缺陷：

- **酷狗上的歌需要登录后才能完整播放。** 未登录时接口只给 60 秒试听片段，并返回「需要购买」。打开「在线音乐搜索」时会先弹登录窗，用酷狗App扫码即可。
- **波点音乐的付费曲需要会员。** 免费曲可以直接播，付费曲会返回「需要波点会员」。实测搜索「晴天」的前几首里，第二首（免费曲）能正常播放。
- **在线歌曲的播放地址有时效**，所以程序每次都重新取，不缓存地址。
- 音质目前按 `flac → 320 → 128` 依次降级尝试，取到哪个用哪个。
- 登录状态存在 `%APPDATA%\MusicPlayer2\kugou.ini`，目前没有做 token 自动续期，失效了需要重新扫码。

## 目录说明

```
MusicPlayer2/BodianSource.*      波点音乐音源
MusicPlayer2/KugouCrypto.*       酷狗概念版的签名与设备身份
MusicPlayer2/KugouSource.*       酷狗概念版音源（含扫码登录）
MusicPlayer2/KugouLoginDlg.*     酷狗扫码登录窗口
MusicPlayer2/OnlineSource.*      音源抽象层与注册表
MusicPlayer2/OnlineMusicDlg.*    在线音乐搜索对话框
MusicPlayer2/qrcodegen/          二维码生成库（MIT，第三方）
docs/research/                   调研资料（接口、实现经验等）
```

在线歌曲在播放列表里存的是一个虚拟地址（形如 `kugou://<hash>`、`bodian://<歌曲号>`），播放时由音源层换成真实的网络地址。这样曲库、歌单、收藏这些现成功能都不用改。

## 调试用命令行开关

```powershell
# 检查音源是否可用：搜索一首歌并尝试取播放地址，结果写到 source_test.log
MusicPlayer2.exe --test-source
```

日志在 `%APPDATA%\MusicPlayer2\` 下。

## 使用须知

- 这个项目是基于公开接口做的第三方客户端，**仅供个人学习和技术研究**，请勿用于商业用途。
- 程序不存储、不传播任何音频文件，所有内容都来自对应平台的接口。
- 使用第三方客户端可能违反平台的服务条款，风险自负。想稳定听歌还是用官方客户端。
- 在线音源部分参考了 [MakcRe/KuGouMusicApi](https://github.com/MakcRe/KuGouMusicApi)（MIT）和 [lianchengwu/lmplayer](https://github.com/lianchengwu/lmplayer)（MIT）的公开实现，相关调研记录在 `docs/research/` 下。

## 许可

沿用上游 MusicPlayer2 的 **GPL-3.0**。见 [LICENSE](LICENSE)。
