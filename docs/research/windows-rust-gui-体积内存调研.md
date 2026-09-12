# Windows 轻量音乐播放器：Rust GUI 技术栈体积与内存实测调研

> 调研目标：为一款「轻量、小巧、无浏览器内核的 Windows 音乐播放器」选型。
> 所有数字均标注来源与可信度分级：**【官方】**官方文档/官方仓库数据 · **【第三方实测】**独立第三方测量 · **【社区报告】**issue/讨论中的用户报告 · **【未找到】**无可靠数据。
> 调研日期：2026-09（数据时效以各来源标注为准）。凡我自己的推断一律标【我的推测】。

---

## 0. 一句话结论

| 结论 | 依据 |
|---|---|
| **Tauri 在 Windows 上不是「无浏览器内核」**，官方原文写「WebView2 ... therefore Chromium」 | 【官方】Tauri 文档原文 |
| **空白 WebView2（无任何 UI）实测就吃 336.6 MB**（7 进程，其中 6 个 msedgewebview2 占 286.1 MB） | 【自建实测】Windows 10 |
| **Tauri 的「体积小」和「内存小」是两件事**：安装包 2MB，跑起来 336MB 起 | 综合 |
| Tauri 官方**没有**可引用的「体积/内存 benchmark 页面」，benchmark 面板原文写明「只在 Linux 测内存」；维护者自己说老 benchmark「要带一大把盐看」 | 【官方】原文 |
| 纯 Rust 方案（egui / iced / Slint）在内存上对 Tauri 有**数量级级别**的优势（约 100-170MB vs 300-740MB） | 【第三方实测】Zenn 15 框架横评 |
| 但**没有一个 Rust 方案能同时做到「极小体积 + 极小内存 + 成熟生态」**，各有取舍 | 综合 |

---

## 1. Tauri 2（依赖 WebView2）

### 1.1 官方是否承认「WebView2 = Chromium 内核」——承认

**【官方】** [Tauri Process Model 文档](https://v2.tauri.app/concept/process-model/) 原文：

> "Currently, Tauri uses **Microsoft Edge WebView2** on Windows, WKWebView on macOS, and WebKitGTK on Linux."
> "A WebView is a **browser-like environment** that executes your HTML, CSS, and JavaScript."
> "Tauri employs a **multi-process architecture similar to Electron** or many modern web browsers."
> "Unlike other similar solutions, the WebView libraries are **not included in your final executable but dynamically linked at runtime**."

**【官方】** [Tauri Prerequisites 文档](https://v2.tauri.app/start/prerequisites/) 原文：

> "Tauri uses **Microsoft Edge WebView2** to render content on Windows."

**【官方，最直白的一句】** [Webview Versions 文档](https://v2.tauri.app/reference/webview-versions/) 原文：

> "**Tauri uses WebView2 which is based on Microsoft Edge and therefore Chromium.** WebView2 can update itself, you are guaranteed a relatively recent chromium build on all Windows targets."

这句是本次调研中**关于「Tauri 是不是无浏览器内核」最直接、最无争议的官方答案**。Tauri 官方从未把自己描述为「无浏览器内核」，恰恰相反，官方文档主动、明确地说明了 Windows 上就是 Chromium。

**判定**：你的「无浏览器内核」诉求下，**Tauri 在 Windows 上明确不满足**。它的体积优势来自「不打包内核」，而不是「没有内核」——内核以 WebView2 形式预装在系统里，仍然要跑 Chromium 的多进程架构，仍然吃 Chromium 的内存。这一点 Tauri 官方文档本身写得很坦白，没有任何掩饰。

### 1.2 体积：官方实时 benchmark 的原始数据（Windows）

Tauri 官方 benchmark 结果页的图表是 JS 动态渲染的，但**原始数据 JSON 在公开仓库里**，我直接读取了：

- 数据仓库：<https://github.com/tauri-apps/benchmark_results>（分支 `gh-pages`）
- 结果页：<https://tauri-apps.github.io/benchmark_results/>
- 我读的文件：`tauri-recent-windows.json`，**最后一条记录时间 2026-09-11 17:18 UTC**，commit `d0f38df06a2f4406b388a336694aa18b3c6bf1a9`

**【官方】Tauri Windows 二进制体积（release 编译）**：

| 基准程序 | 字节 | MB |
|---|---:|---:|
| `tauri_hello_world` | 2,741,248 | **2.61 MB** |
| `tauri_cpu_intensive` | 2,742,784 | 2.62 MB |
| `tauri_3mb_transfer` | 2,746,880 | 2.62 MB |
| `wry_rlib`（对比参考） | 523,570 | 0.50 MB |

⚠️ **重要限制**：这是**裸可执行文件**大小，**不含** WebView2 运行时（预装于系统，不计入）。且要注意 `cargo_deps` 显示 Windows 平台依赖 **753 个 crate**。

**【官方】** 同仓库 Linux 记录（2026-09-11）：`tauri_hello_world` = 2.81 MB；macOS = 2.66 MB。三平台裸二进制都在 2.6–2.8 MB 量级。

**【官方】** [Tauri 官网首页](https://v2.tauri.app/start/) 的宣称：

> "A Tauri app only contains the code and assets specific for that app and doesn't need to bundle a browser engine with every app. This means that a **minimal Tauri app can be less than 600KB** in size."

**注意**：官方宣称的 <600KB 与我读到的官方 benchmark 2.61MB **对不上**（大约 4 倍差距）。600KB 可能是特定编译优化下的极限值或旧数据。**决策时请以 2.6MB 为准，不要引用 600KB。**

### 1.3 Tauri 官方文档里唯一带数字的体积表：WebView2 安装模式

**【官方】** [Windows Installer 文档](https://v2.tauri.app/distribute/windows-installer/) 给出了各 WebView2 分发模式的体积代价，这是**官方数字**：

| 安装模式 | 需要联网 | 安装包增大 | 说明 |
|---|---|---:|---|
| `downloadBootstrapper`（默认） | 是 | **0 MB** | 体积最小，但要联网下载 |
| `embedBootstrapper` | 是 | **~1.8 MB** | 对 Win7 兼容更好 |
| `offlineInstaller` | 否 | **~127 MB** | 内嵌 WebView2 安装器 |
| `fixedVersion` | 否 | **~180 MB** | 内嵌固定版 WebView2 运行时 |
| `skip` | — | 0 MB | ⚠️ 官方标注「不推荐」 |

**这条对决策极其关键**：
- 若你**接受依赖系统预装的 WebView2**（Win10 1803+ / Win11 都自带），安装包可以只有几 MB。
- 若你**要求离线可用 / 版本可控**（音乐播放器用户常见诉求），安装包直接 **+127MB 或 +180MB**——此时 Tauri 体积优势完全消失，比 Electron 还大。

**【官方】** 文档另注：Win10 (1803+) 和 Win11 上 WebView2 运行时随操作系统分发。

### 1.3b 真实 Tauri 2 项目的 Windows 安装包体积（第三方实测）

**【第三方实测，逐个查 release assets 字节数核得】** 真实的 Tauri 2 项目在 GitHub Release 里的 Windows 安装包：

| 项目 | 产物 | 体积 |
|---|---|---:|
| [Amoner/inkwell](https://github.com/Amoner/inkwell/releases/tag/v0.2.4) v0.2.4（轻量 Markdown 编辑器，2026-08-29） | `-Windows-x64-Setup.exe` | **1.85 MB** |
| 同上 | `_x64_en-US.msi` | **2.44 MB** |
| [MaxxTopia/optimizationmaxxing](https://github.com/MaxxTopia/optimizationmaxxing/releases) v0.4.4（2026-08-27） | `_x64-setup.exe` | **10.91 MB** |
| [localsend/localsend](https://github.com/localsend/localsend/releases) v1.18.2（2026-08-21） | `windows-x86-64-unsigned.exe` | **18.08 MB** |

**解读**：Tauri 2 的真实安装包**普遍 2MB 起**，随前端资源增长。注意 inkwell 那个 1.85MB 是**真·极简项目**（依赖 WebView2 预装）。**Tauri 1.x 时代流传的「600KB」不应直接套到 Tauri 2 安装包上。**

### 1.4 内存：官方 benchmark 在 Windows 上**没有采集到数据**

我检查了 `tauri-recent-windows.json` 全部 20 条记录的 `max_memory` 字段：**全部为空 `{}`**（`records with non-empty max_memory: 0 / 20`）。macOS 同样为空。**历史 154 条 Windows 记录也全部没有 `max_memory`。**

**【官方】** 官方 benchmark 面板页 [tauri-apps.github.io/benchmark_results](https://tauri-apps.github.io/benchmark_results/) 原文直接写明：

> "**We currently only measure the Memory Usage on Linux**"

Thread Count / Syscall Count 同样只测 Linux；Windows 与 macOS 的图表容器在 HTML 里是**注释掉的**。**官方从未发布过 Windows 内存基准，且明确说明了这一点。**

**【官方】只有 Linux 有内存数据**（`tauri-recent-linux.json`，2026-09-11）：

| 基准程序 | 峰值内存 |
|---|---:|
| `tauri_hello_world` | **261.2 MB** |
| `tauri_3mb_transfer` | 260.4 MB |
| `tauri_cpu_intensive` | 498.7 MB |

⚠️ **这个数字不能直接套到 Windows**：Linux 上 Tauri 用 **WebKitGTK**，Windows 上用 **WebView2/Chromium**，两者内存特性差别很大（见下）。仅作为「空白窗口也能吃掉 260MB」的量级参考。

### 1.5 ⚠️ Tauri 官方 benchmark 方法论被官方自己否定

**【官方/issue】** [tauri-apps/tauri#5889 "Memory benchmark might be incorrect: Tauri might consume more RAM than Electron"](https://github.com/tauri-apps/tauri/issues/5889)（2022-12-21 开，2024-05-17 关闭）

这个 issue 里，Tauri 团队成员 **Beanow** 的原话：

> "the benchmarks on https://tauri.app/v1/references/benchmarks/ **need to be taken with a good handful of salt**. It's by no means a scientific report and honestly only should be considered a **smoke test / regression test**."

同 issue 中报告者 jviotti（Postman）的实测数据表，**含 Windows 10**：

**加载 postman.com 时的总内存**：

| 方案 | macOS 12.6.1 | Ubuntu 22.04.1 | **Windows 10** |
|---|---:|---:|---:|
| **Tauri** | 421 MB | 581 MB | **399 MB** |
| Safari | 471 MB | — | — |
| **Electron** | 337 MB | 240 MB | **318 MB** |
| Chrome | 381 MB | 370 MB | — |

**加载 vscode.dev 时**：

| 方案 | macOS | Ubuntu | **Windows 10** |
|---|---:|---:|---:|
| **Tauri** | 429 MB | 572 MB | **370 MB** |
| **Electron** | 332 MB | 222 MB | **312 MB** |

报告者原文（关键结论，直接回答了你的核心疑问）：

> "Tauri using Edge WebView 2 had **similar memory usage with Electron**, which makes sense given **both are based on Chromium**."

Tauri 另一位成员 **JonasKruckenberg** 的回应（承认了方法论问题）：

> "Since you're not using any native functionality... you're basically **benchmarking Webkit against Chromium**... That there is a memory consumption difference between webkit and chromium doesn't surprise much."

**判定**：这条是本次调研中**最有价值的单一发现**。它直接说明——在 Windows 上，Tauri 相对 Electron 的**内存优势基本不存在**，因为两者底层都是 Chromium。Tauri 真正的优势只在**磁盘体积**（不打包内核）。

### 1.6 Tauri 2 自身的 Windows 内存问题（issue，均 open）

| Issue | 标题 | 状态 | 说明 |
|---|---|---|---|
| [#12724](https://github.com/tauri-apps/tauri/issues/12724) | `[bug] Memory leak when emitting events` | **OPEN**（2025-02-17） | Tauri 2 官方模板复现：连续 emit 200 万事件后**前端 ≈ 1.1 GB、后端 ≈ 120 MB** |
| [#9190](https://github.com/tauri-apps/tauri/issues/9190) | `[bug] Memory leaks when reading files` | **OPEN**（2024-03-14） | **明确 Windows**：`readBinaryFile()` 读 140MB 文件时占用涨到 **11.5 GB** 后 OOM 崩溃；50–80MB 文件也吃 2–5 GB。v1 与 v2 beta 均复现 |
| [#5889](https://github.com/tauri-apps/tauri/issues/5889) | Memory benchmark might be incorrect | CLOSED | 见 §1.5 |

⚠️ 对音乐播放器尤其要注意 **#9190**：读本地音频文件是核心功能。虽然该 issue 用的是 `readBinaryFile()`（把整个文件读进内存，本身用法就不当），但**读取路径上的内存放大是 Tauri 2 至今未修复的已知问题**。

**【官方】** 微软 WebView2 性能文档也承认（[learn.microsoft.com](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance)）：

> "Many past performance issues, like memory leaks and high CPU usage, have been addressed in newer versions of the WebView2 Runtime."

且微软建议**优先使用 Evergreen 运行时**：
> "Whenever possible, deploy your app with the Evergreen WebView2 Runtime... Using a fixed version risks missing out on recent optimizations."

——即「用系统运行时更安全」是**微软与 Tauri 双方一致的官方立场**。这与 §1.3 里 fixedVersion 要 +180MB 的代价要一起权衡。

### 1.5b ⭐ 空白 WebView2 基线实测（本次调研最硬的一手数据，Win10）

因为官方在 Windows 上不采内存数据，我们**自己搭了一个最小 WebView2 宿主来测基线**：

**【自建实测】** 环境：Windows 10 专业版 19045 + WebView2 Evergreen **146.0.3856.72**，用 .NET 8 WinForms + WebView2 SDK 1.0.4191.47，加载**一张空白 HTML 页**，预热 15–18 秒后采样 6 次：

| 指标 | 数值 |
|---|---:|
| **进程树合计 Working Set** | **336.0 – 338.5 MB（均值 336.6 MB）** |
| 进程树合计 Private Bytes | 189.8 – 190.3 MB |
| 进程总数 | **7 个** |
| 其中 `msedgewebview2.exe` | **6 个进程，Working Set 合计 285.5 – 288.0 MB（均值 286.1 MB）** |

单进程明细（WS / Private，单位 MiB）：

| 进程 | Working Set | Private Bytes |
|---|---:|---:|
| browser (main) | 116.8 | 39.8 |
| **gpu-process** | 73.8 | 88.3 |
| 宿主进程 wv2mem | 51.8 | 14.5 |
| renderer | 48.0 | 22.9 |
| utility #1 | 28.6 | 11.2 |
| utility #2 | 18.6 | 8.8 |
| crashpad-handler | 8.5 | 2.2 |

**这条数据的意义极大**：
- 一个**连 UI 都没有、什么都没干的空白 WebView2**，就已经吃掉 **~286 MB** 的 WebView2 进程组（含 6 个 `msedgewebview2.exe`）。
- **Tauri 应用是在这个基础上再加 Rust 主进程**——所以任何「Tauri 空载只占几十 MB」的说法都是错的。
- 这正是「WebView2 就是 Chromium、不是无内核」的**体感来源**：你省下的是安装包体积，省不下的是运行时内存。
- 也解释了 §1.5 里 Tauri 与 Electron 内存相近的实测结果。

**【官方】** [Tauri discussion #11553](https://github.com/orgs/tauri-apps/discussions/11553) 中维护者 FabianLars 解释了进程构成：`tauri://localhost` 是主浏览器进程（相当于浏览器的一个标签页进程），第二个是 Rust 进程，**其余是 webview/浏览器的 GPU 和网络子进程**——与上面的实测明细完全吻合。

### 1.7 第三方实测：Tauri 内存显著高于原生方案

**【第三方实测】** [Zenn: デスクトップUIフレームワーク15種のメモリ使用量計測](https://zenn.dev/mizugeeks/articles/1019cf2353d343)（2026-08-15，作者 mizugeek）

- **方法**：同一个 App（Gallery / Editor / Grid / Form / Canvas 五个模块）用 15 个框架各实现一遍；用专用工具 `rsscap` 每 250ms 采样；**明确把 WebView 的 helper 进程合并计算**（原文：「ElectronやTauriのように内部でChromiumやWebKitのヘルパープロセスを立ち上げるフレームワークは、その分も合算しています」）
- **平台**：⚠️ **macOS（Mac mini M1 16GB）**，非 Windows。作者明说源码仓库「macOS専用」。
- **代码**：<https://github.com/mizugeek/workbench_mem_bench>

结果（单位 MB，peak = 采样峰值）：

| 框架 | Gallery | Editor | Grid | Form | Canvas | 顺序切换 | 四分屏 |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Slint** | **124.4** | **106.3** | **109.3** | **105.9** | **115.0** | **130.2** | **133.0** |
| AppKit (Swift) | 129.0 | 105.9 | 115.6 | 129.7 | 102.9 | 155.4 | 210.2 |
| GTK4 (C) | 139.8 | 124.1 | 135.3 | 138.6 | 134.9 | 165.0 | 186.3 |
| SwiftUI | 150.4 | 102.5 | 107.1 | 136.0 | 101.2 | 148.0 | 183.1 |
| Qt 6 (C++) | 151.0 | 138.7 | 133.4 | 144.6 | 143.3 | 169.1 | 174.0 |
| **egui** | 157.3 | 148.6 | 150.0 | 142.2 | 153.3 | 156.8 | 155.3 |
| **iced** | 167.2 | 145.8 | 144.4 | 139.8 | 147.8 | 177.3 | 154.6 |
| Flutter | 199.0 | 181.5 | 176.8 | 203.2 | 165.6 | 216.2 | 363.8 |
| Avalonia (.NET) | 216.6 | 198.0 | 227.2 | 345.7 | 282.4 | 412.3 | 361.3 |
| Wails (Go+WebView) | 322.6 | 346.4 | 356.0 | 372.3 | 599.0 | 637.3 | 683.7 |
| Neutralino | 335.3 | 248.1 | — | 275.5 | 582.3 | 715.4 | 744.3 |
| **Tauri** | **336.0** | **309.0** | **348.8** | **307.6** | **660.6** | **686.9** | **739.0** |
| Compose Multiplatform | 390.8 | 283.6 | 565.7 | 512.6 | 380.0 | 611.8 | 652.6 |
| Fyne (Go) | 395.3 | 389.7 | 437.1 | 540.6 | 213.1 | 525.1 | 926.9 |
| **Electron** | **543.4** | **449.7** | **588.0** | **443.5** | **502.7** | **643.6** | **511.8** |

作者结论原文：

> 「ネイティブ・Rust 系は 100〜170 MB 台に収まる一方、WebView系(Electron・Tauri・Wails・Neutralino)と JVM/Go 系(Compose・Fyne)は 450〜530 MB 帯に集まります。**Electron と Slint の差は実に約 4.5 倍**もあります。」

⚠️ **作者的自我修正（必须一起引用）**：
> 「日本語表示のため **egui, iced は CJKフォントをプロセス内に読み込むのでメモリ使用量が約25MBほど増えています**。それを除くと egui, iced は **Slint と互角以上**のパフォーマンスです。」

**判定**：**egui / iced 的数字要减掉约 25MB 再和 Slint 比**（因为中日韩字体是进程内加载的，Slint 走系统字体）。减去后 egui ≈ 132MB、iced ≈ 142MB，与 Slint ≈ 124MB 基本同级。

### 1.8 第三方实测：Tauri 体积小但内存不便宜

**【第三方实测】** [IronOxidizer/gui-toolkit-benchmarks](https://ironoxidizer.github.io/gui-toolkit-benchmarks/)（[源码仓库](https://github.com/IronOxidizer/gui-toolkit-benchmarks)，28 stars，页面数据更新于 2023-10-29）

- **平台**：⚠️ **Linux X11**（Debian + OpenBox），非 Windows
- **方法**：作者自述方法论，Size 分「可执行文件」与「运行时依赖总量（total）」两种变体；内存为物理内存占用

| 排名 | 框架 | 语言 | 模式 | 启动(ms) | **内存(KB)** | 可执行文件(KB) | 依赖总量(KB) |
|---:|---|---|---|---:|---:|---:|---:|
| 1 | fltk | C++ | retained | 40 | 1,829 | 14 | 39,417 |
| 2 | fltk-rs | Rust | retained | 64 | 5,906 | 1,346 | 36,763 |
| 8 | qt | C++ | retained | 106 | 16,486 | 17 | 288,456 |
| 11 | **iced** | Rust | retained | 99 | **47,397** | 4,496 | 228,186 |
| 12 | **egui** | Rust | immediate | 175 | **52,100** | 10,597 | 491,040 |
| 15 | **tauri** | Rust | retained | 470 | **109,117** | **4,918** | 573,604 |
| 16 | electron | JS | retained | 717 | 192,814 | 1 | 1,327,205 |

**解读**：tauri 可执行文件仅 4.8MB（因为内核不打包），但内存 106MB，几乎是 egui/iced 的 2 倍、fltk 的 58 倍。注意 egui/iced 的「依赖总量」很大是因为该基准在 Linux 上经 GTK 链。

⚠️ 该页数据较旧（2023-10），且非 Windows。仅作相对趋势参考。

### 1.9 为什么官方不再提供 benchmark 页

**【官方】** 原 v1 benchmark 页 `https://tauri.app/v1/references/benchmarks/` 现已 **404**；现存数据页为 <https://tauri-apps.github.io/benchmark_results/>（基于 GitHub Actions 的 ubuntu/windows/macos-latest runner）。v2 文档中**没有独立的 benchmark 或体积对比页面**——我在 [v2.tauri.app](https://v2.tauri.app) 的站点结构中未发现此类页面。

**判定**：**Tauri 2 官方文档不存在可引用的「体积/内存 benchmark 页面」**。唯一带数字的官方体积信息是 §1.3 的 WebView2 安装模式表和首页的 600KB 宣称。

---

## 2. egui / eframe

### 2.1 体积

| 数据 | 数值 | 来源 | 分级 |
|---|---:|---|---|
| 官方 benchmark（Linux, hello_world 二进制, 2023） | 18 MB | [Lukas Kalbertodt 博文](https://lukaskalbertodt.github.io/2023/02/03/tauri-iced-egui-performance-comparison.html) | 【第三方实测】Linux |
| GUI Toolkit Benchmarks（Linux, 可执行文件） | 10.6 MB | [同上](https://ironoxidizer.github.io/gui-toolkit-benchmarks/) | 【第三方实测】Linux |

⚠️ **未找到可靠的 Windows 平台 egui 最小应用 release 二进制体积数据**。egui 官方仓库不含体积基准。

**【我的推测】** egui/eframe 通过 wgpu 或 glow 静态链接，Windows 上裸 exe 大致在 **5–15 MB** 量级，但**这是我基于 Linux 数据的推断，未经实测验证，请勿直接引用**。

### 2.2 内存

**【官方/维护者】** [emilk/egui#3689 "RAM usage of egui"](https://github.com/emilk/egui/issues/3689)（2023-12-07）

issue 正文（用户报告）：

> "It's quite big. Not as big as Electron or similar WebView tech, but still too big for what it is. My fairly simple app with few buttons and few labels eats **30mb RAM**... Similar app in fltk-rs consumes **1mb RAM**!"

维护者的诊断结论（issue 中的分析）：

> "Counted memory: **3.1 MiB**. Resident memory: **35.9 MiB**. The 'counted' memory are all RAM allocations made by Rust... In other words: **egui itself is using 3.1 MiB of memory (or less)**. About **2MiB of that is the font texture**. Another **1MiB is the default egui icon set** by eframe."

**判定**：egui 框架本身只占 ~3.1MB，其余 ~33MB 是 GPU 缓冲、字体纹理、系统分配器与 OS 开销。这个数字很硬，可以作为「egui 内存下限」的权威依据。

其他实测：Zenn 横评 egui ≈ 142–157MB（含约 25MB CJK 字体，见 §1.7）；IronOxidizer ≈ 52MB（Linux，2023）。

### 2.3 ⚠️「immediate mode 会一直重绘」——这个说法**不准确**

这是你特别问到的一点，答案很明确：**官方文档直接否认了「空闲也一直重绘」**。

**【官方】** [egui README "Why immediate mode"](https://github.com/emilk/egui#why-immediate-mode) 原文：

> "`egui` **only repaints when there is interaction (e.g. mouse movement) or an animation, so if your app is idle, no CPU is wasted.**"
> "For most cases you can expect **`egui` to take up 1-2 ms per frame**."
> "Since an immediate mode GUI does a full layout each frame, the layout code needs to be quick. If you have a very complex GUI this can tax the CPU. In particular, **having a very large UI in a scroll area (with very long scrollback) can be slow**, as the content needs to be laid out each frame."

**【官方】** [egui docs - Understanding immediate mode](https://docs.rs/egui/latest/egui/) 原文（说明「60fps」指的是设计模型，不是强制行为）：

> "Immediate mode has its roots in gaming, where everything on the screen is painted at the display refresh rate, i.e. at 60+ frames per second."

**准确表述应为**：egui 是 immediate mode（每帧重建 UI 树），但采用 **reactive/按需重绘** 策略，空闲时不重绘。代价在于**每帧布局成本**——连续动画（如音乐播放器的进度条、频谱）会触发持续重绘，此时 1-2ms/帧 的布局开销会变成常态 CPU 占用。

### 2.4 ⚠️ egui 在 **Windows** 上有真实的高 CPU 问题（官方确认）

这一节对你的项目最关键，因为都是 Windows 专项问题。

**【社区报告 + 官方确认】** [emilk/egui#4173 "eframe on Windows high CPU usage due to excessive calls to glutin::context::make_not_current"](https://github.com/emilk/egui/issues/4173)（2024-03-15 开，**至今 open**）

报告者原文：

> "I noticed that my application CPU usage was much higher than expected on Windows. On a similar machine I was seeing **1%-2% usage on Linux, and around 20%-30% on Windows while moving the mouse in window** (just mouse movement)."
> "After some optimization I got my rendering loop down to less than 1ms and still the CPU usage was too high... almost 90% of the time, it is calling the `make_not_current` function"

维护者 **emilk** 亲自确认并定位原因：

> "I think this was introduced in [#3172] when we added support for multiple viewports. When there are multiple viewports we need to switch to which has the 'current' gl context, and **we unfortunately pay this cost even when there is only one viewport**, but that should be fixable."
> （2024-04-01）"**This is still an issue on Windows.**"
> （2024-04-03，对用户）"if you want this fixed you **start rolling up your selves** ;)"

报告者后续补充：

> "CPU load does not noticeably decrease and **still hovers around 30% on my machine in the `hello_world` example**."（即使单个 viewport）

**判定**：截至该 issue 最后活跃讨论（2024-04），egui 在 Windows 上鼠标移动即可吃到 20-30% CPU（连 hello_world 都是），原因是 OpenGL 上下文切换。**issue 至今 open，未见修复闭环。** 这对「轻量音乐播放器」是重大风险点（用户会持续移动鼠标、悬停列表）。

其他相关 open issue：

| Issue | 标题 | 状态 | 日期 |
|---|---|---|---|
| [#5092](https://github.com/emilk/egui/issues/5092) | 100% CPU usage since the update to wgpu 0.20 | open | 2024-09-08 |
| [#7059](https://github.com/emilk/egui/issues/7059) | High CPU usage on macOS (when launching by double clicking executable) | open | 2025-05-19 |
| [#7401](https://github.com/emilk/egui/issues/7401) | `pure_glow` high CPU usage | open | 2025-08-01 |
| [#3801](https://github.com/emilk/egui/issues/3801) | A design to reduce CPU usage | open | 2024-01-10 |
| [#7776](https://github.com/emilk/egui/issues/7776) | Windows: High CPU Usage when not visible（~16% CPU 当窗口隐藏） | closed | 2025-12-14 |

**【社区报告】** [r/rust: Memory usage of egui](https://www.reddit.com/r/rust/comments/18d0ahb/memory_usage_of_egui/) — 用户报告「fairly simple app... eats 30mb RAM with strip enabled」，与 #3689 一致。

---

## 3. iced

### 3.1 体积（有真实的 Windows 实测数据）

**【社区报告】** [iced-rs/iced#1531 "Any expectations for executable size and ram usage?"](https://github.com/iced-rs/iced/discussions/1531)（2022-11-12，**Windows 10**，counter 示例程序）

报告者 BlackSharkfr 的逐步优化数据：

| 配置 | exe 体积 | RAM |
|---|---:|---:|
| debug | 14 MB | 90 MB |
| `--release` + `windows_subsystem` | 6 MB | 76 MB |
| `+ opt-level="z" + lto=true` | **3.1 MB** | 76 MB（无变化） |
| **`features=["glow"]`（替代默认 wgpu）** | **1.5 MB** | **27 MB** |

**这是本次调研里最有操作价值的一条**：iced 默认走 **wgpu**（→ Vulkan/DX12），换成 **glow**（OpenGL）后，**体积从 3.1MB 降到 1.5MB，内存从 76MB 降到 27MB**。一个特性开关换来 2 倍体积和 2.8 倍内存的改善。

⚠️ 注意：该讨论**至今无人正式回答**（最后回复 2024-12-10），属用户单方实测；但数字具体、方法可复现。

**【第三方实测】** [IronOxidizer 基准](https://ironoxidizer.github.io/gui-toolkit-benchmarks/)（Linux X11, 2023）：iced 可执行文件 4,496 KB ≈ 4.4 MB，内存 47,397 KB ≈ 46 MB。

**【第三方实测】** [Lukas Kalbertodt 博文](https://lukaskalbertodt.github.io/2023/02/03/tauri-iced-egui-performance-comparison.html)（Linux, 2023）：iced 二进制 17 MB（todos 示例，非 hello world）。

**【官方】** iced issue [#618](https://github.com/iced-rs/iced/issues/618)（2020，已关闭）："After `cargo build --release` on Windows with only the counter example, the release folder is **459MB**"——指整个 release 目录（含中间产物），**不是分发体积**，勿混淆。

### 3.2 内存

| 数据 | 数值 | 来源 | 分级 |
|---|---:|---|---|
| Windows, counter, wgpu 默认 | 76 MB | [iced#1531](https://github.com/iced-rs/iced/discussions/1531) | 【社区报告】 |
| Windows, counter, **glow 后端** | **27 MB** | [iced#1531](https://github.com/iced-rs/iced/discussions/1531) | 【社区报告】 |
| Linux, 2023 | 46 MB | [IronOxidizer](https://ironoxidizer.github.io/gui-toolkit-benchmarks/) | 【第三方实测】 |
| macOS, 复杂 App | ~140-177 MB（含 25MB CJK 字体） | [Zenn](https://zenn.dev/mizugeeks/articles/1019cf2353d343) | 【第三方实测】 |

### 3.3 iced 的 Windows 专项内存问题

**【社区报告】** 以下 issue **至今 open**：

| Issue | 标题 | 日期 | 相关性 |
|---|---|---|---|
| [#2064](https://github.com/iced-rs/iced/issues/2064) | **Text rendering is memory hungry on Windows** — "Application becomes memory hungry on text rendering, while resizing window. No significant difference wgpu or tiny_skia" | 2023-09-02 | ⚠️ 高（音乐播放器文字多） |
| [#2659](https://github.com/iced-rs/iced/issues/2659) | Memory leak when window is minimized | 2024-11-04 | ⚠️ 高（播放器常驻托盘/最小化） |
| [#3233](https://github.com/iced-rs/iced/issues/3233) | multi_window example memory leaks in MacOS | 2026-02-03 | 中 |
| [#3390](https://github.com/iced-rs/iced/issues/3390) | `iced_wgpu` incorrectly selects Vulkan colour format on HDR display | 2026-07-14 | 低 |

**#2064 值得单独警惕**：报告者期望「stay in under 50 mb using tiny skia」，实际在文本渲染+改窗口大小时内存飙升。**issue 2023 年开到现在仍 open。**

**【社区报告】** iced 维护者/社区成员在 #1531 中的反方观点（应一并引用以求平衡）——用户 `airstrike`：

> "A lot can be attributed to the graphics buffers for WGPU... to my knowledge, there is **not a single issue or discussion on GitHub, Discourse or Discord about actual 'bloated' iced apps**. Yet, for some reason, this question keeps being asked by people running very short, contrived snippets like the counter example... **I encourage you to leave the memory usage profiling to a real benchmark or to real-world issues**."

**判定**：iced 的「内存高」主要来自 wgpu 的图形缓冲（固定成本），换 glow 后端可大幅缓解；但 Windows 文本渲染吃内存（#2064）和最小化泄漏（#2659）是真实存在的未修复问题。

---

## 4. Slint

### 4.1 ⚠️ 你记的「Slint 官方有专门的 size/memory 对比页」——**不存在**

我把 `slint.dev` 的 [sitemap.xml](https://slint.dev/sitemap.xml) 完整列了出来（约 200 个 URL），**没有任何 benchmark / size / memory 对比页**。

Slint 官网的「对比页」只有这些，全部是**纯定性文案，不含实测数字表格**：

| 页面 | 内容 |
|---|---|
| [alternative-to-electron](https://slint.dev/alternative-to-electron) | 定性：Native / Lightweight / Security / Multi-language |
| [alternative-to-qt](https://slint.dev/alternative-to-qt) | 定性：Performance / Avoid Runtime Surprises / Live-Preview / Scalable |
| [alternative-to-flutter](https://slint.dev/alternative-to-flutter) | 同类 |
| [alternative-to-lvgl](https://slint.dev/alternative-to-lvgl) | 同类（嵌入式） |
| [declarative-rust-gui](https://slint.dev/declarative-rust-gui) | 面向 Rust 的对比，定性 |

页脚有一个 "Rust GUI Toolkits" 的 Compare 入口，但它**没有独立 URL**（不在 sitemap 中），不是 benchmark 页。

### 4.2 「比 Electron/Qt 小很多」的原始出处

**【官方】** 官方宣称集中在这几处，**都是定性 + 一个嵌入式场景的数字**：

[slint.dev/faqs](https://slint.dev/faqs)：
> "How much memory does Slint need? Slint compiles your UI to native machine code and is designed for low resource use. **Its runtime fits in under 300 KiB of RAM**, which makes it suitable for resource-constrained embedded devices."

[slint.dev/alternative-to-electron](https://slint.dev/alternative-to-electron) 与 [declarative-rust-gui](https://slint.dev/declarative-rust-gui)（同一句文案）：
> "Achieve low footprint and minimal resource consumption. **The Slint runtime fits in less than 300KiB RAM**, features a lazy property system, and is built with Rust."

[slint.dev/alternative-to-qt](https://slint.dev/alternative-to-qt)：
> "The Slint runtime is lightweight and can run on a wide range of platforms - **from resource-constrained devices with less 300KiB RAM** to desktops"

⚠️ **重要提醒**：这个 **<300 KiB** 是**嵌入式/MCU 场景下 Slint 运行时**的占用，**不是桌面应用的进程内存**。桌面应用实测是 100MB+ 量级（见下）。**引用这个数字时必须标注场景，否则会严重误导。**

**【官方】Slint 自己的 README 反而更保守**（[slint-ui/slint README](https://github.com/slint-ui/slint)）——没有 300KiB 这种绝对数字，只说：
> "**Lightweight**: Slint should require minimal resources, in terms of memory and processing power"
> "Components with their elements, items, and properties are **laid out in a single memory region, to reduce memory allocations**."

### 4.3 Slint 的真实体积/内存数字（这些才可用）

**【官方/维护者】** [slint-ui/slint#3376 "how can I optimize Slint's performance and memory usage?"](https://github.com/slint-ui/slint/discussions/3376)（2023-08-30，**Windows**）

Slint 维护者 **tronical**（Simon Hausmann，Slint 联合创始人）给出的体积数据：

> "on Windows, Qt6Core.dll is **5.6 MB**, and if you add Gui, a Qt application will have a disk footprint starting at **13-14MB**."
> "Our Slint Rust template starts for me at around **3.6 MB** and with LTO, stripping, etc. I get it down to **2.6MB** (although that's with the **femtovg** renderer, not Skia). I think that's pretty competitive as a starting point, since that's **statically linked** (no extra DLLs apart from Windows system DLLs needed)."

维护者给出的内存数据（Windows, VMware 虚拟机）：
> "an _empty_ qmlscene window weighs in at around **35MB** in the task manager... A similar minimal `export component App inherits Window {}` run with `slint-viewer.exe` and with `SLINT_BACKEND=Skia` clocks in at around **20MB** at the same window size... With `SLINT_BACKEND=Qt` I get a similar consumption at around **18MB**."

**【社区报告】同一讨论中用户 Horbin-Magician 的 Windows 实测表**（1080p + Intel HD Graphics 530 + Win10 + slint-rust-template）：

| 渲染后端 | 内存占用 | 文件大小 |
|---|---:|---:|
| femtovg | 51.1 MB | **3.45 MB** |
| skia-opengl | 52.6 MB | 19.5 MB |
| **skia-d3d** | **17.5 MB** | 20.3 MB |
| **winit-software** | **14.0 MB** | **3.87 MB** |

该用户补充的关键警告：
> "The memory usage of 'femtovg' and 'skia-opengl' will change dramatically when resize the window (**easily increase to more than 200MB**)"

原始报告者开头则说：
> "A blank window will use about **50M** of memory. This number increases dramatically as the number of windows increases."

**判定**：Slint 在 Windows 上的数字**强依赖渲染后端选择**，差距可达 3 倍以上：
- **体积最优**：femtovg 或 winit-software，约 **3.4–3.9 MB**
- **内存最优**：winit-software 14.0 MB / skia-d3d 17.5 MB
- **要避开**：femtovg 和 skia-opengl 在改窗口大小时可飙到 200MB+

维护者 tronical 对内存机制的权威解释（2025-03-22 回复）：
> "In practice **the biggest consumer of memory are rendering buffers that correlate with the window size**... those can skew results because they tend to be orders of magnitude bigger than the rest of the UI related data structures typically allocated."

**【第三方实测】** Zenn 横评（macOS M1）：Slint 在**全部 7 个测试场景中都是第一名或接近第一**（105.9–133.0 MB），是 15 个框架中最省内存的。作者原话：「Electron 与 Slint 的差距实为约 **4.5 倍**」。

### 4.4 ⚠️ 许可证：这是 Slint 的真实成本，不是技术问题

**【官方】** [slint.dev/pricing](https://slint.dev/pricing)：

| 许可 | 适用 | 费用 |
|---|---|---|
| **GPLv3** | 开源应用 | 免费 |
| **Startup & Individual** | 专有应用；≤10 员工、≤200 万欧元营收、成立 <5 年 | 付费 |
| **Small Enterprise** | 专有应用；≤50 员工、≤1000 万欧元营收 | 付费 |
| **Royalty-Free** | 专有应用（**不含嵌入式系统**） | 付费 |
| **Enterprise** | 专有应用；含 Perpetual Fallback License | 付费 |

**【官方】** [slint.dev/faqs](https://slint.dev/faqs)：
> "Slint is available under three licenses: an **open-source GPLv3 license**, a **royalty-free license for proprietary projects**, and a **paid commercial license** with additional support."

**判定**：若你的播放器要闭源分发，**Slint 需要购买商业许可**（价格未在页面公开，需联系销售）。GPLv3 意味着闭源分发不合规。这是选型中必须提前确认的商业约束，**egui（MIT/Apache-2.0）和 iced（MIT）没有这个问题**。

（注：Slint 的 `qt` style 后端依赖系统安装的 Qt，见 [README](https://github.com/slint-ui/slint) 原文："When Qt is installed on the system, the `qt` style becomes available"。）

---

## 5. GPUI（Zed 的 GUI 框架）

### 5.1 可用性：可用于独立应用，但官方明确说 pre-1.0

**【官方】** [crates.io/crates/gpui](https://crates.io/crates/gpui) 实测（我直接查了 crates.io API）：

| 项 | 值 |
|---|---|
| 最新版本 | **0.2.2** |
| 发布日期 | **2025-10-22** |
| 总下载量 | 272,251 |
| 描述 | "Zed's GPU-accelerated UI framework" |
| 历史版本 | 0.1.0 (2022-06-23)、0.2.0 (2025-10-09)、0.2.1 (2025-10-14)、0.2.2 (2025-10-22) |

⚠️ **注意**：0.1.0 是 2022 年，然后**空窗三年**，直到 2025-10 才重新发版。说明它是**近期才真正对外发布**的。

**【官方】** [crates/gpui/README.md](https://github.com/zed-industries/zed/blob/main/crates/gpui/README.md) 原文（这是最重要的风险提示）：

> "**GPUI is still in active development as we work on the Zed code editor, and is still pre-1.0. There will often be breaking changes between versions.** You'll also need to use the latest version of **stable Rust**."

> "Currently, the best way to learn about these APIs is to **read the Zed source code** or drop a question in the Zed Discord. We're working on improving the documentation, creating more examples..."

**判定**：官方**明确承认** API 不稳定、会有破坏性变更、文档主要靠读 Zed 源码。这是「能不能用于生产」的核心答案：**技术上可以，工程风险自担。**

### 5.2 Windows 支持：官方支持，用 Win32 + DirectWrite

**【官方】** 同一 README 原文：

> "**Windows** — **no features are required**. Windowing uses **Win32** and text uses **DirectWrite**. `font-kit` has no effect here."

对比 macOS 需要 Xcode/Metal、Linux 需要 `wayland`/`x11` feature，**Windows 反而是零配置的**。

Windows 支持时间线（【官方】PR/commit）：

| 时间 | 事件 |
|---|---|
| **2024-03-03** | PR [#8490](https://github.com/zed-industries/zed/pull/8490) `Windows gpui platform` 合并，作者 **`kazatsuyu`（社区贡献者，非 Zed 员工）** → Windows 地基由社区打的 |
| 2025-08 | 处于 **alpha** 阶段 |
| **2025-09-30** | 发布 Windows **公开 beta** |
| **2025-10-19** | PR [#40650](https://github.com/zed-industries/zed/pull/40650) 移除 beta 模板 → **转正** |
| 2026-02-19 | PR [#49277](https://github.com/zed-industries/zed/pull/49277) 拆出 `gpui_windows` / `gpui_platform` |

平台层实证：`crates/gpui_windows/src/` 含 `directx_renderer.rs`(72.6KB)、`direct_write.rs`(75.9KB)、`shaders.hlsl`(47KB) → **Windows 走自研 DirectX + HLSL 后端，不是 wgpu**。

**判定**：GPUI 的 Windows 支持是官方的、完整平台层的（Zed 自身在 Windows 发布）。但**成熟得很晚**——2025-10 才转正。

### 5.2b ⚠️ Windows 构建的硬门槛：release 需要 `fxc.exe`

**【官方】** `crates/gpui_windows/build.rs`：**只在 release 构建时**（`#[cfg(not(debug_assertions))]`）编译 HLSL shader，调用 **`fxc.exe`**（Windows SDK 里的旧版 shader 编译器），目标 profile `vs_4_1` / `ps_4_1`。查找顺序：`GPUI_FXC_PATH` 环境变量 → PATH → 注册表找最新 SDK；**找不到就 `panic!("Failed to find fxc.exe")`**。

⚠️ **debug 构建不需要，release 构建才需要**——这正是 issue #46263 里「debug 能跑、release 崩」的原因。

**【官方回复】** Zed 核心维护者 `reflectronic` 在 [issue #46263](https://github.com/zed-industries/zed/issues/46263)（2026-01-07）原文：

> "The version of GPUI that's currently on crates.io **requires that the 10.0.26100.0 version of the Windows SDK** is installed. I would guess that you have a different version. You can either: Install the 26100 SDK / Set the `GPUI_FXC_PATH` environment variable... / **Reference GPUI from this git repository directly, which has been updated to look for any installed SDK version**"

另外还需要（【官方】[docs/src/development/windows.md](https://github.com/zed-industries/zed/blob/main/docs/src/development/windows.md)）：MSVC + **Spectre-mitigated libs**、Windows 10 SDK ≥ 10.0.20348.0、**CMake**。

✅ **纠正一个常见误解**：GPUI **不需要 nightly**。README 原文 "latest version of **stable Rust**"，仓库 `rust-toolchain.toml` 锁定 `channel = "1.98.1"`（stable）。网上「GPUI 要 nightly」是过时信息。

### 5.2c ⚠️⚠️ 最大的坑：crates.io 交付断层近 11 个月

这是 GPUI 选型中**最容易踩、也最致命**的问题：【官方 API 实测】

| crate | crates.io 最新版 | 发布时间 | 仓库当前 |
|---|---|---|---|
| `gpui` | **0.2.2** | **2025-10-22** | workspace 0.62 |
| `gpui_platform` | **不存在** | — | 从未发布 |
| `gpui_windows` / `gpui_linux` / `gpui_macos` / `gpui_web` / `gpui_wgpu` 等 | **均不存在** | — | 均从未发布 |

- `gpui` 版本史：`0.1.0`(2022, yanked) → `0.2.0`(2025-10-09) → `0.2.1`(2025-10-14) → **`0.2.2`(2025-10-22，唯一非 yank 的最新版)**。**距调研时已约 10.8 个月未发版**，期间 `crates/gpui` 有 100+ 次提交。
- **官方 README 推荐的 `gpui_platform` 在 crates.io 上根本不存在**（crates.io API 返回 `does not exist`，docs.rs 返回 `no such crate`）。**照官方 README 建新项目会直接构建失败。**
- 而 crates.io 上 0.2.2 那份 README 写的是 "be on **macOS or Linux**"——**把 Windows 排除在外**，且用旧入口 `Application::new()`。
- 社区被迫自己发了一堆替代品：`gpui-ce`（社区 fork，star 1,047）、`gpui-pre`（仓库快照，0.3.4 发于 2026-09-07）、`gpui-box`（自述 "not an official Zed project"）。crates.io 上依赖 `gpui` 的 crate 共 **126 个**。

**【社区最直白的一句】** [issue #59928](https://github.com/zed-industries/zed/issues/59928)（2026-07-19）用户 `grinapo`：

> "Let us all hope there will be further releases then since **I can't really use development version for (kind of prod) code.**"

**判定**：想要能用的新版本，只能 `gpui = { git = "..." }` 跟 main 分支，或用社区快照——**这两条路都跟「生产稳定性」直接冲突**。这是 GPUI 目前最难绕过的工程障碍。

### 5.3 成熟度：有真实的独立应用，而且是**音乐播放器**

**【官方】** [zed-industries/awesome-gpui](https://github.com/zed-industries/awesome-gpui)（1,272 stars）——官方维护的 GPUI 项目列表。

我实测核实的独立应用（非 Zed）：

| 项目 | Stars | 说明 | 链接 |
|---|---:|---|---|
| **sonora** | **1,044** | **「A native music streaming client, built with Rust and GPUI」** — 支持 Spotify/YouTube Music/Subsonic/本地文件 | [sonorahq/sonora](https://github.com/sonorahq/sonora) |
| gpui-kit | 14,306 | Rust GUI 组件库（Longbridge 出品） | [longbridge/gpui-kit](https://github.com/longbridge/gpui-kit) |
| gpuix | 1,784 | Node.js/React 绑定「no Electron」 | [remorses/gpuix](https://github.com/remorses/gpuix) |
| Loungy | 1,735 | Spotlight/Alfred/Raycast 类启动器 | [MatthiasGrandl/Loungy](https://github.com/MatthiasGrandl/Loungy) |
| gpui-ce | 1,047 | **GPUI – Community Edition**（社区 fork，说明有人在官方之外自己维护） | [gpui-ce/gpui-ce](https://github.com/gpui-ce/gpui-ce) |
| helix-gpui | 540 | Helix 编辑器的 GPUI 前端 | [polachok/helix-gpui](https://github.com/polachok/helix-gpui) |
| zed | 90,144 | 本体 | [zed-industries/zed](https://github.com/zed-industries/zed) |

我还实测核对了 GPUI 生态的规模（【官方 API 实测】）：`zed` star **90,145**、fork 10,569、**贡献者 482 人**、`area:gpui` 标签 issue **open 121 / closed 322**；`crates/gpui` 90 天内 ≥100 次提交。

其他千星以上的 GPUI 独立应用（均来自官方清单 [awesome-gpui](https://github.com/zed-industries/awesome-gpui)，80+ 项目）：
- **OpenLogi**（罗技 Options+ 原生替代）— **20,773 stars**
- **Zedis**（原生 Redis GUI）— 2,074 stars
- Waku / Zeron / Navop（agent 客户端、数据库工作台）— ~1,500 / ~1,400 / ~1,300
- tty7 / GitComet / Arbor — ~869 / ~828 / ~812
- hummingbird（音乐播放器）/ helix-gpui / termy — ~604 / ~540 / ~423

⚠️ 注意 [Loungy](https://github.com/MatthiasGrandl/Loungy)（1,735 stars 的启动器）在官方清单里已被标为 **📦 archived**——生态有真实项目停摆。官方脚手架 [create-gpui-app](https://github.com/zed-industries/create-gpui-app) 最后推送 **2025-04-13**，已 17 个月未维护。

### 5.3b ⚠️ GPUI 没有局部重绘（damage tracking）——官方成员确认

**【官方成员回复】** [issue #37727](https://github.com/zed-industries/zed/issues/37727)「Windows Alpha: Text typing loads GPU as FullHD video playing」（2025-09-07 开，**至今 open**，标签 `severity:S2`、`reach:many users`）

用户实测原文：
> "Text typing in the VSCode loads CPU: 5% / GPU: 5%. Text typing in the Zed loads: CPU: 5% / **GPU: up to 20%**"

Zed 官方成员 `reflectronic` 回复原文：
> "My guess is that this is caused by us **always re-drawing the whole window whenever anything changes**... I think we need to accumulate **dirty rects** as we run layout, do some basic CPU side culling of draw items, and then use scissor rect/stencil buffer to only paint the changed pixels on top of the old buffer."

2026-07-17 追加实测（另一用户）：
> "I can reproduce high GPU usage on **Windows 11 with Zed 1.11.3**. The small spinner while Zed is waiting... is enough to trigger **70-80% GPU use on the Intel integrated GPU**."

**判定**：**GPUI 目前没有 damage/dirty-rect 局部重绘，任何变化都整窗重绘**——由官方成员亲口确认，且被列为「需要做但还没做」。这不是编辑器特有问题，而是**框架层面的架构缺口**，会影响所有 GPUI 应用（含音乐播放器的动画/进度条）。

其他 Windows 专属 open issue：[#63471](https://github.com/zed-industries/zed/issues/63471)（`WM_DISPLAYCHANGE` 无条件 `ShowWindow`）、[#58746](https://github.com/zed-industries/zed/issues/58746)（`WM_MOUSEMOVE` 未去重致 hover 失效）、[#52448](https://github.com/zed-industries/zed/issues/52448)（原生 Win32 模态阻塞 GPUI 消息泵）。

### 5.4 ⭐ GPUI 音乐播放器的真实体积（最有参考价值的数据）

**Sonora** 跟你的项目几乎同类（原生音乐客户端、纯 Rust + GPUI、跨平台、有 Windows 版）。我直接量了它最新 release 的**实际产物大小**：

**【第三方实测/我实测】** [Sonora v0.34.3 release assets](https://github.com/sonorahq/sonora/releases/tag/v0.34.3)（发布于 2026-09-12，GPL-3.0，1,044 stars，创建于 2026-08-03）

| 产物 | 字节 | MB | 下载数 |
|---|---:|---:|---:|
| **`Sonora-Setup.exe`（Windows 安装包 x64）** | 16,422,634 | **15.66 MB** | 122 |
| `Sonora-Setup-arm64.exe` | 15,332,671 | 14.62 MB | 2 |
| **`sonora-v0.34.3-x86_64-pc-windows-msvc.exe`（裸 exe）** | 60,818,944 | **58.0 MB** | 7 |
| `sonora-v0.34.3-macos.dmg` | 46,217,899 | 44.08 MB | 1 |
| `sonora-v0.34.3-x86_64-unknown-linux-gnu` | 77,138,304 | 73.56 MB | 35 |
| `sonora-v0.34.3-x86_64.flatpak` | 18,577,216 | 17.72 MB | 5 |

⚠️ **必须说明这个 58MB 里装了什么**（我读了它的 `Cargo.toml` 和 README）：
- 静态链接的**重型依赖**：`librespot`（Spotify 协议）、`symphonia`（mkv/ogg/mp4 解码）、`rustfft`、`rodio`、`rusqlite`、`reqwest`+`rustls`、`tokio`、`image`(jpeg/png/webp)、`lofty`、`moka`
- **嵌入资源**：Inter 字体 + **4 套图标集**（Lucide/Iconoir 等）
- `Cargo.toml` 中**没有** `[profile.release]` 优化段（无 LTO/strip/opt-level="z"）

**判定**：58MB 是「功能完整的音乐播放器 + 未做体积优化 + 嵌入字体图标」的结果，**不代表 GPUI 的框架开销**。但它是**同类应用的真实参照**。若做 LTO + strip + 精简依赖，明显有压缩空间。

**【未找到】** GPUI **最小 hello_world 应用**的二进制体积或内存占用数据——官方无 benchmark，我也未在社区找到可靠实测。**这项数据缺失。**

但**有其他独立应用的一手体积数据**（【gh api 实测 release asset 字节数】）：

**Zedis v0.10.0**（[vicanso/zedis](https://github.com/vicanso/zedis/releases)，原生 Redis GUI，2,074 stars，2026-09-12 发布）：

| 产物 | 体积 |
|---|---:|
| `zedis-linux-x86_64.tar.gz` | 22.68 MB |
| `zedis-x86_64.AppImage.tar.gz` | 21.13 MB |
| `zedis-linux-x86_64.rpm` | 17.07 MB |
| **`zedis-windows-x86_64.msi`** | **17.08 MB** |
| `zedis-windows-x86_64.zip` | **16.48 MB** |
| `Zedis-x86_64.dmg` (macOS) | 17.28 MB |
| `zedis-windows-aarch64.zip` | 15.95 MB |

与 Sonora（Windows 安装包 15.66 MB）交叉印证：**一个功能完整的 GPUI 独立应用，Windows 安装包在 15–23 MB 量级。** 两个不同项目、不同功能域，落在同一区间，可信度较高。

⚠️ **Zed 本体体积（仅供量级参考，绝不可当作 GPUI 应用体积）**：`zed-linux-x86_64.tar.gz` **141.66 MB**、`Zed-aarch64.dmg` **140.14 MB**、`Zed-aarch64.exe` **73.19 MB**——这些含编辑器全部功能，与「GPUI 框架开销」无关。

### 5.4b 内存：框架自身曾固定吃掉 600 MB（官方 PR）

**【官方一手】** [PR #45197 "Don't preallocate 600MB for GPUI profiler"](https://github.com/zed-industries/zed/pull/45197)（作者 sourcefrog，2025-12-18 建，**2026-03-27 合并**）body 原文：

> "Previously, the GPUI profiler allocates one CircularBuffer per thread, and `CircularBuffer<N>` always preallocates space for N entries. As a result it **allocates ~20MB/thread, and on my machine about 33 threads are created at startup for a total of 600MB used.** In this PR I change it to use a VecDeque that can gradually grow up to 20MB as data is written. … it seems that this caps overall usage at about **21MB** … Since this is **fixed overhead for everyone running Zed** it seems like a worthwhile gain."

**判定**：2026-03-27 之前，GPUI 的 profiler 在每个进程里**固定预分配约 600 MB**；修复后约 21 MB。这是 GPUI 在 Zed 里的默认开销，**属于「框架自身固定成本」的量级证据**，但**不能直接等同**于任意 GPUI 独立应用的内存占用。

**【社区 issue，仅供参考】** [issue #18673](https://github.com/zed-industries/zed/issues/18673)「Zed uses lot of RAM」（2024-10-03 开，**至今 open**）：用户原文 "Here zed is using around **2GB of ram** and I have checked it on Reddit some people were saying their device is showing just **200mb to 300mb**"——单点观测，波动大。

⚠️ [issue #20490](https://github.com/zed-industries/zed/issues/20490) 报告的 14.51 GB **已被澄清大概是 rust-analyzer 而非 Zed 本体**，**不可用作 GPUI 内存数据**。

**【未找到】** 「GPUI 独立应用进程空载内存」**无任何可靠实测数据**。第三方「比 Electron 省 60-80%」的说法属宣传口径，无测试方法说明，**不建议引用**。

### 5.5 风险提示

- **crates.io 交付断层近 11 个月**，官方 README 推荐的 `gpui_platform` 根本没发布（§5.2c）——**最容易踩的坑**
- **pre-1.0，破坏性变更是常态**（官方 README 原话）
- **release 构建硬依赖 Windows SDK 26100 的 `fxc.exe`**，否则 panic（§5.2b）
- **没有 damage tracking**，整窗重绘，Windows 上实测打字 20% GPU、spinner 70-80% GPU（§5.3b）
- **文档稀缺**，官方建议「读 Zed 源码」（官方 README 原话）
- 存在**社区 fork**（gpui-ce）和**第三方组件库**（gpui-kit），说明生态靠社区补位，但也说明官方投入有限
- 官方脚手架 `create-gpui-app` 已 17 个月未更新；`Loungy`（1.7k stars）已被归档
- 需要**最新 stable Rust**（不是 nightly）
- ⚠️ **许可证反而友好**：GPUI 本体是 **Apache-2.0**（`crates/gpui/Cargo.toml`），而 Zed 主体是 GPL-3.0-or-later。即**用 GPUI 写闭源应用在许可上没问题**，这点比 Slint 有利——代价是要依赖 git 版。

---

## 6. 横向对比总表

### 6.1 体积

| 技术栈 | 最小二进制 | Windows 安装包 | 平台/来源 | 分级 |
|---|---:|---:|---|---|
| **Tauri 2** | **2.61 MB**（hello_world） | +0 / +1.8 / +127 / **+180 MB**（WebView2 模式） | Windows / Tauri 官方 benchmark JSON + 官方文档 | 【官方】 |
| **egui/eframe** | 10.6 MB(Linux) | 未找到 | Linux / IronOxidizer | 【第三方实测】 |
| **iced** | **1.5 MB**（glow 后端）/ 3.1 MB（wgpu+LTO） | 未找到 | **Windows** / iced#1531 | 【社区报告】 |
| **Slint** | **3.45–3.87 MB**（femtovg / winit-software）/ 2.6 MB 带 LTO（维护者） | 未找到 | **Windows** / slint#3376 | 【官方+社区报告】 |
| **GPUI** | 未找到（最小应用无数据）；同类完整应用 58 MB | **15.66 MB**（Sonora）/ **16.48–17.08 MB**（Zedis） | Windows / 我实测 release assets | 【第三方实测】 |

补充：**真实 Tauri 2 项目**的 Windows 安装包实测为 **1.85–18.08 MB**（inkwell 1.85MB exe / 2.44MB msi；optimizationmaxxing 10.91MB；LocalSend 18.08MB），见 §1.3b。

### 6.2 空载/峰值内存

| 技术栈 | 空载内存 | 复杂场景内存 | 平台/来源 | 分级 |
|---|---:|---:|---|---|
| **Tauri 2** | **261 MB**（hello_world） | **307–739 MB** | Linux 官方 / macOS Zenn | 【官方】/【第三方实测】 |
| — **空白 WebView2 基线（实测）** | **336.6 MB**（7 进程，其中 6 个 msedgewebview2 占 286.1 MB） | — | **Windows 10 / 自建实测** | 【自建实测】 |
| — Tauri 真实浏览器内容 | Windows: **370–399 MB** | — | Windows 10 / tauri#5889 | 【社区报告】 |
| **egui** | 框架本身 **3.1 MB**；进程 30–36 MB | 142–157 MB（含25MB字体） | 官方issue / Zenn | 【官方】/【第三方实测】 |
| **iced** | **27 MB**（glow）/ 76 MB（wgpu 默认） | 140–177 MB | **Windows** / Zenn | 【社区报告】/【第三方实测】 |
| **Slint** | **14.0 MB**（winit-software）/ 17.5 MB（skia-d3d）/ ~20 MB（维护者） | 105.9–133.0 MB（横评第一） | **Windows** / macOS Zenn | 【官方+社区】/【第三方实测】 |
| **GPUI** | 未找到（独立应用无数据）；框架 profiler 曾固定 600MB → 修复后 ~21MB | 未找到 | 官方 PR #45197 | 【官方】/【未找到】 |

### 6.3 关键风险对照

| 技术栈 | 主要风险 | 状态 |
|---|---|---|
| Tauri 2 | Windows 上是 Chromium；**空白 WebView2 就吃 336MB**；内存对 Electron 无优势；离线分发 +180MB | 架构固有，无法修复 |
| — Tauri 2 额外 | 读文件内存放大（#9190，140MB→11.5GB）、事件泄漏（#12724，前端 1.1GB） | **均 open** |
| egui | **Windows 鼠标移动即 20-30% CPU**（官方确认，#4173） | **open** |
| iced | Windows 文本渲染吃内存 (#2064)、最小化泄漏 (#2659) | **open** |
| Slint | **闭源需购买商业许可**；内存随窗口尺寸/后端波动大（改尺寸可飙到 200MB+） | 商业约束 |
| GPUI | **crates.io 断层近 11 个月**、release 构建要 SDK 26100 的 fxc.exe、**无局部重绘（整窗重绘）**、pre-1.0 breaking | 官方明示 |

---

## 7. 直接回答你的三个特别关注点

### Q1：「无浏览器内核」诉求下，Tauri 算不算？

**不算。如实说：Tauri 在 Windows 上就是 Chromium。**

依据（全部为 Tauri 官方文档原文 / 自建实测）：
1. "**Tauri uses WebView2 which is based on Microsoft Edge and therefore Chromium.**"（[webview-versions](https://v2.tauri.app/reference/webview-versions/)）← 最直接的一句
2. "Tauri uses **Microsoft Edge WebView2** to render content on Windows"（[prerequisites](https://v2.tauri.app/start/prerequisites/)）
3. "A WebView is a **browser-like environment** that executes your HTML, CSS, and JavaScript"（[process-model](https://v2.tauri.app/concept/process-model/)）
4. "Tauri employs a **multi-process architecture similar to Electron** or many modern web browsers"（同上）
5. **实测**：空白 WebView2（无任何 UI）进程树合计 Working Set **336.6 MB**，其中 6 个 `msedgewebview2.exe` 占 **286.1 MB**（§1.5b）
6. Tauri 团队在 #5889 中自己确认：「both are based on Chromium」

**Tauri 的真实优势是「不把内核打进安装包」，不是「没有内核」。** 在 Windows 上，内核以 WebView2 形式预装、由系统更新，你的应用仍然要承担 Chromium 的内存与多进程开销——**省下的是磁盘，省不下的是内存**。若你的核心诉求是「真正无浏览器内核」，**必须排除 Tauri**，走 egui / iced / Slint / GPUI 之一。

唯一例外：若「无浏览器内核」对你实际意味着「安装包小、不捆绑内核」，那 Tauri 是合规的——但要把这条诉求写清楚，别混为一谈。

### Q2：有没有同一个 app 用多个框架实现的横向对比仓库？

**有，但都有明显局限**，我找到这几个（按可用性排序）：

| 对比 | 覆盖 | 平台 | 局限 |
|---|---|---|---|
| [Zenn 15 框架内存横评](https://zenn.dev/mizugeeks/articles/1019cf2353d343) + [源码](https://github.com/mizugeek/workbench_mem_bench) | **Electron/Tauri/Wails/Neutralino/Flutter/Compose/Avalonia/Fyne/Slint/egui/iced/GTK4/Qt6/AppKit/SwiftUI** | ⚠️ macOS M1 | **非 Windows**；egui/iced 含 +25MB CJK 字体（作者已说明） |
| [IronOxidizer/gui-toolkit-benchmarks](https://github.com/IronOxidizer/gui-toolkit-benchmarks) | fltk/fltk-rs/druid/gtk/wx/gtk-rs/tkinter/qt/qml/swing/**iced/egui**/imgui-rs/dioxus/**tauri**/electron | ⚠️ Linux X11 | **非 Windows**；数据 2023-10 较旧 |
| [Lukas Kalbertodt 博文](https://lukaskalbertodt.github.io/2023/02/03/tauri-iced-egui-performance-comparison.html) | **Tauri / Iced / egui** | ⚠️ Ubuntu 20.04 | **非 Windows**；2023-02；作者自己声明「未测 Windows/macOS，可能完全不同」 |
| [buildwithrust.com Iced vs Tauri 2](https://buildwithrust.com/iced-vs-tauri-2-we-built-the-same-app-twice-in-rust) | iced / Tauri 2（同一区块链钱包 App 双实现） | 未明确 | **只有定性结论，无体积/内存数字**；2026-04 |
| [ironoxidizer 表格中的 tauri 行](https://ironoxidizer.github.io/gui-toolkit-benchmarks/) | 见上 | Linux | 同上 |

⚠️ **关于「2026 年 Tauri vs Electron 完整 benchmark」类文章**：我找到了 [buildr.sh/posts/tauri-vs-electron-2026](https://buildr.sh/posts/tauri-vs-electron-2026)，它给出 Tauri 8.1MB 安装包 / 38MB 空载 RAM 的数字，但**明确标注测试平台是 M2 macOS**（走 WebKit，非 WebView2）。**该数字不能用于 Windows 决策**，我也不建议引用——它容易被误当成 Windows 数据。

**结论：我没有找到任何在 Windows 上对 tauri/egui/iced/slint 做同一 app 横向实测的可靠仓库。** 这是本次调研最大的数据缺口。**【未找到可靠数据】**

### Q3：有没有 egui 内存异常 / immediate mode 一直重绘的权威说法？

**分两半回答：**

**（a）「immediate mode 会一直重绘」——不成立，有官方反证。**
egui 官方 README 原文：「egui **only repaints when there is interaction (e.g. mouse movement) or an animation, so if your app is idle, no CPU is wasted.**」这是最权威的出处，直接否定该说法。（详见 §2.3）

**（b）但 egui 在 Windows 上确实有高 CPU 问题——成立，且由维护者确认。**
[emilk/egui#4173](https://github.com/emilk/egui/issues/4173)：Windows 上鼠标移动即 **20-30% CPU**（Linux 同机 1-2%），连 `hello_world` 都 30%。emilk 确认根因是 OpenGL context 切换（`make_not_current` 占 90% 时间），2024-04 时仍未修复，**issue 至今 open**。相关 open issue 还有 #5092（wgpu 0.20 后 100% CPU）、#7401、#3801。

**（c）内存「异常高」的说法要拆开看。**
- egui 框架自身仅 **3.1 MB**（官方 issue 诊断），进程 30-36MB 里主要是字体纹理 + GPU 缓冲 + OS 开销。
- 所以「egui 吃 30MB」不是泄漏，是**渲染栈的固定成本**。
- 真正的量级问题在 Tauri/Electron 那一档（300-740MB），不在 egui 这一档。

---

## 8. 数据缺口（明确列出，未编造）

| 缺口 | 说明 |
|---|---|
| **Windows 上 egui/iced/Slint 最小应用 release 二进制的统一实测** | 无。iced 有 #1531（counter 示例，2022）、Slint 有 #3376（模板，2023），egui 只有 Linux 数据 |
| **GPUI 最小应用体积/内存** | 最小应用无数据（但同类完整应用有：Sonora 15.66MB、Zedis 16.48MB）；**GPUI 独立应用空载内存完全无数据** |
| **Windows 上 tauri/egui/iced/slint 同 app 横向对比** | 不存在（见 Q2） |
| **Tauri 官方 Windows 内存数据** | 官方明确「只在 Linux 测内存」（benchmark 面板原文），Windows/macOS 的 `max_memory` 全空 |
| **Slint 官方 benchmark 页** | 不存在（sitemap 已全量核对） |
| **egui 官方体积基准** | 不存在 |
| **iced / Slint 的 Windows 安装包体积** | 无官方数据；需自己打包实测（egui/iced/Slint 通常直接发裸 exe 或简单 zip，无标准"安装包"概念） |
| **GPUI 首次 `cargo build` / release 编译耗时** | 无可靠数据（docs.rs 的 2m9s 只跑 `cargo doc`，不含 codegen/链接，不能代表构建时间） |
| **GPU 显存占用对比** | 所有来源都未覆盖 |
| **CPU 占用横向对比（Windows）** | 仅零散 issue，无系统对比 |

### 建议的补测方案（针对缺口）

以上缺口都很容易自己补，成本很低：
1. 各框架写一个**同一 UI 的最小播放器外壳**（列表 + 播放条 + 封面）
2. `cargo build --release`，量**裸 exe 大小**
3. 空载后 `Get-Process | Select WorkingSet64`，**Tauri 需额外累加所有 `msedgewebview2.exe` 子进程**
4. 同一台 Windows 机器上量 **CPU 占用**（空载 / 鼠标移动 / 播放动画三种状态）
5. Tauri 额外测 `fixedVersion` 打包后的安装包大小

---

## 9. 给 BoTapMusic 的取向建议（含【我的推测】标注）

以下为**基于上述数据的判断**，非实测结论：

1. **「无浏览器内核」是硬约束 → 排除 Tauri。** 依据是官方文档原文 + 空白 WebView2 实测 336MB，无争议。
2. **若要「闭源 + 免费 + 最省内存」→ Slint 有许可证问题，egui/iced/GPUI 无。** Slint 技术指标最好（Zenn 横评全项第一），但商业许可要付费；egui（MIT/Apache-2.0）、iced（MIT）、**GPUI（本体 Apache-2.0）**免费。
3. **egui 的 Windows CPU 问题是最大隐忧**（#4173 open）。音乐播放器用户会长时间悬停、拖动进度条——正是触发该问题的场景。**【我的推测】** 建议在 Windows 上先写 30 行 demo 实测「鼠标移动时的 CPU 占用」，再决定。
4. **iced 建议直接关掉默认 wgpu，用 glow 后端**（#1531 实测 76MB→27MB、3.1MB→1.5MB）。但要留意 #2064（Windows 文字渲染吃内存）和 **#2659（最小化内存泄漏）**——播放器常驻托盘很常见。
5. **GPUI 不建议用于要交付的项目**：crates.io 断层近 11 个月、README 推荐的 crate 不存在、release 要特定 SDK 的 fxc.exe、无局部重绘、官方明说 pre-1.0 会 breaking。**但 Sonora 与 Zedis 证明了技术上完全可行**（Windows 安装包 15.66 / 16.48 MB）。
6. **体积上真正的赢家是 iced（glow，1.5MB）和 Slint（femtovg，3.45MB）**，都在 Windows 上有实测；egui 缺 Windows 数据，需自测。GPUI 同类应用实测 15–23MB。
7. **内存上只有 Tauri 那一档（300MB+）有明显劣势**，egui/iced/Slint 都在同一量级（30-170MB，取决于场景和字体策略）。GPUI 无数据。
8. **「体积小」和「内存小」在 Tauri 身上是两件事**：安装包 2MB，但跑起来 336MB 起。做决策时务必把这两个指标分开看 —— 这是本次调研最容易误导人的地方。

---

## 附：来源清单（全部为本次实际读取过的页面）

### 官方文档
- [Tauri Process Model](https://v2.tauri.app/concept/process-model/) — WebView2/多进程架构原文
- [Tauri Webview Versions](https://v2.tauri.app/reference/webview-versions/) — **"based on Microsoft Edge and therefore Chromium"**
- [Tauri Prerequisites](https://v2.tauri.app/start/prerequisites/) — "uses Microsoft Edge WebView2"
- [Tauri Windows Installer](https://v2.tauri.app/distribute/windows-installer/) — WebView2 各模式体积表
- [Tauri 首页](https://v2.tauri.app/start/) — "less than 600KB" 宣称
- [Tauri App Size](https://v2.tauri.app/concept/size/) — release profile 优化项（无绝对数字）
- [微软 WebView2 性能文档](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance) — 优先 Evergreen 的官方立场
- [Slint FAQ](https://slint.dev/faqs) — "under 300 KiB of RAM"
- [Slint Pricing](https://slint.dev/pricing) — 许可分级
- [Slint alternative-to-electron](https://slint.dev/alternative-to-electron) / [alternative-to-qt](https://slint.dev/alternative-to-qt) / [declarative-rust-gui](https://slint.dev/declarative-rust-gui)
- [Slint sitemap.xml](https://slint.dev/sitemap.xml) — 用于确认无 benchmark 页
- [Zed Windows 构建文档](https://github.com/zed-industries/zed/blob/main/docs/src/development/windows.md) — VS 组件/SDK/CMake 要求
- [Zed rust-toolchain.toml](https://github.com/zed-industries/zed/blob/main/rust-toolchain.toml) — stable 1.98.1（非 nightly）

### 官方仓库 / 原始数据
- [tauri-apps/benchmark_results](https://github.com/tauri-apps/benchmark_results)（`gh-pages`：`tauri-recent-windows.json` / `tauri-recent-linux.json` / `tauri-recent-macos.json` / `electron-recent.json`）
- [Tauri 官方 benchmark 结果页](https://tauri-apps.github.io/benchmark_results/) — 原文 "We currently only measure the Memory Usage on Linux"
- [Tauri WebviewInstallMode 源码注释](https://github.com/tauri-apps/tauri/blob/dev/crates/tauri-utils/src/config.rs) — 1.8/127/180MB 与文档一致
- [egui README - Why immediate mode](https://github.com/emilk/egui#why-immediate-mode)
- [eframe README](https://github.com/emilk/egui/blob/main/crates/eframe/README.md)
- [Slint README](https://github.com/slint-ui/slint)
- [GPUI README](https://github.com/zed-industries/zed/blob/main/crates/gpui/README.md)
- [GPUI Cargo.toml](https://github.com/zed-industries/zed/blob/main/crates/gpui/Cargo.toml) — license = "Apache-2.0"
- [gpui_windows/build.rs](https://github.com/zed-industries/zed/blob/main/crates/gpui_windows/build.rs) — fxc.exe / SDK 查找逻辑
- [awesome-gpui](https://github.com/zed-industries/awesome-gpui)
- [crates.io/crates/gpui](https://crates.io/crates/gpui) — 0.2.2 / 2025-10-22
- [docs.rs/crate/gpui/0.2.2](https://docs.rs/crate/gpui/0.2.2)

### Issue / 讨论（真实社区报告）
- [tauri#5889](https://github.com/tauri-apps/tauri/issues/5889) — Tauri 内存可能高于 Electron（含 Windows 10 数据 + 维护者否定官方 benchmark）
- [tauri#12724](https://github.com/tauri-apps/tauri/issues/12724) — Tauri 2 事件内存泄漏（前端 1.1GB，**OPEN**）
- [tauri#9190](https://github.com/tauri-apps/tauri/issues/9190) — Windows 读文件内存放大到 11.5GB（**OPEN**）
- [tauri discussion#11553](https://github.com/orgs/tauri-apps/discussions/11553) — 维护者解释 WebView2 进程构成
- [emilk/egui#3689](https://github.com/emilk/egui/issues/3689) — egui RAM 组成分析
- [emilk/egui#4173](https://github.com/emilk/egui/issues/4173) — Windows 高 CPU（维护者确认，open）
- [emilk/egui#5092](https://github.com/emilk/egui/issues/5092) / [#7059](https://github.com/emilk/egui/issues/7059) / [#7401](https://github.com/emilk/egui/issues/7401) / [#3801](https://github.com/emilk/egui/issues/3801) / [#7776](https://github.com/emilk/egui/issues/7776)
- [emilk/egui#5112](https://github.com/emilk/egui/issues/5112) — 隐藏时 request_repaint 被忽略
- [iced-rs/iced#1531](https://github.com/iced-rs/iced/discussions/1531) — Windows 体积/RAM 实测（glow vs wgpu）
- [iced-rs/iced#2064](https://github.com/iced-rs/iced/issues/2064) — Windows 文字渲染吃内存
- [iced-rs/iced#2659](https://github.com/iced-rs/iced/issues/2659) — 最小化内存泄漏
- [slint-ui/slint#3376](https://github.com/slint-ui/slint/discussions/3376) — Slint 各后端体积/内存实测（维护者 + 用户）
- [zed#45197](https://github.com/zed-industries/zed/pull/45197) — GPUI profiler 曾固定预分配 600MB → 21MB
- [zed#37727](https://github.com/zed-industries/zed/issues/37727) — Windows GPU 占用过高；官方承认「整窗重绘」、缺 damage tracking
- [zed#46263](https://github.com/zed-industries/zed/issues/46263) — crates.io 版需 SDK 26100 的 fxc.exe（含官方成员回复）
- [zed#59928](https://github.com/zed-industries/zed/issues/59928) — "can't really use development version for (kind of prod) code"
- [zed#8490](https://github.com/zed-industries/zed/pull/8490) — Windows 平台首个 PR（社区作者）
- [zed#40650](https://github.com/zed-industries/zed/pull/40650) — 移除 Windows beta 模板（转正）
- [zed#63471](https://github.com/zed-industries/zed/issues/63471) / [#58746](https://github.com/zed-industries/zed/issues/58746) / [#52448](https://github.com/zed-industries/zed/issues/52448) — Windows 专属缺陷
- [zed#18673](https://github.com/zed-industries/zed/issues/18673) — Zed 内存观测（参考，非 GPUI 框架数据）

### 第三方实测 / 文章
- [Zenn: デスクトップUIフレームワーク15種のメモリ使用量計測](https://zenn.dev/mizugeeks/articles/1019cf2353d343)（2026-08-15）
- [mizugeek/workbench_mem_bench](https://github.com/mizugeek/workbench_mem_bench)（横评源码，macOS 专用）
- [IronOxidizer GUI Toolkit Benchmarks](https://ironoxidizer.github.io/gui-toolkit-benchmarks/) / [源码](https://github.com/IronOxidizer/gui-toolkit-benchmarks)
- [Lukas Kalbertodt: Tauri vs Iced vs egui](https://lukaskalbertodt.github.io/2023/02/03/tauri-iced-egui-performance-comparison.html)（2023-02-03）
- [sonorahq/sonora](https://github.com/sonorahq/sonora) + [v0.34.3 release assets](https://github.com/sonorahq/sonora/releases/tag/v0.34.3)（GPUI 音乐播放器实测体积）
- [buildwithrust.com: Iced vs Tauri 2](https://buildwithrust.com/iced-vs-tauri-2-we-built-the-same-app-twice-in-rust)（仅定性）
- [buildr.sh: Tauri vs Electron 2026](https://buildr.sh/posts/tauri-vs-electron-2026)（⚠️ M2 macOS，不可用于 Windows 决策）
- [r/rust: Memory usage of egui](https://www.reddit.com/r/rust/comments/18d0ahb/memory_usage_of_egui/)

### 引用时需注意的排除项
- `https://tauri.app/v1/references/benchmarks/` 已 404，网上大量文章仍在引用其数字，**不可溯源**。
- [technic-insider.org](https://tech-insider.org/tauri-vs-electron-2026/)、[noqta.tn](https://noqta.tn/en/blog/tauri-2-desktop-apps-rust-web-technologies-2026)、[automatalabs.ca](https://automatalabs.ca/blog/tauri-2-rust-windows-utility-footprint-taskbar-sentinel/) 等页面存在大量二手数字，**我无法核实其测量方法，本次未采用**。
