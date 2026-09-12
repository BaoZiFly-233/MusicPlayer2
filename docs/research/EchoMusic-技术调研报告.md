# EchoMusic 技术调研报告

调研对象：[`hoowhoami/EchoMusic`](https://github.com/hoowhoami/EchoMusic)（2969 stars / 217 forks / GPL-3.0-only / TypeScript）
调研时间点：仓库 HEAD 在 2026-09-12，主分支 `main`，最新正式版 `v2.3.1`，开发版 `v2.3.2-beta.4`。
所有引用均通过 `gh api` 读取远端文件，未 clone 仓库。

> **先说结论（最重要的一条）**：这个项目**没有自己实现酷狗签名算法**。它把上游开源项目 [`MakcRe/KuGouMusicApi`](https://github.com/MakcRe/KuGouMusicApi)（MIT，946 stars）以 **git submodule** 的形式挂在 `server/` 目录，打包时把它的 `module/`（217 个接口实现）和 `util/`（签名、加密、设备指纹）一起塞进安装包，然后**在 Electron 主进程里直接 `require` 这些模块、通过 IPC 调用**，不启动任何 HTTP 监听端口。所以：**你要借鉴的核心是 KuGouMusicApi，不是 EchoMusic。**

---

## 1. 技术栈与架构

### 1.1 技术栈（来自 `package.json` 与 README）

| 层 | 选型 | 版本 |
|---|---|---|
| 桌面壳 | **Electron** | `43.7.0`（README 徽章写 43.4.1） |
| 前端 | **Vue 3** + TypeScript | `vue ^3.5.38` / `typescript ^5.9.3` |
| 构建 | **Vite** | `^8.0.14`（配 `vite-plugin-electron` `^0.29.0`） |
| 路由 | vue-router | `^4.6.4` |
| 状态 | **Pinia** | `^3.0.4`，持久化到原生 SQLite |
| UI 基元 | **Reka UI** | `^2.9.10` |
| CSS | **Tailwind CSS v4** | `^4.3.0` |
| 打包 | **electron-builder** | `26.8.1` |
| 包管理 | pnpm（workspace 含 `server`） | — |
| 原生模块 | **Rust + napi-rs** | 5 个 addon |

**不是 Tauri、不是 Capacitor，是纯 Electron。** 仓库 topics 也写着 `["desktop","electron","kugou","music","music-player"]`。

Rust 原生模块共 5 个（`native/` 下，`.node` 不入库，需本地构建）：

| 模块 | 用途 |
|---|---|
| `native/echo-audio-player` | 播放、解码、音效（FFmpeg + SoundTouch） |
| `native/echo-audio-capture` | 系统音频/麦克风采集（听歌识曲） |
| `native/echo-media-controls` | macOS NowPlaying / Windows SMTC / Linux MPRIS |
| `native/echo-sqlite-store` | SQLite 持久化 |
| `native/echo-platform-adaptor` | 系统窗口/任务栏适配（Linux 不需要） |

### 1.2 代码量

GitHub linguist 统计（`gh api repos/hoowhoami/EchoMusic/languages`）：

```
TypeScript  2,721,650 字符   ← 最大头
Vue         2,118,532 字符
Rust        1,116,145 字符
JavaScript    283,365 字符
CSS           120,368 字符
HTML           13,455 字符
NSIS            1,471 字符
```

文件数（`git/trees?recursive=1`，共 1155 个 path）按顶层目录：

```
src/      576    native/  400    tests/  87    build/  25
screenshots/ 15  docs/     10    .github/ 8     cloudflare/ 6
```

`tests/` 下 87 个测试文件，用 `.test.ts` / `.test.mjs` 混用。

### 1.3 进程架构图

```
┌─────────────────────────── Electron ───────────────────────────┐
│  Renderer (Vue 3 + Pinia)                                      │
│    src/renderer/api/*.ts   ← 25+ 个业务接口封装                 │
│         │                                                      │
│         └─ src/renderer/utils/request.ts                       │
│             request.get('/song/url', {params})                 │
│             ├─ 拼 Authorization header（token/userid/dfid/…）  │
│             └─ window.electron.api.request(ipcConfig)  ──┐     │
│                                                          │ IPC │
│  Main process                                            ▼     │
│    src/main/ipc/server.ts  → 'api:request' handler             │
│         │                                                      │
│         └─ src/main/server.ts  handleApiRequest()              │
│             ├─ scanModules()  扫 server/module/*.js 建路由表   │
│             ├─ parseAuthCookie(Authorization) → cookie 对象    │
│             ├─ buildDefaultCookies()                          │
│             │     KUGOU_API_PLATFORM / MID / GUID / DEV / MAC  │
│             └─ require(module).js → module(query, createRequest)│
│                       │                                        │
│                       └─ server/util/request.js               │
│                          签名 → axios → *.kugou.com            │
└────────────────────────────────────────────────────────────────┘
```

关键点在 `src/main/server.ts` 的 `scanModules()`——它把文件名映射成路由：

```ts
files
  .reverse()
  .filter((fileName) => fileName.endsWith('.js') && !fileName.startsWith('_'))
  .forEach((fileName) => {
    const route = '/' + fileName.replace(/\.js$/i, '').replace(/_/g, '/');
    const modulePath = path.resolve(modulesPath, fileName);
    routeMap.set(route, modulePath);
  });
```

所以 `server/module/song_url.js` → 路由 `/song/url`，`server/module/login_cellphone.js` → `/login/cellphone`。这就是渲染层 `request.get('/song/url')` 能对上的原因——**和 KuGouMusicApi 原始 Express 路由规则完全一致**，只是把 Express 换成了 IPC。

主进程初始化（`src/main/server.ts` → `initApiServer()`）：

```ts
process.env.platform = 'lite';                        // 概念版开关，关键！
const { cryptoMd5 } = require(path.join(utilPath, 'crypto'));
const { getGuid, calculateMid, generateWebGLHash } = require(path.join(utilPath, 'util'));
const { createRequest } = require(path.join(utilPath, 'request'));
applyCliOverrides(['--platform=lite']);
process.env.KUGOU_API_DEV = SERVER_DEV;              // 常量 'EchoMusic'
guid = process.env.KUGOU_API_GUID || cryptoMd5(getGuid());
mid  = calculateMid(guid);
webglHash = process.env.KUGOU_API_WEBGL || generateWebGLHash();
```

对照上游 `server.js`，原本是 `const guid = cryptoMd5(getGuid());` 和 `const serverDev = randomString(10).toUpperCase();`。EchoMusic 把这两者改成**可持久化**（`KUGOU_API_GUID` 环境变量 + `mergePersistedDeviceInfo()` 写回 KV），并锁死 `KUGOU_API_DEV = 'EchoMusic'`。这是它相对上游唯一实质性的设备身份改造。

---

## 2. 网络层：直连酷狗官方接口

**是官方接口，域名就是 `*.kugou.com`。** 没有第三方中转，没有作者自建的服务端，没有 `kugouapi.com` 之类的第三方 API。

`server/util/request.js` 里默认 `baseURL` 是 `https://gateway.kugou.com`，其余接口各自覆盖：

| baseURL | 用途 | 出处 |
|---|---|---|
| `https://gateway.kugou.com` | 默认网关 | `util/request.js` |
| `http://login.user.kugou.com` | token 刷新 `/v5/login_by_token`、发短信 `/v7/send_mobile_code` | `module/login_token.js`、`module/captcha_sent.js` |
| `https://loginserviceretry.kugou.com` | 手机验证码登录 `/v7/login_by_verifycode` | `module/login_cellphone.js` |
| `https://login-user.kugou.com` | 二维码 `/v2/qrcode`、`/v2/get_userinfo_qrcode` | `module/login_qr_key.js`、`module/login_qr_check.js` |
| `https://userservice.kugou.com` | 设备注册 `/risk/v2/r_register_dev` | `module/register_dev.js` |
| `https://lyrics.kugou.com` | 歌词搜索 `/v1/search`、下载 `/download` | `module/search_lyric.js`、`module/lyric.js` |
| `http://tracker.kugou.com` | `/v6/priv_url` | `module/song_url_new.js` |
| `http://relation.user.kugou.com` | `/v1/get_my_userinfo` | `module/user_info.js` |
| `https://kugouvip.kugou.com` | `/v1/get_union_vip` | `module/user_vip_detail.js` |

其余大都走 gateway + `x-router` 头指定后端集群，例如：

```js
// module/privilege_lite.js
return useAxios({
  url: '/v2/get_res_privilege/lite',
  data: dataMap,
  method: 'post',
  encryptType: 'android',
  headers: { 'x-router': 'media.store.kugou.com', 'Content-Type': 'application/json' },
});
```

```js
// module/search.js
return useAxios({
  url: `/${type === 'song' ? 'v3' : 'v1'}/search/${type}`,
  headers: { 'x-router': 'complexsearch.kugou.com' },
});
```

```js
// module/song_url.js
return useAxios({
  url: '/v5/url',
  method: 'GET',
  headers: { 'x-router': 'trackercdn.kugou.com' },
});
```

### 代理

上游支持 `KUGOU_API_PROXY` 环境变量（`util/runtime.js` 的 `resolveProxy()`），EchoMusic 在 `src/main/networkSettings.ts` 里把它接到自己的「网络设置」界面（跟随系统 / 强制直连 / WPAD / PAC / 手动代理），密码用 Electron `safeStorage` 加密后存 SQLite：

```ts
const encryptProxyPassword = (password: string): string | null => {
  if (!password) return null;
  if (!safeStorage.isEncryptionAvailable()) throw new Error('当前系统无法安全保存代理密码');
  return safeStorage.encryptString(password).toString('base64');
};
```

网络会话按用途分了 partition（`src/main/networkPolicy.ts`）：`echo-app-network`、`echo-kugou-api`、`echo-community-audio`、`persist:desktop-lyric`、`electron-updater`。

### 依赖清单

`server/package.json` 声明的运行依赖很轻：`axios`、`express`、`pako`、`qrcode`、`safe-decode-uri-component`（Express 在 IPC 模式下实际不用，但打包时 `electron-builder` 会把 `server/node_modules` 整个塞进 `extraResources`）。版本 `1.6.2`。

`server` 子模块 **pin 在 commit `99ca12fb9d464e9cf1e893a4f967a6024885336b`**（2026-09-11，"feat: 添加专辑动态封面接口 (#265)"）。核对过 `util/helper.js` 的 `-X GET` 读取结果与 pin 一致。

---

## 3. 签名 / 加密机制

全部在 `server/util/helper.js`（签名）和 `server/util/crypto.js`（加解密）。**清一色 MD5 + salt 拼接**，另有 RSA/AES 用于登录链路和歌单导入。

### 3.1 平台参数（`server/util/config.json`）

```json
{
  "srcappid": 2919,
  "appid": 1005,
  "apiver": 20,
  "clientver": 20489,
  "liteAppid": 3116,
  "liteClientver": 11440,
  "wx_appid": "wx79f2c4418704b4f8",
  "wx_lite_appid": "wx72b795aca60ad321",
  "wx_secret": "4efcab88b700769e376e3f6087b8abc9",
  "wx_lite_secret": "33e486041e5e25729a4e3d2da7502f9a",
  "qq_appid": "205141",
  "qq_lite_appid": "101706348"
}
```

`process.env.platform === 'lite'` 时用 `appid=3116` / `clientver=11440`（酷狗**概念版**）；否则 `1005` / `20489`（标准版）。EchoMusic 恒定 `lite`。

### 3.2 三套 signature 算法

```js
// 1) Android 签名（最常用）。salt 区分标准版/概念版
const signatureAndroidParams = (params, data) => {
  const isLite = process.env.platform === 'lite';
  const str = isLite ? 'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA' : `OIlwieks28dk2k092lksi2UIkp`;
  const paramsString = Object.keys(params)
    .sort()
    .map((key) => `${key}=${typeof params[key] === 'object' ? JSON.stringify(params[key]) : params[key]}`)
    .join('');
  // Buffer body 走流式 MD5，否则直接拼
  if (Buffer.isBuffer(data)) { /* hasher.update(str/paramsString/data/str) */ }
  return cryptoMd5(`${str}${paramsString}${data || ''}${str}`);
};

// 2) Web 签名
const signatureWebParams = (params, data) => {
  const str = 'NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt';
  const paramsString = Object.keys(params).map((key) => `${key}=${params[key]}`).sort().join('');
  return cryptoMd5(`${str}${paramsString}${data || ''}${str}`);
};

// 3) 设备注册签名（只取值，不含 key，salt 是 '1014'）
const signatureRegisterParams = (params) => {
  const paramsString = Object.keys(params).map((key) => params[key]).sort().join('');
  return cryptoMd5(`1014${paramsString}1014`);
};
```

即：**`MD5(salt + 按 key 排序后的 k=v 拼接 [+ body] + salt)`**。`signatureWebParams` 里 key 和 value 都用默认 `toString`，所以数组会变成 `a,b`，对象会变成 `[object Object]`（这是上游的历史行为，改了反而不对）。

### 3.3 其他签名的 salt（全部硬编码）

```js
const signParams = (params, data) => {
  const str = 'R6snCXJgbCaj9WFRJKefTMIFp0ey6Gza';
  const paramsString = Object.keys(params).sort().map((key) => `${key}${params[key]}`).join(''); // 无等号
  return cryptoMd5(`${paramsString}${data || ''}${str}`);   // salt 在末尾
};

const signKey = (hash, mid, userid, appid) => {           // 概念版 salt
  const str = isLite ? '185672dd44712f60bb1736df5a377e82' : '57ae12eb6890223e355ccfcb74edf70d';
  return cryptoMd5(`${hash}${str}${appid || useAppid}${mid}${userid || 0}`);
};

const signCloudKey = (hash, pid) => {
  const str = 'ebd1ac3134c880bda6a2194537843caa0162e2e7';   // 云盘
  return cryptoMd5(`musicclound${hash}${pid}${str}`);
};

const signParamsKey = (data, appid, clientver) => {        // 概念版复用 Android salt
  const str = isLite ? 'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA' : 'OIlwieks28dk2k092lksi2UIkp';
  appid = appid || (isLite ? liteAppid : useAppid);
  clientver = clientver || (isLite ? liteClientver : useClientver);
  return cryptoMd5(`${appid}${str}${clientver}${data}`);
};
```

### 3.4 每个请求自动注入的参数（`util/request.js`）

```js
const dfid       = options?.cookie?.dfid || '-';        // 设备指纹 ID
const mid        = `${options?.cookie?.KUGOU_API_MID}`; // 设备 MID
const uuid       = '-';                                 // 恒定 '-'
const token      = options?.cookie?.token || '';
const userid     = options?.cookie?.userid || 0;
const clienttime = Math.floor(Date.now() / 1000);

const headers = { dfid, clienttime, mid, 'kg-rc': '1', 'kg-thash': '5d816a0',
                  'kg-rec': 1, 'kg-rf': 'B9EDA08A64250DEFFBCADDEE00F8F25F' };
if (ip) { headers['X-Real-IP'] = ip; headers['X-Forwarded-For'] = ip; }

const defaultParams = {
  dfid, mid, uuid,
  appid:     isLite ? liteAppid : appid,
  clientver: isLite ? liteClientver : clientver,
  clienttime,
};
if (token) defaultParams['token'] = token;
if (userid && userid !== 0) defaultParams['userid'] = userid;

if (options?.encryptKey) {
  params['key'] = signKey(params['hash'], params['mid'], params['userid'], params['appid']);
}
```

默认 User-Agent（可以拿去对着抓包）：

```
Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi
```

手机登录另外用了 `Android16-1070-11440-130-0-LOGIN-wifi`。

注意 `uuid` 被**写死成 `'-'`**，不是真 uuid；`kg-rf`、`kg-thash` 是固定的内部标识头。

### 3.5 RSA / AES（`util/crypto.js`）

两把硬编码 RSA 公钥（1024 bit）：

```js
const publicRasKey     = 'MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDIAG7QOELSYoIJvTFJhMpe1s/gbjDJX51HBNnEl5HXqTW6lQ7LC8jr9fWZTwusknp+sVGzwd40MwP6U5yDE27M/X1+UR4tvOGOqp94TJtQ1EPnWGWXngpeIW5GxoQGao1rmYWAu6oi1z9XkChrsUdC6DJE5E221wf/4WLFxwAtRQIDAQAB';  // 标准版
const publicLiteRasKey = 'MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDECi0Np2UR87scwrvTr72L6oO01rBbbBPriSDFPxr3Z5syug0O24QyQO8bg27+0+4kBzTBTBOZ/WWU0WryL1JSXRTXLgFVxtzIY41Pe7lPOgsfTCn5kZcvKhYKJesKnnJDNr5/abvTGf+rHG3YRwsCHcQ08/q6ifSioBszvb3QiwIDAQAB';  // 概念版
```

加密原语：

- `cryptoMd5` / `cryptoSha1` — crypto-js
- `cryptoAesEncrypt(data, {key, iv})` — AES-128-CBC + PKCS7。**注意如果传了 `opt.key` 但没传 `opt.iv`，或没传 `opt`，会走 `tempKey = randomString(16).toLowerCase()`，然后 `key = MD5(tempKey).substring(0,32)`、`iv = key.substring(16)`**，返回 `{ str: hex, key: tempKey }`——这是登录链路里「客户端随机生成 AES key，再用 RSA 把 key 传给服务端」的模式。
- `cryptoAesDecrypt(data, key, iv)`
- `cryptoRSAEncrypt(data, publicKey)` — 裸 RSA（自己 BigInt modPow，高位补零，输出 hex）；`rsaEncrypt2` 用 node-forge 的 `RSAES-PKCS1-V1_5`
- `playlistAesEncrypt(data)` — 歌单导入：`key = randomString(6)`，`encryptKey = MD5(key)[0:16]`、`iv = MD5(key)[16:32]`，AES-CBC，输出 base64

### 3.6 概念版登录链路的静态密钥（`module/login_token.js`）

```js
const liteKey   = 'c24f74ca2820225badc01946dba4fdf7';
const liteIv    = 'adc01946dba4fdf7';
let liteT2Key   = 'fd14b35e3f81af3817a20ae7adae7020';
let liteT2Iv    = '17a20ae7adae7020';
let liteT1Key   = '5e4ef500e9597fe004bd09a46d8add98';
let liteT1Iv    = '04bd09a46d8add98';

const encrypt = cryptoAesEncrypt({ clienttime, token }, { key: liteKey, iv: liteIv });
const encryptParams = cryptoAesEncrypt({});                       // 随机 key
const pk = cryptoRSAEncrypt({ clienttime_ms: dateNow, key: encryptParams.key });

const t2 = cryptoAesEncrypt(
  `${GUID}|0f607264fc6318a92b9e13c65db7cd3c|${MAC}|${DEV}|${dateNow}`,
  { key: liteT2Key, iv: liteT2Iv }
);
const t1 = cryptoAesEncrypt(`${t1 || ''}|${dateNow}`, { key: liteT1Key, iv: liteT1Iv });
```

`t2` 里的 `0f607264fc6318a92b9e13c65db7cd3c` 是硬编码的常量串（不是设备相关的）。

### 3.7 KRC 歌词解密（`util/util.js` 的 `decodeLyrics`）

```js
const enKey = [64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105];
const krcBytes = bytes.slice(4);                       // 跳过 4 字节文件头
for (let index = 0; index < len; index += 1) {
  krcBytes[index] = krcBytes[index] ^ enKey[index % enKey.length];  // XOR
}
const inflate = pako.inflate(krcBytes);                // zlib 解压
return Buffer.from(inflate).toString('utf8');
```

**krc = 跳过 4 字节头 → 16 字节密钥循环 XOR → zlib inflate。**

---

## 4. 登录方式

EchoMusic 支持 **5 种**（`src/renderer/views/Login.vue`）：

```ts
type LoginMethod = 'kugou' | 'sms' | 'account' | 'qq' | 'wechat';
const loginMethods = [
  { value: 'kugou',   label: '酷狗',   icon: iconQrCode },      // 酷狗 App 扫码
  { value: 'sms',     label: '验证码', icon: iconSmartphone },  // 手机号+短信
  ...
];
```

| 方式 | 渲染层调用 | 上游模块 | 实际上游地址 |
|---|---|---|---|
| 酷狗扫码 | `/login/qr/key` → `/login/qr/create` → `/login/qr/check` | `login_qr_key.js` / `login_qr_create.js` / `login_qr_check.js` | `login-user.kugou.com/v2/qrcode`、`/v2/get_userinfo_qrcode` |
| 手机验证码 | `/captcha/sent` → `/login/cellphone` | `captcha_sent.js` / `login_cellphone.js` | `/v7/send_mobile_code`、`loginserviceretry.kugou.com/v7/login_by_verifycode` |
| 账号密码 | `/login` | `login.js` | — |
| QQ 扫码 | `/login/qq/qr/create` → `/login/qq/qr/check` | `login_qq_qr_create.js` / `login_qq_qr_check.js` | 上游 2026-09-02 新增的开放平台链路 |
| 微信扫码 | `/login/wx/create` → `/login/wx/check` → `/login/openplat` | `login_wx_create.js` / `login_wx_check.js` / `login_openplat.js` | 走 `wx_lite_appid` |

**不提供「手动粘贴 token」的登录入口**（渲染层 API 里没有这个调用），但 token 是持久化在 Pinia store 里的，理论上可以直接改存储。

### 4.1 token 存储与刷新

**存储**：`src/renderer/stores/user.ts`，Pinia + `persist: true`，落盘到原生 SQLite（`native/echo-sqlite-store`，经 `src/renderer/stores/sqlitePersist.ts`）。字段：`token` / `userid` / `t1` / `vip_type` / `vip_token`。

**注入**：`src/renderer/utils/request.ts` 的 `buildAuthHeader()` 把用户和设备身份拼成一个字符串塞进 `Authorization` 头：

```ts
export const buildAuthHeader = (skipAuth = false): string => {
  if (skipAuth) return '';
  const authParts: string[] = [];
  if (userStore.info) {
    if (userStore.info.token)  authParts.push(`token=${userStore.info.token}`);
    if (userStore.info.userid) authParts.push(`userid=${userStore.info.userid}`);
    if (userStore.info.t1)     authParts.push(`t1=${userStore.info.t1}`);
  }
  if (deviceStore.info) {
    const device = deviceStore.info;
    if (device.dfid)      authParts.push(`dfid=${device.dfid}`);
    if (device.mid)       authParts.push(`KUGOU_API_MID=${device.mid}`);
    if (device.uuid)      authParts.push(`uuid=${device.uuid}`);
    if (device.guid)      authParts.push(`KUGOU_API_GUID=${device.guid}`);
    if (device.serverDev) authParts.push(`KUGOU_API_DEV=${device.serverDev}`);
    if (device.mac)       authParts.push(`KUGOU_API_MAC=${device.mac}`);
  }
  return authParts.join(';');
};
```

主进程 `src/main/server.ts` 用 `parseAuthCookie()` 按 `;` 和 `=` 拆回对象当 cookie，再合并给模块。这实际上就是把上游原本的 HTTP Cookie 机制换成了 IPC 传参——**`KUGOU_API_*` 这些名字本来就是上游 `server.js` 里的 cookie 键名**。

**刷新**：上游有 `module/login_token.js`（`/v5/login_by_token`）可以刷新 token，但 **EchoMusic 渲染层根本没调用它**（我逐个扫过 `src/renderer/api/*.ts`，没有 `/login/token`）。它只在 token 过期时弹窗：

```ts
// src/renderer/utils/request.ts
const checkAuthExpiration = (path: string, data: any): boolean => {
  const rules = [
    () => Number(data.error_code) === 20018,
    () => data.msg && typeof data.msg.includes === 'function' && data.msg.includes('登录已过期'),
  ];
  return rules.some((rule) => rule());
};
// ...
useAuthStore().showSessionExpiredDialog();   // 只提示用户重新登录
```

所以 **EchoMusic 不做自动续期，过期就要求用户重新登录**。

### 4.2 设备指纹 / 风控

设备身份链：`guid` → `mid = MD5(guid) 当 16 进制大整数转十进制` → `dfid`（向 `/risk/v2/r_register_dev` 注册拿到）。

`calculateMid`（`util/util.js`）：

```js
const calculateMid = (str) => {
  let bigInteger = bigInt(0);
  const digest = CryptoJS.MD5(str).toString(CryptoJS.enc.Hex);
  for (let i = 0; i < digest.length; i += 1) {
    const charValue = bigInt(parseInt(digest.charAt(i), 16));
    bigInteger = bigInteger.add(charValue.multiply(bigInt(16).pow(digest.length - 1 - i)));
  }
  return bigInteger.toString();   // 十进制字符串
};
```

`guid` 生成：优先读 `process.env.KUGOU_API_GUID`，否则 `cryptoMd5(getGuid())`（`getGuid()` 是 UUID v4 格式的随机串），生成后**回写持久化**，保证重启复用同一身份：

```ts
// src/main/server.ts
guid = process.env.KUGOU_API_GUID || cryptoMd5(getGuid());
mid  = calculateMid(guid);
webglHash = process.env.KUGOU_API_WEBGL || generateWebGLHash();
mergePersistedDeviceInfo({ guid, mid, serverDev: SERVER_DEV, mac: ... });
```

MAC 地址取真实网卡（没有则回落 `02:00:00:00:00:00`）：

```ts
const getRealMacAddress = (): string => {
  const interfaces = os.networkInterfaces();
  for (const entries of Object.values(interfaces)) {
    if (!entries) continue;
    for (const entry of entries) {
      if (!entry.internal && entry.mac && entry.mac !== '00:00:00:00:00:00') {
        return entry.mac.toUpperCase();
      }
    }
  }
  return '02:00:00:00:00:00';
};
```

`WebGL` 指纹（`util/util.js` 的 `generateWebGLHash`）：浏览器里真的编译着色器、画三角形、`readPixels`，再对像素 + `UNMASKED_VENDOR_WEBGL`/`UNMASKED_RENDERER_WEBGL`/`VERSION` 元数据做 **FNV-1a 64-bit** 哈希；Node 环境（EchoMusic 就是这种）**直接随机生成一个 uint64 顶替**：

```js
// Node 环境或 WebGL 不可用：生成随机 uint64 作为模拟指纹
const hi = Math.floor(Math.random() * 0xffffffff);
const lo = Math.floor(Math.random() * 0xffffffff);
return (BigInt(hi) * BigInt(0x100000000) + BigInt(lo)).toString();
```

设备注册（`module/register_dev.js`）本质是**伪造一整套安卓设备档案**：品牌写死 `Redmi`、厂商 `Xiaomi`、机型 `marble`，内存 `4983533568` 字节、电池 100%、一连串传感器布尔值全 `false`。body 用 `playlistAesEncrypt` 加密、`rsaEncrypt2` 打包 AES key，POST 到 `https://userservice.kugou.com/risk/v2/r_register_dev?part=1&platid=1&p=...`，拿回的 body 再用同一 AES key 解密，取出 `dfid`。

**行为指纹 `sid`/`edt`**：酷狗风控要求时会在响应头返回 `ssa-code`（`gz_tx_event_xxx` 格式），需要拿它去 `/get/verify/info` 换验证挑战，完成后还要提交 `sid`/`edt`。EchoMusic 的做法是**把这一步整体甩给服务端模拟**（`util/generate_simulate.js`）：

```ts
// src/renderer/utils/kugouVerification.ts
// 桌面端无法在浏览器侧采集行为指纹，统一交由服务端 /sidedt 模拟生成 sid/edt 并完成校验。
const result = await challenge.request('/sidedt', {
  eventid: challenge.eventId,
  v_type: verifyType,
  verifycode: code,
});
```

而 `module/sidedt.js` 就是 `generateSimulate(mid, userid, dfid, webglHash)` 生成假指纹后调 `verify_user_info`。EchoMusic 还做了一套挺完整的验证码弹窗（`src/renderer/components/app/KugouVerificationFlow.vue`），认得这些类型：

```ts
export const KUGOU_CAPTCHA_PROVIDER_NAMES: Record<KugouCaptchaProvider, string> = {
  TX: '腾讯验证码', GT: '极验验证码', KG: '酷狗滑块', KG2: '酷狗旋转',
  SM: '数美验证码', YD: '网易易盾', SMS: '手机验证码', LOGIN: '登录确认',
  BIND_PHONE: '绑定手机号', REAL_NAME: '实名认证',
  ACCOUNT_RISK: '账号风控', UNKNOWN: '未知验证',
};
```

其中 `v_type=51` 直接判为账号被风控，提示「当前账号被酷狗风控限制，请在酷狗客户端完成申诉流程」。

---

## 5. 核心功能接口对照表

以下是 EchoMusic 实际调用的路径（扫 `src/renderer/api/*.ts` 得到的全集）与上游模块、实际上游地址的对应关系。

### 搜索

| EchoMusic 路径 | 上游模块 | 上游地址 |
|---|---|---|
| `/search` | `search.js` | `gateway.kugou.com/v3/search/song`（type=song）或 `/v1/search/{special,lyric,album,author,mv}`，`x-router: complexsearch.kugou.com` |
| `/search/hot` | `search_hot.js` | — |
| `/search/default` | `search_default.js` | — |
| `/search/suggest` | `search_suggest.js` | — |

`search.js` 参数：`keyword` / `page` / `pagesize`(默认 30) / `platform: 'AndroidFilter'` / `iscorrection: 1`。

### 播放地址与音质

| EchoMusic 路径 | 上游模块 | 上游地址 |
|---|---|---|
| `/song/url` | `song_url.js` | `gateway.kugou.com/v5/url`，`x-router: trackercdn.kugou.com` |
| `/privilege/lite` | `privilege_lite.js` | `gateway.kugou.com/v2/get_res_privilege/lite`，`x-router: media.store.kugou.com` |
| `/audio` | `audio.js` | 批量取歌名/歌手/封面 |
| `/song/climax` | `song_climax.js` | 高潮片段 |
| `/song/ranking` `/song/ranking/filter` | `song_ranking.js` / `song_ranking_filter.js` | — |
| `/images/audio` | `images_audio.js` | 歌手写真 |

`/song/url` 的完整参数（概念版分支）：

```js
const page_id  = isLite ? 967177915 : 151369488;
const ppage_id = isLite ? (params.ppage_id || '356753938,823673182,967485191')
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
  IsFreePart: params?.free_part ? 1 : 0,
  ppage_id,
  cdnBackup: 1,
  module: '',
  clientver: 11430,
};
// 注意：notSign: true + encryptKey: true → 不做 signature，但生成 key
// cookie 里额外塞了个随机 dfid：Object.assign({}, {dfid: randomString(24)}, params?.cookie)
```

**`notSign: true`** 是个容易踩的点——这个接口不做 signature 签名，但用 `encryptKey: true` 生成 `key = signKey(hash, mid, userid, appid)`（概念版 salt `185672dd44712f60bb1736df5a377e82`）。`song_url_auth.js` 同理。

还有三个变体模块（EchoMusic 当前只用基础版）：

- `song_url_new.js` → `http://tracker.kugou.com/v6/priv_url`（POST，带 `vip_token`/`vip_type`/`priv_vip_type: '6'`）
- `song_url_auth.js` → `/tracker/v5/url`（GET，需 `auth` + `open_time`）
- `song_url_auth_merge.js` → 先 `song_auth` 拿 `auth`/`open_time`，再调上面这个（概念版「免费听」链路）

`/privilege/lite` 请求体（决定你能拿到哪些音质）：

```js
const dataMap = {
  appid, area_code: 1, behavior: 'play', clientver,
  need_hash_offset: 1, relate: 1, support_verify: 1,
  resource,   // [{ type:'audio', page_id:0, hash, album_id }]
  qualities: ['128','320','flac','high','viper_atmos','viper_tape',
              'viper_clear','super','multitrack'],
};
```

EchoMusic 在 `src/renderer/utils/song.ts` 里把 `relate_goods` 的 `quality`/`level` 映射成界面标签：

```ts
if (hasQuality('viper_tape', 101)) return '母带';
if (hasQuality('high',       6))   return 'Hi-Res';
if (hasQuality('flac',       5))   return 'SQ';
if (hasQuality('320',        4))   return 'HQ';
```

### 歌词

| EchoMusic 路径 | 上游模块 | 上游地址 |
|---|---|---|
| `/search/lyric` | `search_lyric.js` | `lyrics.kugou.com/v1/search`（`clearDefaultParams: true` + `notSign: true`） |
| `/lyric` | `lyric.js` | `lyrics.kugou.com/download` |

EchoMusic 请求歌词固定 `decode=true&fmt=krc`，上游自动解密：

```js
if (params?.decode && res.body?.content) {
  res.body['decodeContent'] =
    params?.fmt == 'lrc' || Number(res.body?.contenttype) !== 0
      ? Buffer.from(res.body?.content, 'base64').toString()
      : decodeLyrics(res.body.content);   // 走 XOR + zlib
}
```

### 歌单 / 推荐 / 排行榜 / FM

| EchoMusic 路径 | 说明 |
|---|---|
| `/playlist/recommend` | 推荐歌单 |
| `/playlist/detail` `/playlist/track/all` `/playlist/track/all/new` | 歌单详情与曲目 |
| `/user/playlist` | 我的歌单 |
| `/rank/list` `/rank/top` `/rank/audio` | 排行榜 |
| `/playlist/tags` `/top/playlist` `/top/ip` | 标签、分类歌单、IP 专区 |
| `/playlist/add` `/playlist/del` `/playlist/tracks/add` `/playlist/tracks/del` | 歌单增删改 |
| `/top/song` `/top/album` | 新歌榜、新碟上架 |
| `/everyday/recommend` | 每日推荐 → `/everyday_song_recommend`，`x-router: everydayrec.service.kugou.com` |
| `/everyday/style/recommend` | 风格推荐（`platform: 'ios'`） |
| `/personal/fm` | 私人 FM（支持 `action: play/garbage`、`mode: normal/small/peak`） |
| `/import/playlist` | 外部歌单导入（网易云/QQ/酷我/酷狗/汽水/Spotify/Apple Music） |
| `/comment/*` | 歌曲/歌单/专辑评论与楼层、发评论、弹幕 |
| `/video/url` `/video/privilege` `/video/detail` `/kmr/audio/mv` | MV |
| `/audio/match` | 听歌识曲（octet-stream 上传 PCM） |
| `/user/cloud` `/user/cloud/url` `/user/cloud/upload` `/user/cloud/del` | 音乐云盘 |
| `/user/history` `/playhistory/upload` | 播放历史 |
| `/user/listen/report` `/user/grade/info` | 听歌时长上报与等级 |
| `/user/purchased/songs` `/user/purchased/albums` | 已购单曲/专辑 |

### 用户与 VIP

| EchoMusic 路径 | 上游模块 | 上游地址 |
|---|---|---|
| `/user/detail` | `user_detail.js` | — |
| `/user/vip/detail` | `user_vip_detail.js` | `kugouvip.kugou.com/v1/get_union_vip?busi_type=concept` |
| `/user/info`（上游） | `user_info.js` | `relation.user.kugou.com/v1/get_my_userinfo`，参数 `p=RSA(clienttime,token)` + `key=signParamsKey(clienttime)` |
| `/youth/day/vip` `/youth/day/vip/upgrade` `/youth/month/vip/record` | 每日畅听会员 | EchoMusic 保留了接口封装，但见第 8 节说明 |
| `/login/device` `/login/device/kick` | 登录设备管理 | — |

上游 `module/` 目录共 **217 个 `.js`**，EchoMusic 只用了其中约 60 个。

---

## 6. 播放地址解密

### 结论：**不需要解密，返回的就是明文 HTTP(S) URL。**

证据：

**1. 服务端不返回加密内容。** `module/song_url.js` 原样把上游响应 `resolve` 出去，没有任何解密步骤（对比 `register_dev.js` / `lyric.js` 那种明确调用 `playlistAesDecrypt` / `decodeLyrics` 的写法）。`src/renderer/api/music.ts` 直接读 `data.url` / `backup_url`：

```ts
export interface CloudSongUrlData {
  url?: string;
  backup_url?: string | string[];
  hash?: string;
  fileSize?: string | number;
  extName?: string;
}

const normalizeCloudSongUrls = (data: CloudSongUrlData): string[] => {
  const urls = new Set<string>();
  const add = (value: unknown) => {
    if (typeof value !== 'string') return;
    const url = value.trim();
    if (url) urls.add(url);
  };
  add(data.url);
  const backups = data.backup_url ?? data.backupUrl;
  if (Array.isArray(backups)) backups.forEach(add); else add(backups);
  return [...urls];
};
```

**2. Rust 播放器直接 HTTP 拉流。** `native/echo-audio-player/src/stream/http.rs`：

```rust
pub fn is_http_url(url: &str) -> bool {
    url.starts_with("http://") || url.starts_with("https://")
}

pub fn open(url: &str, interrupt: Arc<AtomicBool>, options: &StreamOptions)
    -> Result<Box<dyn ReadSeek>, String> {
    let candidates = if options.http_proxies.is_empty() { vec![None] }
                     else { options.http_proxies.clone() };
    // ... HttpAudioSource::new_with_options_and_cancel_flag(url, ...)
}
```

交给 `ffmpeg_audio::HttpAudioSource` 直接喂 FFmpeg 解码，**代码库里没有任何 `.kgm` / `.kgma` 解密逻辑**。`stream/url.rs` 只做 `file://` → 本地路径的解析，纯路径处理。

历史注脚：issue #317（2026-07-06）的日志里还能看到 `[MpvController] Starting libmpv player` 和 `resources/mpv/libmpv-2.dll`——说明当时用的是 **libmpv**。现在换成了自研 Rust/FFmpeg 播放引擎（`native/echo-audio-player`）。

### 实际拿到的 URL 长这样

从 issue #317 日志可以看到（已截断）：

```
Failed to open http://fs.youthandroid2.kugou.com/...flac
```

即概念版音源 CDN `fs.youthandroid*.kugou.com`，明文 `http://`（不是 https）。

### VIP 歌曲能不能播

**能，取决于你账号本身的权限，客户端不做任何破解。** 具体规则：

- 音质由 `/privilege/lite` 返回的 `relate_goods` 决定，EchoMusic 按 `level` 选择可用档位（`src/renderer/utils/song.ts` 的 `doesRelateGoodMatchQuality`）。
- 免费试听片段：`song_url.js` 支持 `free_part` 参数 → `IsFreePart: 1`，配合 `ssa_flag: 'is_fromtrack'`。
- **母带（`viper_tape` / level 101）在概念版里拿不到**。issue #392 里作者明确解释：

  > 因为母带要官方充值的会员才能听，领取的会员是不能听的
  > 不是概念版VIP 概念版貌似没有母带

  用户提问「手机 app 充的会员」，作者答复「不是概念版VIP，概念版貌似没有母带」。

- 拿不到音源时界面提示「暂时无法获取可用音源」。
- 每日畅听 VIP 的领取是**玩家自己在手机端酷狗概念版完成**的，EchoMusic 只读取状态（见第 8 节）。

有一个「越权」味道的功能被作者主动删掉了：`/youth/day/vip`（一键领取每日 VIP）接口封装还在 `src/renderer/api/user.ts` 里，但 issue #387 中作者说「自动领会员这个功能我去掉了」，issue #414 里也回复「领取功能已删除，自己想办法或者手动领取吧」。

---

## 7. 许可协议

| 组件 | 协议 | 出处 |
|---|---|---|
| **EchoMusic**（主仓库） | **GPL-3.0-only** | `package.json` 的 `"license": "GPL-3.0-only"`，`LICENSE`（GitHub API 报 `gpl-3.0`） |
| **KuGouMusicApi**（`server/` 子模块） | **MIT** | 上游 `package.json` `"license": "MIT"` |
| 部分第三方 | LGPL-2.1 | 仓库内 `LICENSES/LGPL-2.1.txt`；依赖与授权明细见 `THIRD_PARTY_NOTICES.md` |

### 对你的项目意味着什么

**关键事实：Electron 安装包把 GPL-3.0 的 EchoMusic 前端代码和 MIT 的 KuGouMusicApi 模块一起分发。** `package.json` 的 `extraResources` 明确打包了：

```json
{ "from": "server/module",      "to": "server/module",      "filter": ["**/*.js"] },
{ "from": "server/util",        "to": "server/util",        "filter": ["**/*.js", "**/*.json"] },
{ "from": "server/node_modules","to": "server/node_modules" }
```

**注意事项：**

1. **别抄 EchoMusic 的源码。** 它是 GPL-3.0-only，任何衍生作品都必须以 GPL-3.0 开源，且不能附加额外限制。如果你打算闭源或换别的协议，抄一行代码都是风险。
2. **要抄就抄 KuGouMusicApi（MIT）。** 你需要的所有东西——签名算法、设备注册、KRC 解密、217 个接口实现——都在那边，MIT 协议只要求保留版权声明和许可文本。EchoMusic 相对它只多了三件事：`platform=lite` 的启动方式、设备身份持久化、IPC 替代 Express。这三件事的做法可以独立复现，不必复制代码。
3. **`server/util/crypto.js` 里的 RSA 公钥、AES key 都是从酷狗客户端逆出来的常量。** 这些常量本身不受版权保护（是事实性数据），但**逆向工程行为本身可能违反酷狗的用户协议**，这是法律风险而不是版权风险。EchoMusic 的 README 有免责声明：
   > 本项目是基于公开 API 接口开发的第三方音乐客户端，仅供个人学习和技术研究使用。
   > 所有音乐数据通过公开接口获取，本项目不存储、不传播任何音频文件。
   > **本项目不接受任何商业合作、广告或捐赠。**
4. **上游 KuGouMusicApi 的免责声明更狠**，值得一并读：
   > 使用本项目的过程中可能会产生版权数据……为了避免侵权，使用者务必在 **24 小时内清除**使用本项目的过程中所产生的版权数据。
   > **禁止在违反当地法律法规的情况下使用本项目。**
5. **EchoMusic 作者对「有争议功能」的边界划得很清楚**（issue #387，2026-08-24）：
   > 这个项目的定位是做好一个播放器壳子——界面、体验、扩展系统，这些是我想投入精力的地方。自动领会员这个功能我去掉的原因很简单：不想因为一个有争议的功能给项目和自己带来不必要的风险。
   > 项目的插件系统是开放的，用户可以基于它自由扩展，但请遵守相关法律法规和平台服务条款，自行承担使用风险。本体保持干净，这是我觉得比较合理的边界。

   它把有风险的能力（自动领 VIP、下载）外推给**插件系统**（[`hoowhoami/EchoMusicPlugins`](https://github.com/hoowhoami/EchoMusicPlugins)），这是个值得注意的产品/法务设计模式。

---

## 8. 已知的坑

### 8.1 接口与风控

**① 酷狗的验证码风控是最大摩擦点。** 上游有一个长期开放的 issue #206「关于接口出现 20028 触发验证码临时解决方案」，列出了所有验证类型和 `verifycode` 格式：

| 验证码类型 | verifycode 格式 |
|---|---|
| 极验 (GT) | `KGCodeGT\|{geetest_challenge, geetest_validate, geetest_seccode}` |
| 腾讯 (TX) | `KGCodeTX\|{ticket, randstr, txappid}` |
| 酷狗自研 (KG) | `KGCodeKG\|{orderno}` |
| 酷狗旋转 (KG2) | `KGCodeKG2\|{orderno}` |
| 数美 (SM) | `KGCodeSM\|{ticket}` |
| 易盾 (YD) | `KGCodeYD\|{ticket}` |
| 手机验证码 | `code` 字符串 |

`v_type` 对照：`22` 活体、`23` 滑块、`32` 手机、`34` 人脸、`36` 绑定手机、`38` 登录确认、`51` 账号风控。

上游 issue 里相关讨论（都是 OPEN 状态）：
- #257「关注歌手 /artist/follow 502 失败 error_code 20028，需设备验证」
- #264「绑定设备接口返回了 error_code 20010」
- #217「手机验证码出现 v_type=38」
- #240「关于搜索单曲的 vip 筛选问题」

EchoMusic 侧的具体症状见 issue #298（2026-06-30）：
> 使用 EchoMusic 播放音乐时遭遇官方风控阻断，系统弹出安全验证弹窗，提示"当前需要未知验证，暂不支持自动处理。验证类型：未知验证 / v_type=38"，导致歌曲播放失败，提示"暂时无法获取可用音源"。

作者的应对是 `generate_simulate.js`（伪造 `sid`/`edt` 行为指纹）+ 验证码弹窗 + `v_type=51` 时提示用户去官方客户端申诉。**这条防线是持续对抗，随时可能失效。**

**② 设备注册会失败。** issue #314：Win11 启动卡在「正在注册设备信息」，重装无效；作者的回复是「换个网络注册设备」。上游对应的是 issue #264（`error_code 20010`）。设备注册是一次性但必要的前置步骤，失败则**整个联网功能不可用**（连登录二维码都刷不出来）。

**③ 设备身份要固定，否则登录设备列表会串。** 上游 issue #223：刷新 token 时登录设备不固定，作者回复要用 `.env` 固定 `KUGOU_API_GUID` 和 `KUGOU_API_DEV`。EchoMusic 把这个做成了持久化 + 界面上的「重置设备身份」入口（CHANGELOG 2.3.1）：
> 新增重置设备身份入口，可清除本机 `guid`、`mid`、`dfid` 并在重启后重新生成

### 8.2 网络

**④ 音频 CDN 在特定网络下被重置。** issue #317（校园网/认证网关环境）：
> 播放时连接酷狗音频源（`fs.youthandroid2.kugou.com`）时被对端重置
> 多个 API 接口（`/top/ip`、`/search/lyric`、`/song/climax`）均返回 502

issue #415 / #416 也是同类（内网无法听歌），作者回复：
> 检查自己的网络环境 我也不知道你的内网环境 可以在软件的网络设置里选择强制直连模式试试

issue #402 讨论了代理相关：
> 理解你的疑惑，但应用直连不能绕过系统层面的 TUN、VPN 或 DNS 改动

**⑤ 上游 CDN 是明文 `http://`**（`fs.youthandroid2.kugou.com`），抓包和中间盒都可能干扰。

**⑥ GitHub 相关请求在国内需要加速。** EchoMusic 内置了「GitHub 加速地址」（`src/shared/github-accelerator.ts`），CHANGELOG 2.3.1：
> 优化 GitHub 加速地址回退逻辑，加速站不可用时自动切换原始 GitHub，且两次请求均遵循全局代理规则

### 8.3 其他

**⑦ 上游 `server/` 是 git submodule，必须 `git submodule update --init --recursive`。** 打包时 `electron-builder` 只带 `server/module`、`server/util`、`server/node_modules`，不含 Express 入口（因为 IPC 模式不需要）。

**⑧ 构建门槛不低。** 需要 Node 22.12+、pnpm 9+、Rust stable、C/C++ 工具链 **和 LLVM/libclang**（`bindgen` 要用），5 个 `.node` 产物不入库，每个模块都要单独 `npm install && npm run build`。Windows 还要装 VS Build Tools 的 C++ 工作负载。Linux 要同时装 ALSA、PulseAudio、PipeWire 三套开发库。

**⑨ macOS 用 ad-hoc 签名，自动更新装不了。** README 明说：
> 当前发行版使用 ad-hoc 签名，不支持 Squirrel.Mac 自动安装更新。应用内检查更新后，请下载对应架构的 DMG……再将新版本拖入「应用程序」替换旧版本。

issue #411 就是这个（`Code signature did not pass validation`）。

**⑩ 「概念版」指的是酷狗概念版 App 的接口 + 界面风格，不是破解版。** 首次登录前必须**先在手机端完成酷狗概念版账号的注册与登录**（CHANGELOG 2.3.1 的「优化首次登录提示」）。概念版的 token 和标准版**不通用**（上游 README：`注意不同版本的平台的 token 是不通用的`）。

**⑪ token 不会自动续期。** 见 4.1——上游有 `/v5/login_by_token` 但没有被 EchoMusic 调用，过期只能重新登录。

---

## 附：给「想参考它思路」的落地建议

如果你的目标是做一个同类播放器，按投入产出排序：

1. **直接用 `MakcRe/KuGouMusicApi`（MIT），把它当库用。** 它已经有 217 个接口实现和完整的签名体系，还支持 Express HTTP 和 Docker 两种跑法。EchoMusic 的做法是「去掉 Express，只 `require` module」，如果你不介意多开一个本地 HTTP 服务，直接 `npm run dev` 更省事（默认 `localhost:3000`）。
2. **概念版必须设 `process.env.platform = 'lite'`**，否则 appid/clientver 和签名 salt 全错。
3. **设备身份一定要持久化**（`KUGOU_API_GUID` / `KUGOU_API_MAC` / `KUGOU_API_DEV` / `dfid`），并给用户一个重置入口。不固定会导致登录设备列表越来越长、风控评分上升。
4. **播放器不要自己写**。EchoMusic 在这上面花了 110 万字符的 Rust（FFmpeg 解码 + SoundTouch 变速 + 五平台输出后端 + 系统媒体控制）；如果只需要能播，`<audio>` 标签或一个现成的 WASM 解码器就够了。
5. **不要碰自动领 VIP / 下载这类功能**，把它们留给插件，这是 EchoMusic 作者验证过的边界。
6. **做好验证码弹窗的心理准备**，`20028` / `ssa-code` 是这条路上绕不过去的对手，且没有任何稳定解法。

---

### 参考链接

- EchoMusic：https://github.com/hoowhoami/EchoMusic
- 上游 API 服务：https://github.com/MakcRe/KuGouMusicApi
- 上游验证码方案讨论：https://github.com/MakcRe/KuGouMusicApi/issues/206
- 官方插件市场：https://github.com/hoowhoami/EchoMusicPlugins
- 子模块 pin：`99ca12fb9d464e9cf1e893a4f967a6024885336b`（2026-09-11）
- 同类项目：MoeKoeMusic、SPlayer、KuGouMusicApi
