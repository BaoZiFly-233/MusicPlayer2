# 酷狗音乐 / 波点音乐 第三方实现技术栈评估

调查时间：2026-09-12 · 工作目录 `D:\BoTapMusic` · 全程使用 `gh` CLI + GitHub raw API 实读源码

**本次覆盖**：Go（深度）· Java/Kotlin（中度）· Python / PHP / Dart / C++ / Swift（扫描）· 波点音乐（Bodian）验证 · Rust / C#（额外交叉验证）

**证据等级约定**

- **VERIFIED** = 实际读取了源码文件，下文引用具体行
- **INFERRED** = 从 README / 元数据 / 间接证据推断
- **没有** = 明确确认不存在

---

## 目录

- [0. 先纠正两个前提（重要）](#0-先纠正两个前提重要)
- [PART 1 — Go 深度评估](#part-1--go-深度评估)
- [PART 2 — Java / Kotlin 中度评估](#part-2--java--kotlin-中度评估)
- [PART 3 — Python / PHP / Dart / C++ / Swift 轻量扫描](#part-3--python--php--dart--c--swift-轻量扫描)
- [波点音乐（Bodian）](#波点音乐bodian酷我旗下bd-apikuwocn)
- [附：Rust / C# 额外发现](#附额外发现--rust-生态意外地强不在原任务范围内)
- [汇总：各语言可用性判定](#汇总各语言可用性判定)
- [最终建议：三条技术栈路线](#最终建议三条技术栈路线)
- [方法与置信度声明](#方法与置信度声明)

**TL;DR（急着看结论就从这里开始）**

1. **两个 salt 的标注在任务简报里是反的** —— `OIlwieks28dk2k092lksi2UIkp` 是**标准版**（appid 1005/20489），`LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` 是**概念版**（3116/11440）。见 §0.1。
2. **波点音乐不是"无需签名"** —— `audioUrl`/`checkRight` 必须带 `md5("kuwotest" + ...)` + `timestamp`。`plat` 必需但可取 `ar`/`win`/`ip`/`h5`，不是只有 `ar`。
3. **Go 生态最成熟**，但只有 3 个能真正 `go get`；`zhouchentao666/SugarPlayer` 是 `music-lib` 的**逐行复制品**，不是独立实现。
4. **Java/Kotlin 只有 1 个能当依赖**：`ghhccghk/KuGouApi_Kotlin_SDK`（真的在 Maven Central 上，6 个版本）。`rRemix/APlayer`（1820★）的酷狗部分**只有 447 行且缺播放地址/登录/设备注册**。
5. **PHP 和 C++ 明确「没有」** —— 两种语言下两个 salt 的 code search 均为 **0 命中**。
6. **任务简报里点名的多个仓库根本不含对应代码**：`chaser114/taemspeak3-bodian`（走第三方聚合站）、`BsaLee/bodian_music_api`（只有签到）、`BetterBDM`/`bodianhelper`（二进制 hook）、`ck2246/Kugou-Lite`（28 分钟的空壳）。

---

## 0. 先纠正前提（重要）

### 0.1 两个 salt 的命名是反的

任务简报里写「`LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` = 标准版，`OIlwieks28dk2k092lksi2UIkp` = 概念版」。**实际相反。**

参考实现 [`MakcRe/KuGouMusicApi`](https://github.com/MakcRe/KuGouMusicApi) 的 `util/helper.js` 原文：

```js
const signatureAndroidParams = (params, data) => {
  const isLite = process.env.platform === 'lite';
  const str = isLite ? 'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA' : `OIlwieks28dk2k092lksi2UIkp`;
```

`signParamsKey` 里是同一个三元表达式。同仓库 `util/config.json`：

```json
{ "appid": 1005, "clientver": 20489, "liteAppid": 3116, "liteClientver": 11440 }
```

所以 **VERIFIED**：

| 用途 | 标准版 | 概念版 / Lite |
|---|---|---|
| Android 签名 salt | `OIlwieks28dk2k092lksi2UIkp` | `LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` |
| signKey salt | `57ae12eb6890223e355ccfcb74edf70d` | `185672dd44712f60bb1736df5a377e82` |
| appid / clientver | 1005 / 20489 | 3116 / 11440 |

我用 8 个 Go 仓库独立复核，全部与 MakcRe 一致（`music-lib`、`MusicBot-Go`、`Ion-nsx`、`miaosic`、`lfhy`、`musedl`、`Meting`、`SugarPlayer`）；Java/Kotlin 侧又有 9 个仓库复核，同样一致。

**这个反转陷阱在真实源码里存在，值得警惕**：`CharlesPikachu/musicdl` 的 `kugouutils.py:33` 写成

```python
SIGNATURE_ANDROID_SECRET = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA" if IS_LITE else "OIlwieks28dk2k092lksi2UIkp"
```

而它 `IS_LITE = True` 配的却是 `APPID = 1005 / CLIENTVER = 20489`。**变量名叫 IS_LITE，行为却是标准版** —— 代码是对的，名字是错的。这类"Lite 视图 vs 标准版凭据"的历史混淆很可能就是简报标注反了的来源。

**ground truth**：`9xhk-1/kugou-source` 是**反编译的酷狗官方 Android App**（6,919 文件，含混淆包 `e/c/a/g/a/f/e/b.java`），其 `dataclass/feedback/DexFeedBackActivity.java` 用标准 salt 拼接 —— 这是极性判断的地面真值。⚠️ 反编译专有软件法律风险高，**只做证据，不要抄**。

### 0.2 完整的 salt / 密钥清单（VERIFIED，多仓库交叉确认）

| 名称 | 值 | 算法 |
|---|---|---|
| Web/H5 salt | `NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt` | `md5(salt + sorted(k=v) + body + salt)` |
| Android salt | 见上表 | `md5(salt + sorted(k=v) + body + salt)` |
| signKey | 见上表 | `md5(hash + salt + appid + mid + userid)` |
| 设备注册 salt | `1014` | `md5("1014" + sorted(仅 value) + "1014")` |
| 通用 signParams | `R6snCXJgbCaj9WFRJKefTMIFp0ey6Gza` | `md5(sorted(k+v) + data + salt)` |
| 云盘 key | `ebd1ac3134c880bda6a2194537843caa0162e2e7` | `md5("musicclound" + hash + pid + salt)` |
| KRC XOR key | `40,71,97,119,94,50,116,71,81,54,49,45,206,210,110,105` | 循环 XOR → zlib inflate |
| RSA 公钥（标准版） | `MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDIAG7QOELSYoIJvTFJhMpe1s/g...` | PKCS#1 v1.5 |
| RSA 公钥（概念版） | `MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDECi0Np2UR87scwrvTr72L6oO0...` | PKCS#1 v1.5 或裸填充 |

### 0.3 其他三处前提需要修正

**(1) 设备注册路由不是 `/register/devices`。** 参考实现与所有 Java/Go/Kotlin 实现走的都是：

```
POST https://userservice.kugou.com/risk/v2/r_register_dev?part=1&platid=1&p=<RSA>
body = base64(AES-CBC(Redmi "marble" 设备指纹))
p    = RSA_PKCS1({aes, uid, token})
签名  = md5("1014" + 排序后的**纯 value** 拼接 + "1014")   ← 忽略 key
```

部分较老分支用 `/risk/v1/r_register_dev`（如 `a1sunyuwen/kugou-api`、`develop202/kgcheckin`、`CJ-Hot/KuGoMusic`）。**没有任何仓库用 `/register/devices`。**

**(2) `rRemix/APlayer` 不是 fork。** 简报称它是"某知名播放器的 fork"。`gh api repos/rRemix/APlayer` 返回 `{"fork":false,"parent":null,"source":null}`，最早提交 `036e8494`（2016-05-20，作者 rRemix 本人），共 1457 次提交，默认分支 `compose`。**原创项目。** 所以"上游是谁"这个问题本身不成立。

**(3) `Winnie0408/LocalMusicHelper` 不是文件解密。** 简报猜它"likely just KGM/KGMA file decryption, unrelated to API signing" —— **不成立**，它是真的 API 请求签名器（用标准版 salt 打 `gateway.kugou.com/pubsongs/v4/get_other_list_file`）。详见 PART 2 §2.8。

**另外两处 star 数被低估得离谱：**

| 仓库 | 简报印象 | 实际 | 实际规模 |
|---|---|---|---|
| `LTLXS/Net-music-KUGOU` | "3 stars" | 3★ 属实，但 | **酷狗代码 5,666 行 —— Java 侧最大** |
| `AffectionParadise/LightMusic` | 未给 star | **357★** | 4,340 行（但含 7 个整份注释掉的死文件） |
| `lladlam/MeloX-Android` | 未列出 | **133★** | 2,312 行，标准版 Kotlin 里最完整 |
| `Winnie0408/LocalMusicHelper` | 未给 star | **395★** | 但酷狗代码只有 2 处 |
| `rRemix/APlayer` | "1820 stars, Kotlin 实现是否完整" | 1820★ 属实 | **酷狗代码只有 447 行，且无播放地址/登录/设备注册** |

---

## PART 1 — Go 深度评估

### 1.1 总览表

| 仓库 | ★ | pushed_at | License | LICENSE 文件 | 形态 | `go get` 可得 | 酷狗部分 ~LOC | 测试 |
|---|---|---|---|---|---|---|---|---|
| [guohuiyuan/music-lib](https://github.com/guohuiyuan/music-lib) | 134 | 2026-09-02 | AGPL-3.0 | ✅ | **可复用库** | ✅ `github.com/guohuiyuan/music-lib` | ~3320 | ✅ 2 个 |
| [liuran001/MusicBot-Go](https://github.com/liuran001/MusicBot-Go) | 198 | 2026-09-11 | GPL-3.0 | ✅ | Telegram Bot（插件式） | ⚠️ 模块路径正规，但插件依赖 bot 内部包 | ~10430 | ✅ 9 个 |
| [lfhy/kugou-music-api](https://github.com/lfhy/kugou-music-api) | 2 | 2026-04-26 | MIT | ✅ | **可复用 SDK** | ✅ `github.com/lfhy/kugou-music-api` | ~13778 | ✅ 4 个 |
| [Ion-nsx/Kugoumusic-web](https://github.com/Ion-nsx/Kugoumusic-web) | 2 | 2026-08-25 | Apache-2.0 | ✅ | 应用（go+vue 服务端） | ❌ module `vibe` | ~5868 | ❌ 0 |
| [AynaLivePlayer/miaosic](https://github.com/AynaLivePlayer/miaosic) | 2 | 2026-06-03 | MIT | ✅ `LICENSE.md` | **可复用库**（聚合多平台） | ✅ `github.com/AynaLivePlayer/miaosic` | ~986 | ✅ 3 个 |
| [DeeCen/yourMusic](https://github.com/DeeCen/yourMusic) | 19 | 2025-12-04 | GPL-2.0 | ✅ | 桌面应用（wails） | ❌ module `yourMusic` | ~1199 | ✅ 8 个 |
| [CN-Grace/musedl](https://github.com/CN-Grace/musedl) | 0 | 2026-08-05 | GPL-3.0 | ✅ | CLI 应用 | ❌ 见 1.9 | ~1629 | ❌ 0 |
| [gentpan/Meting](https://github.com/gentpan/Meting) | 0 | 2026-07-24 | 无 | ❌ **没有** | 服务端应用 | ❌ module `metingio` | ~967 | ❌ 0 |
| [zhouchentao666/SugarPlayer](https://github.com/zhouchentao666/SugarPlayer) | 21 | 2026-08-03 | 无 | ❌ **没有** | 桌面播放器 | ❌ module `sugarplayer` | ~3327（**复制品**） | ✅ 2 个 |
| [Aniu456/kugou-player-backend](https://github.com/Aniu456/kugou-player-backend) | 0 | 2026-08-28 | 无 | ❌ **没有** | HTTP 代理服务 | ❌ module `github.com/benny/player-backend-go` | ~5000+ | ✅ 有 |
| [muchenspace/XyMusic](https://github.com/muchenspace/XyMusic) | 7 | 2026-09-08 | MIT | ✅ | 应用（**仅歌词**） | ❌ | ~146 | ❌ 0 |
| [skxxxkx666/Kugo-Music-Converter](https://github.com/skxxxkx666/Kugo-Music-Converter) | 308 | 2026-08-24 | GPL-3.0 | ✅ | **本地文件解密，无 API** | — | — | ✅ |
| [leafxdd/unlock-music](https://github.com/leafxdd/unlock-music) | 2 | 2026-07-08 | MIT | ✅ | **本地文件解密，无 API** | — | — | ✅ |
| [tamnd/kugou-cli](https://github.com/tamnd/kugou-cli) | 0 | 2026-06-29 | Apache-2.0 | ✅ | **脚手架，无实现** | — | ~200 | ✅ |

### 1.2 能力矩阵（a–g）

| 仓库 | (a) 签名 salt | (b) 播放地址 / VIP 音质 | (c) 登录 | (d) 设备注册 | (e) 歌词 KRC | (f) 搜索/歌单/专辑/排行 | (g) 概念版 |
|---|---|---|---|---|---|---|---|
| **music-lib** | Web + Lite 双 salt ✅ | ✅ `/v5/url` + `/v6/priv_url` + 4 条 tracker 回退 | ✅ 扫码（手机号/SMS ❌） | ✅ `r_register_dev` + RSA + AES-CBC | ✅ XOR + ParseKRC | ✅ 全部 | ✅ 用 Lite appid 3116 |
| **MusicBot-Go** | **标准 + Lite + Web 全 3 套** ✅ | ✅ `/v5/url` + `/v6/priv_url` + 验证码挑战处理 | ✅ 扫码 + token 续期 + 签到 | ✅ `r_register_dev` + 强制重注册 | ✅ `bot/lyric.DecodeKRC` | ✅ 全部 + 云盘 | ✅ **完整独立实现** |
| **lfhy/kugou-music-api** | **5 种签名全有** ✅ | ✅ `ResolveSongPlayURL` 多音质 | ✅ 密码/手机号/token 刷新 | ✅ `RegisterDev` | ✅ XOR + zlib inflate | ✅ 153 个接口（自动生成） | ✅ `isLite` 分支 |
| **Ion-nsx/Kugoumusic-web** | 标准 + Lite + Web + register ✅ | ✅ `/v5/url` + `/v6/priv_url` + `get_res_privilege/lite` + `get_kmr_audio` | ✅ 密码 + 手机号验证码 + 扫码 | ✅ `r_register_dev` | ✅ XOR 解密 + 转 LRC | ✅ 全部 | ✅ `SetUseLite` 切换 |
| **miaosic** | 标准 + Lite + Web ✅ | ⚠️ 仅 `/v5/url`，自己 todo.txt 承认 `ppage_id` 拼写有问题 | ✅ 扫码 | ❌ 用 `md5(dfid)` 当 mid | ⚠️ 有歌词接口，KRC 解码为 TODO | ✅ 搜索/歌单 | ⚠️ 定义了 Lite 常量但未走通 |
| **yourMusic** | **仅 Lite** | ✅ `/v5/url` | ✅ 扫码 + 手机号 | ✅ dfid | ✅ | ✅ | ✅ **专为概念版** |
| **musedl** | Lite + 标准 gateway ✅ | ✅ `/v5/url` + `/v6/priv_url` | ✅ 扫码 | ✅ `r_register_dev` | ⚠️ 有歌词，KRC 未验证 | ✅ 搜索 | ✅ 概念版会话 |
| **Meting** | 全套 5 个 salt 已定义 ✅ | ✅ 4 级回退（含调 music-lib） | ⚠️ 依赖 cookie | ❌ | ⚠️ 未验证 | ✅ | ⚠️ 有 Lite 常量 |
| **SugarPlayer** | = music-lib 复制品 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **kugou-player-backend** | 标准 + Lite + Web ✅ | ✅ `185672dd...` signKey 出现 | ⚠️ 有 login 相关 | ✅ `/risk/v2/r_register_dev` | ❌ | ✅ 大量 | ✅ `IsLite()` |

### 1.3 逐个仓库详述

#### ① [guohuiyuan/music-lib](https://github.com/guohuiyuan/music-lib) — 最推荐的 Go 库

- 134★ / 37 forks，AGPL-3.0（**注意：AGPL 有传染性，商用需谨慎**），`LICENSE` 文件已提交
- module `github.com/guohuiyuan/music-lib`，`go 1.18`，唯一依赖 `golang.org/x/text` —— **依赖极轻**
- 聚合库，酷狗只是 `kugou/` 一个子包，另有 netease/qq/kuwo/bilibili/soda 等

**(a) 签名** — `kugou/kugou.go` 常量区：

```go
KugouSignKey    = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"   // Web
KugouLiteSign   = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"   // 概念版 Android
KugouLiteAppID  = "3116"
KugouLiteVer    = "11440"
KugouLiteKey    = "185672dd44712f60bb1736df5a377e82"   // 概念版 signKey
```

两个签名函数：

```go
func signKugouSonginfoParams(params map[string]string) string {
    // 排序后 md5(KugouSignKey + joined + KugouSignKey)
}
func signKugouAndroidParams(params map[string]string, data string) string {
    // 排序后 md5(KugouLiteSign + joined + data + KugouLiteSign)
}
```

**(b) 播放地址** — 四级回退链，覆盖很实用：
`fetchURLV5`（`gateway.kugou.com/v5/url`，带 `x-router: trackercdn.kugou.com`）
→ `fetchPrivURLV6`（`tracker.kugou.com/v6/priv_url`，POST，qualities 含 `flac/high/viper_atmos/viper_tape/viper_clear/super`）
→ `fetchSonginfoV2`（`wwwapi.kugou.com/play/songinfo`，走 Web salt，两段式 encode_album_audio_id）
→ `fetchTrackerSongInfo`（3 个 tracker CDN，`md5(hash+"kgcloudv2")` / `md5(hash+"kgcloud")`）

`looksLossless()` 判断无损；`IsVipAccount()` 打 `vip.kugou.com/recharge/roleinfo` 探测 VIP。VIP 音质需要 cookie 里同时有 `token` / `userid` / `KUGOU_API_MID`。

**(c) 登录** — `kugou/login.go` 只有**扫码**：`CreateQRLogin` / `CheckQRLogin`，状态码 4=成功、2/3=已扫、-1/5/6=过期。**没有手机号 / SMS 登录**。

**(d) 设备注册** — `registerKugouLoginDevice()`：随机 6 位 seed → `md5(seed)` 切 16/16 作 AES-128-CBC key/iv → 加密一份 Redmi marble 设备指纹 → 用内置 RSA 公钥 PKCS#1 v1.5 加密 `{aes, uid, token}` → POST `userservice.kugou.com/risk/v2/r_register_dev`。响应可能是 AES 密文，会用 `aesCBCDecrypt` 回落。

**(e) 歌词** — `kugou/lyric.go`：`krcs.kugou.com/search` 取 candidate → `lyrics.kugou.com/download?fmt=krc` → `lyrics.DecodeKRCBase64()`（在兄弟包 `lyrics/`）。**krc 解码逻辑不在 kugou 包内，估算的 ~3320 LOC 不含它。**

**(f) 搜索/歌单/专辑/排行** — `song.go`（`song_search_v2`）、`album.go`、`playlist.go`（分类 + `tag/specialList`）、`user_playlist.go`（gateway 用户歌单）、`cloudlist.go`（云盘）、`account.go`。**没有独立的"排行榜"文件**，排行靠推荐歌单近似。

**(g) 概念版** — 是**事实上的主路径**：所有 gateway 调用都用 `KugouLiteAppID=3116` / `KugouLiteVer=11440`。但**没有标准版 1005 的 Android 签名路径**（标准版只用 Web salt 打 `wwwapi`）。

**测试**：`download_test.go`、`user_playlist_test.go` —— 只有 2 个，覆盖很薄。

**注**：`kugouLoginWebGet()` 用 `signKugouSonginfoParams`（Web salt）去签一套 `appid=3116` 的 Android 风格参数。看着像有意为之（`login-user.kugou.com` 是 Web 端点），但值得留意。

---

#### ② [liuran001/MusicBot-Go](https://github.com/liuran001/MusicBot-Go) — Go 生态里最完整的酷狗实现

- 198★，GPL-3.0，`LICENSE` 已提交，活跃（2026-09-11 刚推）
- **不是库，是 Telegram Bot**；`plugins/kugou/` 是插件包，26 个文件、~10430 LOC、**9 个测试文件**

**(a) 签名** — `plugins/kugou/client.go` L36-45：

```go
kugouGatewayAppID     = "1005"
kugouGatewayClientVer = "11451"
kugouGatewaySignKey   = "OIlwieks28dk2k092lksi2UIkp"   // 标准版
kugouPlaySignKey      = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"  // Web
kugouPlayPidVerSec    = "57ae12eb6890223e355ccfcb74edf70d"  // 标准版 signKey
```

`plugins/kugou/concept_client.go` L33-39：

```go
kugouConceptAppID      = "3116"
kugouConceptClientVer  = "11440"
kugouConceptSignSecret = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"  // 概念版
kugouConceptPlaySecret = "185672dd44712f60bb1736df5a377e82"
kugouConceptRefreshKey = "c24f74ca2820225badc01946dba4fdf7"
kugouConceptT2Key      = "fd14b35e3f81af3817a20ae7adae7020"
kugouConceptT1Key      = "5e4ef500e9597fe004bd09a46d8add98"
```

**标准版和概念版是两套并行实现**，各有独立 client，这是其他 Go 仓库都没做到的。签名函数在 `concept_crypto.go`：

```go
func conceptSignatureAndroid(params url.Values, body string) string {
    // sorted("k=v") 拼接 → md5(secret + s + body + secret)
}
func conceptSignKey(hash, mid, userID, appID string) string {
    return conceptMD5(hash + kugouConceptPlaySecret + appID + mid + userID)
}
```

**(b) 播放地址** — `/v5/url` + `tracker.kugou.com/v6/priv_url`，并**专门处理风控**：`conceptHTTPResponseMeta.SSAEventID` 从响应头 `ssa-code` 读取，`newConceptVerificationChallenge()` 构造验证码挑战，`plugins/kugou/verification.go`（~20K）+ `verification_relay.go` 做验证码中继 —— 这是**唯一处理了酷狗风控验证码的 Go 实现**。

**(c) 登录** — 扫码（`/v2/qrcode` + `/v2/get_userinfo_qrcode`，go-qrcode 现场出 PNG）+ token 续期 `ManualRenew()`（POST `login.user.kugou.com/v5/login_by_token`，body 里 p3/t1/t2/t3/pk/params 全套 AES+RSA）+ `SignIn()` 每日签到领 VIP（`/youth/v1/recharge/receive_vip_listen_song`）。

**(d) 设备注册** — `concept_client.registerDevice()` → `/risk/v2/r_register_dev`，`conceptPlaylistAesEncrypt` 加密设备体，`conceptRSAPKCS1v15EncryptHex` 加密 `{aes, uid, token}`。有 `ForceRegisterDevice()` 做 dfid 被拒后强制重注册 —— **最健壮的设备注册实现**。

**(e) 歌词** — `lyric_krc.go`：`krcs.kugou.com/search` → `lyrics.kugou.com/download` → `lyricpkg.DecodeKRC()`（在 `bot/lyric/`，该包另有 `krc_e2e_test.go`）。输出逐词 QRC 轨道 + LRC + 翻译 + 罗马音。

**(f)** — `platform.go`（~20K）实现 `platform.Platform` 接口：Search / GetTrack / GetLyrics / GetPlaylist / GetAlbumPlaylist / ResolveDownloadByQuality。歌单解析极其细致（GCID 解码、legacy special、global collection 三条路径）。

**(g) 概念版** — **完整实现**，文件就叫 `concept_*`。`plugins/kugou/concept_session.go`（~14K）管会话持久化 + 自动刷新 daemon。

**注**：`go.mod` 里依赖了 `github.com/guohuiyuan/music-lib v1.0.6-...` —— 生态在互相复用（`model.Song` 类型来自 music-lib）。

---

#### ③ [lfhy/kugou-music-api](https://github.com/lfhy/kugou-music-api) — README 的 "Go port" 声明属实

- 只有 2★，MIT，`LICENSE` 已提交，module `github.com/lfhy/kugou-music-api`，**`go get` 可用**
- README 自己声明（诚实，值得表扬）：

> 本项目为使用 Codex 基于源项目 MakcRe/KuGouMusicApi（JavaScript 版本）迁移和更新的 Go 版本。由于迁移实现与接口行为可能存在偏差，当前版本可能存在问题，请谨慎使用并自行评估风险。

**"Go port of MakcRe/KuGouMusicApi" 这个声明 —— VERIFIED 属实。** `core/kugou/signature.go` 把 6 个签名函数一个不差地搬过来了，连注释里的算法说明都对得上：

```go
func SignatureWebParams(params map[string]any) string     // NVPh5oo...
func SignatureAndroidParams(p map[string]any, data string, isLite bool) string
func SignatureRegisterParams(params map[string]any) string // "1014" 前后缀
func SignKey(hash, mid, userid, appid string, isLite bool) string
```

`core/kugou/client.go` L125-132 按 `EncryptType` 分发 Android/Web/Register 三种签名。

**规模**：`sdk/` 下有 17 个 `generated_api_*.go` + 12 个 `signature_*.go` + 11 个 `manual_*.go`，~13778 LOC。自带 `sdk/API_CATALOG.md`、`API_COMPAT_AUDIT.md`、`API_SIGN_CHECK.md`。后者统计："总计接口: 153，需自定义签名/加密: 44，已校对修复: 44，待校对: 0"。

**(a)** 全部 6 种 ✅ **(b)** `sdk/song_play_url.go` 的 `ResolveSongPlayURL()`，带 `magic_` 前缀音质、fallback、`WithSongURLDFID` ✅
**(c)** `sdk/login_flows.go`：`LoginByPassword` / `LoginByCellphone` / `LoginByToken`，且 `ternaryString(c.isLite, "/v4/login_by_token", "/v5/login_by_token")` —— 概念版用 v4、标准版用 v5 ✅
**(d)** `sdk/signature_device_manual.go` `RegisterDev()` → `/risk/v2/r_register_dev` ✅
**(e)** `sdk/lyric_result.go` 完整 KRC 解码：`krcXORKey` 同 16 字节 + `inflateLyricPayload`（zlib）+ `ToLrc()` ✅
**(f)** 153 个接口，含 `rank` / `top_playlist` / `yueku` / `song_top` / `search_sheet` 等 ✅
**(g)** 默认就是 lite（"SDK 默认使用 lite 平台参数构造客户端"），有 `isLite` 切换 ✅

**风险**：2★、单人、Codex 生成、无 e2e 测试（只有 4 个单元测试）、几乎无人验证。文档诚实说明"可能存在问题"。

---

#### ④ [Ion-nsx/Kugoumusic-web](https://github.com/Ion-nsx/Kugoumusic-web) — 最干净的 Go 服务端实现

- 仅 2★，Apache-2.0，`LICENSE` 已提交；go+vue 的**概念版 Web 客户端**，`api/` 20 个文件 ~5868 LOC，**0 测试**
- module 名是 `vibe` → **不可 `go get`**，只能复制粘贴

`api/request.go` 的常量区是全网最清晰的一份注释：

```go
// 标准版 (appid=1005) 与概念版 (lite, appid=3116) 共用同一套签名算法，仅盐值与 appid/clientver 不同。
StdAppIDInt = 1005;  StdClientVerInt = 20489
StdAndroidSalt = "OIlwieks28dk2k092lksi2UIkp"
StdSignKeySalt = "57ae12eb6890223e355ccfcb74edf70d"
LiteAppIDInt = 3116; LiteClientVerInt = 11440
LiteAndroidSalt = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"
LiteSignKeySalt = "185672dd44712f60bb1736df5a377e82"
WebSalt = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"
RegisterSalt = "1014"
UserAgent = "Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi"
```

`SetUseLite(bool)` 全局切换。实现细节讲究：

- `RequestOptions.StdPlatform` —— **注释指出登录后的用户接口不支持 Lite，必须强制标准版签名**，这是很实用的踩坑经验
- `signatureAndroidParamsWithSalt()` 处理有 body 时的流式 md5（`h.Write([]byte(salt)); h.Write(params); h.Write(data); h.Write([]byte(salt))`）
- `signCloudKey` / `signParams` / `signParamsKey` 全覆盖
- **两个 RSA 公钥都内置**（标准版 `...DIAG7QOELSYoIJ...` 与概念版 `...DECi0Np2UR87sc...`）

**(b)** `api/song.go` 覆盖最全：`/v5/url`、`/v6/priv_url`、`/v2/get_res_privilege/lite`（x-router `media.store.kugou.com`）、`get_kmr_audio`、`get_song_climax`
**(c)** `api/auth.go`：`Login`（密码）+ `LoginCellphone`（手机号）+ `SendCaptcha` + QR 生成/轮询 + `AuthStore` 序列化
**(d)** `api/device.go` → `/risk/v2/r_register_dev`；GUID 会**持久化到文件**（`/root/X-music/.device-guid`，可用 `VIBE_GUID_FILE` 覆盖），注释解释了原因：mid 与登录 token 绑定，变了会导致 VIP 播放返回 20018
**(e)** `api/lyric.go` + `api/crypto.go` 的 `krcXORKey`，带 `ExtractLyricText` / `stripKRCHeadTags`
**(f)** album/artist/cloud/cloud_upload/discover/fm/history/image/playlist/rank/search/social/song/user —— 20 个文件铺满
**(g)** 主推概念版（`UseLite` 默认 false 但项目定位是概念版）

**亮点**：`SharedTransport` 那段注释解释了为什么不能每次请求新建 `&http.Transport{}`（IdleConnTimeout 默认 0 会导致句柄泄漏）—— 工程素养最好的一个。

---

#### ⑤ [AynaLivePlayer/miaosic](https://github.com/AynaLivePlayer/miaosic) — 多平台聚合库，酷狗部分偏薄

- 仅 2★（2023 年建仓，酷狗是后加的），MIT，`LICENSE.md` 已提交，module `github.com/AynaLivePlayer/miaosic`，**`go get` 可用**
- 酷狗部分 ~986 LOC / 12 文件 / 3 测试

`providers/kugou/utils.go` 常量：

```go
appid = "1005";  clientver = "20489"
appidLite = "3116"; clientverLite = "11440"
signkey     = "OIlwieks28dk2k092lksi2UIkp"
signkeyLite = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"
```

有 3 个签名函数（`signKey` / `signatureAndroidParams` / `signatureWebParams`），**但 `signKey()` 把标准版 salt `57ae12eb...` 硬编码进去了，没有 Lite 分支** —— 而 `signkeyLite` 定义了却没被任何签名函数使用（只在 `addAndroidParams` 里挑 `k.signkey`）。

**仓库自带的 `providers/kugou/todo.txt` 是最有价值的诚实证据**：

```
part of code is inspired by https://github.com/MakcRe/KuGouMusicApi under MIT License

KugouMusicApi 对齐基线：ref/KuGouMusicApi commit 566de020...（2026-05-20）

TODO：
- 对齐 privilege_lite/song_url_new 的音质支持
- 重新检查 GetMediaUrl：先修正当前 /v5/url 请求里的 ppage_id 拼写，
  再评估是否迁移到 song_url_new.js 使用的 /v6/priv_url。
- 将 Search 更新到 complexsearch.kugou.com 流程
```

**(b) 有已知 bug**（`ppage_id` 拼写错误），**(d) 没有设备注册** —— 用 `getMD5Hash(k.dfid)` 直接当 mid/uuid 使，这是简化做法，实际会被 `r_register_dev` 拒绝的路径下会失效。**(e) KRC 解码是 TODO**。

**结论：miaosic 的酷狗 provider 是 PARTIAL，不是 FULL。**

---

#### ⑥ [DeeCen/yourMusic](https://github.com/DeeCen/yourMusic) — 专做概念版，工程规整

- 19★，GPL-2.0，`LICENSE` 已提交；go + wails 桌面客户端；README 明写"**酷狗概念版第三方客户端**"
- **module 是裸名 `yourMusic`** → 不可 `go get`，只能复制

`api/helper.go` 开头注释 `// Package api kuGou lite API`，**只有 Lite 一套**：

```go
const signKey = `LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` // lite 版本
const signAppId = `3116`                           // lite appid
const signKeyVer = `11040`                         // lite 版本
const publicLiteRasKey = "-----BEGIN PUBLIC KEY-----..."
func SignKeyB(hash, appid, mid, userid string) string {
    return md5Str(hash + `185672dd44712f60bb1736df5a377e82` + appid + mid + userid)
}
```

设计上有个亮点：`CallAPIConfig{SignType}` + `signAndroid` / `signRegister` 两种签名模式，统一入口 `CallKuGouAPI()`；`RequestParam.ToSignStr()` / `ToURLStr()` / `ToRegisterSignStr()` 三种序列化 —— **代码结构是几个仓库里最好读的**。`RSAPublicEncryptNoPadding()` 手写裸 RSA（对应 Node 的 `RSA_NO_PADDING`）。

- **(c)** `api/login.go` 扫码 + 手机号；**(d)** `api/dfid.go`；**(e)** `api/lyric.go`；**(f)** search/song/vip
- **测试**：8 个 `*_test.go`，覆盖率是纯酷狗项目里最高的（虽然每个都很小）
- **缺**：标准版完全不支持；无歌单/专辑/排行

---

#### ⑦ [CN-Grace/musedl](https://github.com/CN-Grace/musedl) — 0★，但是**带完整协议注释的实现**

- 0★ / 0 fork，GPL-3.0，`LICENSE` 已提交；CLI 工具，`platforms/kugou/` 只有 2 个文件 ~1629 LOC，0 测试
- **这是 [`liuran001/musedl`](https://github.com/liuran001/musedl) 的 fork**（`go.mod` 里 module 名没改，仍是 `github.com/liuran001/musedl`）

`platforms/kugou/kugou.go` 开头的协议注释是全部 Go 仓库里写得最清楚的：

```go
// Protocol notes (derived from the app client behavior):
//   - Search:  GET songsearch.kugou.com/song_search_v2 (public, no auth)
//   - Track:   POST gateway.kugou.com/v3/album_audio/audio (JSON, signed with
//     a static key in headers)
//   - Download: requires a 概念版 (concept) session obtained by QR login at
//     login-user.kugou.com; the play URL is fetched from
//     gateway.kugou.com/v5/url with a signed query and device identity.
```

常量区同时容纳了**标准版 gateway 与概念版播放两条线**：

```go
gatewayAppID  = "1005";  gatewayVer = "11451"
gatewaySign   = "OIlwieks28dk2k092lksi2UIkp"        // 标准版
playSignKey   = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"  // Web
conceptSecret = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"  // 概念版
playSecret    = "185672dd44712f60bb1736df5a377e82"
```

`session.go` 里 `registerDevice()` → `/risk/v2/r_register_dev`，**且有优雅降级**：注册失败不报错，退回随机 24 位 dfid（注释 "device registration is best-effort"）。

**不可 `go get`**：module 名是 `github.com/liuran001/musedl`，却从 `github.com/liuran001/musedl/internal/...` 导入 —— 作为依赖会因 `internal/` 规则 + 路径不匹配直接失败。

---

#### ⑧ [gentpan/Meting](https://github.com/gentpan/Meting) — 有签名，但**无 License，不可用**

- 0★ / 0 fork，**GitHub 显示 license 为空，仓库里没有 LICENSE / COPYING 文件** —— 法律上默认保留所有权利，**不能用于任何项目**
- module `metingio`（裸名）→ 不可 `go get`
- 925 行 `kugou.go` + `kugou_sign.go`，0 测试

`kugou_sign.go` 里的 salt 定义得很齐全，注释也诚实：

```go
// Reverse-engineered constants from KuGou Android client.
// These are widely published in open-source projects (e.g. KuGouMusicApi).
kgAndroidSecret     = "OIlwieks28dk2k092lksi2UIkp"
kgSignKeySecret     = "57ae12eb6890223e355ccfcb74edf70d"
kgLiteAndroidSecret = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"
kgLiteSignKeySecret = "185672dd44712f60bb1736df5a377e82"
kgWebSalt           = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"
```

有 `kgSignAndroid` / `kgSignAndroidLite` / `kgSignWeb` / `kgSignKey` / `kgSignKeyLite` 五个函数。

`kugou.go` 的取流是**四级回退**：

```go
if got, err := p.streamViaLib(id); err == nil && got.URL != "" {    // 调 music-lib
if got, err := p.streamPrivV6(id, quality); ...                     // /v6/priv_url
if got, err := p.streamSigned(id, quality); ...                     // 签名接口
if got, err := p.streamWeb(id, quality); ...                        // play/songinfo
```

`go.mod` 里确实依赖 `github.com/guohuiyuan/music-lib v1.1.1-...` —— 又一个生态复用证据。

**结论**：技术上是 PARTIAL-TO-FULL，**但无 License 这点直接出局**。

---

### 1.4 额外 Go 发现

#### [zhouchentao666/SugarPlayer](https://github.com/zhouchentao666/SugarPlayer) — 21★，但是**复制品，不是独立实现**

我做了逐行 diff。`internal/music/kugou/*.go` 与 music-lib 的 `kugou/*.go` **除了 import 路径外完全一致**：

```
InputObject                                  SideIndicator
"sugarplayer/internal/music/model"              =>
"sugarplayer/internal/music/utils"              =>
"github.com/guohuiyuan/music-lib/model"         <=
"github.com/guohuiyuan/music-lib/utils"         <=
```

文件字节数也几乎一致（`kugou.go` 44651 vs 44661，`login.go` 12406 vs 12416，`lyric.go` 3203 vs 3218）。**VERIFIED：SugarPlayer 直接把 music-lib 的酷狗代码 vendor 进来改了 import。**

无 License、`LICENSE` 文件不存在、module `sugarplayer` 不可 `go get`。**不要把它当成独立实现来评估。**

#### [Aniu456/kugou-player-backend](https://github.com/Aniu456/kugou-player-backend) — 0★，但**是 KuGouMusicApi 的纯 Go HTTP 代理移植**

- 0★ / 0 fork，**无 License**，2026-08-27 建仓（非常新）；~330KB Go
- README："KuGouMusicApi 的纯 Go HTTP 代理实现……保留参考项目的 `module/` 文件名到公开路由的映射"
- 有 `host_test.go` / `module/parity_handlers_test.go` / `special_test.go` / `registry_test.go` —— **有 parity 测试**

`salt` 证据（`module/ordinary.go` L1404、`module/special.go` L193/1536）：

```go
const salt = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"
data["signature"] = util.CryptoMD5("OIlwieks28dk2k092lksi2UIkp" + strings.Join(parts, "") + "OIlwieks28dk2k092lksi2UIkp")
upstream.Path = "/risk/v2/r_register_dev"
appKey := firstOr(ctx.Query.Get("appkey"), ternary(util.IsLite(), "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA", "OIlwieks28dk2k092lksi2UIkp"))
```

`ordinary.go` L687 出现 Lite 的 signKey salt：
```go
"key": util.CryptoMD5(hash + "185672dd44712f60bb1736df5a377e82" + strconv.Itoa(util.AppID) + mid + strconv.Itoa(userID))
```

支持 `platform=lite` 环境变量切换。**技术上有料，但 0★ 无 License，只能当参考资料读。**

#### [muchenspace/XyMusic](https://github.com/muchenspace/XyMusic) — 只有歌词，没有 API 客户端

`Server/Backend/internal/modules/admintagscraping/lyrics_decryptor.go`（146 行）：

```go
krcKey          = []byte("@Gaw^2tGQ61-\xce\xd2ni")
kugouLyricsSalt = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"
func decryptKRC(encrypted []byte) ([]byte, error) {
    data[i] ^= krcKey[i%keyLen]
}
```

只有 KRC 解密 + 一个用 Lite salt 签名的**歌词请求**。**没有搜索/播放/登录/设备注册。** 所以网上那个 `LnT6xpN3...` 的 code-search 命中是歌词接口，不是音频接口。

#### 明确 `没有` API 签名的 Go 仓库

- **[skxxxkx666/Kugo-Music-Converter](https://github.com/skxxxkx666/Kugo-Music-Converter)**（308★，GPL-3.0）—— 全部代码在 `backend/internal/algo/kgg/`（`aes_cbc_std.go`、`ekey.go`、`qmc2.go`、`decoder.go`），**本地 KGM/KGMA/KGG 文件解密工具，零 API 调用**。三个 salt 的 code search 全部无命中。
- **[leafxdd/unlock-music](https://github.com/leafxdd/unlock-music)**（2★，MIT）—— `algo/kgm/` 是 KGM 文件格式解析 + PC 数据库解密，同样是**本地文件工具**，无 API。
- **[tamnd/kugou-cli](https://github.com/tamnd/kugou-cli)**（0★，Apache-2.0）—— **是脚手架，不是实现**。`kugou/kugou.go` 自己承认：`// The scaffold points it at kugou.com; change it once you know the real endpoints you want to read.` `Host = "kugou.com"`，没有任何 salt。**没有。**
- **[ayuayue/kugou](https://github.com/ayuayue/kugou)**（3★，2021）—— 3375 字节 `main.go`，无 salt。**没有。**
- **[iKuiki/kugou-sdk](https://github.com/iKuiki/kugou-sdk)**（0★，2016）—— 老 SDK，无 salt。**没有。**
- **[Aleapord/KuGou](https://github.com/Aleapord/KuGou)**（0★，2021）—— 爬虫，无 salt。**没有。**
- **[17sho/musicbot-go](https://github.com/17sho/musicbot-go)**（0★）—— 就是 MusicBot-Go 的 fork，加了个 `/song` 别名。
- **[Aniu456/…](https://github.com/Aniu456/kugou-player-backend)** 见上（有 salt，但无 License）。
- **[muchenspace/XyMusic](https://github.com/muchenspace/XyMusic)** 见上（仅歌词）。

### 1.5 Go 生态结论

**可以作为库直接 `go get` 的只有 3 个：**

| 库 | License | 适用场景 | 主要短板 |
|---|---|---|---|
| `github.com/guohuiyuan/music-lib` | **AGPL-3.0** ⚠️ | 聚合搜索/下载，酷狗覆盖最平衡 | AGPL 传染性；仅扫码登录；测试薄 |
| `github.com/lfhy/kugou-music-api` | MIT | 想要"全套酷狗接口" | 2★、Codex 生成、几乎无人验证 |
| `github.com/AynaLivePlayer/miaosic` | MIT | 已经在用 miaosic 的多平台抽象 | 酷狗 provider 是 PARTIAL，有已知 bug |

**其余全部不可 `go get`**（裸 module 名或路径不匹配）：`yourMusic`、`vibe`、`metingio`、`sugarplayer`、`github.com/liuran001/musedl`（fork 后未改）。

**质量最高的参考实现是 `liuran001/MusicBot-Go` 的 `plugins/kugou/`** —— 唯一同时做到：标准版 + 概念版双实现、风控验证码处理、设备注册强制重试、逐词 KRC、~10430 LOC + 9 个测试。代价：**GPL-3.0**，且它是 Bot 插件而非库（但代码可以照着写）。

**通用空白**：Go 生态里**手机号 + SMS 登录**只有 4 个仓库有（`Ion-nsx`、`lfhy`、`yourMusic`、`Aniu456` 代理），而**两个最推荐的库都没有** —— `music-lib` 只有扫码，`miaosic` 只有扫码；`MusicBot-Go` 也只有扫码 + token 续期。**排行榜**基本靠歌单/推荐接口近似，没有真正的 rank 实现。

补充两条路由级细节（VERIFIED）：

- **设备注册路由不是 `/register/devices`**。参考实现 [`MakcRe/KuGouMusicApi`](https://github.com/MakcRe/KuGouMusicApi) `module/register_dev.js` 用的是：
  ```js
  baseURL: 'https://userservice.kugou.com',
  url: '/risk/v2/r_register_dev',
  method: 'POST',
  params: { part: 1, platid: 1, p },
  encryptType: 'android',
  ```
  所有 Go 实现都跟这条。`yourMusic` 用的是 **`/risk/v1/r_register_dev`**（v1，旧版），其余用 v2。
- **短信验证码登录走的是**：`http://login.user.kugou.com/v7/send_mobile_code`（`{businessid:5, mobile, plat:3}`）+ 登录 `loginserviceretry.kugou.com/v7/login_by_verifycode`（`yourMusic` 用 `gateway.kugou.com/v6/login_by_verifycode`）。手机号与验证码要先 AES 加密，响应里 `data.secu_params` 是 AES 密文，解出来才是 token/userid。

---

## PART 2 — Java / Kotlin 中度评估

### 2.0 又一条前提纠正：`rRemix/APlayer` **不是 fork**

简报说它是"某知名播放器的 fork"。`gh api repos/rRemix/APlayer` 返回 `{"fork":false,"parent":null,"source":null}`，最早提交 `036e8494`（2016-05-20，作者本人 rRemix），共 1457 次提交，默认分支 `compose`。**这是原创项目。**

### 2.1 总览表

| 仓库 | ★ | pushed_at | License SPDX | LICENSE 文件 | 形态 | 酷狗 ~LOC | 测试 | 可作依赖 |
|---|---|---|---|---|---|---|---|---|
| [ghhccghk/KuGouApi_Kotlin_SDK](https://github.com/ghhccghk/KuGouApi_Kotlin_SDK) | 11 | 2026-09-06 | MIT | ✅ | **库**（KMP） | **10,194**（42 文件） | ⚠️ 仅 1 个 | ✅ **Maven Central 已发布** |
| [LTLXS/Net-music-KUGOU](https://github.com/LTLXS/Net-music-KUGOU) | **3** | 2026-08-28 | BSD-3-Clause | ✅ | App（MC 模组附加） | **5,666**（17 文件） | ❌ | ❌ 复制粘贴 |
| [AffectionParadise/LightMusic](https://github.com/AffectionParadise/LightMusic) | **357** | 2026-09-08 | Apache-2.0 | ✅ | App（桌面） | 4,340（39 文件） | ❌ | ❌ 复制粘贴 |
| [lladlam/MeloX-Android](https://github.com/lladlam/MeloX-Android) | **133** | 2026-09-12 | GPL-3.0 | ✅ | App | 2,312（12 文件） | ✅ | ❌ |
| [GregTaoo/Concerto](https://github.com/GregTaoo/Concerto) | 31 | 2026-08-14 | **NOASSERTION** ⚠️ | ✅（中文自定义声明） | App（MC mod） | 2,663（11 文件） | ❌ | ❌ 复制粘贴 |
| [hutuyee/AllMusic_Kugou](https://github.com/hutuyee/AllMusic_Kugou) | 2 | 2026-08-29 | MIT | ✅ | App（MC 插件） | 2,492（6 文件） | ❌ | ❌ |
| [88541/YinDong-Music-Android](https://github.com/88541/YinDong-Music-Android) | 14 | 2026-07-25 | MIT | ✅ | App | ~1,641 | ❌ | ❌ |
| [DenvoZonis/java-kgcheckin](https://github.com/DenvoZonis/java-kgcheckin) | 6 | 2026-07-14 | **无** | ❌ **没有** | App（CLI） | ~600（13 文件） | ❌ | ❌ |
| [kukume/tgbot](https://github.com/kukume/tgbot) | 140 | 2025-06-07 | AGPL-3.0 | ✅ | App（TG bot） | 314 | ❌ | ❌ |
| [JamesGZM/Resonote](https://github.com/JamesGZM/Resonote) | 0 | 2026-09-07 | MIT | ✅ | App | ~4,000（network 模块） | ✅ | ❌ |
| [lanfunoe/Gocache](https://github.com/lanfunoe/Gocache) | 1 | 2026-01-11 | MIT | ✅ | App（Spring Boot） | 238 签名 + ~180 文件 | ❌ | ❌ |
| [rRemix/APlayer](https://github.com/rRemix/APlayer) | **1820** | 2026-09-11 | GPL-3.0 | ✅ | App | **447**（4 文件） | ❌ | ❌ |
| [Winnie0408/LocalMusicHelper](https://github.com/Winnie0408/LocalMusicHelper) | **395** | 2026-03-15 | MIT | ✅ | App | 2 处签名 | ⚠️ 模板 | ❌ |
| [OneDongua/KugouLiteProvider](https://github.com/OneDongua/KugouLiteProvider) | 4 | 2026-07-08 | Apache-2.0 | ✅ | App（Xposed） | 220 + krckit | ✅ krckit | ❌（krckit 可源码引用） |
| [ck2246/Kugou-Lite](https://github.com/ck2246/Kugou-Lite) | 0 | 2026-09-12 | **无** | ❌ **没有** | App（Xposed） | 332 | ⚠️ 模板 | ❌ |
| [WoZhiZhan/ConcertoForge](https://github.com/WoZhiZhan/ConcertoForge) | 1 | 2025-12-31 | GPL-3.0 | ✅ | App（MC mod） | ~272 | ❌ | ❌ |
| [czqwq/FMusic](https://github.com/czqwq/FMusic) | 3 | 2026-09-10 | AGPL-3.0 | ✅ | App（MC mod） | 2,594 | ❌ | ❌ |
| [Zeehan2005/AMLL-DroidMate](https://github.com/Zeehan2005/AMLL-DroidMate) | 16 | 2026-09-07 | AGPL-3.0 | ✅ | App | ~90 | ❌ | ❌ 但可抄 |
| [juren233/HyperLyrics-Enhanced](https://github.com/juren233/HyperLyrics-Enhanced) | 56 | 2026-09-11 | GPL-3.0 | ✅ | App（LSPosed） | 285 | ✅ | ❌ |
| [limczhh/LyricInfo](https://github.com/limczhh/LyricInfo) | 43 | 2026-09-02 | Apache-2.0 | ✅ | App | 981 | ❌ | ❌ |
| [ysyhlly/Yukine-android](https://github.com/ysyhlly/Yukine-android) | 16 | 2026-07-27 | **无** | ❌ | App | 1708 | ❌ | ❌ |
| [zhayinggang/ktv-home](https://github.com/zhayinggang/ktv-home) | 152 | 2026-08-01 | MIT | ✅ | App | 112 | ❌ | ❌ |
| [duringk/Examate](https://github.com/duringk/Examate) | 1 | 2026-08-27 | **无** | ❌ | App | ~1,000 | ❌ | ❌ |
| [weixiao888/KuGouLiteVipAutoClaim](https://github.com/weixiao888/KuGouLiteVipAutoClaim) | 5 | 2026-06-28 | **无** | ❌ | App（Xposed） | 小 | ❌ | ❌ |
| [qaz320621/kugou-token-server-android](https://github.com/qaz320621/kugou-token-server-android) | 0 | 2026-08-24 | **无** | ❌ | App | 小 | ❌ | ❌ |
| [9xhk-1/kugou-source](https://github.com/9xhk-1/kugou-source) | 0 | 2026-04-03 | **无** | ❌ | **官方 App 反编译** ⚠️ | 巨大 | — | ❌ **法律风险** |
| [shub39/echo-kugou-extension](https://github.com/shub39/echo-kugou-extension) | 14 | 2025-08-30 | **无** | ❌ | 插件 | **0 盐** | ❌ | ❌ |

### 2.2 能力矩阵

| 仓库 | (a) 签名 + salt | (b) 播放地址（含 VIP） | (c) 登录 | (d) 设备注册 | (e) KRC | (f) 搜索/歌单/专辑/榜 | (g) 概念版 |
|---|---|---|---|---|---|---|---|
| **KuGouApi_Kotlin_SDK** | ✅ **3 套**全 | ✅ `/v5/url`+`/v6/priv_url`+`/tracker/v5/url`，9 档音质 | ✅ 密码/短信/Token/扫码/微信/QQ | ✅ `/risk/v2/r_register_dev` + SSA 风控 | ✅ | ✅ ~200 路由 | ✅ **一等公民**（`isLite` 默认 true） |
| **Net-music-KUGOU** | ✅ **仅 Lite** | ✅ **5 级回退**（privilege_lite→yiting→v5→v6） | ✅ | ✅ | ✅ | ✅ | ⚠️ 纯 Lite（3116） |
| **LightMusic** | ✅ **仅标准** | ⚠️ **自签排最后**，前 3 个是第三方代理 | ❌ | ❌ **没有**（mid/dfid 硬编码） | ✅ | ✅ 很宽 | ❌ |
| **MeloX-Android** | ✅ 仅标准 | ✅ `/v5/url` + 音质降级链 | ✅ 扫码 | ❌ | ✅ | ✅ | ❌ |
| **Concerto** | ✅ 全套（lite 可切） | ⚠️ 有取址逻辑 | ✅ | ✅ | ✅ + Inflater | ✅ | ✅ `kuGouMusicLite` |
| **AllMusic_Kugou** | ✅ 仅标准 | ⚠️ 网页抓取 + 试听判别 | ❌ | ❌ | ✅ | ⚠️ 网页端 | ❌ |
| **Resonote** | ✅ **仅 Lite** + signKey/云盘 | ✅ 播放 + 云盘 | ✅ | ✅ + 风控挑战 | ✅ | ✅ | ⚠️ 纯 Lite |
| **Gocache** | ✅ 标准+Lite（有 bug） | ✅ 两条策略 | ✅ 密码+扫码 | ❌ | ✅ | ✅ | ⚠️ 部分 |
| **java-kgcheckin** | ✅ 标准+Lite | ❌ | ✅ 短信/扫码/Token | ❌ | ❌ | ⚠️ 仅 VIP | ✅ `isLite()` |
| **LocalMusicHelper** | ⚠️ **仅标准，单端点** | ❌ | ❌ | ❌ | ❌ | ⚠️ 仅歌单导入 | ❌ |
| **tgbot** | ✅ Web+Lite | ❌ | ✅ 扫码/密码/短信 | ❌ | ❌ | ❌ | ⚠️ 零散 |
| **YinDong** | ✅ 两处不同来源 | ❌ | ❌ | ❌ | ✅ | ⚠️ 仅歌单解码 | ❌ |
| **APlayer** | ✅ **仅 Lite** | **❌ 没有** | **❌ 没有** | **❌ 没有**（dfid 硬编码 `"-"`） | ✅ 含翻译 | ⚠️ 仅搜索 | ⚠️ 全站 lite，不可切 |
| **KugouLiteProvider** | **没有** | **没有** | **没有** | **没有** | ✅ krckit | **没有** | 靠 Xposed hook |
| **Kugou-Lite** | **没有** | **没有** | **没有** | **没有** | **没有** | **没有** | 仅隐藏 UI Tab |
| **echo-kugou-extension** | **没有** | **没有** | **没有** | **没有** | ✅ | **没有** | ❌ |

### 2.3 `ghhccghk/KuGouApi_Kotlin_SDK` — 唯一真正可当依赖用的库 ✅

KMP 库，4 个 Gradle 模块（`shared` + androidApp/desktopApp/webApp/iosApp），`shared` 的 target 覆盖 **jvm / js / wasmJs / android / iosArm64 / iosSimulatorArm64**。

**规模**：`shared/src/commonMain` **42 个 .kt，10,194 行**，~30 个 Api 模块，**约 200 条路由**。对比参考实现 `MakcRe/KuGouMusicApi` 的 217 个 `module/*.js`，**覆盖面约 90%**。

**签名（VERIFIED）** — `core/RequestSigner.kt` 移植了 helper.js 全部 5 个函数。`KuGouConfig.kt:14-21`：

```kotlin
internal val androidSignatureSalt: String
    get() = if (isLite) "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA" else "OIlwieks28dk2k092lksi2UIkp"
internal val webSignatureSalt: String = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"
```

**Maven Central 发布 —— 我独立核实了，是真的 ✅**

我用 HTTP 直接列了 `repo1.maven.org`（不是读 README）：

```
https://repo1.maven.org/maven2/top/ghhccghk/multiplatform/kugouapi/kugouapi-kmp/
  1.0.0/ … 1.0.5/   ← 6 个版本，最新 2026-09-06
  maven-metadata.xml → groupId top.ghhccghk.multiplatform.kugouapi, latest 1.0.5
```

而且**6 个平台 artifact 都真的发了**：`kugouapi-kmp` / `-android` / `-iosarm64` / `-iossimulatorarm64` / `-js` / `-jvm` / `-wasm-js`。

坐标：
```kotlin
implementation("top.ghhccghk.multiplatform.kugouapi:kugouapi-kmp:1.0.5")
```

> 注意：`search.maven.org` 的 Solr 接口搜 `kugouapi` 返回 `numFound: 0`（索引不全），但 `repo1.maven.org` 的路径列表是权威且直接可见的。**别用 search.maven.org 的 0 结果去否定它。**

**⚠️ 源码里 4 个真实缺陷**（都是移植精度问题，采用前必须知道）：

1. **`signatureWebParams` 丢了 `data` 参数** —— SDK 是 `md5("$salt$paramsString$salt")`，MakcRe 是 ``cryptoMd5(`${str}${paramsString}${data || ''}${str}`)``。**任何 `EncryptType.WEB` 且带 body 的请求都会签错。**
2. **`signKey()` 里漏删了 `println`**，会把 salt 打到 stdout：
   ```kotlin
   val salt = config.signKeySalt
   println("$hash$salt$appid$mid$userid")   // ← 运行时泄露 signKey salt
   ```
3. **`liteClientVersion = 11436`，参考实现是 `11440`**（社区里两个值都在流传：DenvoZonis 用 11436，LTLXS/Resonote/MakcRe 用 11440）。
4. `WX_SECRET` / `WX_LITE_SECRET` 明文硬编码在伴生对象里。

**其他亮点**：`core/PlatformIdentity.kt` 手写十进制大数运算复现官方 mid 推导；`core/Fingerprint.kt` 实现 `generateWebGLHash`/`generateEDTData`/`encryptSid`（RSA-OAEP-SHA256）用于 **SSA 风控挑战**；`RequestExecutor.kt`（19,584 字节）检测 `ssa-code` 响应头自动补 `edt`/`sid`。

**登录**：`AuthApi.kt`（**59,814 字节**，全仓库最大）24 条路由：`/v9/login_by_pwd`、`/v7/send_mobile_code`+`/v7/login_by_verifycode`、`/v5/login_by_token`、`/v2/qrcode`、微信、QQ（含 `hash33` ptqrtoken 算法）、设备管理、验证。

**缺陷**：① 测试**只有 1 个** `FingerprintTest.kt`（5 用例），签名/登录/播放地址**全无测试**；② `shared` 把 **Compose Multiplatform UI 依赖一起打包**（还有 `App.kt`），不是纯 headless 库 —— 引入会带进 Compose；③ `LICENSE` 文件是从参考项目直接复制的，署名还是 `Copyright (c) 2023 MakcRe`；④ **仓库里没有 `.github/`** → 无 CI，发布靠本地手动。

### 2.4 `rRemix/APlayer` — 1820★，但酷狗部分只有 447 行且**缺关键能力**

**酷狗部分真实规模：4 个文件 447 行**（`KuGouClient.kt` 373 / `KuGouDecrypt.kt` 59 / `KuGouProvider.kt` 32 / `KuGouModel.kt` 15）。

`KuGouClient.kt:26`：
```kotlin
private val kgSecret = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"
```
配套 `"appid" to "3116"`、`"clientver" to "11070"` → **纯概念版客户端**（salt 与 appid 自洽，所以能工作），但**不可切换**。

**能力**：
- ✅ 搜索 `complexsearch.kugou.com/v2/search/song`
- ✅ 歌词：`/v1/search` 候选 + `/download` 取 KRC + `krcDecrypt` + 转增强 LRC，**还解析了 `language` base64 标签里的逐行翻译**（做得颇细致）
- **❌ 播放地址：整个文件不存在** `/v5/url` / `/v6/priv_url`
- **❌ 登录：无**
- **❌ 设备注册：无** —— `"dfid" to "-"` 硬编码，注释写 `// 简化处理，必要时可实现 dfid 获取与缓存`
- ⚠️ `mid = md5Hex(System.currentTimeMillis().toString())` —— **每次请求都换 mid**，不持久化，容易被风控

**回答简报的问题「Kotlin 实现是否完整？」→ 不完整，差得远。** **可提取性**：❌ 不可直接提取 —— 耦合 `android.util.Base64`、OkHttp、`org.json`、Timber、`javax.inject`（`@Singleton @Inject`），以及本项目自己的 `remix.myplayer.lyric.LrcParser` 和 `remix.myplayer.lyric.decrypt.KuGouDecrypt`。**核心签名那 ~15 行和 KRC 解密 59 行可以抄，其余不行。**

### 2.5 `GregTaoo/Concerto` — **最值得抄的 Java crypto（272 行单文件）**

`core/.../KuGouMusicApiCrypto.java` 基本是全套，且**只依赖 JDK**：

| 方法 | 内容 |
|---|---|
| `signAndroidParams` | `lite ? "LnT6…" : "OIlwieks…"` + 排序 + data + salt |
| `signWebParams` | `NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt` |
| `signRegisterParams` | `md5("1014" + 排序 values + "1014")` |
| `signKey` / `signParamsKey` | lite ? `185672dd…` : `57ae12eb…` |
| `cryptoRSAEncrypt` | `RSA/ECB/NoPadding` + 128 字节左对齐 → 大写 hex |
| `rsaEncrypt2` | `RSA/ECB/PKCS1Padding` |
| `cryptoAesEncrypt/Decrypt` | AES-CBC/PKCS5 |
| `playlistAesEncrypt/Decrypt` | 6 位随机 key → md5 切 16/16 |
| `decodeLyrics` | **KRC**：跳 4 字节 → 16 字节 `enKey` XOR → `Inflater` |
| 两个 RSA 公钥 | 标准版 + Lite 版都齐 |

`enKey = {64,71,97,119,94,50,116,71,81,54,49,45,206,210,110,105}` —— 就是 `@Gaw^2tGQ61-ÎÒni`。

配套 `KuGouMusicApiClient.java`（**1,198 行**）：`/v5/url`、`/v2/qrcode`、`/v2/get_userinfo_qrcode`、`/v7/login_by_verifycode`、`/v9/login_by_pwd`、`/v5/login_by_token`、`/risk/v2/r_register_dev`、`lyrics.kugou.com`，搜索走 `/v3/search/song` + `/v1/search`（x-router `complexsearch.kugou.com`）。

**⚠️ License 是 `NOASSERTION`** —— `LICENSE` 文件存在但是**中文自定义声明**（"本模组仅为学习性质…"，还限制分发渠道），不是标准协议，GitHub 无法识别 SPDX。**商业使用有法律不确定性。**

**可依赖**：❌。虽然 `build.gradle` 有 `maven-publish`，但发布的是**模组 artifact**（Modrinth/MC），不是可复用库。

### 2.6 `LTLXS/Net-music-KUGOU` — **3★ 但 Java 侧代码量最大（5,666 行）**

**来源（VERIFIED）**：简报猜"可能派生自 `tartaricacid/Net-music`" —— **确认属实，且更准确的说法是上游项目的附加模块**：
- `build.gradle:10` → `group = 'com.github.tartaricacid'`
- `build.gradle:96` → `implementation "maven.modrinth:net-music:1.5.1-neoforge+mc1.21.1"`
- 包名保留上游命名空间 `com.github.tartaricacid.netmusic.kugou`

`util/KuGouSignature.java` —— 作者自己标注了归属：
```java
// Android 签名密钥（酷狗概念版）
public static final String ANDROID_SECRET = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA";  // :13
public static final String WEB_SECRET = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";      // :16
public static final String REGISTER_SECRET = "1014";                              // :19
public static final String KEY_SECRET = "185672dd44712f60bb1736df5a377e82";       // :22
public static final int APPID = 3116;  CLIENTVER = 11440;                          // :25-26
```

**`api/KuGouApiClient.java` 是 2,371 行 / 122KB**，取流做到 **5 级回退**：
```
0. POST /v2/get_res_privilege/lite  （x-router media.store.kugou.com）拉 relateGoods[] 各音质独立 hash
1. /v5/url
2. /v3/yiting/song/info  → magic ppage_id
3. /v6/priv_url POST 兜底（"对应 EchoMusic song_url_new.js"）
4. m.kugou.com/app/i/getSongInfo.php 公开接口兜底
```
还有 `KuGouDeviceRegister.java`（Lite RSA 公钥 + `DeviceInfo(dfid, mid, guid)` 带 `isValid()` 校验）、`KuGouVipApi`、`KuGouMaidLyricCache`、`KuGouAudioStreamHandler`、`kugouvip.kugou.com/v1/get_union_vip`、`usercenter.kugou.com/v1/server_now`。

**额外独有**：`signatureV2` = `md5(排序 k=v + "kgcloudv2").toUpperCase()` —— 用于 trackercdn，**这个变体 MakcRe 里没有**。

**⚠️ 简报的「3 stars, BSD-3-Clause」严重低估了它。这可能是 Java 侧最完整的实现。**

### 2.7 `AffectionParadise/LightMusic` — 357★，表面很宽但有两个定时炸弹

39 个 `Kg*` 文件 **4,340 行**，只读覆盖面很宽（专辑/歌手/评论/MV/歌单/榜单/搜索/歌词）。`sdk/common/builder/KugouReqBuilder.java`：

```java
public static final String appid = "1005";
public static final String clientver = "12569";
private final String pidversec = "57ae12eb6890223e355ccfcb74edf70d";   // :29 标准 signKey
public static final String androidSignKey = "OIlwieks28dk2k092lksi2UIkp"; // :34 标准 salt
public static final String dfid = "-";                                   // :31 硬编码
public static final String mid = "16249512204336365674023395779019";     // :32 硬编码
```
→ **仅标准版，无 Lite 分支。**

**❗ 炸弹 1：签名参数没有排序。** `buildSignParams`（:83-87）直接遍历 `params.keySet()`，而上一行留着**被注释掉的排序**：
```java
//        Map<String, Object> paramsTreeMap = new TreeMap<>(params);   // ← :91 被注释掉
```
它**现在能跑只因为运气** —— 36 个 `Kg*Req` 里只有 14 个传 `TreeMap`。一旦某处传 `HashMap`，签名随机失配。

**❗ 炸弹 2：播放地址优先级是反的。** `KgMusicUrlReq.java` 的真实顺序：
```java
String trackUrl = QqovoKgTrackReq.getInstance().getTrackUrl(hash, quality);  // 第三方代理 1
if (empty) trackUrl = ChkszKgTrackReq...                                     // 第三方代理 2
if (empty) trackUrl = BakaKgTrackReq...                                      // 第三方代理 3
if (empty) trackUrl = KgTrackReqV2...                                        // ← 自签排最后
```
三个代理是 `qqovo.top/api/meting`、`api.chksz.com/api/kugou_music`、`api.baka.plus/meting`。**只有前三个全失败才用自己签名。** 隐私与可用性都有隐患。

**❗ 死代码**：`musicurl/track/kg/deprecated/` 里 **7 个文件全部整份注释掉**（100% 注释行）：`KgTrackReq`、`KgTrackReqV3`、`Ak317KgTrackReq`、`CggKgTrackReq`、`JbsouKgTrackReq`、`TomKgTrackReq`、`XuanluogeKgTrackReq`。**别被文件数骗了。**

❌ 无登录、无设备注册、无 VIP token、无概念版。0 测试。

### 2.8 其余值得单列的

**`lladlam/MeloX-Android`（133★，GPL-3.0）— 标准版 Kotlin 客户端里最完整的**
`core/provider/kugou/` **12 文件 2,312 行**，分层清晰（`KugouApiClient` 406 / `KugouCatalogClient` 302 / `KugouDiscoveryClient` 299 / `KugouLyricsClient` 174 / `KugouRequestClient` 173 / …）。`KugouRequestClient.kt`:
```kotlin
const val AppId = 1005;  const val ClientVersion = 20489
private const val AndroidSignatureSalt = "OIlwieks28dk2k092lksi2UIkp"
```
音质 128/320/flac/high 带**降级链**（HiRes→High→Standard 逐级回退并记录 `actualTier`）。但有 `KugouKrcLyricsParserTest.kt`。**❌ 无设备注册、无 `viper_*`、无 VIP。** 比 APlayer 完整得多。

**`JamesGZM/Resonote`（0★，MIT）— 算法上最严谨的 Kotlin 实现，纯概念版**
`ApiProtocolConfig.kt`（14 行）：`APP_ID = "3116"` / `ANDROID_SIGNATURE_SALT = "LnT6xpN3…"` → **纯概念版，自洽**（简报说它"出现在 Lite-salt 搜索里"，那是**正确用法**不是错配）。
`ApiRequestSigner.kt` 用 `MessageDigest.update()` 分段喂字节，**支持二进制 body**，完全对齐 MakcRe 对 Buffer 的处理。含 `signSongKey`（`185672dd…`）、`signCloudKey`（`ebd1ac31…` + `musicclound` 前缀 + pid 20026）。设备注册 `/risk/v2/r_register_dev` **且有测试断言该路径**。还有 `risk/ApiRiskChallengeDetector.kt` 等风控处理。**仓库共 350 个测试文件。这份 `protocol/` 包（~2,000 行）是全 Kotlin 生态设计最干净的。**

**`lanfunoe/Gocache`（1★，MIT）— 骨架不小（~180 文件）但签名层有 bug**
`util/SignatureUtils.java`（103 行）常量齐全（含 `SIGN_CLOUD_KEY`、`SIGN_KEY_NORMAL`/`SIGN_KEY_LITE`）。**❗ 两个 bug**：① `signParamsKey(data, appid, clientver)` **无条件是标准 salt**，缺 Lite 分支（:47-53）→ 概念版走这条会签错；② `SIGN_KEY = "R6snCXJgb…"` 声明后**从未被引用**，命名与实际用途不符。

**`OneDongua/KugouLiteProvider`（4★，Apache-2.0）— 没有签名，是 Xposed hook**
**0 处 salt 命中。** 真实机制：YukiHookAPI 注入**酷狗概念版自己**（`processName.endsWith(":support")`），hook `MediaSession.setPlaybackState`/`setMetadata`，转发给 Lyricon。**不联网请求酷狗 API，完全寄生在已安装的 App 上。**
有价值的副产品是 **`share/krckit`** 模块（`KrcDecryptor` / `KrcParser` / `KrcDocument` / `Language`），**带真测试**（`KrcParserTest.kt` + 真实 `.krc` 夹具 `黄梅戏.krc`、`極楽浄土.krc`）—— **全生态 KRC 解析做得最完整的一份 Kotlin 代码**。无 `maven-publish`，只能源码引用。

**`ck2246/Kugou-Lite`（0★，无 License）— 空壳**
**没有签名。** 唯一文件 `KugouTabHook.kt`（332 行）作用是**隐藏酷狗 App 底部「视频/K歌/福利」三个 Tab**。`created_at` 13:38:51 → `pushed_at` 14:06:16，**28 分钟**；2 个测试都是 Android Studio 默认模板。无 License = 默认保留所有权利，**不要用**。

**`DenvoZonis/java-kgcheckin`（6★，无 License）— 小而干净**
`util/Config.java` 是全仓库最清晰的"标准 vs 概念版"对照表（`ANDROID_SECRET` / `LITE_ANDROID_SECRET` / `SIGN_KEY_SECRET` / `LITE_SIGN_KEY_SECRET`），`isLite()` 读环境变量 `platform` —— 与 MakcRe 的 `process.env.platform` 完全一致。✅ 短信/扫码/Token 续期，两套 AES key（`90b8382a1bb4ccdcf063102053fd75b8` 标准 / `c24f74ca2820225badc01946dba4fdf7` lite）。**❌ 无设备注册、无播放地址、无歌词。无 LICENSE 文件。**

**`Winnie0408/LocalMusicHelper`（395★，MIT）— 它**确实**是真签名器**
> 简报猜"likely just KGM/KGMA file decryption, unrelated to API signing" —— **这个猜测不成立。** 我读了 `ConvertPage.kt` 里 salt 的完整上下文：
```kotlin
3 -> "https://gateway.kugou.com/pubsongs/v4/get_other_list_file"   // 分支 3 = 酷狗
val signature = Tools().md5(
    input = "OIlwieks28dk2k092lksi2UIkp${getParams.replace("&","")}OIlwieks28dk2k092lksi2UIkp")
request.url("${url}?signature=${signature}&${getParams}")
```
用的是**标准版 salt**，模块 `CloudMusic`。**没有 KGM/KGMA 解密代码** —— 那些 "kugou" 命中是 `SourceApp` 枚举名、`res/drawable/kugou.xml` 图标、`assets/kugou_music_phone_v7.db`。
**⚠️ 但写法很脆**：`getParams.replace("&","")` 直接删 `&` 而不排序 —— 结果正确**纯粹因为参数串是手工按字母序写死的**。插一个参数就签错。范围极窄（只做歌单导入）。

**`9xhk-1/kugou-source`（0★，无 License）— 官方 App 反编译，作为**证据**用**
6,919 个文件，含混淆包 `e/c/a/g/a/f/e/b.java`。其 `datacollect/feedback/DexFeedBackActivity.java` 用标准 salt 拼接，`UpdateDeviceFingerProtocol` 含 `/risk/v2/r_register_dev`。**这是 salt 极性的地面真值来源 —— 但它也佐证了错误的标注从何而来（社区把 Lite 视图当成了标准版）。** ⚠️ **反编译专有软件，法律风险高，只做证据，不要抄。**

**`hutuyee/AllMusic_Kugou`（2★，MIT）**：`KugouCrypto.java:14` `ANDROID_SIGN_SALT = "OIlwieks…"`。**⚠️ 移植偏差**：`androidSignature` 只拼 `salt + 排序 k=v + salt`，**没有 `data` 项** → 只能安全用于 GET。播放地址靠**网页端 JSON 抓取** + 试听片段判别（`isKnownTrialUrl` / `isClearlyFullAudioUrl` / 优先 `/full/`），20 秒缓存。启发式脆弱，但作者对"VIP 只能拿试听"是有意识的。
> **许可链提醒**：`czqwq/FMusic`（AGPL-3.0，created 2026-08-22）与它有**完全相同的 6 个文件名**且常量/方法名逐一对得上。**hutuyee 更早**（07-19），所以更像是 FMusic 取自 hutuyee **并把 MIT 改成了 AGPL-3.0**。

**`kukume/tgbot`（140★，AGPL-3.0）— 自己签名，不转发外部服务**
```kotlin
private fun signature2(...) = signature("NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt", map, other)  // :71 Web
private fun signature3(...) = signature("LnT6xpN3khm36zse0QzvmgTZ3waWdRSA", map, other)  // :75 Lite
```
还有**第三套** `signature(map)` = `md5(排序后的纯值拼接)`，**无 salt**，用于扫码 URL。✅ 扫码/密码/短信登录 + 音乐人签到 + 听歌领 VIP。**❌ 无设备注册**（`dfid` 全写 `"-"`）· ❌ 无播放地址 · ❌ 无歌词。

**`88541/YinDong-Music-Android`（14★，MIT）— 两套互不相干的部分实现**
`LxSdkSongList.kt` 是 **lx-music（洛雪）自定义源 JS 插件的 Kotlin 移植**（注释锚定"与 kg/util.js signatureParams 完全一致"，用**标准 salt** 做 `t.kugou.com/v1/songlist/batch_decode`）；而 `kotlin/com/lyrics/api/provider/KugouProvider.kt` 用 **Lite salt** 做歌词。**两处 salt 分属不同来源，不要当成一个客户端看。**

**`Zeehan2005/AMLL-DroidMate`（16★，AGPL-3.0）— 最干净的可独立抄的 Kotlin 签名工具**
`KugouSignature.kt` ~90 行，零耦合：`KUGOU_ANDROID_SALT="OIlwieks…"` / `APP_ID=1005` / `CLIENT_VER=12569` / `generateSignature`=md5(salt+排序+body+salt) / `generateDeviceMid`=md5("-")。

**`ysyhlly/Yukine-android`（16★，无 License）**：`LocalLuoxueStreamingClient.kt`（1708 行）是**洛雪脚本宿主** —— QuickJS 跑导入的 LX JS 源，内置 `"kg"` 处理器。**引擎式方案，非自研客户端。**

**`juren233/HyperLyrics-Enhanced`（56★，GPL-3.0）**：`SIGNING_SECRET = "OIlwieks…"`，签名**无 body**，仅歌词。**✅ 有酷狗专属单测** `KugouSourceTest.kt`。

**`shub39/echo-kugou-extension`（14★，无 License）**：**0 处 salt 命中 → 它不自己签名。排除。**

### 2.9 Java/Kotlin 结论

**唯一可以直接当依赖用的只有 `ghhccghk/KuGouApi_Kotlin_SDK`**（已上 Maven Central，~200 路由、三套 salt、含设备注册与 SSA 风控）。其余全部是**应用内嵌实现**，最多复制粘贴 —— 但有几份单文件 crypto 值得抄。

| 需求 | 推荐 | 理由 |
|---|---|---|
| 直接引入依赖 | **`ghhccghk/KuGouApi_Kotlin_SDK`** | 唯一上 Maven Central；注意会带进 Compose 依赖，且 §2.3 那 4 个缺陷要自己规避 |
| 抄一份 Java crypto（Signing+RSA+AES+KRC 全套） | **`GregTaoo/Concerto` 的 `KuGouMusicApiCrypto.java`** | 272 行单文件、只依赖 JDK、标准+Lite 双支持。**本次最优** |
| 抄一份最小 Kotlin 签名 | **`Zeehan2005/AMLL-DroidMate` 的 `KugouSignature.kt`** | ~90 行、零耦合 |
| 抄完整 Kotlin 骨架（标准版） | **`lladlam/MeloX-Android` `core/provider/kugou/`** | 2,312 行、分层清晰；GPL-3.0 |
| 抄完整 Kotlin 骨架（概念版） | **`JamesGZM/Resonote` `core/network/protocol/`** | 算法最严谨、含风控、有测试；纯 Lite |
| 参考 KRC 解析 | **`OneDongua/KugouLiteProvider` 的 `share/krckit`** | 唯一带真实 `.krc` 夹具测试 |
| **不要用** | `ck2246/Kugou-Lite`（空壳+无证）、`duringk/Examate`、`shub39/echo-kugou-extension` | — |

**必须记住三条**：
1. salt 映射按 §0.1（`OIlwieks28…`=标准版，`LnT6xpN3…`=概念版）。
2. 设备注册是 `/risk/v2/r_register_dev`，不是 `/register/devices`。
3. **判断一个库"支持概念版"要看 appid，不是看有没有那个 salt 字符串** —— APlayer/LTLXS/Resonote 用了 `LnT6…`，是因为它们**本来就是纯概念版客户端**（appid 3116），不是"支持双版本"。

---

## PART 3 — Python / PHP / Dart / C++ / Swift 轻量扫描

### 3.1 总表

分类：**FULL** = 本仓库自带盐 + 签名 + 取流；**PARTIAL** = 有签名但缺取流/登录，或只做歌词；**WRAPPER** = 只转发第三方服务；**UNRELATED** = 与酷狗 API 无关。

| 仓库 | ★ | pushed_at | License | 语言 | 分类 | 盐 | a | b | c | d | e | f | g |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [CharlesPikachu/musicdl](https://github.com/CharlesPikachu/musicdl) | 6093 | 2026-09-12 | NOASSERTION | Python | **FULL** | 标准+Lite+web | ✅ | ✅ VIP | ❌ cookie | ✅ v2 | ✅ | ✅ | ✅ |
| [MeoProject/lx-music-api-server](https://github.com/MeoProject/lx-music-api-server) | 844 | 2026-01-10 | MIT | Python | **FULL** | Lite+web | ✅ | ✅ | ⚠️ 仅 token | ✅ | ✅ | ⚠️ 无 search | ✅ |
| [chenmozhijin/LDDC](https://github.com/chenmozhijin/LDDC) | 1774 | 2026-09-12 | GPL-3.0 | Python | PARTIAL | 标准+`1014` | ✅ | ❌ | ❌ | ✅ v1 | ✅ | ✅ | ❌ |
| [zzyoxml/md3Music](https://github.com/zzyoxml/md3Music) | 317 | 2026-09-05 | AGPL-3.0 | Dart+Rust | **FULL** | Lite+标准+web | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| [XIaodou0416/Beans-Music](https://github.com/XIaodou0416/Beans-Music) | 349 | 2026-09-12 | MIT | **Swift** | **FULL** | Lite+标准+web+signKey | ✅ | ✅ | ✅ 扫码 | ✅ v2 | ⚠️ 无 KRC | ✅ | ✅ |
| [Sen0E/Battery-Music](https://github.com/Sen0E/Battery-Music) | 0 | 2026-05-10 | AGPL-3.0 | Dart | **FULL** | Lite+标准+`1014` | ✅ | ✅ priv_url | ✅ 手机/短信/扫码/微信 | ✅ | ✅ | ✅ | ✅ |
| [KevinllBin/CyShineMusic](https://github.com/KevinllBin/CyShineMusic) | 46 | 2026-09-11 | MIT | Dart | 混合 | 标准 | ✅ | ⚠️ 仅音质元数据 | ❌ | ❌ | ✅ | ✅ | ❌ |
| [kuilei0926/FnMusicEnhance](https://github.com/kuilei0926/FnMusicEnhance) | 36 | 2026-09-06 | 无 | Python | PARTIAL | Lite | ✅ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ |
| [Superheroff/musicapi](https://github.com/Superheroff/musicapi) | 209 | 2026-05-26 | MIT | Python | PARTIAL | 标准 | ✅ | ❌ | ❌ | ❌ | ❌ | ⚠️ 仅歌单 | ❌ |
| [Tangmjiu/NGS-KG](https://github.com/Tangmjiu/NGS-KG) | 15 | 2026-08-21 | MIT | Dart | PARTIAL | Lite+标准+`1014` | ✅ | ❌ 走外部服务端 | ✅ | ✅ | ✅ | ✅ | ✅ |
| [WisteriaZy/lyricGeter](https://github.com/WisteriaZy/lyricGeter) | 7 | 2026-07-06 | MIT | Python | PARTIAL | Lite | ✅ | ❌ | ❌ | ❌ | ✅ | ⚠️ 仅搜索 | ✅ |
| [Hjdd14/Mconnect-Music_connect](https://github.com/Hjdd14/Mconnect-Music_connect) | 7 | 2026-09-08 | MIT | Dart | **FULL** | Lite+标准+`1014` | ✅ | ✅ v5+priv_url | ✅ 手机/短信/扫码 | ✅ | ✅ | ✅ | ✅ |
| [lisalee23042605/kgmusicbot](https://github.com/lisalee23042605/kgmusicbot) | 4 | 2026-08-16 | NOASSERTION | Python | PARTIAL | Lite | ✅ | ✅ 概念版 Hi-Res | ✅ 扫码 | ✅ | ✅ | ✅ | ✅ |
| [bamboostrip/shiyin-music](https://github.com/bamboostrip/shiyin-music) | 4 | 2026-09-12 | 无 | Dart+Rust | **FULL** | Lite+标准+web+V5 | ✅ | ✅ V5 | ✅ | ✅ | ✅ | ✅ | ✅ |
| [06xy/HenkMusic](https://github.com/06xy/HenkMusic) | 1 | 2026-07-18 | 无 | Dart | PARTIAL | Lite+标准 signKey | ✅ | ⚠️ 仅 128k | ✅ 手机+短信 | ❌ | ✅ | ✅ | ✅ |
| [huqiu0313/AudioToLyrics](https://github.com/huqiu0313/AudioToLyrics) | 1 | 2026-09-05 | 无 | Python | PARTIAL | 标准 | ✅ | ❌ | ❌ | ❌ | ✅ | ⚠️ 仅搜索 | ✅ |
| [qingyueyin/Pure-music](https://github.com/qingyueyin/Pure-music) | — | — | — | Dart | PARTIAL | Lite+`1014` | ✅ | ❌ | ❌ | ✅ v1 | ✅ | ⚠️ 仅搜索 | ✅ |
| [DreamlingBig/kugou](https://github.com/DreamlingBig/kugou) | 1 | 2024-04-17 | 无 | PHP | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| [ELDment/Meting-Fixed](https://github.com/ELDment/Meting-Fixed) | 492 | 2025-10-15 | MIT | PHP | WRAPPER | **无盐** | ❌ | ⚠️ 2015 老接口 | ❌ | ❌ | ❌ | ✅ | ❌ |
| [BsaLee/bodian_music_api](https://github.com/BsaLee/bodian_music_api) | 3 | 2023-10-30 | 无 | PHP | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| [aeagean/QtKugouApi](https://github.com/aeagean/QtKugouApi) | 5 | 2019-12-24 | GPL-3.0 | C++ | WRAPPER | ❌ | ❌ | ⚠️ 老公开接口 | ❌ | ❌ | ❌ | ❌ | ❌ |
| [XSong1205/BetterBDM](https://github.com/XSong1205/BetterBDM) | 1 | 2026-04-26 | 无 | C++ | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| [Jerry-Z07/bodianhelper](https://github.com/Jerry-Z07/bodianhelper) | 1 | 2026-07-31 | Apache-2.0 | C++ | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| [gee1k/sonimbus](https://github.com/gee1k/sonimbus) | 20 | 2026-09-11 | LGPL-3.0 | Swift | UNRELATED | ❌ 酷狗 | ❌ | ✅ 波点 | ✅ 波点 | ❌ | ✅ 波点 | ❌ | ❌ |
| [Yudaotor/lyrimuse](https://github.com/Yudaotor/lyrimuse) | 42 | 2026-09-12 | GPL-3.0 | Swift+**Go** | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ |
| [VZService/getmusic](https://github.com/VZService/getmusic) | 2 | 2026-08-25 | MIT | Python | WRAPPER | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| [MoeclubM/PyBodian](https://github.com/MoeclubM/PyBodian) | 1 | 2026-05-29 | MIT | Python | UNRELATED | ❌ 酷狗 | ❌ | ✅ 波点 | ✅ 波点 | ❌ | ✅ 波点 | ❌ | ❌ |
| [ouzking/Bodian](https://github.com/ouzking/Bodian) | 0 | 2023-04-11 | 无 | Python | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| [CiyLei/flutter_kugou](https://github.com/CiyLei/flutter_kugou) | 12 | 2023-03-22 | — | Dart | UNRELATED | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |

列 a–g 同 PART 1 的定义。

### 3.2 Python — **有**，两个真正可用

**① [`CharlesPikachu/musicdl`](https://github.com/CharlesPikachu/musicdl)（6093★）—— 本次全语言评估中功能最完整的单一实现。**

`musicdl/modules/utils/kugouutils.py:31-33` 三套盐全在源码里，且 `getsongurl()` 真打 tracker：

```python
params = {..., "version": 11436,
          "page_id": 151369488 if not IS_LITE else 967177915,
          "quality": quality, "pid": 2 if not IS_LITE else 411, "cmd": 26, ...}
return KugouMusicClientUtils.sendrequest(session, "GET", "/v5/url", params=params,
    headers={"x-router": "trackercdn.kugou.com"}, encrypt_type="android", encrypt_key=True, ...)
```

音质链 `('viper_tape','viper_clear','viper_atmos','flac','high','320','128')` 覆盖 VIP；带 `registerdevice()`（AES + RSA PKCS1 → `risk/v2/r_register_dev`）和 **11 个第三方兜底 API**。是仅有的两个 b 级（VIP/无损）实现之一。

⚠️ 小 bug：`kugouutils.py:143` 的 `json.loads(text) if (text := ...)` 在某分支会引用未绑定的 `text`。

**② [`MeoProject/lx-music-api-server`](https://github.com/MeoProject/lx-music-api-server)（844★）—— 原生 Python 签名，不是委派。**

`utils/platform/kg/__init__.py` 只有 73 行但完整：

```python
def sign(params, body=""):
    if isinstance(body, dict): body = json.dumps(body)
    params = sort_dict(params); params = build_signature_params(params, body)
    return create("OIlwieks28dk2k092lksi2UIkp" + params + "OIlwieks28dk2k092lksi2UIkp")

def getKey(hash_, user_info):
    return create(hash_.lower() + "57ae12eb6890223e355ccfcb74edf70d" + "1005" + tools["mid"] + user_info["userid"])
```

`modules/url/kg.py` 走 `tracker.kugou.com/v5/url`；`modules/lyric/kg.py` 自带完整 KRC 解密器（16 字节 XOR key + `zlib.decompress`）；`modules/refresh/kg.py` 实现 `login_by_token` 的 AES-CBC + RSA 裸模幂。**缺 search 模块**（搜歌靠 LX Music 客户端传 hash），**没有**手机/短信/扫码登录。

`nanci0406/lx-api-server` 是同一代码库的 fork（旧目录结构），**不是独立实现**。

**仅歌词（PARTIAL）**：`WisteriaZy/lyricGeter`、`huqiu0313/AudioToLyrics`、`kuilei0926/FnMusicEnhance`、`qingyueyin/Pure-music`。

其中 **`chenmozhijin/LDDC`（1774★）签名写得最规范** —— `LDDC/core/api/lyrics/kg.py:148-155` 用标准盐，`:74-83` 用 `1014` 走 `risk/v1/r_register_dev` 拿 dfid，能力覆盖 search(song/album/songlist) + get_songlist + get_lyrics + get_lyricslist，并接 `decryptor/krc_decrypt` → `parser/krc.py`。**大量下游（`lyricGeter`、`AudioToLyrics`、`my-name-is-not-available/vercel-LDDC-api-python`、`HengXin666/HX-Music`、`sitiyou/layrics`、`ZhuHuan521/SMTCLyrics`）都是抄它的** —— 请按「LDDC 系」整体看，不要当独立发现计数。

`WisteriaZy/lyricGeter` 的 `docs/KUGOU_KRC_IMPLEMENTATION.md` **值得单独表扬**：完整记录了签名公式、XOR 密钥 `@Gaw^2tGQ61-Íni`、以及 KRC 逐词时间戳用相对偏移的语义 —— 是全网少见的把方案写清楚的文档。

**WRAPPER / 无关**：`VZService/getmusic`（三个平台全是 `https://a.aa.cab/*.music`，纯转发）；`MoeclubM/PyBodian`（波点，非酷狗）；`ouzking/Bodian` —— **是个法语记账脚本**（`gestion_budget.py` + `budget.db`），与音乐完全无关。

**Fork 关系（别重复计数）**：`Everest-CN/musicdl`、`seanzjxgit/MusicDL_UI` 是 `CharlesPikachu/musicdl` 的 fork。`Superheroff/musicapi` 与 `coyoteXujie/vibe-music` 只到歌单签名（`md5(salt + sorted(query) + salt)`）。

### 3.3 PHP — **没有**

三个候选全部落空：

- [`BsaLee/bodian_music_api`](https://github.com/BsaLee/bodian_music_api)：`index.php`（签到表单 + MySQL）+ `db.php`（硬编码库密码）+ `worker.js`。worker.js 只打两个**波点**接口，**零音乐/播放/签名代码**。
- [`DreamlingBig/kugou`](https://github.com/DreamlingBig/kugou)：`com.kugou.android/api.php` 里 `class 酷狗音乐` 带一个**空方法** `reconfiguration_signature($url) {}`；`com.kugou.android.lite/api.php` **是空文件**。骨架都算不上。
- [`ELDment/Meting-Fixed`](https://github.com/ELDment/Meting-Fixed)（492★，PHP 侧规模最大，1286 行）：走**老一代无签名公开接口**。`src/Meting.php:556` 用 `media.store.kugou.com/v1/get_res_privilege`，播放地址解码器 `kugou_url()`（:992-1030）用 `'key' => md5($vo['hash'].'kgcloudv2')` 打 `trackercdn.kugou.com/i/v2/` —— 这是 **2015 年前后的做法，与 `OIlwieks/LnT6` 盐体系完全无关**。真正的 provider 上游已迁到 JS（`metowolf/Meting::src/providers/kugou.js`）。

**我的 code search 独立确认**：`language:php` 下两个盐均 **0 命中**。**PHP 里不存在带盐的酷狗签名实现。**

### 3.4 Dart / Flutter — **有，而且是最活跃的一档**

**FULL 五个：**

| 仓库 | 亮点 |
|---|---|
| [`bamboostrip/shiyin-music`](https://github.com/bamboostrip/shiyin-music) | `rust/src/kugou/signer.rs` 225 行，**9 个签名单测**，明确区分 `calc_post_signature` / `calc_v5_key` / `calc_web_qr_signature` / `calc_login_key` / `calc_official_key` / `calc_cloud_key`。**全语言里唯一带正经签名测试的实现**。自述 1:1 对应 .NET 的 `KGSigner.cs`（即 C# 上游 `Linsxyx/KugouMusic.NET`） |
| [`zzyoxml/md3Music`](https://github.com/zzyoxml/md3Music)（317★） | Rust `helper.rs:4-10` 四常量齐全，`modules/` 下 song_url/login/register_dev/lyric/search/rank/playlist/album/user/verify 全在，配 `device.rs` / `simulate.rs`。**概念版方向最完整的实现** |
| [`Sen0E/Battery-Music`](https://github.com/Sen0E/Battery-Music) | `plugin/kugou_music_api_dart/` 是 **MakcRe/KuGouMusicApi 的接近逐文件 Dart 移植** —— 22 个模块（album/artist/comment/device/everyday/fm/images/login/longaudio/lyric/play_history/playlist/rank/scene/search/sheet/song/theme_music/top/user/video/yueku）+ `crypto_util.dart` 内含 KRC XOR+ZLib。`song.dart` 的 `songUrlNew()` 打 `/v6/priv_url` 并一次请求 `viper_atmos/viper_tape/viper_clear/multitrack`；`login.dart` **1007 行**含手机/短信/扫码/微信 |
| [`Hjdd14/Mconnect-Music_connect`](https://github.com/Hjdd14/Mconnect-Music_connect) | 884 行单文件，`getSongPlaybackUrl()` 双客户端 switch + `getSongPrivatePlaybackUrl()` 打私链 |
| [`Tangmjiu/NGS-KG`](https://github.com/Tangmjiu/NGS-KG) | `lib/services/kugou_signer.dart` 192 行，**最干净的单文件签名器**，7 个方法逐一对应 helper.js，注释写明"移植自 MakcRe/KuGouMusicApi"。但它自己不发播放请求（走可配置服务端）→ 记 PARTIAL |

**PARTIAL**：`06xy/HenkMusic`（Lite 盐 + `185672dd…` signKey，**有真·手机短信登录** `/v7/send_mobile_code` + `/v7/login_by_verifycode`，但播放只要 `'quality': 128`）；`qingyueyin/Pure-music`。

**混合**：`KevinllBin/CyShineMusic` —— 自带一套只到音质元数据的 KG SDK（`kg_sdk.dart` 里只有 `search / _getQualityDetails / getPicUrl / getLyric`，**没有** `resolveSongUrl`），播放地址靠 `JavaScriptCore` 加载外部 JS 音源（`assets/music_source_preload.js`）。

**UNRELATED**：`qyo123oyq/bodian-music`、`guqlule/bodian-music` 是**波点**不是酷狗；`CiyLei/flutter_kugou`（2023，UI 仿写）。

> **一处纠正**：`qingyueyin/Pure-music` 的 `lib/native/rust/api/kg.dart` **不是** Rust FFI 薄绑定，而是 **363 行纯 Dart**（`dart:io` HttpClient + `package:crypto`），自己带盐、自己注册设备、自己解 KRC。其 `rust/Cargo.toml` 里**没有任何 kugou 模块**（只有 tag_reader / smart_sort / library_db 之类）—— 目录名有 `native/rust` 只是因为同目录放了别的绑定。

### 3.5 C++ — **没有**

两个被点名的 Bodian 项目与 HTTP API 无关：

- [`XSong1205/BetterBDM`](https://github.com/XSong1205/BetterBDM)：`injector/` + `core/hook_manager.cpp` + `include/plugin_api.h`（`PluginManifest`/`HookInfo`/`HWND`）—— **DLL 注入 + 窗口消息 hook** 框架，全树 0 处 URL/签名。
- [`Jerry-Z07/bodianhelper`](https://github.com/Jerry-Z07/bodianhelper)：**DLL 代理替换**。README 明说交付 `libmpv-2.dll`（代理）+ `libmpv_real.dll`（原名重命名），`src/proxy/mpv_proxy.cpp` 只做 mpv client API 转发 + SMTC 集成，`bridge_core.cpp:234` 甚至去读波点日志目录 `cn.wenyu.bodian/bodian_pc/bdlog`。

唯一带 HTTP 的是 [`aeagean/QtKugouApi`](https://github.com/aeagean/QtKugouApi)（2019，GPL-3.0）：`WebApi.cpp:28/42` 打 `mobilecdn.kugou.com/api/v3/search/song` 和 `m.kugou.com/app/i/getSongInfo.php`，后者 `cmd=playInfo&hash=…` 直接返回 play_url —— **零签名，老公开接口**。

**我的 code search 独立确认**：`language:cpp` 下两个盐均 **0 命中**。**C++ 里不存在酷狗签名实现。**

### 3.6 Swift — **有一个大实现**

[`XIaodou0416/Beans-Music`](https://github.com/XIaodou0416/Beans-Music)（349★，MIT，`Beans/KugouMusicAPI.swift` **2085 行单文件**）：

- **确认是 Swift，不是 Objective-C** —— `import Foundation / Security / UIKit`，`final class`、async/await、`URLSession`。
- **四套盐全在**（:14-22）：`upstreamSignSalt = "OIlwieks28dk2k092lksi2UIkp"`（配 `upstreamAppID = "1005"` / `upstreamClientVersion = "20489"`）、`androidSignKey = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA"`（配 `appid = "3116"` / `clientver = "11440"`）、`webSignKey = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"`、`playSignSalt = "57ae12eb6890223e355ccfcb74edf70d"`。
- **三个签名函数并存**（:1709-1722）：`androidSignature`、`upstreamAndroidSignature`、`webSignature`。
- 能力面很宽：`qrKey()`/`pollQR()` 扫码登录、`registerDevice()`（:1508-1559 完整 AES-CBC + RSA PKCS1 + `risk/v2/r_register_dev` 往返并解出 dfid）、`songURL()/songURLV5Once()/songURLWebOnce()/upstreamSongURLOnce()` **四级播放取址**、搜索/歌手/专辑/榜单/歌单/FM/评论/云盘。
- ⚠️ **唯一短板**：`lyric()`（:1337-1363）请求 `fmt=lrc` 直接 base64 解码，**没有 KRC 解密**。

[`gee1k/sonimbus`](https://github.com/gee1k/sonimbus)（20★，LGPL-3.0）：`UnblockService.swift` 是 **NetEase 客户端的换源兜底**（`automaticRetrySources = [Source.bodian]`），打 `bd-api.kuwo.cn/api/play/music/v2/audioUrl`（`kuwotest` 系签名）。**0 处酷狗盐** → 与酷狗无关。

[`Yudaotor/lyrimuse`](https://github.com/Yudaotor/lyrimuse)（42★）：酷狗歌词实现在 **Go** 侧（`lyrimuse-collector/kugou.go`），且带 `testdata/lyricsgolden/` 金标准用例；Swift 侧只是 macOS UI。

其余 Swift 仓库（`Darren-chenchen/KuGou_swift`、`ydoily/Kugoudemo`、`yehkong/Imitate-Kugou-App`、`FMYang/KGDownloader`、`hellomyonly55/CloudLyrics-for-macOS`）为 UI 仿写或本地文件处理。**没有。**

### 3.7 「拿来就能用」推荐排序（Python / Dart / Swift 方向）

1. **Python 下载/聚合**：[`CharlesPikachu/musicdl`](https://github.com/CharlesPikachu/musicdl) —— 唯一同时有标准版 + 概念版 + VIP 音质链 + 设备注册 + 第三方兜底（6093★，活跃）。缺点：是下载器不是 API server，且无登录。
2. **Python 自建服务**：[`MeoProject/lx-music-api-server`](https://github.com/MeoProject/lx-music-api-server) —— 代码量小、MIT、原生签名、KRC 完整，适合当骨架；需自己补 search。
3. **Rust 引擎**：[`bamboostrip/shiyin-music`](https://github.com/bamboostrip/shiyin-music) —— 唯一有签名单测，四种签名策略注释最清楚。
4. **Swift/iOS**：[`XIaodou0416/Beans-Music`](https://github.com/XIaodou0416/Beans-Music) —— 2085 行覆盖登录到播放到评论，缺 KRC。
5. **Dart**：[`Sen0E/Battery-Music`](https://github.com/Sen0E/Battery-Music) 的 `plugin/kugou_music_api_dart` —— 移植完整度最高，可直接当 pub 包用（虽然宿主 App 0★）。

**不建议**：任何 PHP（**没有**）、任何 C++（**没有**）、`VZService/getmusic`（转发 `a.aa.cab`）、`ouzking/Bodian`（记账脚本）、`DreamlingBig/kugou`（空文件）。

---

## 波点音乐（Bodian，酷我旗下，`bd-api.kuwo.cn`）

### 核心结论：**「无需签名」这个说法是错的**

正确的表述是：**波点没有全局强制签名；读取类接口确实只要 headers；但取播放地址和权限校验必须带 MD5 签名。**

salt 是明文固定串 `kuwotest`。算法（逐字引自 [`UnblockNeteaseMusic/server`](https://github.com/UnblockNeteaseMusic/server) `src/provider/bodian.js` L30-42）：

```js
const generateSign = (str) => {
	const url = new URL(str);
	const currentTime = Date.now();
	str += `&timestamp=${currentTime}`;
	const filteredChars = str
		.substring(str.indexOf('?') + 1)
		.replace(/[^a-zA-Z0-9]/g, '')
		.split('')
		.sort();
	const dataToEncrypt = `kuwotest${filteredChars.join('')}${url.pathname}`;
	const md5 = crypto.md5.digest(dataToEncrypt);
	return `${str}&sign=${md5}`;
};
```

**被 5 个不同语言的独立实现复现**（这是我判定它真实的依据）：

| 实现 | 语言 | 位置 |
|---|---|---|
| [UnblockNeteaseMusic/server](https://github.com/UnblockNeteaseMusic/server/blob/HEAD/src/provider/bodian.js) | JS | 最权威参考 |
| [CharlesPikachu/musicdl](https://github.com/CharlesPikachu/musicdl/blob/HEAD/musicdl/modules/sources/bodian.py) | Python | `_signquery()`，带 body 时再拼 `md5(f"{body}kuwotest")` |
| [gee1k/sonimbus](https://github.com/gee1k/sonimbus/blob/HEAD/Sonimbus/Core/UnblockService.swift) | Swift | `Insecure.MD5` |
| [Fantasy-XY808/Soto-Player-Community](https://github.com/Fantasy-XY808/Soto-Player-Community/blob/HEAD/electron/main/apis/unblock/bodian.ts) | TS | 移植自 UNM |
| [MoeclubM/PyBodian](https://github.com/MoeclubM/PyBodian/blob/HEAD/bodian_toolkit.py) | Python | `_request()` L497-498 自动补签，35 个端点 |

**只有 MD5。没有 HMAC、没有 AES、没有 RSA。** 注意 Soto 的注释写「需要 SHA-256 + MD5 签名」，但代码里只有 `createHash("md5")` —— **那句注释是错的**。

签名只出现在两个端点：`/api/play/music/v2/checkRight` 和 `/api/play/music/v2/audioUrl`。
`/api/service/advert/watch` 用的是**写死的常量签名**（三个 repo 共用同一串 `sign=15a676d66285117ad714e8c8371691da`）→ 说明服务端根本不校验它（INFERRED）。

### `plat: ar` —— `plat` 必需，但 `ar` 不是

`plat` 是唯一 100% 出现在所有实现里的头，但取值至少 4 种：

| 值 | UA / channel / ver | 来源 |
|---|---|---|
| `ar` | `Dart/2.19 (dart:io)` / `aliopen` / `3.9.0` | UNM、sonimbus、Soto |
| `win` | `Dart/3.3 (dart:io)` / `W1` / `1.1.5` | musicdl 主路径、PyBodian、coco-downloader |
| `ip` | iPhone UA / `appstore` / `3.2.3` | BsaLee worker.js、qyo123oyq |
| `h5` | 桌面 Chrome UA | any-listen、Mio-Music（**纯读取，确实免签**） |

### 最小可用 header 集（VERIFIED）

**取流（`plat: ar` 路线，必须签名）：**
```
user-agent: Dart/2.19 (dart:io)
plat: ar
channel: aliopen
devid: <随机数字>          # UNM 用 Math.random()，sonimbus 用歌曲 ID！
ver: 3.9.0
X-Forwarded-For: 1.0.1.114
host: bd-api.kuwo.cn
```
再往 query 追加 `&timestamp=<ms>&sign=<MD5>`。

**仅搜索（`plat: win` 路线，免签）：**
```
user-agent: Dart/3.3 (dart:io)
plat: win
channel: W1
api-ver: application/json
brand: Windows 11 Pro for Workstations
net: wifi
content-type: application/json
ver: 1.1.5
svrver: 13
devid: <md5(uuid4)>
qimei36: <同 devid>
```

**关键观察**：`devid` 在 UNM 里是随机数字、在 sonimbus 里直接拿歌曲 ID 当设备 ID、在 musicdl 里是 `md5(uuid4)` —— **服务端不校验 devid 真实性**（INFERRED，但三个独立实现都这么干）。

**`X-Real-IP` 在已审源码中不存在**，只有 `X-Forwarded-For`，值恒为 `1.0.1.114`。
**`qi` / `httpsStatus` 在 17 个已审实现里均未出现** → 未证实。

### 端点与是否需要签名

| 端点 | 需签 | 必需 query |
|---|---|---|
| `/api/search/music/list` | ❌ | `pn`,`rn`,`keyword`,`correct=1`,`uid=-1`,`token=` |
| `/api/play/music/v2/audioUrl` | ✅ | `br`,`musicId`,`uid`,`token`,`timestamp`,`devId`,`format`,`freeSign`,`sign` |
| `/api/play/music/v2/checkRight` | ✅ | `uid`,`token`,`timestamp`,`musicId`,`freeSign`,`sign` + JSON body |
| `/api/service/advert/watch` | 常量 sign | `uid=-1`,`token=`,`timestamp`,`sign` |
| `/api/service/playlist/{id}/musicList` | ❌ | `source`,`pn`,`rn`,`reqId`,`uid`,`token` |
| `/api/service/playlist/info/{id}` | ❌ | `source`,`reqId`,`uid`,`token` |
| `/api/ucenter/users/pub/{uid}` | ❌ | `fromUid`,`platform=ios` |
| `/api/ucenter/vip/give/popup` | ❌ | `action=play`,`uid`,`token` |
| `/mlyric.kuwo.cn/mobi.s` | ❌ | `f=bodian`,`q`(base64),`uid`,`token` |

### 免登录取流 & 音质（VERIFIED）

- 未登录时 `uid=-1`、`token=` 空串（四处实现一致）
- `checkRight` 返回 `data.status`：**`3` = 仅试听**，走 `data.audition.*`；否则走正式 `audioUrl`
- 音质：`"6": ("flac", "2000kflac")` 是默认。`7/9/10`(mflac)、`11`(mgg)、`12`(zp) 是 **DRM 加密格式，需客户端解密**（musicdl 注释原文确认）
- **`freeSign`/`fsig` 是取流必需参数**，由 search 响应直接下发 —— 三处实现一致
- VIP 路线走的是**酷我老接口** `mobi.kuwo.cn` 的 `r.s?stype=comprehensive&mtype=convert_url_with_sign`（伪装车机/HD 客户端），**不是 `bd-api.kuwo.cn`**

### 「没有」清单 —— 被点名但其实不含波点 API 代码的

| 仓库 | 真相 |
|---|---|
| [chaser114/taemspeak3-bodian](https://github.com/chaser114/taemspeak3-bodian) (C#) | **没有。** 通读 `KuwoMusicPlugin.cs` 227 行，**零处** `bd-api.kuwo.cn`。L21 `ApiUrl = "https://api.xcvts.cn/api/music/bdyy"` —— 调第三方聚合站。README 的"无需登录"指的是那个第三方站 |
| [BsaLee/bodian_music_api](https://github.com/BsaLee/bodian_music_api) (PHP) | **没有音乐接口。** 只有签到 `/api/ucenter/vip/give/popup` + 查用户 `/api/ucenter/users/pub/{id}` |
| [qyo123oyq/bodian-music](https://github.com/qyo123oyq/bodian-music) (Dart) | **取流不是波点接口。** 走 `antiserver.kuwo.cn/anti.s`。它调的 `/api/v1/*` 路径**在波点真实 API 中不存在**（PyBodian 的 35 个端点里没有任何 `/v1/`）→ 路径是臆造的。文件里有个 `generateMd5()` 但**从未被调用** |
| [guqlule/bodian-music](https://github.com/guqlule/bodian-music) (Dart) | **没有。** 用户自定义 JS 源框架 |
| [XSong1205/BetterBDM](https://github.com/XSong1205/BetterBDM) (C++) | **没有。** `injector/pe_utils.cpp` + `hook_manager.cpp` —— **PE 注入 / Flutter 桥，纯二进制 hook** |
| [Jerry-Z07/bodianhelper](https://github.com/Jerry-Z07/bodianhelper) (C++) | **没有。** `src/proxy/mpv_proxy.cpp` —— mpv 进程代理 |
| [windbullet/koishi-plugin-nazrin-music-bodian](https://github.com/windbullet/koishi-plugin-nazrin-music-bodian) | **没有。** 走 `api.xingzhige.com/API/Kuwo_BD_new` |
| `axtyet/Luminous` 等 17 个 `bodian.js` | **没有。** 是 Loon/Surge/QuantumultX **MITM 响应改写**脚本，只改 `payInfo.isVip = 1`，不发任何自己的请求 |

### 时间线（INFERRED from git history）

- musicdl 的签名逻辑从最早 commit `f4f9f8a759`（2026-05-11）就存在
- UNM 的 `bodian.js` 自 `1441d35db3`（2025-09-23）加入起即带 sign

→ **签名要求至少在 2025-09 至今持续存在**，不是近期新增。

### 局限

未做**动态验证** —— 没有任何请求真的发到 `bd-api.kuwo.cn`。因此「服务端是否真的拒绝无签名请求」属 **INFERRED**。**VERIFIED 的部分是**："实现方认为必须签，且 5 个独立实现用同一算法同一 salt"。

---

## 附：额外发现 —— Rust 生态意外地强（不在原任务范围内）

做跨语言 code search 时发现的意外结果：**Rust 的酷狗实现数量和质量都超过 Java/Kotlin/Python，仅次于 Go。** 两个 salt 在 Rust 里命中 20+ 个文件。

| 仓库 | ★ | License | 说明 |
|---|---|---|---|
| [MuLiuSaMa/NexBox](https://github.com/MuLiuSaMa/NexBox) | 395 | GPL-3.0 | 游戏工具箱，酷狗只是附属功能，**不建议为酷狗采用** |
| [zzyoxml/md3Music](https://github.com/zzyoxml/md3Music) | 317 | AGPL-3.0 | **酷狗概念版第三方播放器，Flutter + Rust**。`kugou_api_server/rust/src/` 有 `helper.rs` / `crypto` / `modules/audio_more.rs` / `search_mixed.rs` —— **两个 salt 都在，是概念版方向的完整实现** |
| [lianchengwu/lmplayer](https://github.com/lianchengwu/lmplayer) | 100 | MIT | 自称"唯一 rust 版本酷狗第三方客户端"，`kugou/src/proto/sign.rs` |
| [apoint123/Unilyric](https://github.com/apoint123/Unilyric) | 34 | MIT | 歌词工具，`lyrics_helper_rs/src/providers/kugou/signature.rs`（**仅歌词**） |
| [Mio888888/Mio-Music](https://github.com/Mio888888/Mio-Music) | 26 | 无 | `src-tauri/src/music_sdk/sources/kg/crypto.rs` + `playlist.rs` |
| [burenLee/seraphine-music](https://github.com/burenLee/seraphine-music) | 9 | MIT | `src-tauri/src/http/config.rs`，支持概念版 |
| [bamboostrip/KugouMusic.rs](https://github.com/bamboostrip/KugouMusic.rs) | 3 | 无 | **Rust (Axum) 重构的酷狗 Web API 后端**，自述"致敬并参考 KugouMusic.NET" —— 即 C# 那个 SDK 的 Rust 移植 |
| [smiling11123/polomusic-tauri](https://github.com/smiling11123/polomusic-tauri) | 1 | MIT | `kg-rs/` crate，带 `public/API.md` |
| [tagore-cai/lx-music-api-rs](https://github.com/tagore-cai/lx-music-api-rs) | 0 | MIT | lx-music-api-server 的 Rust 版，`crates/lx-providers/src/providers/kg/sign.rs` |

**结论**：如果技术栈允许，Rust 值得纳入评估 —— `md3Music`（概念版全栈）和 `lmplayer` 是两个比 Java/Kotlin 同类更完整的实现。

### C# 也值得一提（不在原任务范围内）

两个 salt 在 C# 命中 5 个文件，其中 [`Linsxyx/KugouMusic.NET`](https://github.com/Linsxyx/KugouMusic.NET) 是一个**真正的 .NET SDK**（`src/Libraries/KuGou.Net/util/KuGouConfig.cs` + `Protocol/Raw/RawSongApi.cs`，两个 salt 都有），[`UnrealMultiple/VortexQ`](https://github.com/UnrealMultiple/VortexQ) 是它的 fork，[`Dr-hydra/OmniMix`](https://github.com/Dr-hydra/OmniMix) 用了 `KugouBridge.cs`。而且它已经被 Rust 项目 `KugouMusic.rs` 移植过一次 —— 说明它的实现质量被同行认可。

---

## 汇总：各语言可用性判定

| 语言 | 有真实签名实现？ | 最强实现 | 备注 |
|---|---|---|---|
| **Go** | ✅ 是（10+） | `guohuiyuan/music-lib`（可 `go get`）/ `liuran001/MusicBot-Go`（最完整） | 生态最成熟 |
| **Rust** | ✅ 是（20+ 文件命中） | `zzyoxml/md3Music`、`lianchengwu/lmplayer` | 意外地强 |
| **Kotlin** | ✅ 是 | **`ghhccghk/KuGouApi_Kotlin_SDK`** —— 唯一上 Maven Central，~200 路由，10,194 行 | 见 PART 2 |
| **Java** | ✅ 是 | `LTLXS/Net-music-KUGOU`（5,666 行，仅 3★）/ `GregTaoo/Concerto`（crypto 最好抄） | 全部是 App 内嵌，不可依赖 |
| **C# / .NET** | ✅ 是 | `Linsxyx/KugouMusic.NET` | 被 Rust 项目移植过 |
| **Dart / Flutter** | ✅ 是（8+） | `Sen0E/Battery-Music` 的 `plugin/kugou_music_api_dart`（22 模块逐文件移植） | 概念版方向多 |
| **Python** | ✅ 是 | `CharlesPikachu/musicdl`（6093★，功能最全）/ `MeoProject/lx-music-api-server`（最适合做骨架） | 大量同源 fork |
| **Swift** | ⚠️ 仅 1 个 | [`XIaodou0416/Beans-Music`](https://github.com/XIaodou0416/Beans-Music) `Beans/KugouMusicAPI.swift`（2085 行，四套盐） | 单点实现，缺 KRC |
| **PHP** | ❌ **没有** | — | `language:php` 下**两个 salt 均 0 命中**；且三个候选仓库分别是签到表单、空文件、2015 老接口 |
| **C++** | ❌ **没有** | — | `language:cpp` 下**两个 salt 均 0 命中**；`BetterBDM` / `bodianhelper` 是二进制 hook，不含 HTTP API |

---

## 最终建议：三条技术栈路线

### 路线 A — Go 后端（最成熟）

```
首选库:  github.com/guohuiyuan/music-lib        AGPL-3.0 ⚠️  134★  轻依赖，覆盖均衡
替代:    github.com/lfhy/kugou-music-api        MIT          2★   接口最全（153 个），但几乎无人验证
聚合:    github.com/AynaLivePlayer/miaosic      MIT          2★   若已在用 miaosic；酷狗 provider 是 PARTIAL
参照实现: liuran001/MusicBot-Go plugins/kugou/  GPL-3.0      198★  唯一双版本 + 风控验证码 + 强制重注册
```
**注意**：AGPL-3.0 有传染性。若要闭源商用，只有 `lfhy`（MIT）或 `miaosic`（MIT）可选，或照着 `MusicBot-Go`/`Ion-nsx` 自己写。

### 路线 B — JVM（Kotlin/Java）

```
引入依赖: top.ghhccghk.multiplatform.kugouapi:kugouapi-kmp:1.0.5   MIT   Maven Central
          ⚠️ 会带进 Compose 依赖；4 个已知缺陷见 PART 2 §2.3，必须自行规避
抄代码:   GregTaoo/Concerto 的 KuGouMusicApiCrypto.java（272 行，只依赖 JDK，标准+Lite 全套）
          ⚠️ 该仓库 License 是 NOASSERTION（中文自定义声明）
```

### 路线 C — 波点音乐（Bodian）

```
权威参考: UnblockNeteaseMusic/server  src/provider/bodian.js      （算法最清楚）
最全实现: CharlesPikachu/musicdl      musicdl/modules/sources/bodian.py
Python:   MoeclubM/PyBodian           bodian_toolkit.py（35 个端点，自动补签）
Swift:    gee1k/sonimbus              Sonimbus/Core/UnblockService.swift
```
**核心要点**：读取端点免签；`audioUrl` / `checkRight` **必须** `sign = md5("kuwotest" + 排序后的字母数字字符 + path)` 并带 `timestamp`；`plat` 必需但可取 `ar`/`win`/`ip`/`h5`；`devid`/`qimei36` 随机值即可。

---

## 方法与置信度声明

**VERIFIED（实际读取源码并引用行号）**：本报告所有 ✅ / ⚠️ / ❌ 判定、所有代码片段与行号、所有「没有盐」的结论、所有仓库元数据（`gh api repos/...`）、以及 Maven Central 发布状态（HTTP 直接列举 `repo1.maven.org` 路径）。

**INFERRED（未验证）**：
- 「服务端是否真的拒绝无签名 / 错误签名的请求」—— **全程未发出任何真实请求**。所有签名结论都是"实现方认为必须这样签，且 N 个独立实现一致"。
- 波点签名要求的时间线（来自 git history，未逐 commit 复核）。
- 少数仓库的个别能力列（PART 2 中 Concerto / LTLXS / Gocache 的播放地址音质档位只确认了"有取址逻辑"，未逐档验证）。
- 部分"小酷狗"系复制粘贴插件未逐一精读。

**时效性**：播放地址与账号类接口时效性强（salt 与路由都可能随 App 版本变化）。本报告描述的是各仓库 **pushed_at 时点**的代码状态。

**Code search 覆盖**：`gh search code` 索引有限。我用 4 轮不同关键词的 repo/code 检索 + 全部已知 salt 常量的常量级扫描（分语言）交叉补齐。`search.maven.org` 的 Solr 索引不全（对已发布的 `kugouapi-kmp` 返回 0 结果），**不要用它做否定判断**。

