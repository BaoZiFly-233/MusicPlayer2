# 用 Rust 做酷狗（含概念版）播放器：生态调研

调研日期 2026-09。全部结论基于**实际读到的源码 / 实际编译运行 / gh CLI 一手数据**。
凡是我没能验证的一律标注「未验证」或「未找到」，不做推测填补。

---

## 0. 一句话结论

**不存在**成熟到可以「直接 `cargo add` 就拿来当技术基础」的 Rust 酷狗实现。
但**存在 3 个高质量的 Rust 参考实现**，其中 `lmplayer` 的 `kugou` crate（MIT）与 `md3Music` 的 Rust 服务器（AGPL）在协议覆盖上已经等同于 Node 版 `KuGouMusicApi`。

**并且我在本机实测证明了**：crates.io 上唯一的 `kugou_sdk` 能编译、签名能与官方服务器互通 —— 但它有一个真实的接口缺陷，且 API 极不稳定。

最务实的路线不是「依赖现成 crate」，而是 **参考 `lmplayer/kugou` 自己重写签名层（约 300 行核心代码）**，理由见第 6 节。

---

## 1. 候选项目普查

### 1.1 GitHub 全站规模（硬数据）

| 检索方式 | 结果 |
|---|---|
| `search/repositories?q=kugou+language:rust` | **total_count = 10** |
| `search/repositories?q=酷狗`（全语言，Rust 过滤后） | 671 个仓库里 Rust **只有 1 个** |
| `search/code` 搜概念版 salt `LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` | 命中 100+ 仓库，其中 **Rust 仅 11 个** |

**这是本调研最重要的方法论发现**：GitHub 的 `language:rust` 仓库检索**严重漏检** —— 真正的 Rust 实现大多藏在不叫 kugou 的音乐播放器项目里（meliora、seraphine-music、Mio-Music…）。只按仓库名/描述搜会得出「几乎不存在」的错误结论。

**定位手段**：用 salt 字符串做 code search 是最有效的。我另外用 KRC 16 字节密钥数组 `64, 71, 97, 119, 94, 50, 116, 71` 反查，全语言 124 命中、Rust 10 命中，两个指纹交叉验证一致。

### 1.2 完整候选表

| 仓库 | ★ | 语言构成 | 最后提交 | License | 实现了什么 |
|---|---|---|---|---|---|
| **[lianchengwu/lmplayer](https://github.com/lianchengwu/lmplayer)** | 100 | JS 656KB / CSS 270KB / **Rust 181KB** / HTML 65KB | 2026-09-12 | **MIT** | **独立 `kugou` crate（0.1.0）**：签名(4盐)、设备指纹、session/cookie、QR/密码/短信/令牌登录、搜索、`/v5/url`+`/v6/priv_url` 播放地址、歌词+KRC 解码、歌单、榜单、FM。UI 是 Wails（GTK4/WebKitGTK）→ **仅 Linux 可构建完整播放器** |
| **[zzyoxml/md3Music](https://github.com/zzyoxml/md3Music)** | 317 | Dart 主 + **Rust 345KB** + CMake/C++ | 2026-09-05 | **AGPL-3.0** ⚠️ | **协议覆盖最全**：`kugou_api_server/rust` 是 Node 版 1:1 移植，**45 个模块 / 100+ 路由**，含 `/register/dev`、`/song/url`、`/song/url/new`、`privilege_lite`(`/v2/get_res_privilege/lite`)、登录全套、KRC。编译为 `libkugou_server.so`，经 JNI/FFI 给 Flutter 调 |
| **[bamboostrip/KugouMusic.rs](https://github.com/bamboostrip/KugouMusic.rs)** | 3 | Rust 345KB | 2026-09-02 | **无 LICENSE** ⚠️⚠️ | Axum Web API 后端，`src/kugou/signer.rs` 有 4 种签名策略 + 二进制 body 版，`/register/dev`、`/song/url`、KRC、登录。**代码质量最高、注释最详细（明确写了迁移易错点）** |
| **[burenLee/seraphine-music](https://github.com/burenLee/seraphine-music)** | 9 | TS 主 + Rust | 2026-08-14 | **MIT** | **Tauri 2**，Rust 端 `src-tauri/src/api/*` 含 `/risk/v2/r_register_dev`、`/v5/url`、`/v2/get_res_privilege/lite`、KRC、扫码登录。**默认就是概念版**（`Mode::KgLite` 为 Default），Standard 也可切。README 自述「酷狗接口从 KuGouMusicApi 移植到 rust，后端直发请求，无额外服务」 |
| **[li-ming1/meliora](https://github.com/li-ming1/meliora)** | 3 | **纯 Rust 2.8MB** | 2026-09-06 | **Apache-2.0** | **GPUI 纯原生渲染播放器**（无 WebView）+ cpal + symphonia + rubato + lofty。`src/kugou/` 含 sign/crypto/client/api，QR 登录、VIP 权益、搜索、歌单增删、榜单、每日推荐、`/v5/url`、LRC/KRC/YRC。有 3 个 release（Windows x64 20.3MB） |
| **[smiling11123/polomusic-tauri](https://github.com/smiling11123/polomusic-tauri)** | 1 | Rust | 2026-08-15 | **MIT** | `kg-rs` 完整移植 `helper.js`+`crypto.js`：7 种 MD5 签名、AES-128/256-CBC、RSA-2048（裸模幂/PKCS1/OAEP）、**KRC XOR+inflate**、`/register/dev`、`privilege_lite` |
| **[DreamAlone666/kg2lx](https://github.com/DreamAlone666/kg2lx)** | 8 | Rust 98KB + Svelte | 2026-08-31 | **MIT** | ⚠️ **更正**：它本身**不做签名**。Rust/Axum 只做账号持久化 + 代理，`KUGOU_API_BASE_URL` 指向 **Node 版 KuGouMusicApi 容器**（docker-compose 里两个服务）。只是「Rust 外层的概念版音源网关」 |
| **[MOPELotus/TuneWeave](https://github.com/MOPELotus/TuneWeave)** | 0 | Rust | 2026-09-08 | NOASSERTION | 多平台统一 API，`crates/tuneweave-provider-kugou` 有 device/client/provider。⚠️ **只用标准版 salt `OIlwieks…`，不是概念版** |
| **[emoeem/voicefox](https://github.com/emoeem/voicefox)** | 31 | Rust | 2026-09-12 | MIT | TUI 播放器，独立 `source` crate 有 kg 模块（search/url/lyric/playlist/crypto）。⚠️ **无签名 salt**，走 `/v3/album_audio/audio` 免签路径；只做 KRC 解密 |
| [xcqm12/lx-music-rust](https://github.com/xcqm12/lx-music-rust) | 2 | TS 1.6MB + Rust 361KB | 2026-07-02 | Apache-2.0 | LX Music 移动版 Rust 重写。⚠️ `sources/kg.rs` 用的是**老的免签 Web 接口**（`songsearch.kugou.com` + `get_res_privilege`），不是概念版协议 |
| [JYHjyh001/desktop_pet](https://github.com/JYHjyh001/desktop_pet) | 3 | Vue + Rust | 2026-07-07 | **无 LICENSE** ⚠️ | 单文件 `kugou_music.rs` 里塞了完整协议：QR 登录、`/register/dev`、RSA 裸模幂、AES、KRC、播放代理。功能很全但**无许可、结构差** |
| [ghtz08/kugou-kgm-decoder](https://github.com/ghtz08/kugou-kgm-decoder) | 454 | Rust | 2026-03-01 | NOASSERTION | 仅 KGM/KGMA 加密文件转 MP3。**无网络协议** |
| [CGQAQ/krc-rs](https://github.com/CGQAQ/krc-rs) | 3 | Rust | **2020-12-07** | GPL-3.0 | 仅 KRC 解码。5 年未更新 |
| [ChouChiu/Lyrics-Helper](https://github.com/ChouChiu/Lyrics-Helper) | 4 | Rust | 2026-07-07 | Apache-2.0 | 歌词工具库，KRC/QRC 解密 |
| [Aaron-gx/KGG-Decryptor](https://github.com/Aaron-gx/KGG-Decryptor) | 9 | Rust (Tauri) | 2026-08-30 | MIT | KGG 文件解密转码 |

---

## 2. 关键问题：有没有把酷狗签名算法用 Rust 完整实现的？

### 2.1 答案：**有，至少 8 个独立实现，其中 3 个达到生产级完整度**

概念版 salt `LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` 出现在下列 **Rust** 源码里（全部经我逐个读取核实，非搜索摘要）：

| 仓库 | 文件 | 读到的常量 |
|---|---|---|
| lmplayer | `kugou/src/proto/sign.rs` | `ANDROID_LITE_SALT`、`REGISTER_SALT="1014"`、`SIGN_KEY_LITE_SALT="185672…"`、`WEB_SALT`、`CLOUD_SALT` |
| md3Music | `kugou_api_server/rust/src/helper.rs` + `config.rs` | `ROUTE`、`APP_ID="3116"`、`CLIENT_VER="11440"`、`SIGN_PARAMS_KEY_STR` |
| KugouMusic.rs | `src/kugou/signer.rs` + `config.rs` | `LITE_SALT`、`V5_KEY_SALT`、`WEB_SIGNATURE_SALT`、RSA 双公钥、`KG_RF`/`KG_THASH` 反风控头 |
| seraphine-music | `src-tauri/src/http/config.rs` | `params_android_padding`（Lite/Standard 双套 9 个 salt） |
| polomusic-tauri | `kg-rs/src/crypto.rs` | 7 种 MD5 签名 + AES + RSA + KRC |
| meliora | `src/kugou/sign.rs` | `ANDROID_SALT`/`WEB_SALT`/`KEY_SALT` |
| desktop_pet | `src-tauri/src/kugou_music.rs` | 单文件内联全部常量 |
| Miliastra-Wonderland-Music | `crates/miliastra-playback/src/catalog/kugou.rs` | MIT，签名+KRC |
| kugou_sdk (crate) | `src/config.rs` | `PlatformProfile::{Standard,Lite}` 双套 salt |

签名算法本身极简，所有实现都收敛到同一式子（我逐位验证过，见 2.3）：

```
signature = md5(salt + 按 key 字节序排序的 "k1=v1k2=v2" 无分隔拼接 + body + salt)
```

### 2.2 你点名的 4 个接口在 Rust 里的实现情况

| 接口 | Rust 实现 | 证据 |
|---|---|---|
| **`/register/dev` 设备注册** | ✅ **有**（md3Music `register_dev.rs` 完整含 40+ 字段设备指纹 + AES；seraphine `api_register_dev` 打到 `/risk/v2/r_register_dev`；kugou_sdk `auth::register_dev`；lmplayer `device.rs`） | 我**实测跑通**，见 2.3 |
| **`/song/url` 播放地址** | ✅ **有**（md3Music `song_url.rs` + `song_url_new.rs` 双实现；seraphine `/v5/url`；lmplayer `/v5/url`+`/v6/priv_url`；meliora `song_url`） | 实测 `/v5/url` 返回合法业务 JSON |
| **`/v2/get_res_privilege/lite`** | ✅ **有**（md3Music `misc.rs::handle_privilege_lite`；seraphine `api/privilege.rs`；polomusic `api/privilege_lite.rs`） | 源码逐行读到 |
| **KRC 歌词 XOR+inflate** | ✅ **有**（lmplayer `proto/lyrics.rs`；meliora；seraphine`utils/tools.rs`；kugou_sdk `lyric/decode.rs`；polomusic `crypto.rs`） | 我**实测解密成功**，见 2.3 |

**KRC 解码算法**（所有实现一致）：base64 → 去掉前 4 字节头 → 16 字节 XOR 密钥逐字节异或 → zlib inflate。

### 2.3 我在本机做的实测验证（最硬的证据）

环境：Windows，cargo 1.94.0，x86_64-pc-windows-gnu（本机无 MSVC linker，用 msys64 mingw64）。

**测试 1 — 签名算法对拍。** 我用 Node.js 独立实现了一遍 `KuGouMusicApi` 的签名算法，与 `kugou_sdk` 的输出逐位对比：

```
                         Rust kugou_sdk                          Node 独立实现
lite_android_sig   ebde10f96646f1af96ab2fb6e48e1730   =   ebde10f96646f1af96ab2fb6e48e1730  ✅
std_android_sig    ed6859d742ae7eb9c51675c53ead1c9a   =   ed6859d742ae7eb9c51675c53ead1c9a  ✅
sign_params_key    bad0ee207bf429bd91402b77fc8f7a5b   =   bad0ee207bf429bd91402b77fc8f7a5b  ✅
sign_key(lite)     4318d8dc5fafc1a88a413fbbb8fdc06c   =   4318d8dc5fafc1a88a413fbbb8fdc06c  ✅
```

**测试 2 — KRC 解密**：输入官方样例密文，解出 `[00:00.00]hello lyrics` ✅

**测试 3 — 真实联网（这是决定性的）**：

```
=== A. 设备注册 /risk/v2/r_register_dev ===
OK  upstream_status=Some(1)
    dfid = 3497uT4Ifj3K4ZlU0V0xHqCl     ← 酷狗服务器真实下发了 dfid

=== B. 搜索 "海阔天空" (Lite 身份, appid 3116/clientver 11440) ===
upstream_status = Some(1)
  "FileHash": "C41E80A18D1448FA47086372999C7F43",
  "FileName": "BEYOND - <em>海阔天空</em>",
  "AlbumName": "乐与怒", "Bitrate": 128, "Duration": 324

=== C. 播放地址 GET /v5/url ===
upstream_status = Some(2)          ← 签名通过（返回了合法业务 JSON，不是签名错误）
{"fail_process":["pkg","buy"], "priv_status":0,
 "tracker_through":{"all_quality_free":0, "identity_block":0, ...},
 "hash_offset":{"end_byte":960125, "end_ms":60000, ...}}
```

**结论**：签名算法在真实酷狗服务器上**验证通过**。
播放地址返回 `status=2` + `fail_process:["pkg","buy"]` 是**匿名未登录的权限限制，不是签名失败** —— 拿到真实播放地址需要登录概念版 VIP 账号（本次调研无凭据，未验证登录后的播放链路）。

---

## 3. 有没有可直接复用的 Rust crate？

用 crates.io 官方 JSON API 实抓。

### 3.1 酷狗：有 1 个，但极不成熟

| crate | 版本 | 最后更新 | 总下载 | 反向依赖 | 评价 |
|---|---|---|---|---|---|
| **`kugou_sdk`** | 0.2.9 | 2026-07-18 | **249** | **0** | 唯一完整实现，见下 |
| `unm_engine_kugou` | 0.4.0 | **2023-01-22** | 8,762 | — | UnblockNeteaseMusic 换源引擎，3 年 8 个月未更新 |
| `termusic` | 0.13.2 | 2026-05-06 | 99,892 | — | 活跃，但是终端播放器**应用**，不是可复用库 |

**`kugou_sdk` 详细评估**（我下载 .crate 解包后亲自核实，284,797 字节）：

- **真是实现，不是空壳**：`src/` 共 **16,561 行 Rust**，70 个文件，10 个业务域（auth/search/song/lyric/playlist/recommend/discovery/user/album/artist/rank）
- **算法正确**：`signing.rs` 的 `PlatformProfile::{Standard,Lite}` 双套 salt 完整（appid 1005/3116、clientver 20489/11440、salt 与 key_salt 四件套齐全）
- **我实测编译通过**（Windows GNU toolchain）
- **我实测联网通过**：设备注册拿到真实 dfid、搜索返回真实歌曲数据

**但它不能作为你产品的依赖基础**，理由（全部实证）：

1. **仓库链接是死链**：Cargo.toml / crates.io / docs.rs 三处都写 `https://github.com/zephyrixel/kugou_sdk`，实测 **HTTP 404**。
2. **许可来源无法追溯** ⚠️：crate 声明 **MIT**，但唯一可知的源码上下文是 `zephyrixel/kugou_music`（Flutter 客户端，**GPL-3.0**），其中 `native/kugou_bridge/Cargo.toml` 写死 `kugou_sdk = "=0.2.9"`。MIT 授权人身份、代码出处**都无法核实**。
3. **API 极不稳定**：11 个版本**全部集中在 6 天内**（2026-07-12 → 07-18）连发，自述「0.2.x 快速对齐阶段，不承诺保留早期接口」。
4. **生态为零**：反向依赖 0，下载 249 次，docs.rs 文档覆盖率 52.6%。
5. **我实测发现真实缺陷**：主播放路径 `/v6/priv_url` 三种参数组合全部返回

   ```
   KuGou business error 20010: param error, unmarshal failed.
   ```
   而同项目 `/v5/url` 正常。对比 Node 参考实现 `song_url_new.js`，`/v6/priv_url` 需要嵌套的 `resource`{album_audio_id/collect_list_id/hash/id/page_id/type} + `tracker_param`{key/is_free_part/need_climax/...} 结构，SDK 的构造与之不符。

### 3.2 酷我 / 波点：**不存在**

- 酷我：无专门 crate。唯一沾边的 `unm_engine_kuwo` 0.4.0，**2023-01-22 停更**，只做播放地址，无登录/搜索/歌单/歌词。
- 波点：`bodian` 查询 **0 结果**。

### 3.3 对照：网易云的成熟度天花板

| crate | 版本 | 最后更新 | 总下载 |
|---|---|---|---|
| `ncmapi` | 1.0.0 | 2026-08-26 | **17,589**（15 版本横跨 5 年） |
| `ncmc_lib` | 0.3.1 | 2026-07-18 | 14,458 |

即便是生态最成熟的网易云，天花板也只有 1.7 万下载。**Rust 音乐 API crate 整体都处于「个人项目顺手开源」阶段，酷狗的 249 下载比网易云最差的还低一个数量级。**

---

## 4. Rust 播放器壳的技术栈选择

数字来源：本机实测 + gh CLI 读 release 产物 + 社区/官方 issue。**报告 `windows-rust-gui-体积内存调研.md` 有 900 行完整版，含每个数字的来源 URL 与可信度分级。**

### 4.1 首先纠正一个前提：「无浏览器内核」和 Tauri 是互斥的

Tauri 官方文档 [webview-versions](https://v2.tauri.app/reference/webview-versions/) 原文：

> "**Tauri uses WebView2 which is based on Microsoft Edge and therefore Chromium.**"

Tauri 的优势是**不把内核打进安装包**，不是**没有内核**。**省下的是磁盘，省不下的是内存。**

本机实测（本机 WebView2 版本 146.0.3856.72）：**一个连 UI 都没有的空白 WebView2 页面**——

| 指标 | 实测值 |
|---|---:|
| 进程树 Working Set 合计 | **336.6 MB** |
| 进程数 | 7 个（其中 `msedgewebview2.exe` 6 个 = 286.1 MB） |

### 4.2 横向数据

**体积（Windows）**

| 技术栈 | 体积 | 来源分级 |
|---|---:|---|
| Tauri 2 裸二进制 | 2.61 MB | 【官方】benchmark |
| Tauri 2 真实安装包 | 1.85–18 MB | 【第三方】release 产物 |
| Tauri + WebView2 离线分发 | **+127 MB**（固定版 +180 MB） | 【官方】文档 |
| iced | 1.5 MB（glow）/ 3.1 MB（wgpu+LTO） | 【社区】iced#1531 Windows |
| Slint | 3.45 MB（femtovg）/ 2.6 MB（LTO） | 【官方+社区】slint#3376 Windows |
| egui | ⚠️ Windows 无数据（仅 Linux 10.6 MB） | 【未找到】 |
| GPUI | 15.66 MB（sonora）/ 20.3 MB（meliora Windows zip） | 【实测】release 产物 |

**内存**

| 技术栈 | 空载 | 复杂场景 | 平台 |
|---|---:|---:|---|
| Tauri | 261 MB（官方 hello_world） | 307–739 MB | Linux 官方 / macOS |
| — 空白 WebView2 | **336.6 MB** | — | **本机 Win 实测** |
| egui | 30–36 MB | 142–157 MB | 官方 issue / macOS |
| iced | 27 MB（glow）/ 76 MB（wgpu） | 140–177 MB | Windows / macOS |
| Slint | **14.0 MB**（winit-software） | 105.9–133.0 MB | Windows / macOS |

### 4.3 三个必须知道的坑

1. **Tauri 官方 benchmark 被官方自己否定**。Tauri 成员在 [tauri#5889](https://github.com/tauri-apps/tauri/issues/5889) 说那套数字「要带一大把盐看，只是 smoke test」；且官方面板原文写明「**只在 Linux 测内存**」，Windows 的 `max_memory` 历史记录**全空**。同 issue 实测 Windows 上 Tauri 399MB **比 Electron 318MB 更吃内存**。

2. **egui 在 Windows 有真实的高 CPU 问题**（[egui#4173](https://github.com/emilk/egui/issues/4173)，**至今 open**）：鼠标移动就 20–30% CPU，连 hello_world 都是 30%，维护者 emilk 确认「This is still an issue on Windows」。注意「immediate mode 会一直重绘」这个常见说法**是错的** —— egui README 明确说空闲时不重绘，问题是 OpenGL 上下文切换。

3. **GPUI 的坑不在技术而在交付**：crates.io 上 `gpui` 停在 0.2.2（2025-10-22）近 11 个月未发版，官方 README 推荐的 `gpui_platform` **在 crates.io 上不存在** —— 照 README 建项目会直接构建失败。需走 git 依赖（meliora 就是这么做的，且依赖 `gpui-unofficial` 这个第三方 shim）。

### 4.4 一个很有说服力的对比

| 项目 | GUI | Windows 安装包 |
|---|---|---|
| **seraphine-music**（Tauri，协议在 Rust 端） | Tauri 2 + Vue | **3.33 MB**（exe）/ 4.38 MB（msi） |
| **meliora**（GPUI 纯原生） | GPUI | 20.31 MB（zip） |

seraphine 的包**只有 meliora 的 1/6** —— 因为 Tauri 不打包内核。但代价是运行时要拉起 336MB 的 WebView2。
**「安装包小」和「内存小」是两件事，这是本次调研最容易误导人的地方。**

### 4.5 推荐

既然你的硬需求是**无浏览器内核**，Tauri 直接出局。

| 场景 | 推荐 | 理由 |
|---|---|---|
| 追求最小内存 + 能接受 GPU 依赖 | **Slint** | Windows 实测空载 14 MB，横评第一。⚠️ **GPLv3 或商业授权**，专有应用需付费 |
| 追求许可最干净 | **egui**（MIT/Apache）或 **iced**（MIT） | 但要接受 egui 的 Windows 高 CPU 问题 |
| 想抄一个现成的原生播放器结构 | **GPUI + 参考 meliora** | meliora 已经跑通了「GPUI + cpal + symphonia + 酷狗协议」全套 |

---

## 5. 许可与 GPL 传染风险

| 项目 | License | 能否复用代码 |
|---|---|---|
| **lmplayer**（含 `kugou` crate） | **MIT** | ✅ **可以**，署名即可 |
| **seraphine-music** | **MIT** | ✅ 可以 |
| **polomusic-tauri** | **MIT** | ✅ 可以 |
| **meliora** | **Apache-2.0** | ✅ 可以（含专利授权，需保留 NOTICE） |
| **ChouChiu/Lyrics-Helper**（KRC 解密） | Apache-2.0 | ✅ 可以 |
| **kugou_sdk**（crate） | 声明 MIT ⚠️ | ⚠️ **授权人不可核实**（仓库 404、上下文是 GPL-3.0 项目）。法律上风险不明 |
| **md3Music** | **AGPL-3.0** | ❌ **强烈不建议**。AGPL 是最强的传染性许可：只要你分发或**通过网络提供服务**，整个作品都要以 AGPL 开源。抄它的 Rust 代码 = 你的播放器必须 AGPL |
| **bamboostrip/KugouMusic.rs** | **无 LICENSE** | ❌ **不能用**。无许可 = 默认保留全部权利，法律上等同于专有代码 |
| **JYHjyh001/desktop_pet** | **无 LICENSE** | ❌ 同上 |
| **CGQAQ/krc-rs** | GPL-3.0 | ❌ 传染。且 2020 年后未更新，没必要 |
| **ghtz08/kugou-kgm-decoder** | NOASSERTION | ⚠️ 需自行确认 |

**GPL 传染的具体后果**：md3Music 的 Rust 服务器协议覆盖最全（45 模块），是技术上最诱人的抄袭对象，但它是 AGPL-3.0。**如果你想闭源或不想被强制开源，一行都不能抄** —— 包括算法实现的结构。算法本身（MD5 拼接）不受著作权保护，但**具体的代码表达受保护**，所以要「照着公式自己写」，而不是「改改变量名」。

---

## 6. 结论：最务实的路线

### 6.1 直接回答你的二选一

**不要「找一个现成的 Rust 酷狗实现直接依赖」，要「参考现有实现自己重写签名层」。**

- ❌「直接依赖 crate」：唯一选项 `kugou_sdk` 授权不可追溯、API 不稳、主播放路径有实测缺陷。
- ✅「自己重写」：**要写的核心代码只有约 300 行** —— 签名 5 个函数 + KRC 解码 1 个函数 + 设备指纹。我已经验证过这条路的可行性：签名算法我用 Node 独立复现并与 Rust 实现对拍**逐位一致**，且在真实服务器上拿到了 dfid 和搜索结果。

### 6.2 具体推荐路线

```
Rust 播放器
├── 协议层：自己写（约 300 行）
│   ├── 参考 lmplayer/kugou/src/proto/{sign,hash,lyrics}.rs  ← MIT，可读可抄
│   ├── 交叉验证 md3Music 的 helper.rs（AGPL，只读不抄）
│   └── salt/appid 常量从任意实现取值（常量本身不受著作权保护）
├── 播放/解码：symphonia + cpal + rubato + lofty（纯 Rust，MIT）
├── 歌词：KRC 自写（30 行）；或依赖 lyrics-crypto（Apache-2.0）
└── GUI：Slint（内存最优，注意 GPL）/ egui / iced / GPUI
```

**为什么是「抄 lmplayer 而不是抄 md3Music」**：lmplayer 是 MIT，且它的 `kugou` crate 是**独立 crate（`kugou = { path = "kugou" }`）、无 GUI 耦合、有测试**，结构上本来就是可复用的库。md3Music 功能更全但 AGPL 且和 Flutter 用 JNI 耦合。

**冷启动成本最低的做法**：先把 lmplayer 的 `kugou` crate 整个 vendor 进你的项目（MIT 允许），跑通登录+播放，再按需精简/重写。这样能在第一天就有一个可用的协议层，同时不被任何不稳定依赖绑架。

### 6.3 风险清单

| 风险 | 严重度 | 说明 |
|---|---|---|
| **协议随时会变** | 🔴 高 | 酷狗会改接口。我实测中 `/v1/search/hot_tab` 就已经 **404** 了。任何实现都需要持续维护 |
| **播放地址需登录** | 🔴 高 | 我实测匿名状态 `/v5/url` 一律返回 `fail_process:["pkg","buy"]`。**必须实现概念版扫码登录并拿到 VIP 账号**才有播放能力。登录后的链路本次**未验证** |
| **风控/封号** | 🟠 中 | md3Music 源码注释明确提到「重复的新设备注册会让账号看起来像一堆设备并触发风控」，它的对策是持久化 dfid 复用。你需要照做 |
| **`/v6/priv_url` 格式复杂** | 🟠 中 | 嵌套 `resource`+`tracker_param` 结构，`kugou_sdk` 就栽在这。建议优先用 `/v5/url`（结构简单，我实测签名通过） |
| **Rust crate 生态为零** | 🟡 低 | 但反过来说，你要写的核心逻辑不多，依赖风险也小 |
| **法律风险** | 🟠 中 | 所有第三方酷狗客户端都处于灰色地带。上面各项目的 README 都带免责声明 |

### 6.4 我没能验证的（诚实清单）

- ❌ **登录后的完整播放链路**：无概念版 VIP 凭据，未能验证 `/v5/url` 在登录后是否返回真实播放地址
- ❌ **`kugou_sdk` 的 `/v6/priv_url` 缺陷是 SDK 构造错误还是服务端接口已变**：未能区分
- ❌ **各 Rust 实现在 Windows 上长时间运行的稳定性**：只做了编译和单次联网验证
- ❌ **`kugou_sdk` 的 MIT 授权是否有效**：无法核实
- ❌ **Windows 上同 app 多 GUI 框架的横向实测**：不存在这样的公开仓库（三篇横评全是 macOS/Linux）
- ❌ **GPUI 独立应用的空载内存**：完全无数据

---

## 附：可点击链接

**候选项目**
- [lianchengwu/lmplayer](https://github.com/lianchengwu/lmplayer) · [kugou/src/proto/sign.rs](https://github.com/lianchengwu/lmplayer/blob/HEAD/kugou/src/proto/sign.rs)
- [zzyoxml/md3Music](https://github.com/zzyoxml/md3Music) · [Rust 服务器](https://github.com/zzyoxml/md3Music/tree/HEAD/kugou_api_server/rust/src)
- [bamboostrip/KugouMusic.rs](https://github.com/bamboostrip/KugouMusic.rs) · [signer.rs](https://github.com/bamboostrip/KugouMusic.rs/blob/HEAD/src/kugou/signer.rs)
- [burenLee/seraphine-music](https://github.com/burenLee/seraphine-music) · [releases](https://github.com/burenLee/seraphine-music/releases)
- [li-ming1/meliora](https://github.com/li-ming1/meliora) · [src/kugou](https://github.com/li-ming1/meliora/tree/HEAD/src/kugou)
- [smiling11123/polomusic-tauri](https://github.com/smiling11123/polomusic-tauri) · [kg-rs/src/crypto.rs](https://github.com/smiling11123/polomusic-tauri/blob/HEAD/kg-rs/src/crypto.rs)
- [DreamAlone666/kg2lx](https://github.com/DreamAlone666/kg2lx)
- [MOPELotus/TuneWeave](https://github.com/MOPELotus/TuneWeave)
- [emoeem/voicefox](https://github.com/emoeem/voicefox)

**签名算法活水源**
- [MakcRe/KuGouMusicApi](https://github.com/MakcRe/KuGouMusicApi) · [util/helper.js](https://github.com/MakcRe/KuGouMusicApi/blob/HEAD/util/helper.js)

**crates**
- [kugou_sdk](https://crates.io/crates/kugou_sdk) · [docs.rs](https://docs.rs/kugou_sdk)
- [lyrics-crypto](https://crates.io/crates/lyrics-crypto) · [ncmapi](https://crates.io/crates/ncmapi)（网易云对照）
- [unm_engine_kugou](https://crates.io/crates/unm_engine_kugou) · [unm_engine_kuwo](https://crates.io/crates/unm_engine_kuwo)

**GUI 技术栈**
- [Tauri: WebView versions（承认基于 Chromium）](https://v2.tauri.app/reference/webview-versions/)
- [tauri#5889（Windows 内存实测比 Electron 高）](https://github.com/tauri-apps/tauri/issues/5889)
- [egui#4173（Windows 高 CPU，仍 open）](https://github.com/emilk/egui/issues/4173)
- [iced#1531（Windows 体积）](https://github.com/iced-rs/iced/discussions/1531) · [slint#3376](https://github.com/slint-ui/slint/discussions/3376)
- [Slint 定价（GPLv3 / 商业授权）](https://slint.dev/pricing)
- [sonora（GPUI 音乐播放器参考）](https://github.com/sonorahq/sonora)
