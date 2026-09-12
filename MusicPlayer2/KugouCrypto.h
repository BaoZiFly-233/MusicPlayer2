#pragma once

#include <string>
#include <vector>

// 酷狗概念版接口用到的加密与设备身份工具。
//
// 这些常量与算法来自公开的第三方实现（MIT 协议的 MakcRe/KuGouMusicApi
// 与 lianchengwu/lmplayer），仅用于个人学习与研究。详见 docs/research/。
//
// 需要注意的几件事：
//   * 概念版和标准版的 salt、appid、clientver 都不能混用，token 也不通用
//   * 设备身份（guid/mid/dfid）必须固定并持久化，否则会被当成新设备，容易触发风控
//   * 播放地址有有效期，不要缓存，每次播放前重新取

namespace kugou
{

// ---- 平台常量（概念版 / Lite）----
constexpr const char* LITE_APPID = "3116";
constexpr const char* LITE_CLIENTVER = "11440";
constexpr const char* DEFAULT_USER_AGENT = "Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi";
constexpr const char* LITE_ANDROID_SALT = "LnT6xpN3khm36zse0QzvmgTZ3waWdRSA";
constexpr const char* LITE_SIGN_KEY_SALT = "185672dd44712f60bb1736df5a377e82";
constexpr const char* DEFAULT_MAC = "02:00:00:00:00:00";

// UTF-8 与宽字符互转（接口都是 UTF-8，程序内部用 wstring）
std::string ToUtf8(const std::wstring& wstr);
std::wstring FromUtf8(const std::string& str);

// 计算字符串的 MD5，返回 32 位小写十六进制
std::string Md5Hex(const std::string& input);

// 生成一个 UUID v4 形式的随机串（用作 guid 的原料）
std::string GenerateUuid();

// 取本机第一个可用网卡的 MAC 地址，取不到时返回默认值
std::string GetLocalMac();

// guid 转 mid：把 MD5(guid) 的十六进制串当作大整数，转成十进制字符串
std::string CalculateMid(const std::string& guid);

// 把十六进制串当作无符号大整数转成十进制串（CalculateMid 的底层实现）
std::string HexToDecimal(const std::string& hex);

// ---- 签名 ----
//
// 概念版的 Android 签名算法：
//   MD5( salt + 按 key 排序后的 "k=v" 拼接 + body + salt )
// 其中 key 用字节序排序，对象类型的值取 JSON 文本。
struct SignParam
{
    std::string key;
    std::string value;
};

// 计算 signature。params 会自动按 key 排序，调用方不必先排。
std::string SignatureAndroid(const std::vector<SignParam>& params, const std::string& body = std::string());

// 计算 v5 取址接口的 key（该接口不签名，但需要 key）
//   MD5( hash + salt + appid + mid + userid )
std::string CalcV5Key(const std::string& hash, const std::string& mid, const std::string& userid);

// URL 编码（酷狗接口的参数需要）
std::string UrlEncode(const std::string& str);

// Base64 解码。接口返回的歌词内容是 Base64 编码的。
// 非法字符会被跳过，解码失败时返回空串。
std::string DecodeBase64(const std::string& encoded);

} // namespace kugou
