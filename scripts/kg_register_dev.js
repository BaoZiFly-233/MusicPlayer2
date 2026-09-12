/**
 * 酷狗（概念版）设备注册 —— 取 dfid
 * 接口: POST https://userservice.kugou.com/risk/v2/r_register_dev
 *
 * 完整复刻 MakcRe/KuGouMusicApi 的 module/register_dev.js + util/crypto.js + util/helper.js
 * 零第三方依赖，只用 node 内置 crypto（Node 18+）。
 *
 * 运行:  node kg_register_dev.js
 */

const crypto = require('node:crypto');

// ───────────── 概念版（lite）常量 ─────────────
const LITE_PUBLIC_KEY = `-----BEGIN PUBLIC KEY-----
MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDECi0Np2UR87scwrvTr72L6oO01rBbbBPriSDFPxr3Z5syug0O24QyQO8bg27+0+4kBzTBTBOZ/WWU0WryL1JSXRTXLgFVxtzIY41Pe7lPOgsfTCn5kZcvKhYKJesKnnJDNr5/abvTGf+rHG3YRwsCHcQ08/q6ifSioBszvb3QiwIDAQAB
-----END PUBLIC KEY-----`;

const CONCEPT = {
  endpoint: 'https://userservice.kugou.com/risk/v2/r_register_dev',
  appid: 3116,
  clientver: 11440,
  signSalt: 'LnT6xpN3khm36zse0QzvmgTZ3waWdRSA',  // 概念版 android 签名盐
  userAgent: 'Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi',
};

// ───────────── 基础工具 ─────────────
const md5hex = (s) => crypto.createHash('md5').update(s, 'utf8').digest('hex');

/** 上游 util/util.js randomString：字符集 0-9 + A-Z，取 6 位后转小写 */
function randomAesKey() {
  const chars = '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ';
  let out = '';
  for (let i = 0; i < 6; i++) out += chars[crypto.randomInt(chars.length)];
  return out.toLowerCase();
}

/** 上游 util/util.js calculateMid：MD5(guid) 视作十六进制大整数，转十进制字符串 */
const calculateMid = (guid) => BigInt('0x' + md5hex(guid)).toString(10);

// ───────────── 加密 ─────────────
/**
 * 上游 crypto.js playlistAesEncrypt
 * key = MD5(k)[0:16]、iv = MD5(k)[16:32]（当 ASCII 字符串用），AES-128-CBC + PKCS7，输出 Base64
 */
function playlistAesEncrypt(data, key) {
  const cipher = crypto.createCipheriv(
    'aes-128-cbc',
    Buffer.from(md5hex(key).substring(0, 16), 'utf8'),
    Buffer.from(md5hex(key).substring(16, 32), 'utf8'),
  );
  return Buffer.concat([cipher.update(JSON.stringify(data), 'utf8'), cipher.final()]).toString('base64');
}

/** 上游 crypto.js playlistAesDecrypt（响应体解密） */
function playlistAesDecrypt(base64Cipher, key) {
  const decipher = crypto.createDecipheriv(
    'aes-128-cbc',
    Buffer.from(md5hex(key).substring(0, 16), 'utf8'),
    Buffer.from(md5hex(key).substring(16, 32), 'utf8'),
  );
  return Buffer.concat([
    decipher.update(Buffer.from(base64Cipher, 'base64')),
    decipher.final(),
  ]).toString('utf8');
}

/**
 * 上游 crypto.js rsaEncrypt2 —— 标准 PKCS#1 v1.5 加密，输出小写 hex
 * 注意：register_dev 用的就是这个；cryptoRSAEncrypt（裸 RSA、无填充）是登录接口 /v2/get_dev 用的，别搞混。
 */
function rsaEncrypt2(data, publicKey = LITE_PUBLIC_KEY) {
  return crypto.publicEncrypt(
    { key: publicKey, padding: crypto.constants.RSA_PKCS1_PADDING },
    Buffer.from(JSON.stringify(data), 'utf8'),
  ).toString('hex');
}

/** 上游 util/helper.js signatureAndroidParams（概念版盐） */
function signatureAndroidParams(params, body) {
  const { signSalt } = CONCEPT;
  const paramsString = Object.keys(params)
    .sort()
    .map((k) => `${k}=${typeof params[k] === 'object' ? JSON.stringify(params[k]) : params[k]}`)
    .join('');
  return md5hex(`${signSalt}${paramsString}${body || ''}${signSalt}`);
}

// ───────────── 设备档案 ─────────────
function buildDeviceProfile(guid) {
  return {
    availableRamSize: 4983533568, availableRomSize: 48114719, availableSDSize: 48114717,
    basebandVer: '', batteryLevel: 100, batteryStatus: 3,
    brand: 'Redmi', buildSerial: 'unknown', device: 'marble',
    imei: guid, imsi: '', manufacturer: 'Xiaomi', uuid: guid,
    accelerometer: false, accelerometerValue: '', gravity: false, gravityValue: '',
    gyroscope: false, gyroscopeValue: '', light: false, lightValue: '',
    magnetic: false, magneticValue: '', orientation: false, orientationValue: '',
    pressure: false, pressureValue: '', step_counter: false, step_counterValue: '',
    temperature: false, temperatureValue: '',
  };
}

/** 本地自检：确认公钥规格与 PKCS#1 v1.5 块结构，避免拿错 key/填充 */
function selfCheck() {
  const key = crypto.createPublicKey(LITE_PUBLIC_KEY);
  const bits = key.asymmetricKeyDetails.modulusLength;
  if (bits !== 1024) throw new Error(`公钥位数不对: ${bits}（应为 1024）`);
  if (key.asymmetricKeyDetails.publicExponent !== 65537n) throw new Error('公钥 e 不是 65537');

  const { privateKey, publicKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 1024 });
  const probe = '{"aes":"abcdef","uid":0,"token":""}';
  const ct = crypto.publicEncrypt({ key: publicKey, padding: crypto.constants.RSA_PKCS1_PADDING }, Buffer.from(probe));
  const em = crypto.privateDecrypt({ key: privateKey, padding: crypto.constants.RSA_NO_PADDING }, ct);
  const zero = em.indexOf(0x00, 2);
  const ok = em[0] === 0x00 && em[1] === 0x02                          // 加密块类型是 02
    && em.subarray(2, zero).every((b) => b !== 0)                       // PS 非零
    && em[zero] === 0x00                                                // 分隔符
    && em.subarray(zero + 1).toString('utf8') === probe;
  if (!ok) throw new Error('PKCS#1 v1.5 块结构自检失败');
  return { bits, maxPlainBytes: 128 - 11, em };
}

// ───────────── 主流程 ─────────────
/**
 * 注册设备，返回 { dfid, guid, mid, ... }
 * @param {{guid?:string, userid?:number, token?:string, verbose?:boolean}} opt
 */
async function registerDevice(opt = {}) {
  const { guid = crypto.randomUUID(), userid = 0, token = '', verbose = true } = opt;
  const log = verbose ? console.log : () => {};

  const mid = calculateMid(guid);
  const clienttime = Math.floor(Date.now() / 1000);

  // 1) 设备档案 → AES-128-CBC → Base64，作为请求体
  const aesKey = randomAesKey();
  const body = playlistAesEncrypt(buildDeviceProfile(guid), aesKey);

  // 2) p = RSA_PKCS1({ aes, uid, token })，uid 必须是数字、token 必须存在
  const pPlain = { aes: aesKey, uid: Number(userid) || 0, token: token || '' };
  const pJson = JSON.stringify(pPlain);
  const pBytes = Buffer.byteLength(pJson);
  if (pBytes > 117) throw new Error(`p 明文过长: ${pBytes}B（PKCS#1 v1.5 上限 117B）`);
  const p = rsaEncrypt2(pPlain);

  // 3) 查询参数 + 签名（body 原样那串 Base64 必须参与签名）
  const params = {
    dfid: '-', mid, uuid: '-',
    appid: CONCEPT.appid, clientver: CONCEPT.clientver, clienttime,
    part: 1, platid: 1, p,
  };
  params.signature = signatureAndroidParams(params, body);

  log('guid       :', guid);
  log('mid        :', mid);
  log('aesKey     :', aesKey);
  log('p 明文     :', pJson, `(${pBytes}B)`);
  log('p hex      :', p.length, '字符（必须 256）');
  log('body       :', body.length, '字符 base64');
  log('signature  :', params.signature);

  // 4) 发送：p 拼在 query 上，AES 密文 Base64 字符串作为 body
  const query = new URLSearchParams(Object.entries(params).map(([k, v]) => [k, String(v)]));
  const res = await fetch(`${CONCEPT.endpoint}?${query}`, {
    method: 'POST',
    headers: {
      'User-Agent': CONCEPT.userAgent,
      'Content-Type': 'text/plain;charset=UTF-8',
      dfid: '-', mid, clienttime: String(clienttime),
      'kg-rc': '1', 'kg-thash': '5d816a0', 'kg-rec': '1',
      'kg-rf': 'B9EDA08A64250DEFFBCADDEE00F8F25F',
    },
    body,
    signal: AbortSignal.timeout(20000),
  });

  // 5) 响应用同一把 aesKey 解出来
  const bytes = Buffer.from(await res.arrayBuffer());
  let text;
  try {
    text = playlistAesDecrypt(bytes.toString('base64'), aesKey);
  } catch {
    text = bytes.toString('utf8');
  }
  let parsed = null;
  try { parsed = JSON.parse(text); } catch { /* 非 JSON */ }

  const dfid = typeof parsed?.data?.dfid === 'string' ? parsed.data.dfid : null;
  return {
    ok: parsed?.status === 1 && !!dfid,
    dfid, guid, mid,
    httpStatus: res.status,
    ssaCode: res.headers.get('ssa-code') || null,
    body: parsed ?? text,
  };
}

module.exports = {
  registerDevice, calculateMid, randomAesKey,
  playlistAesEncrypt, playlistAesDecrypt, rsaEncrypt2, signatureAndroidParams,
  buildDeviceProfile, selfCheck, LITE_PUBLIC_KEY, CONCEPT,
};

// ───────────── 直接运行 ─────────────
if (require.main === module) {
  (async () => {
    const info = selfCheck();
    console.log(`自检通过：RSA-${info.bits}，e=65537，PKCS#1 v1.5 明文上限 ${info.maxPlainBytes}B`);
    console.log(`采样 EM 块结构：${info.em.subarray(0, 2).toString('hex')} | PS(${info.em.indexOf(0x00, 2) - 2}B) | 00 | 载荷\n`);

    const r = await registerDevice();
    console.log('\n响应:', JSON.stringify(r.body));
    if (r.ok) {
      console.log(`\n✅ dfid = ${r.dfid}`);
      console.log(`   后续请求带上:  Cookie: dfid=${r.dfid}; KUGOU_API_MID=${r.mid}; KUGOU_API_GUID=${r.guid}`);
    } else {
      console.log('\n❌ 未取得 dfid');
      process.exitCode = 1;
    }
  })();
}
