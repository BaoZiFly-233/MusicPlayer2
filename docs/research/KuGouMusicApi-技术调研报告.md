# KuGouMusicApi 技术调研报告

调研对象：`MakcRe/KuGouMusicApi`（GitHub，946 star，MIT，Node.js，仍在活跃更新）
调研方式：全部通过 `gh api` 直读远端仓库（未 clone），代码片段均为原文。
调研时间基准：仓库最新提交 `2026-09-11T01:47:40Z`，package.json `version 1.6.2`。
用途：为「轻量第三方酷狗概念版音乐播放器」做技术选型。

**证据分级约定**：本报告中标「确证」的结论来自仓库源码原文；标「推测」的为根据代码行为的合理推断，未经实测。

---

## 1. 它是什么：库还是服务？两种都是

**确证**：这个项目同时提供 HTTP 服务和编程式调用两种入口，通过不同的入口文件区分。

| 入口文件 | 作用 | 依据 |
|---|---|---|
| `app.js` | **HTTP 服务入口**。带 shebang，被 `package.json` 的 `"bin"` 字段指向，也是 `npm start` 的执行目标 | `package.json`: `"bin": "./app.js"`, `"scripts": {"start": "node app.js"}` |
| `index.js` | 仅一行 `require('./app')`，开发用入口（`npm run dev` → `nodemon --config nodemon.json index.js`） | 原文 |
| `main.js` | **库入口**（`"main": "main.js"`）。动态扫描 `module/` 并导出扁平函数对象，不启动 HTTP 服务 | `package.json`: `"main": "main.js"` |
| `interface.d.ts` | TypeScript 类型定义，2530 行，为库用法提供 `.d.ts` | `package.json`: `"types": "./interface.d.ts"` |
| `server.js` | Express 服务器核心：CORS、Cookie 解析、平台标识注入、动态路由注册 | 原文 |

`app.js` 全文：

```js
#!/usr/bin/env node

async function start() {
  require('./util/runtime').applyCliOverrides();
  await require('./server').startService();
}

start().catch(console.error);
```

`main.js` 的核心逻辑（库导出）：

```js
fs.readdirSync(path.join(__dirname, 'module'))
  .reverse()
  .forEach((file) => {
    if (!file.endsWith('.js') || file.startsWith('_')) return;
    let fileModule = require(path.join(__dirname, 'module', file));
    let fn = file.split('.').shift() || '';

    obj[fn] = (data = {}) => {
      if (typeof data.cookie === 'string') data.cookie = cookieToJson(data.cookie);
      return fileModule({ ...data, cookie: data.cookie ? data.cookie : {} }, (...args) => {
        const { createRequest } = require('./util/request');
        return createRequest(...args);
      });
    };
  });

module.exports = { ...require('./server'), ...require('./util/request'), ...obj };
```

### 对外暴露的是 REST 接口还是 JS 函数？

**两者都有，且路由规则一致**：

- `server.js` 的 `getModulesDefinitions()` 按文件名生成路由：去掉 `.js`，把 `_` 换成 `/`，加前导 `/`。
  即 `module/song_url.js` → `/song/url`，`module/login_cellphone.js` → `/login/cellphone`，
  `module/user_detail.js` → `/user/detail`。
- `main.js` 导出同名函数：`api.song_url(...)`、`api.login_cellphone(...)`、`api.user_detail(...)`。

**⚠️ 注意 npm 安装这件事**：`package.json` 里**没有** `name` 之外的发布元数据问题，但它
**不是设计给人 `npm install` 的 SDK**——`package.json` 没有 `files` 白名单，`main.js`
靠 `fs.readdirSync` 读 `module/` 目录，且 API 名是下划线风格而非惯用驼峰。生态里的项目
（见第 8 节）**无一例外是把仓库整体引进项目**（git submodule / vendor 目录），没有直接
当 npm 依赖装的。**推测**：即使 `npm install` 能跑，也不该把它当常规依赖——因为 `module/`
目录必须在运行时真实存在于磁盘上。

### Docker 部署：有

`.dockerignore`、`Dockerfile`、`.github/workflows/build.yml` 都在。Dockerfile 原文：

```dockerfile
FROM node:lts-alpine

RUN apk add --no-cache tini

RUN corepack enable

ENV NODE_ENV=production

WORKDIR /app

RUN chown node:node /app

COPY --chown=node:node package.json pnpm-lock.yaml ./

USER node

RUN pnpm install --prod --frozen-lockfile

COPY --chown=node:node . ./

EXPOSE 3000

ENTRYPOINT ["/sbin/tini", "--"]
CMD ["node", "app.js"]
```

其他部署方式（均确证）：

- **Vercel**：`vercel.json` + README 有完整步骤，环境变量里加 `platform=lite` 切概念版。
- **pkg 单文件打包**：`npm run pkgwin` / `pkglinux` / `pkglinux-arm64` / `pkgmacos`，
  产物在 `bin/`。`pkg.scripts` 字段声明了要打进包里的 `module/*.js` 和依赖。
- **tsdown 打包**：`npm run pkgjs`（`tsdown.config.js`）。

### 服务运行参数（确证）

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `PORT` | `3000` | 服务端口（`server.js`: `Number(process.env.PORT || '3000')`） |
| `HOST` | `''`（监听所有地址） | 监听地址 |
| `platform` | `''` | **`lite` = 概念版，空 = 标准版**。全局唯一开关 |
| `KUGOU_API_PROXY` | 空 | HTTP 代理，支持 `user:pass@host:port` |
| `KUGOU_API_GUID` | 自动生成 | 设备 GUID，**建议固定**，否则设备指纹每次都变 |
| `KUGOU_API_DEV` | 随机 10 位大写 | 开发设备标识 |
| `KUGOU_API_MAC` | `02:00:00:00:00:00` | 设备 MAC |
| `KUGOU_API_WEBGL` | 随机 | WebGL 指纹哈希（过验证码用） |
| `CORS_ALLOW_ORIGIN` | 回退到 `req.headers.origin` 或 `*` | 跨域白名单 |

也支持 CLI 参数覆盖（`util/runtime.js`）：`node app.js --platform=lite --port=4000 --proxy=... --guid=... --dev=... --mac=...`。

**内建 2 分钟响应缓存**：`app.use(cache('2 minutes', (_, res) => res.statusCode === 200))`。
绕过方式是在 URL 后加 `timestamp` 参数（文档里反复强调，扫码轮询类接口必须加，
否则会卡在缓存上）。

---

## 2. 模块划分

`module/` 下共 **171 个公开模块**（另有 `_comment.js`、`_listen_together_common.js`
两个 `_` 前缀内部模块，不注册路由）。命名规则：`xxx_yyy.js` → `/xxx/yyy`。

这是全部文件名与职责（按类别归组，注释取自各文件首行）：

### 重点模块（你要用的核心链路）

| 文件 | 路由 | 职责 |
|---|---|---|
| `search.js` | `/search` | 搜索。`type` 支持 `song`(默认)/`special`/`lyric`/`album`/`author`/`mv`，song 走 `v3` 其余走 `v1` |
| `search_complex.js` | `/search/complex` | 综合搜索（单曲+歌手+歌单），走 `complexsearch.kugou.com/v6/search/complex` |
| `search_suggest.js` | `/search/suggest` | 搜索建议 |
| `search_hot.js` | `/search/hot` | 热搜 |
| `search_default.js` | `/search/default` | 默认搜索关键词 |
| `search_mixed.js` | `/search/mixed` | 另一种综合搜索 |
| `search_lyric.js` | `/search/lyric` | **歌词搜索**（拿 `id` + `accesskey`，是 `/lyric` 的前置） |
| `lyric.js` | `/lyric` | **歌词获取**，支持 `fmt=lrc\|krc` + `decode` |
| `song_url.js` | `/song/url` | **歌曲 URL（主力）**，`GET /v5/url` |
| `song_url_new.js` | `/song/url/new` | 新版：一次性返回所有音质，走 `POST /v6/priv_url`，**音频有加密** |
| `song_url_auth.js` | `/song/url/auth` | Auth 版 URL，需先拿 `auth` + `open_time` |
| `song_url_auth_merge.js` | `/song/url/auth/merge` | 上者的聚合版（内部自己调 `/song/auth`） |
| `song_auth.js` | `/song/auth` | 取 `auth` / `open_time`（`GET /v1/authorization`） |
| `song_climax.js` | `/song/climax` | 副歌高潮时间点 |
| `privilege_lite.js` | `/privilege/lite` | **概念版专用**曲库权限/音质查询，`POST /v2/get_res_privilege/lite` |
| `login.js` | `/login` | 用户名+密码登录（`/v9/login_by_pwd`），不推荐 |
| `login_cellphone.js` | `/login/cellphone` | **手机验证码登录**（`/v7/login_by_verifycode`） |
| `login_token.js` | `/login/token` | **刷新登录**（`/v5/login_by_token`），延长 token |
| `login_qr_key.js` / `login_qr_create.js` / `login_qr_check.js` | `/login/qr/*` | 酷狗二维码登录三步 |
| `login_qr_authorize.js` | `/login/qr/authorize` | 已登录设备授权新设备扫码 |
| `login_openplat.js` | `/login/openplat` | 微信开放平台登录（拿扫码后的 `code` 换） |
| `login_qq.js` | `/login/qq` | QQ 授权登录（openid + access_token） |
| `login_qq_qr_create.js` / `login_qq_qr_check.js` | `/login/qq/qr/*` | QQ 扫码登录两步 |
| `login_wx_create.js` / `login_wx_check.js` | `/login/wx/*` | 微信扫码登录两步 |
| `login_device.js` | `/login/device` | 查已登录设备列表（`/v2/get_dev`） |
| `login_device_kick.js` | `/login/device/kick` | 踢设备下线 |
| `register_dev.js` | `/register/dev` | **设备注册，拿 `dfid`**（`POST /risk/v2/r_register_dev`） |
| `captcha_sent.js` | `/captcha/sent` | 手机验证码发送 |
| `user_detail.js` | `/user/detail` | 我的账号信息（`/v3/get_my_info`） |
| `user_info.js` | `/user/info` | 我的详细信息 |
| `user_verify.js` | `/user/verify` | 拿 `auth`（`/song/url/auth` 需要） |
| `verify_user_info.js` | `/verify/user/info` | 提交验证码数据（`verifycode`/`sid`/`edt`） |
| `user_playlist.js` | `/user/playlist` | **我的歌单列表**（`/v7/get_all_list`） |
| `user_history.js` | `/user/history` | 听歌排行 |
| `user_listen.js` | `/user/listen` | 最近听歌 |
| `user_vip_detail.js` | `/user/vip/detail` | VIP 状态查询（`busi_type: 'concept'`） |
| `user_grade_info.js` | `/user/grade/info` | 听歌等级查询/上报，**概念版 lite v2 协议** |
| `user_listen_report.js` | `/user/listen/report` | CSCC lite 播放上报（播放器真实播放时调用） |
| `playhistory_upload.js` | `/playhistory/upload` | 提交听歌历史 |
| `playlist_detail.js` | `/playlist/detail` | 歌单详情（`/v3/get_list_info`） |
| `playlist_track_all.js` | `/playlist/track/all` | 歌单全部歌曲（`/pubsongs/v2/get_other_list_file_nofilt`） |
| `playlist_track_all_new.js` | `/playlist/track/all/new` | 新版歌单全部歌曲 |
| `playlist_tags.js` | `/playlist/tags` | 歌单分类标签 |
| `playlist_add.js` / `playlist_del.js` | `/playlist/add` `/playlist/del` | 收藏/取消收藏歌单 |
| `playlist_tracks_add.js` / `playlist_tracks_del.js` | `/playlist/tracks/add` `/playlist/tracks/del` | 歌单增删歌曲 |
| `playlist_similar.js` | `/playlist/similar` | 相似歌单 |
| `playlist_effect.js` | `/playlist/effect` | 音效歌单 |

### 搜索 / 推荐 / 电台

`album.js` `album_detail.js` `album_songs.js` `album_dycover.js`（专辑动态封面）
`album_shop.js`（唱片店）、`artist_*` 系列 12 个（歌手详情/专辑/单曲/MV/荣誉/关注）、
`singer_list.js` `artist_lists.js`（歌手列表）、`recommend_songs.js`（每日推荐）、
`everyday_recommend.js` `everyday_history.js` `everyday_friend.js`
`everyday_style_recommend.js`（每日/历史/好友/风格推荐）、`personal_fm.js`（私人 FM）、
`fm_class.js` `fm_image.js` `fm_recommend.js` `fm_songs.js`（电台）、
`top_song.js` `top_album.js` `top_playlist.js` `top_card.js` `top_ip.js`（榜单/精选）、
`rank_list.js` `rank_audio.js` `rank_info.js` `rank_top.js` `rank_vol.js`（排行榜五件套）、
`recommend_songs.js`、`scene_*` 系列 9 个（场景音乐）、`theme_*` 系列 4 个（主题歌单）、
`ip.js` `ip_dateil.js` `ip_playlist.js` `ip_zone.js` `ip_zone_home.js`（IP 专区）、
`yueku.js` `yueku_banner.js` `yueku_fm.js`（乐库）、`pc_diantai.js`（电台 banner）、
`brush.js`（刷刷）、`ai_recommend.js` `ai_recommend_song.js`（AI 推荐）。

### 用户 / 云盘 / 社交

`user_cloud.js` `user_cloud_url.js` `user_cloud_upload.js` `user_cloud_del.js`
`user_cloud_match.js`（云盘五件套）、`user_follow.js` `user_follow_message.js`
`user_purchased_songs.js` `user_purchased_albums.js`（已购）、`user_update.js`
`user_update_avatar.js`、`user_video_collect.js` `user_video_love.js`
`user_listen_report.js`、`blacklist.js` `blacklist_list.js`（内容黑名单）、
`team_*` 系列 5 个（组队活动）、`listen_together_*` 系列 5 个（一起听）。

### 评论 / 弹幕 / 视频

`comment_music.js` `comment_music_send.js` `comment_music_classify.js`
`comment_music_hotword.js`、`comment_album.js` `comment_album_send.js`、
`comment_playlist.js` `comment_playlist_send.js`、`comment_floor.js`
`comment_floor_send.js`、`comment_count.js`、`song_barrage.js`
`song_barrage_send.js`、`video_barrage.js` `video_barrage_send.js`、
`video_url.js` `video_detail.js` `video_privilege.js`、`kmr_audio_mv.js`。

### 音效 / 乐谱 / 听书

`effects_artist.js` `effects_brand.js` `effects_brand_detail.js`
`effects_car_brand.js` `effects_car_brand_lists.js` `effects_match.js`
`get_mode_info.js` `get_model.js`、`sheet_*` 系列 6 个（乐谱：钢琴/吉他/尤克里里/简谱）、
`longaudio_*` 系列 11 个（听书）、`audio.js` `audio_match.js`（听歌识曲）
`audio_related.js` `audio_accompany_matching.js` `audio_ktv_total.js`
`images.js` `images_audio.js`（封面）、`server_now.js`（服务器时间）、
`get_verify_info.js` `sidedt.js`（验证码相关）。

### 概念版专属活动（youth 系列）

`youth_vip.js` `youth_day_vip.js` `youth_day_vip_upgrade.js` `youth_union_vip.js`
`youth_month_vip_record.js` `youth_listen_song.js`（**听歌领 VIP**）
`youth_channel_*` 系列 8 个（频道）、`youth_dynamic.js` `youth_dynamic_recent.js`。

---

## 3. 签名机制

签名/加解密工具文件：`util/helper.js`（签名）、`util/crypto.js`（加解密）、
`util/config.json`（密钥常量）、`util/request.js`（注入公共参数 + 调用签名）。

### 3.1 平台常量（`util/config.json` 原文）

```json
{
  "wx_appid": "wx79f2c4418704b4f8",
  "wx_lite_appid": "wx72b795aca60ad321",
  "wx_secret": "4efcab88b700769e376e3f6087b8abc9",
  "wx_lite_secret": "33e486041e5e25729a4e3d2da7502f9a",
  "srcappid": 2919,
  "appid": 1005,
  "apiver": 20,
  "clientver": 20489,
  "liteAppid": 3116,
  "liteClientver": 11440,
  "qq_appid": "205141",
  "qq_lite_appid": "101706348"
}
```

**重要修正**：`appid: 1005` 是**标准版 Android 客户端**的 appid，`liteAppid: 3116` 才是
**概念版（lite）**。项目 CLAUDE.md 里写的「概念版 lite: `appid=3116`」是对的，而
`appid=1005` 属于标准版。**不要**把 1005 当概念版 appid 用。

平台开关只有一个：环境变量 `platform === 'lite'`。`util/index.js` 据此选出一套常量：

```js
const isLite = process.env.platform === 'lite';
const useAppid = isLite ? liteAppid : appid;
const useClientver = isLite ? liteClientver : clientver;

module.exports = {
  appid: useAppid,        // 概念版=3116，标准版=1005
  clientver: useClientver, // 概念版=11440，标准版=20489
  isLite,                 // 是否为概念版
  // liteAppid,           // 概念版应用 ID（注释掉，不对外暴露）
  // liteClientver,       // 概念版客户端版本号（注释掉，不对外暴露）
  ...
};
```

### 3.2 请求公共参数（`util/request.js` 原文）

```js
const isLite = process.env.platform === 'lite';

// ========== 从 Cookie 中提取设备标识 ==========
const dfid = options?.cookie?.dfid || '-';            // 设备指纹 ID（register_dev 接口返回）
const mid = `${options?.cookie?.KUGOU_API_MID}`;      // 设备 MID（server.js 通过 calculateMid 生成）
const uuid = '-';                                     // 设备 UUID（当前固定为 '-'）
const token = options?.cookie?.token || '';            // 用户登录令牌
const userid = options?.cookie?.userid || 0;           // 用户 ID
const clienttime = Math.floor(Date.now() / 1000);     // 当前时间戳（秒）
const ip = options?.realIP || options?.ip || '';       // 客户端 IP（用于 IP 透传）
const webglHash = options?.cookie?.KUGOU_API_WEBGL;   // WebGL 指纹哈希

// ========== 构建请求头 ==========
const headers = { dfid, clienttime, mid, 'kg-rc': '1', 'kg-thash': '5d816a0', 'kg-rec': 1, 'kg-rf': 'B9EDA08A64250DEFFBCADDEE00F8F25F' };

// IP 透传
if (ip) {
  headers['X-Real-IP'] = ip;
  headers['X-Forwarded-For'] = ip;
}

// ========== 构建默认请求参数 ==========
const defaultParams = {
  dfid,                                           // 设备指纹 ID
  mid,                                            // 设备 MID
  uuid,                                           // 设备 UUID
  appid: isLite ? liteAppid : appid,              // 应用 ID（根据平台选择）
  clientver: isLite ? liteClientver : clientver,  // 客户端版本号（根据平台选择）
  clienttime,                                     // 请求时间戳（秒）
};

if (token) defaultParams['token'] = token;
if (userid && userid !== 0) defaultParams['userid'] = userid;
```

默认 User-Agent：

```js
options['headers'] = options.clearDefaultHeaders ? { ...explicitHeaders } : Object.assign({ 'User-Agent': 'Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi' }, options?.headers || {}, {
  dfid,
  clienttime: params.clienttime,
  mid,
});
```

**公共参数汇总表**：

| 参数 | 来源 | 是否必需 |
|---|---|---|
| `appid` | 概念版 `3116` / 标准版 `1005` | 必需 |
| `clientver` | 概念版 `11440` / 标准版 `20489` | 必需 |
| `mid` | `calculateMid(GUID)` = `MD5(GUID)` 的 hex 转十进制大整数 | 必需 |
| `dfid` | `/register/dev` 返回，**歌曲 URL 接口必需**，否则报「本次请求需要验证」 | 关键 |
| `uuid` | 代码里**固定为 `'-'`** | 占位 |
| `clienttime` | `Math.floor(Date.now()/1000)`，秒级；同时写进请求头 | 必需 |
| `token` / `userid` | 登录后写入 Cookie | 登录接口必需 |
| `signature` | 见下 | 除少数 `notSignature` 接口外必需 |
| `key` | `signKey()`，`encryptKey: true` 时生成 | 部分接口 |

**`mid` 的算法**（`util/util.js` 的 `calculateMid`）——注意它是 `MD5` 的 hex 当 16 进制数
转成 10 进制字符串，是个很长的十进制数：

```js
const calculateMid = (str) => {
  let bigInteger = bigInt(0);
  const bigInteger2 = bigInt(16);
  const digest = CryptoJS.MD5(str).toString(CryptoJS.enc.Hex);
  const length = digest.length;
  for (let i = 0; i < length; i += 1) {
    const charValue = bigInt(parseInt(digest.charAt(i), 16));
    const powerValue = bigInteger2.pow(length - 1 - i);
    bigInteger = bigInteger.add(charValue.multiply(powerValue));
  }
  return bigInteger.toString();
};
```

### 3.3 `signature` 到底怎么算（`util/helper.js` 原文）

**这是本报告最核心的一段代码。** 三种签名：

#### (a) Android 签名 —— 最常用，概念版就是它

```js
const signatureAndroidParams = (params, data) => {
  const isLite = process.env.platform === 'lite';
  const str = isLite ? 'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA' : `OIlwieks28dk2k092lksi2UIkp`;
  const paramsString = Object.keys(params)
    .sort()
    .map((key) => `${key}=${typeof params[key] === 'object' ? JSON.stringify(params[key]) : params[key]}`)
    .join('');

  if (Buffer.isBuffer(data)) {
    const hasher = CryptoJS.algo.MD5.create();
    hasher.update(CryptoJS.enc.Utf8.parse(str));
    hasher.update(CryptoJS.enc.Utf8.parse(paramsString));
    hasher.update(wordArrayFromBuffer(data));
    hasher.update(CryptoJS.enc.Utf8.parse(str));
    return hasher.finalize().toString(CryptoJS.enc.Hex);
  }

  return cryptoMd5(`${str}${paramsString}${data || ''}${str}`);
};
```

**算法**：`signature = MD5( salt + 排序后的 "key=value" 串 + body字符串 + salt )`

- 参数**按 key 字母序排序**（`.sort()` 排的是 `"key=value"` 整串）。
- 对象值先 `JSON.stringify`。
- 拼接时**带 `=` 号**。
- **盐值（salt）**：
  - 概念版 lite：`LnT6xpN3khm36zse0QzvmgTZ3waWdRSA`
  - 标准版：`OIlwieks28dk2k092lksi2UIkp`
- 前后各拼一次盐值（夹心式）。
- POST 时请求体字符串拼在盐值之间、参数字符串之后。

**旁证**：`module/user_grade_info.js` 里概念版的 `appKey` 就是
`'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA'`，且它的 `key = MD5(appId + appKey + clientVer + clienttime)`
——与 `signParamsKey` 同构。**推测**：这个字符串就是概念版客户端的 appKey，
所以它会同时出现在签名 salt 和 key 派生里。

#### (b) Web 签名

```js
const signatureWebParams = (params, data) => {
  const str = 'NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt'; // Web 版签名盐值
  const paramsString = Object.keys(params)
    .map((key) => `${key}=${params[key]}`)
    .sort()
    .join('');
  return cryptoMd5(`${str}${paramsString}${data || ''}${str}`);
};
```

同构，盐值 `NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt`，**不区分平台**。

#### (c) 设备注册签名

```js
const signatureRegisterParams = (params) => {
  const paramsString = Object.keys(params)
    .map((key) => params[key]) // 只取值，忽略 key
    .sort()
    .join('');
  return cryptoMd5(`1014${paramsString}1014`); // 盐值为 "1014"
};
```

**只取值、丢掉 key**，值排序后拼接，盐值 `1014`。

#### 其他签名函数

```js
// 请求密钥：MD5(hash + 盐值 + appid + mid + userid)
const signKey = (hash, mid, userid, appid) => {
  const isLite = process.env.platform === 'lite';
  const str = isLite ? '185672dd44712f60bb1736df5a377e82' : '57ae12eb6890223e355ccfcb74edf70d';
  return cryptoMd5(`${hash}${str}${appid || useAppid}${mid}${userid || 0}`);
};

// 通用 sign：MD5(排序后的"key+value"无等号串 + body + 盐值)
const signParams = (params, data) => {
  const str = 'R6snCXJgbCaj9WFRJKefTMIFp0ey6Gza';
  const paramsString = Object.keys(params)
    .sort()
    .map((key) => `${key}${params[key]}`) // key+value 无等号
    .join('');
  return cryptoMd5(`${paramsString}${data || ''}${str}`); // 注意盐值在后
};

// 云盘：MD5("musicclound" + hash + pid + 盐值)   ← "clound" 是原作者的拼写
const signCloudKey = (hash, pid) => {
  const str = 'ebd1ac3134c880bda6a2194537843caa0162e2e7';
  return cryptoMd5(`musicclound${hash}${pid}${str}`);
};

// 参数密钥：MD5(appid + 盐值 + clientver + data)
const signParamsKey = (data, appid, clientver) => {
  const isLite = process.env.platform === 'lite';
  const str = isLite ? 'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA' : 'OIlwieks28dk2k092lksi2UIkp';
  appid = appid || (isLite ? liteAppid : useAppid);
  clientver = clientver || (isLite ? liteClientver : useClientver);
  return cryptoMd5(`${appid}${str}${clientver}${data}`);
};
```

**签名选择**（`request.js`）：

```js
if (!params['signature'] && !options.notSignature) {
  switch (options?.encryptType) {
    case 'register':
      params['signature'] = signatureRegisterParams(params);
      break;
    case 'web':
      params['signature'] = signatureWebParams(params, data);
      break;
    case 'android':
    default:
      params['signature'] = signatureAndroidParams(params, data);
      break;
  }
}
```

绝大多数模块用 `encryptType: 'android'`（或不写，走 default）。
注意有笔误兼容：`song_url.js` 里传的是 `notSign: true`（少个 `ature`），而
`request.js` 检查的是 `options.notSignature`，所以**这个开关实际没生效**——签名照算。
`user_grade_info.js` 里用的才是正确的 `notSignature: true`。

### 3.4 有没有 RSA/AES 加密 params 的接口？—— 有，而且相当多

`util/crypto.js` 提供四个原语：

- `cryptoMd5` / `cryptoSha1`（CryptoJS）
- `cryptoAesEncrypt` / `cryptoAesDecrypt`：**AES-128-CBC + Pkcs7**。默认行为很特别——
  如果只传 `key` 不传 `iv`，会做 `key = MD5(随机key)[0:32]`、`iv = key[-16:]`，
  并把原始随机 key 一起返回：`return { str: hex, key: tempKey }`。
- `cryptoRSAEncrypt`：**裸 RSA 无填充**（`rsaRawEncrypt` 用 `m.modPow(e, n)`），
  字节串左侧补零到密钥长度，返回 hex。
- `rsaEncrypt2`：**RSAES-PKCS1-V1_5**（node-forge 的 `key.encrypt(...)`），返回 hex。
- `playlistAesEncrypt` / `playlistAesDecrypt`：歌单/设备注册用，key 是 6 位随机串，
  `encryptKey = MD5(key)[0:16]`、`iv = MD5(key)[16:32]`，输出 Base64。

**两把概念版/标准版 RSA 公钥**（`util/crypto.js` 原文）：

```js
const publicRasKey = `-----BEGIN PUBLIC KEY-----\nMIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDIAG7QOELSYoIJvTFJhMpe1s/gbjDJX51HBNnEl5HXqTW6lQ7LC8jr9fWZTwusknp+sVGzwd40MwP6U5yDE27M/X1+UR4tvOGOqp94TJtQ1EPnWGWXngpeIW5GxoQGao1rmYWAu6oi1z9XkChrsUdC6DJE5E221wf/4WLFxwAtRQIDAQAB\n-----END PUBLIC KEY-----`;
const publicLiteRasKey = `-----BEGIN PUBLIC KEY-----\nMIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDECi0Np2UR87scwrvTr72L6oO01rBbbBPriSDFPxr3Z5syug0O24QyQO8bg27+0+4kBzTBTBOZ/WWU0WryL1JSXRTXLgFVxtzIY41Pe7lPOgsfTCn5kZcvKhYKJesKnnJDNr5/abvTGf+rHG3YRwsCHcQ08/q6ifSioBszvb3QiwIDAQAB\n-----END PUBLIC KEY-----`;

function cryptoRSAEncrypt(data, publicKey) {
  const isLite = process.env.platform === 'lite';
  const buffer = normalizeBuffer(data);
  const pem = publicKey || (isLite ? publicLiteRasKey : publicRasKey);
  ...
}
```

**用 AES/RSA 加密 params 的接口清单**：

| 模块 | 加密方式 | 说明 |
|---|---|---|
| `register_dev.js` | `playlistAesEncrypt`(设备信息 JSON) + `rsaEncrypt2`({aes key, uid, token}) | 请求体是 AES 密文，`p` 是 RSA 密文；响应是 arraybuffer，需 `playlistAesDecrypt` 解 |
| `login.js` | `cryptoAesEncrypt`({pwd, code, clienttime_ms}) + `cryptoRSAEncrypt`({clienttime_ms, key}) | `params` = AES 密文，`pk` = RSA 密文 |
| `login_cellphone.js` | 同上 + `signParamsKey(dateTime)` | 概念版额外带 `t1`/`t2`（固定 key 的 AES）和 `dev`/`gitversion` |
| `login_token.js` | `cryptoAesEncrypt` + `cryptoRSAEncrypt` + 概念版 `t1`/`t2` | 概念版有**独立的 AES key/iv** |
| `login_device.js` | `cryptoAesEncrypt`({token}) + `cryptoRSAEncrypt` | `pk` 转大写 |
| `user_detail.js` | `cryptoRSAEncrypt({token, clienttime})` → 参数 `p` | 转大写 |
| `user_grade_info.js` | `cryptoRSAEncrypt({token, md5} 或 {clienttime, userid}, publicLiteRasKey)` | lite v2 协议 |
| `login_qq.js` / `login_openplat.js` / `login_wx_create.js` | 含 AES/RSA（`isLite` 分支） | 开放平台登录 |

概念版专用的 AES 密钥（`login_token.js`，**只在 `isLite` 时启用**）：

```js
const key = '90b8382a1bb4ccdcf063102053fd75b8';        // 标准版
const iv  = 'f063102053fd75b8';
const liteKey = 'c24f74ca2820225badc01946dba4fdf7';    // 概念版
const liteIv  = 'adc01946dba4fdf7';
let liteT2Key = 'fd14b35e3f81af3817a20ae7adae7020';
let liteT2Iv  = '17a20ae7adae7020';
let liteT1Key = '5e4ef500e9597fe004bd09a46d8add98';
let liteT1Iv  = '04bd09a46d8add98';
```

概念版登录时还会传两个特征字段（`login_token.js`）：

```js
const t2 = cryptoAesEncrypt(
  `${params.cookie?.KUGOU_API_GUID}|0f607264fc6318a92b9e13c65db7cd3c|${params.cookie?.KUGOU_API_MAC}|${params.cookie?.KUGOU_API_DEV}|${dateNow}`,
  { key: liteT2Key, iv: liteT2Iv }
);
const t1 = cryptoAesEncrypt(params.cookie?.t1 ? `${params.cookie?.t1}|${dateNow}` : `|${dateNow}`, { key: liteT1Key, iv: liteT1Iv });
```

其中 `0f607264fc6318a92b9e13c65db7cd3c` 是**写死的常量**（标准版与概念版共用）。

### 3.5 行为指纹 SID/EDT（进阶，容易踩坑）

酷狗服务端会检测行为指纹。当响应头带 `ssa-code` 时表示需要二次验证，
`request.js` 会自动调 `generateSimulate(mid, userid, dfid, webglHash)` 生成 `sid` / `edt`
塞进响应体：

```js
if (response.headers['ssa-code'] || response.headers['SSA-CODE']) {
  const _ssaCode = response.headers['ssa-code'] || response.headers['SSA-CODE'];
  answer.headers['ssa-code'] = _ssaCode;
  ssaCode = _ssaCode;
}
...
if (ssaCode) {
  const { edt, sid } = generateSimulate(mid, userid, dfid, webglHash);
  if (edt) answer.body.edt = edt;
  if (sid) answer.body.sid = sid;
  answer.body.ssaCode = ssaCode;
}
```

CLAUDE.md 描述：SID 是 RSA-OAEP 加密的 AES 密钥（Base64），EDT 是 AES-128-CBC
加密的行为数据（Base64），行为数据含鼠标轨迹（贝塞尔曲线）、滚动、窗口事件、WebGL 指纹。
仓库里还有 `public/verify-pkg/verifycode_bg.wasm`（**WASM**）和
`public/login_captcha.html` / `login_captcha_simulate.html` / `sid_edt_generator.html`。

**这对轻量播放器是个真实负担**：issue #206 里作者明确说 WASM「主要是一些加密的函数」，
而且有使用者反馈「这个 wasm 文件似乎一定要部署在 web 服务器上才能工作」。
issue #257（OPEN）和 #264（OPEN）都是这类验证问题。

---

## 4. 概念版 vs 标准版

### 4.1 代码里的关键词搜索结论

在仓库里搜 `isLite` 命中的文件（15 个）：
`util/index.js`、`util/helper.js`、`util/request.js`、`util/crypto.js`、
`module/song_url.js`、`module/song_url_auth.js`、`module/login_token.js`、
`module/login_cellphone.js`、`module/login_qq.js`、`module/login_qq_qr_create.js`、
`module/login_qq_qr_check.js`、`module/login_openplat.js`、`module/login_wx_create.js`、
`module/import_playlist.js`、`module/user_update.js`。

搜 `lite` 额外命中的模块：`module/privilege_lite.js`（`/v2/get_res_privilege/lite`）、
`module/album_songs.js`（`/v1/album_audio/lite`）、`module/user_grade_info.js`、
`module/user_listen_report.js`（CSCC lite）、`module/team_*.js`。

`1005` 只出现在 `util/config.json`（标准版 appid）和文档里。**这是最关键的一条**：
`1005` **不是**概念版 appid，概念版是 `3116`。

### 4.2 概念版 vs 标准版的差异（确证）

| 维度 | 标准版（`platform=''`） | 概念版（`platform='lite'`） |
|---|---|---|
| `appid` | `1005` | `3116` |
| `clientver` | `20489` | `11440` |
| Android 签名 salt | `OIlwieks28dk2k092lksi2UIkp` | `LnT6xpN3khm36zse0QzvmgTZ3waWdRSA` |
| `signKey` salt | `57ae12eb6890223e355ccfcb74edf70d` | `185672dd44712f60bb1736df5a377e82` |
| RSA 公钥 | `publicRasKey` | `publicLiteRasKey` |
| 登录 AES key/iv | `90b838...` / `f06310...` | `c24f74...` / `adc019...` |
| 登录额外字段 | `t3`（常量 `MCwwLDAsMCwwLDAsMCwwLDA=`） | `t1`、`t2`、`dev`、`gitversion` |
| QQ 开放平台 client_id | `205141` | `101706348` |
| 微信小程序 | `wx79f2c4418704b4f8` | `wx72b795aca60ad321` |

**关键**：README 明确警告 —— **「注意不同版本的平台的 token 是不通用的。」**
即概念版登录拿到的 token 不能用于标准版接口，反之亦然。这对播放器意味着：
一旦选概念版，整套账号体系都得跟着走。

### 4.3 `/song/url` 里的概念版分支（确证）

```js
const isLite = process.env.platform === 'lite';
const page_id = isLite ? 967177915 : 151369488;
const ppage_id = isLite
  ? (params.ppage_id || '356753938,823673182,967485191')
  : '463467626,350369493,788954147';

const dataMap = {
  ...
  page_id,
  behavior: 'play',
  pid: isLite ? 411 : 2,
  cmd: 26,
  pidversion: 3001,
  IsFreePart: params?.free_part ? 1 : 0,
  ppage_id,
  cdnBackup: 1,
  clientver: 11430,
};
```

`pid=411` 是概念版特有的播放来源标识；issue #159 的返回里也能看到
`pi411` 和 `ap3116` 出现在音频文件名里——这是概念版 URL 的指纹特征。

### 4.4 概念版能「免 VIP 听歌」的原理

**先说结论：不是破解，是利用概念版的官方运营活动。**

**确证的代码证据**：
`module/youth_listen_song.js`（听歌领 VIP）：

```js
// 听歌领取vip 需要登录
module.exports = (params, useAxios) => {
  const dataMap = {
    mixsongid: params?.mixsongid || 666075191
  }
  return useAxios({
    url: '/youth/v2/report/listen_song',
    data: dataMap,
    method: 'POST',
    encryptTyPe: 'android',
    params: { clientver: 10566 },
    cookie: params?.cookie,
    headers: {
      "user-agent": "Android13-1070-10566-201-0-ReportPlaySongToServerProtocol-wifi",
      "content-type": "application/json; charset=utf-8",
    },
  });
};
```

`module/youth_vip.js`（看广告领 VIP）：

```js
// 领取vip 需要登录
module.exports = (params, useAxios) => {
  const time = Date.now();
  const dataMap = {
    ad_id: 12307537187,
    play_end: time,
    play_start: time - 30000,   // 伪造 30 秒广告播放时长
  };
  return useAxios({
    url: '/youth/v1/ad/play_report',
    ...
  });
};
```

**原理链条（确证 + 推测）**：

1. 酷狗概念版 App 有「听歌领 VIP」「看广告领 VIP」等官方拉新/留存活动，
   服务端接口是 `/youth/v2/report/listen_song` 和 `/youth/v1/ad/play_report`。
2. 这些接口**只校验 token**，不校验是不是真的在 App 里听了 30 秒广告或听了一首歌。
   `youth_vip.js` 直接构造 `play_start = now - 30000`、`play_end = now` 就能通过
   ——**这一段是确证的**（代码就是这么写的，`ad_id` 也是写死的）。
3. 领到的 VIP 是**概念版专属 VIP**（`busi_type: "concept"`，见 issue #156 的返回）。
4. 有了概念版 VIP，概念版账号请求 `/song/url` 时服务端就返回高音质地址。
   **这一点是确证的**——issue #75 里作者亲自回复：
   > 「这个没办法，你可以尝试使用概念版接口，然后领取概念版VIP，在获取」

5. **为什么是概念版而不是标准版**：概念版 VIP 的获取门槛远低于标准版
   （标准版要付费，概念版做活动就送），且概念版接口对 VIP 歌曲的
   `priv_status` 判定更宽松。

**必须说清楚的风险（有反证）**：
issue #179（CLOSED）里用户报告：**即使调用了概念版会员接口、领取了会员，
新接口仍然拿不到高音质数据，只有标准音质**。issue #156（CLOSED）里同样报告
「可以下载普通歌曲的 128k url，但是比如 beyond 的海阔天空，失败，只要是 VIP 的都不行」，
返回 `priv_status: 0, fail_process: ["pkg","buy"]`（表示需要购买）。

**所以「免 VIP 听歌」是一个持续被酷狗收窄的口子，不是稳定能力。**
issue #159 还显示 `/song/url` 会**把请求的 hash 换成实际返回的 hash**
（请求 `8637CE...`，返回 `DE6075...`），且只给 128k。

### 4.5 概念版接口和主站接口的区别（归纳）

1. **appid/clientver/盐值/公钥不同** —— 全套换。
2. **部分接口路径带 `/lite` 后缀**：`/v2/get_res_privilege/lite`、
   `/v1/album_audio/lite`。
3. **部分接口只存在于概念版**：整个 `youth_*` 系列（频道、动态、领 VIP）。
4. **同接口不同参数**：`/song/url` 的 `page_id`、`ppage_id`、`pid`。
5. **返回结构不同**：概念版多 `tracker_through`、`all_quality_free` 等字段。
6. **UA 不同**：例如概念版手机登录用 `Android16-1070-11440-130-0-LOGIN-wifi`
   （`clientver` 段是 `11440`，与概念版常量一致）。

---

## 5. 歌曲 URL 与音质

### 5.1 四个取 URL 的接口对比（确证）

| 路由 | 文件 | 上游 | 方法 | 特点 |
|---|---|---|---|---|
| `/song/url` | `song_url.js` | `https://gateway.kugou.com/v5/url`（`x-router: trackercdn.kugou.com`） | GET | **主力**，一次一个音质 |
| `/song/url/new` | `song_url_new.js` | `http://tracker.kugou.com/v6/priv_url` | POST | 一次返回全部音质，**但音频加密无法解码** |
| `/song/url/auth` | `song_url_auth.js` | `gateway.kugou.com/tracker/v5/url` | GET | 需先拿 `auth` + `open_time` |
| `/song/url/auth/merge` | `song_url_auth_merge.js` | 内部串 `/song/auth` + `/song/url/auth` | — | 聚合版，推荐 |

`/song/url/new` 的文档警告原文：
> 「该接口会一次性返回支持的音质的音频 url，**但该接口存在音频加密（目前无法解码），请谨慎使用**」

**所以：做播放器就用 `/song/url`（概念版 `platform=lite`）。** 别碰 `/song/url/new`。

### 5.2 `/song/url` 完整实现（原文）

```js
const { randomString } = require('../util/util');

module.exports = (params, useAxios) => {
  const quality = ['piano', 'acappella', 'subwoofer', 'ancient', 'dj', 'surnay'].includes(params.quality)
    ? `magic_${params?.quality}`
    : params.quality;

  const isLite = process.env.platform === 'lite';
  const page_id = isLite ? 967177915 : 151369488;
  const ppage_id = isLite
    ? (params.ppage_id || '356753938,823673182,967485191')
    : '463467626,350369493,788954147';

  const dataMap = {
    album_id: Number(params.album_id ?? 0),
    area_code: 1,
    hash: (params?.hash || '').toLowerCase(),
    ssa_flag: 'is_fromtrack',
    version: 11430,
    page_id,
    quality: quality || 128,
    album_audio_id: Number(params.album_audio_id ?? 0),

    behavior: 'play',
    pid: isLite ? 411 : 2,
    cmd: 26,
    pidversion: 3001,
    IsFreePart: params?.free_part ? 1 : 0, //是否返回试听部分（仅部分歌曲）
    ppage_id,
    cdnBackup: 1,
    module: '',
    clientver: 11430,
  };

  return useAxios({
    url: '/v5/url',
    method: 'GET',
    params: dataMap,
    encryptType: 'android',
    headers: { 'x-router': 'trackercdn.kugou.com'},
    encryptKey: true,
    notSign: true,
    cookie: Object.assign({}, {dfid: randomString(24)}, params?.cookie ),
  });
};
```

**注意最后一行**：如果调用方没带 `dfid`，它**用随机 24 位串临时兜底**。
但文档反复强调必须先用 `/register/dev` 拿真 `dfid`，否则报「本次请求需要验证」。
**推测**：随机 dfid 只能过一部分请求，正式做法是持久化真 dfid。

**必需参数**：`hash`（必填）、`album_audio_id`（强烈建议）、`album_id`、`quality`、`free_part`。

### 5.3 支持的音质档位（确证）

从 `song_url_new.js` 的 `qualities` 数组（最完整的一份）：

```js
qualities: ['128', '320', 'flac', 'high', 'multitrack', 'viper_atmos', 'viper_tape', 'viper_clear', 'super'],
```

从 `privilege_lite.js`：

```js
qualities: ['128', '320', 'flac', 'high', 'viper_atmos', 'viper_tape', 'viper_clear', 'super', 'multitrack'],
```

| 参数值 | 含义 | 说明 |
|---|---|---|
| `128` | 标准 mp3 | 128kbps，**默认**，非会员也能拿 |
| `320` | HQ 高品 | 320kbps |
| `flac` | SQ 无损 | FLAC |
| `high` | Hi-Res | 高于 CD |
| `viper_tape` | 母带 | 最高档 |
| `viper_atmos` | 蝰蛇全景声 | |
| `viper_clear` | 蝰蛇清澈 | |
| `super` | DSD | 「支持的音频少的可伶」（文档原话） |
| `multitrack` | 多轨 | |
| `magic_piano` | 魔法音乐·钢琴 | 传 `quality=piano` 自动加 `magic_` 前缀 |
| `magic_acappella` | 人声/伴奏分离 | **返回 `.mkv`**，含人声+伴奏两条音轨 |
| `magic_subwoofer` | 骨笛 | |
| `magic_ancient` | 尤克里里 | |
| `magic_dj` | DJ | |
| `magic_surnay` | 唢呐 | |

**EchoMusic（2969 star 的同类播放器）的实际用法**（`src/renderer/utils/song.ts` 原文）：

```ts
const AUDIO_QUALITY_ORDER: AudioQualityValue[] = ['128', '320', 'flac', 'high', 'viper_tape'];

export const getSongQualityCandidates = (
  preferred: AudioQualityValue,
  compatibilityMode = true,
): AudioQualityValue[] => {
  const normalized = AUDIO_QUALITY_ORDER.includes(preferred) ? preferred : '128';
  const index = AUDIO_QUALITY_ORDER.indexOf(normalized);
  if (!compatibilityMode) return [normalized];
  return AUDIO_QUALITY_ORDER.slice(0, index + 1).reverse();
};
```

**这是很值得抄的策略**：用户选「flac」时，候选顺序是 `['flac','320','128']` ——
**从高往低依次降级重试**，某个音质拿不到 URL 就退一档。这是应对「VIP 歌曲拿不到高音质」
最实用的工程解法。

音质标签映射（同文件）：

```ts
const QUALITY_LABEL_MAP: Record<string, string> = {
  viper_tape: '母带',
  high: 'Hi-Res',
  flac: 'SQ',
  '320': 'HQ',
};
```

### 5.4 URL 时效与防盗链 —— 部分确证，部分是推测

**确证的**：
- **短时效**。音频 URL 形如
  `http://fs.youthandroid.kugou.com/202602062123/31b72b0b8aa9b6bd868fcbd8678a9291/v3/de6075be27a798fc1e81789cbbd6625c/yp/full/ap3116_us672335239_dfv8ibc9fygotbalu4362q89be_pi411_mx0_qu128_ct140600_s2442135278.mp3`
  （issue #159 原文）。路径里带时间戳目录 `202602062123`（年月日时分）和
  `us672335239`（用户 ID）、`pi411`（概念版 pid）、`qu128`（音质）。
  时间戳目录 + `us`/`df` 段强烈说明 URL 是**按用户+时间签发**的。
  实践中这类酷狗 URL 有效期通常在**几小时量级**（会随 `2026020621 23` 这样的
  时间目录滚动），但仓库里**没有**明确文档标注秒数。**这一条属于推测，建议实测。**
- **有防盗链，但不是靠 Referer**。issue #64 原文：
  > `url` 字段的链接在浏览器打开后，提示「拒绝访问 fsandroid.tx.kugou.com /
  > 你没有查看此页面的用户权限。HTTP ERROR 403」

  同一个响应的 `backupUrl` 字段却「在浏览器可以直接播放」。**推测**：
  `url` 是给**酷狗客户端**用的 CDN（可能校验 UA/内部头），`backupUrl` 是通用 CDN。
  工程上应**优先用 `backupUrl`，其次 `url`**。
- **仓库里搜不到 `Referer` 相关代码**（只有 QQ 扫码登录用它）。所有音频 URL 都是
  原样透传给客户端的，服务端不做代理。**所以 Referer 不是必需条件。**
- 服务器会**把请求的 hash 换成实际返回的 hash**（issue #159），
  所以客户端要读响应体里的 `hash` / `std_hash`，不要假设等于请求的 hash。
- **IP 透传**：`request.js` 会把调用方 IP 通过 `X-Real-IP` / `X-Forwarded-For`
  带给酷狗服务端。**推测**：这意味着 URL 可能跟请求时的 IP 绑定，
  若播放器和服务在不同网络环境，可能取到就播放失败。轻量播放器建议**服务和播放同一台机器**。

**返回结构主要字段**（据 issue #64 / #159 的真实响应）：

```
url / backupUrl   : 播放地址（url 可能 403，backupUrl 更稳）
fileSize, bitRate, extName, timeLength, fileName
hash, std_hash    : 实际 hash
priv_status       : 1 = 有权限；0 = 无权限（配合 fail_process 看原因）
fail_process      : ["pkg","buy"] 表示需购买
trans_param       : 含 qualitymap / ogg_128_hash / ogg_320_hash 等
tracker_through   : 含 all_quality_free / cpy_grade 等
hash_offset       : 试听片段信息（start_byte/end_byte/end_ms）
```

### 5.5 VIP 歌曲怎么处理（工程建议）

**确证的判断依据**：
- `priv_status === 0` + `fail_process` 含 `"pkg"`/`"buy"` → 无权限，需要 VIP 或购买。
- 有 `hash_offset` 字段 → 只能试听（`end_ms` 通常是 60000，即 60 秒）。
  传 `free_part=1` 可以让服务端明确返回试听部分。

**推荐的降级链**（结合 EchoMusic 的做法和本仓库文档）：

1. 先用概念版（`platform=lite`）+ 登录态请求 `/song/url`，按
   `['flac','320','128']` 从高到低试。
2. 每次都检查 `priv_status`；为 0 就退下一档。
3. 高音质全失败时，`/song/url/auth/merge` 是备选（文档说：
   「非会员可以获取完整的 128 音质的音频」，且「新版概念版部分请求开始使用该接口」）。
4. 试听片段（有 `hash_offset`）也要能用，UI 上标出来。
5. 实在拿不到就明确报「无版权/需会员」，别静默失败。

**⚠️ 前置依赖**：这四个接口的文档都反复强调同一句：
> 「因接口问题，目前获取 url 接口数据需要先调用 `/register/dev` 接口获取 dfid，
> 否则会提示 `本次请求需要验证`」

**所以 `dfid` 必须先取、必须持久化。**

---

## 6. 登录

### 6.1 支持的登录方式（共 7 类，均确证）

| 方式 | 路由 | 上游接口 | 备注 |
|---|---|---|---|
| 手机验证码 | `/login/cellphone` | `loginserviceretry.kugou.com/v7/login_by_verifycode` | **推荐**，需先 `/captcha/sent` |
| 用户名+密码 | `/login` | `/v9/login_by_pwd` | 文档标「不推荐使用」 |
| 刷新登录 | `/login/token` | `login.user.kugou.com/v5/login_by_token` | 延长 token 有效期 |
| 酷狗二维码 | `/login/qr/key` → `/login/qr/create` → `/login/qr/check` | — | 三步。check 状态：0 过期 / 1 等待 / 2 待确认 / 4 成功 |
| 扫码授权新设备 | `/login/qr/authorize` | — | 用已登录账号授权新设备 |
| 微信开放平台 | `/login/openplat` | — | 接微信扫码后的 `code` |
| QQ 授权 | `/login/qq` | — | 接 `openid` + `access_token` |
| 微信扫码 | `/login/wx/create` → `/login/wx/check` | 轮询 `long.open.weixin.qq.com` | 状态：408 等待 / 404 已扫 / 403 拒绝 / 405 成功 / 402 过期 |
| QQ 扫码 | `/login/qq/qr/create` → `/login/qq/qr/check` | — | 用 `qrsig`/`ptqrtoken`/`pt_login_sig` |

**文档强烈警告**：
> 「不要频繁调登录接口，不然可能会被风控，登录状态还存在就不要重复调登录接口」

**概念版差异**：`login_cellphone.js` 里 `isLite` 分支会额外带
`dfid`、`dev`、`gitversion: '5f0b7c4'`，并且 `t1`/`t2` 从 `0` 变成真实的 AES 密文；
UA 变成 `Android16-1070-11440-130-0-LOGIN-wifi`。

### 6.2 token 怎么获取和保存（确证）

**获取**：登录成功后，token 来自响应体的 `secu_params` 字段，**AES 解密**得到。
以 `login_cellphone.js` 为例：

```js
if (body?.status && body?.status === 1) {
  if (body?.data?.secu_params) {
    const getToken = cryptoAesDecrypt(body.data.secu_params, encrypt.key);
    if (typeof getToken === 'object') {
      res.body.data = { ...body.data, ...getToken };
      Object.keys(getToken).forEach((key) => res.cookie.push(`${key}=${getToken[key]}`));
    } else {
      res.body.data['token'] = getToken;
    }
  }
  res.cookie.push(`t1=${res.body.data['t1']}`);
  res.cookie.push(`token=${res.body.data['token']}`);
  res.cookie.push(`userid=${res.body.data?.userid || 0}`);
  res.cookie.push(`vip_type=${res.body.data?.vip_type || 0}`);
  res.cookie.push(`vip_token=${res.body.data?.vip_token || ''}`);
}
```

注意解密用的 `key` 是**本次请求自己生成的随机 key**（`cryptoAesEncrypt` 返回的
`{ str, key }` 里的 `key`）——这是会话级的一次性密钥。

**保存**：服务端把结果写回 `Set-Cookie`。**必须持久化的 5 个值**：

| Cookie | 说明 |
|---|---|
| `token` | 核心登录票据 |
| `userid` | 用户 ID |
| `t1` | 概念版登录刷新要用（`login_token.js` 读 `cookie.t1`） |
| `vip_type` | VIP 类型 |
| `vip_token` | VIP 票据（`song_url_new.js` 的 `vipertoken` 用它） |
| `dfid` | **设备指纹，单独持久化**（`/register/dev` 的结果） |

**传递 cookie 的三种方式**（`server.js` 全支持，文档也写了）：

1. Query 参数：`?cookie=token%3Dxxx%3B%20userid%3Dxxx`
2. 请求体：`{ cookie: "token=xxx;userid=xxx" }`
3. `Authorization` 请求头（**优先级最高**）：`Authorization: token=xxx;userid=xxx;dfid=xxx`

`server.js` 里 `Authorization` 会被解析并**覆盖**其他来源的同名 cookie：

```js
const authHeader = req.headers['authorization'];
if (authHeader) {
  query.cookie = {
    ...query.cookie,
    ...cookieToJson(authHeader),
  };
}
```

**刷新**：`/login/token`，文档说「可刷新登录状态，可以延长 `token` 过期时间」。
issue #175 报告过它返回 `20018` 错误；issue #110 里有用户反馈 token 放一两个月还能用，
作者回「这个问题我也不知道....」。**推测**：token 有效期不透明，工程上应做
「401/登录态失效 → 提示重新登录」的处理，别硬依赖 token 永久有效。

### 6.3 设备注册（有，且是必需前置）

**有** `/register/dev`，实现是 `module/register_dev.js`，上游
`https://userservice.kugou.com/risk/v2/r_register_dev`。

**这是整个项目最复杂的一个接口**。它上传一份**完整的伪造设备指纹**，字段包括：
`brand`（默认 `Redmi`）、`device`（默认 `marble`）、`manufacturer`（默认 `Xiaomi`）、
`imei`、`imsi`、`uuid`、`buildSerial`、`basebandVer`、`batteryLevel`（默认 100）、
`batteryStatus`、`availableRamSize`（默认 4983533568）、`availableRomSize`、
`availableSDSize`、以及 9 类传感器的存在性与数值
（`accelerometer`、`gravity`、`gyroscope`、`light`、`magnetic`、
`orientation`、`pressure`、`step_counter`、`temperature`）。

核心加密两步：

```js
const aesEncrypt = playlistAesEncrypt(dataMap);
const p = rsaEncrypt2({ aes: aesEncrypt.key, uid: userid, token });

useAxios({
  baseURL: 'https://userservice.kugou.com',
  url: '/risk/v2/r_register_dev',
  method: 'POST',
  data: aesEncrypt.str,                          // 请求体是 AES 密文
  params: { part: 1, platid: 1, p },             // p 是 RSA 密文
  encryptType: 'android',
  responseType: 'arraybuffer',                   // 响应是二进制，得手动解
  ...
})
```

**响应处理**也很特别——整个 body 是 AES 加密的：

```js
.then((res) => {
  res.body = playlistAesDecrypt({ str: res.body.toString('base64'), key: aesEncrypt.key });
  const { body } = res;
  if (body?.status === 1 && body?.data) {
    res.cookie.push(`dfid=${res.body.data['dfid']}`);
  }
  resolve(res);
})
```

**对播放器的建议**：`dfid` **一定要持久化存起来**（本地文件 / localStorage / SQLite）。
issue #264（OPEN，2026-09-10）标题就是「绑定设备接口返回了 error_code 20010」，
说明这接口本身不太稳定。所以：**取一次就存，失败了要有兜底和重试**，
不要让每次启动都重新注册。

---

## 7. 歌词

### 7.1 两步走（确证）

**第一步**：`/search/lyric`（`module/search_lyric.js`）搜歌词，拿 `id` + `accesskey`。

```js
const dataMap = {
  album_audio_id: params?.album_audio_id || 0,
  appid,
  clientver,
  duration: params.duration || 0,
  hash: params?.hash || '',
  keyword: params?.keywords || '',
  lrctxt: 1,
  man: params.man ?? 'no',
};

return useAxios({
  baseURL: 'https://lyrics.kugou.com',
  url: '/v1/search',
  method: 'GET',
  params: dataMap,
  ...
  clearDefaultParams: true,
  notSign: true,
});
```

**第二步**：`/lyric`（`module/lyric.js`）取歌词正文。

```js
// 歌词获取
const { decodeLyrics } = require('../util');

module.exports = (params, useAxios) => {
  const dataMap = {
    ver: 1,
    client: params?.client || 'android',
    id: params?.id,
    accesskey: params?.accesskey,
    fmt: params.fmt || 'krc',
    charset: 'utf8',
  };

  return new Promise((resolve, reject) => {
    useAxios({
      baseURL: 'https://lyrics.kugou.com',
      url: '/download',
      method: 'GET',
      params: dataMap,
      cookie: params?.cookie || {},
      encryptType: 'android',
    })
      .then((res) => {
        if (params?.decode) {
          if (res.body?.content) {
            res.body['decodeContent'] = params?.fmt == 'lrc' || Number(res.body?.contenttype) !== 0 ? Buffer.from(res.body?.content, 'base64').toString() : decodeLyrics(res.body.content);
            resolve(res);
            return;
          }
        }
        resolve(res);
      })
      .catch((e) => reject(e));
  });
};
```

### 7.2 支持的格式

| `fmt` | 说明 | 是否需解密 |
|---|---|---|
| `krc` | **默认**。逐字歌词（带时间轴，可做卡拉OK效果） | **是**，XOR + zlib |
| `lrc` | 普通逐行歌词 | 否，Base64 解码即可 |

`decode` 参数控制是否返回解码结果（放在 `body.decodeContent`）。
判断逻辑：`fmt == 'lrc' || Number(contenttype) !== 0` → 直接 Base64 解码；
否则（`contenttype === 0`）→ 走 `decodeLyrics`。**所以 `contenttype` 是权威标志，不是 `fmt`。**

### 7.3 KRC 客户端解密代码（确证，在 `util/util.js`）

**是的，KRC 需要在客户端解密，而且仓库里就有完整解密代码。**

```js
const decodeLyrics = (val) => {
  let bytes = null;
  if (val instanceof Uint8Array) bytes = val;
  if (Buffer.isBuffer(val)) bytes = new Uint8Array(val);
  if (typeof val === 'string') bytes = new Uint8Array(Buffer.from(val, 'base64'));
  if (bytes === null) return '';

  // XOR 解密密钥（16字节，循环使用）
  const enKey = [64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105];
  const krcBytes = bytes.slice(4); // 跳过前 4 字节文件头
  const len = krcBytes.byteLength;

  // XOR 异或解密
  for (let index = 0; index < len; index += 1) {
    krcBytes[index] = krcBytes[index] ^ enKey[index % enKey.length];
  }

  // zlib 解压
  try {
    const inflate = pako.inflate(krcBytes);
    return Buffer.from(inflate).toString('utf8');
  } catch {
    return '';
  }
};
```

**算法三步**：
1. Base64 解码得到字节流
2. **跳过前 4 字节**（文件头，通常是 `krc1` 标识）
3. 用 16 字节固定密钥循环 XOR
4. **zlib inflate** 解压，得到 UTF-8 文本

XOR 密钥的十进制数组转成可读字符是 `@Ga w^2tGQ61-` 加三个高位字节（206, 210, 110, 105）
—— 明显是某个字符串按有符号/无符号混淆后的结果。**注意这是核心常量，抄的时候别抄错。**

**只依赖 `pako`**（`package.json` 的 dependencies 里有 `"pako": "^2.1.0"`）。
轻量播放器里用 `fflate` 或浏览器原生 `DecompressionStream('deflate')` 也能替代，
XOR 部分纯 JS 十几行就够。**推测**：KRC 解析后的文本格式类似
`[start,duration]<0,100,0>字<100,200,0>幕` 的逐字标注，需要自己写解析器
（本仓库只负责解密，不负责解析成结构化数据）。

**建议**：轻量播放器初期用 `fmt=lrc&decode=true` 最省事；要做逐字效果再上 KRC。

**已知问题**：issue #96、#199 报告过歌词接口失败（`/search/lyric` 失败）。
歌词接口也是需要带认证信息的。

---

## 8. 在实际项目里的用法

### 8.1 搜索结果：**主流是「本地起服务，播放器请求 localhost」，没人直接 import 函数**

`gh search code "KuGouMusicApi"` 的命中里，出现频率最高的模式是
**把整个仓库作为 git submodule 或 vendor 目录引进播放器项目**。
具体证据：

| 项目 | star | 集成方式 | 证据 |
|---|---|---|---|
| `MoeKoeMusic/MoeKoeMusic` | 6283 | **git submodule `api/`** | `.gitmodules`：`[submodule "api"] url = https://github.com/MakcRe/KuGouMusicApi` |
| `hoowhoami/EchoMusic` | 2969 | **git submodule `server/`** | `.gitmodules`：`[submodule "server"] url = https://github.com/MakcRe/KuGouMusicApi.git` |
| `chthollyphile/folia-major` | — | vendor（`deploy/docker/images/kugou-api.Dockerfile`、`electron/kugouApiBridge.cjs`） | 树里直接有 KuGouMusicApi 全套文件 + Docker 镜像 |
| `crayonlu/down-music` | — | vendor（`backend/KuGouMusicApi/`） | 同上 |
| `JS1Lan/Lanote` | — | vendor（`KuGouMusicApi/main.js`） | 同上 |
| `FLC-Team/BetterKuGou` | — | vendor（`KuGouMusicApi/README.md`） | 同上 |
| `zzzzzzp2025/mineradio.kg` | — | vendor（`KuGouMusicApi-1.5.1/`，**带版本号目录**） | 同上 |
| `e-cells/KuGouMusicApi`、`ZxCASD-DEL/KuGouMusicApi` 等 | — | 纯 fork | `gh search repos` |

**MoeKoeMusic 的 README 明说**：「API 源代码来自 MakcRe/KuGouMusicApi」，
并且安装步骤里有「4. 编译API服务端」。它的客户端代码
`src/utils/apiBaseUrl.js` 原文：

```js
export const DEFAULT_API_BASE_URL =
    import.meta.env.VITE_APP_API_URL || 'http://127.0.0.1:6521';

export async function testApiBaseUrl(baseUrl, options = {}) {
  const { path = '/register/dev', timeoutMs = 8000 } = options;
  ...
      const dfid = data?.data?.dfid;
      if (typeof dfid !== 'string' || !dfid) {
        return { ok: false, error: 'no_dfid', data };
      }
      return { ok: true, data, dfid };
}
```

**三个可直接借鉴的点**：
1. `http://127.0.0.1:6521` —— 换端口避免和 3000 冲突，且只监听本地。
2. **用 `/register/dev` 当健康检查端点**（顺手把 `dfid` 拿了）——很聪明。
3. API 地址可配置（localStorage 里存 `settings.apiBaseUrl`），支持远程部署。

**EchoMusic 更进一步：它不用 HTTP，用 Electron IPC。**
`src/main/server.ts` 里复刻了 `server.js` 的 `getModulesDefinitions` 路由逻辑，
但通过 `handleApiRequest` 在**主进程内**直接调模块函数，没有 Express、没有监听端口：

```ts
/**
 * 扫描 server module 路径映射（不立即加载）
 * 复现 server/server.js 中 getModulesDefinitions 的逻辑，但延迟实际 require
 */
const scanModules = (serverPath: string): Map<string, string> => {
  const modulesPath = path.join(serverPath, 'module');
  const files = fs.readdirSync(modulesPath);
  const routeMap = new Map<string, string>();

  files
    .reverse()
    .filter((fileName) => fileName.endsWith('.js') && !fileName.startsWith('_'))
    .forEach((fileName) => {
      const route = '/' + fileName.replace(/\.js$/i, '').replace(/_/g, '/');
      const modulePath = path.resolve(modulesPath, fileName);
      routeMap.set(route, modulePath);
    });

  return routeMap;
};
```

**推测**：它这么改的原因很可能是**要保留 `server.js` 里那套 Cookie/平台标识注入语义**
（`buildDefaultCookies` 复现了 `ensureCookie` 逻辑），同时省掉一个本地监听端口
（更安全、免端口冲突）。代码注释里也写明了「复现 server/server.js 中 Express 路由处理器的逻辑」。

### 8.2 结论：你应该选哪条路

| 方案 | 适用 | 评价 |
|---|---|---|
| **A. 本地起 HTTP 服务（子进程 / 独立进程），播放器请求 `127.0.0.1:PORT`** | Electron / Tauri / 桌面播放器 | ✅ **最推荐**。生态主流（MoeKoeMusic 就是这样），语言无关，调试方便（curl 就能测），出了问题和上游文档完全对得上 |
| **B. 在你的进程内直接 `require` KuGouMusicApi 的 `module/` 并复刻路由** | Electron（主进程） | ✅ 可行，EchoMusic 证明过。省端口、省进程，但你要自己复刻 `server.js` 的 Cookie 注入和 Authorization 解析（约 100 行），且升级上游时要盯着这套 shim |
| **C. 当 npm 依赖 `import` 函数** | — | ❌ **没人这么做**。`main.js` 靠 `fs.readdirSync('module')` 运行时读目录，打包/安装会出问题 |
| **D. 部署到 Vercel/远程服务器** | 多人共用 | ⚠️ 可行（有 `vercel.json`），但**你的 token 和 IP 都到了第三方机器上**，且 `X-Real-IP` 透传会把访问者 IP 带给酷狗，风控风险高。个人自用不推荐 |

**给「轻量播放器」的建议**：走方案 A。用 Node 子进程起 `node app.js --platform=lite --port=<随机空闲端口>`，
`KUGOU_API_GUID` / `KUGOU_API_DEV` / `KUGOU_API_MAC` 固定写死并持久化，
启动后调 `/register/dev` 拿 `dfid` 存起来，然后一切走 `/song/url`。

---

## 9. 许可与风险

### 9.1 MIT 是否允许复用签名代码？—— 允许，但要满足条件

**`LICENSE` 原文**（`Copyright (c) 2023 MakcRe`）：

> Permission is hereby granted, free of charge, to any person obtaining a copy of
> this software and associated documentation files (the "Software"), to deal in
> the Software without restriction, including without limitation the rights to
> use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
> the Software, and to permit persons to whom the Software is furnished to do so,
> subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.

**法律上，MIT 明确允许**：使用、复制、修改、合并、发布、分发、再许可、**出售**。
唯一的硬性条件是**保留版权声明和许可声明**。

**所以你要做的是**：
1. 在项目里放一份 `LICENSE`（MIT，`Copyright (c) 2023 MakcRe`）或
   `THIRD-PARTY-NOTICES.md`，写明 KuGouMusicApi 及其 MIT 许可。
2. 如果只是**复用签名算法**（`signatureAndroidParams` 那 10 行），
   严格说算法本身不受版权保护，但**照抄代码/常量就该署名**。
3. **不要**声称整个项目都是你的原创。

**⚠️ 一个真实的坑：GPL 传染**。
`EchoMusic` 是 **GPL-3.0-only**（`package.json` 的 `"license": "GPL-3.0-only"`）。
而它把 KuGouMusicApi 作为 **git submodule** 引入 —— submodule 是独立仓库，
MIT 与 GPL 可以共存，不构成传染。**但如果你打算把自己的播放器开源，
选 MIT/Apache 都不冲突；如果反过来想抄 EchoMusic 的客户端代码，那部分是 GPL 的，
会传染你的项目**。MoeKoeMusic 的许可请自行确认（本报告未核）。

### 9.2 作者的风险声明（**确证，README 原文，7 条**）

> 1. 本项目仅供学习使用，请尊重版权，请勿利用此项目从事商业行为及非法用途!
> 2. 使用本项目的过程中可能会产生版权数据。对于这些版权数据，本项目不拥有它们的所有权。
>    为了避免侵权，使用者务必在 24 小时内清除使用本项目的过程中所产生的版权数据。
> 3. 由于使用本项目产生的包括由于本协议或由于使用或无法使用本项目而引起的任何性质的
>    任何直接、间接、特殊、偶然或结果性损害（包括但不限于因商誉损失、停工、计算机故障
>    或故障引起的损害赔偿，或任何及所有其他商业损害或损失）由使用者负责。
> 4. **禁止在违反当地法律法规的情况下使用本项目。** 对于使用者在明知或不知当地法律法规
>    不允许的情况下使用本项目所造成的任何违法违规行为由使用者承担，本项目不承担由此
>    造成的任何直接、间接、特殊、偶然或结果性责任。
> 5. 音乐平台不易，请尊重版权，支持正版。
> 6. 本项目仅用于对技术可行性的探索及研究，不接受任何商业（包括但不限于广告等）
>    合作及捐赠。
> 7. 如果官方音乐平台觉得本项目不妥，可联系本项目更改或移除。

**注意第 2 条**：24 小时内清除版权数据 —— 做播放器意味着**必须实现本地缓存清理机制**，
否则字面上就违反了作者的要求。

**README 另有一句原理自述**：
> 工作原理：跨站请求伪造 (CSRF), 伪造请求头, 调用官方 API

这句话本身说明了项目的性质：**它是伪造客户端行为调用官方接口**，
不是什么授权 API。做播放器时，这个性质带来的所有风险（账号风控、封号、接口随时失效）
都由使用者承担。

### 9.3 实际工程风险清单（按严重度排序）

| 风险 | 证据 | 影响 | 缓解 |
|---|---|---|---|
| **接口随时会失效** | issue 列表里大量「url 失败」「登录失败」「领取 VIP 失败」；README 说「如果官方音乐平台觉得本项目不妥，可联系更改或移除」 | 播放器某天突然不能用 | 播放器不要把接口契约写死；做优雅降级；关注上游更新 |
| **20028 验证码风控** | issue #206（OPEN）、#257（OPEN）、#264（OPEN）；`ssa-code` 响应头 + WASM 验证包 | 部分接口（关注歌手、绑定设备、取 URL）会要求人机验证 | 用 `public/sid_edt_generator.html` 生成固定 `KUGOU_API_WEBGL`；**降低请求频率**；加 2 分钟以上的本地缓存 |
| **VIP 免听能力不稳定** | issue #179、#156 显示「领了 VIP 也拿不到高音质」「VIP 歌曲全失败」 | 核心卖点可能不成立 | 必须实现音质降级链（`flac → 320 → 128`）；UI 上诚实标注可用音质 |
| **token 会过期且不透明** | issue #110（作者回「我也不知道」）、#175（刷新返回 20018）、#202 | 用户突然掉登录 | 持久化 `token`/`userid`/`t1`/`dfid`；检测到登录失效就明确提示重新登录；不要静默重试 |
| **`/song/url/new` 音频加密** | 文档原文「存在音频加密（目前无法解码）」 | 用了就播不出声 | **别用**，只用 `/song/url` |
| **URL 可能 403** | issue #64 | 播放失败 | 优先 `backupUrl`，其次 `url`；两个都试 |
| **音频 URL 短时效** | URL 路径含时间戳目录（如 `202602062123`） | 缓存 URL 会失效 | **缓存 hash，不要缓存 URL**；每次播放重新取 |
| **设备注册不稳定** | issue #264（OPEN，error_code 20010） | 首次启动可能失败 | `dfid` 取到就持久化，失败要重试 + 兜底 |
| **请求 IP 透传** | `request.js` 设 `X-Real-IP` / `X-Forwarded-For` | URL 可能与 IP 绑定 | 服务和播放放同一台机器 |
| **EchoMusic 客户端代码是 GPL-3.0** | 它的 `package.json` | 抄它的代码会传染许可 | 只抄思路，不抄代码；或接受 GPL |

---

## 10. 落地清单（给播放器的直接结论）

### 必做

1. 把 KuGouMusicApi 作为 **git submodule** 引进（`server/` 或 `api/`），
   **不要**当 npm 依赖。
2. 用 **子进程起 HTTP 服务**，端口取随机空闲端口或固定 `127.0.0.1:6521` 之类，
   **只监听 127.0.0.1**。启动参数：
   `node app.js --platform=lite --port=<PORT>`
3. **固定并持久化**设备标识（`.env` 或启动时注入环境变量）：
   `KUGOU_API_GUID`（UUID v4）、`KUGOU_API_DEV`（10 位大写）、
   `KUGOU_API_MAC`、`KUGOU_API_WEBGL`。**每次都随机 = 每次都是新设备 = 更容易触发风控。**
4. 启动后第一件事调 `/register/dev`，**把 `dfid` 存下来**（顺便当健康检查）。
5. 播放取 URL 用 `/song/url`，音质按 `['flac','320','128']` 从高到低降级重试。
6. 优先读 `backupUrl`，其次 `url`；**缓存 hash，不缓存 URL**。
7. 读响应里的 `hash`/`std_hash` 而不是假设等于请求的 hash。
8. 处理 `priv_status === 0` 和 `hash_offset`（试听片段），UI 上如实标注。
9. 歌词：先 `/search/lyric` 拿 `id`+`accesskey`，再 `/lyric?fmt=krc&decode=true`。
10. 项目里放 `THIRD-PARTY-NOTICES.md`，写明 KuGouMusicApi（MIT, Copyright MakcRe）。

### 别做

1. ❌ 别用 `/song/url/new`（音频加密）。
2. ❌ 别频繁调登录接口（会被风控）。
3. ❌ 别把 `1005` 当概念版 appid（那是标准版；概念版是 `3116`）。
4. ❌ 别混用两个平台的 token（README 明确警告不通用）。
5. ❌ 别把服务部署到公网给多人用（token + IP 泄露 + 风控）。
6. ❌ 别一次请求就重新 `register/dev`（接口不稳，且没必要）。

### 建议的接口调用顺序（播放一首歌）

```
启动一次：
  KUGOU_API_GUID/DEV/MAC/WEBGL 固定注入
  GET /register/dev                    → 拿 dfid，持久化
  GET /login/qr/create + /login/qr/check 轮询 → 拿 token/userid/t1/vip_type/vip_token
         （或 /login/cellphone + /captcha/sent）

搜索/浏览：
  GET /search?keywords=xxx&type=song&cookie=...
  GET /search/complex?keywords=xxx
  GET /search/suggest?keywords=xxx
  GET /playlist/detail?ids=<global_collection_id>
  GET /playlist/track/all?id=<global_collection_id>&page=1&pagesize=30

播放：
  GET /song/url?hash=<hash>&album_audio_id=<id>&quality=flac&cookie=...
       → priv_status=0 ? 退 quality=320 → 再退 128
       → 取 backupUrl ?? url

歌词：
  GET /search/lyric?hash=<hash>&duration=<ms>&cookie=...
  GET /lyric?id=<id>&accesskey=<key>&fmt=krc&decode=true

播放上报（可选，用于"听歌领VIP"和听歌记录）：
  POST /user/listen/report
  POST /playhistory/upload
```

### 可选：自动续 VIP（概念版核心玩法）

```
POST /youth/v2/report/listen_song  { mixsongid }     ← 听歌领 VIP
POST /youth/v1/ad/play_report      { ad_id: 12307537187, play_start: now-30000, play_end: now }
GET  /user/vip/detail                                ← 查状态（busi_type=concept）
POST /login/token                                    ← 顺便续 token
```

**注意**：issue #205「领取vip不能用了吗?」、#132、#153 都报告过领 VIP 失败。
这个功能的可用性波动很大，**要做成「尽力而为」的可选功能，不能当核心依赖**。

---

## 附：报告未覆盖 / 需实测确认的事项

以下内容本报告**没有**（也无法从静态代码确证），建议你实测：

1. **音频 URL 的确切有效期**（几小时？跨天失效？）—— 静态代码看不出来。
2. **`url` vs `backupUrl` 的 403 差异在概念版下的具体表现** —— issue #64 是标准版场景。
3. **概念版领 VIP 后实际能拿到哪一档音质** —— issue #179 是反例，需自己验证当前状态。
4. **`hash_offset` 试听片段的时长**是否固定 60 秒。
5. **`X-Real-IP` 透传对 URL 的实际约束强度**。
6. **`public/verify-pkg/verifycode_bg.wasm` 是否能在 Node 里跑**（不依赖浏览器）——
   issue #206 里有用户说「似乎一定要部署在 web 服务器上才能工作」，作者说「可以模拟」但没给方案。
7. **MoeKoeMusic 的 License** —— 本报告未核，若你参考它的代码请自行确认。
