#pragma once
#include <string>

// 酷狗的 KRC 歌词。
//
// KRC 是酷狗的加密逐字歌词格式，比普通 LRC 多出每个字的精确时间，能做逐字填色。
// 有些歌只有 KRC 版本、没有普通 LRC，所以只请求 LRC 会「拿不到歌词」——这也是
// 之前歌词时不时缺失的原因之一。
//
// 解密流程（参考开源实现 WXRIW/Kugou-Lyrics-Decoder，MIT）：
//   Base64 解码 → 去掉开头 4 字节（"krc1"）→ 逐字节 XOR 固定密钥 → zlib 解压
// 解出来是这种文本：
//   [ti:歌名]
//   [0,2250]<0,160,0>故<160,160,0>事<320,160,0> …
// 其中 [行起始ms,行时长ms] 是行标签，<字相对起始ms,字时长ms,0> 是逐字标签，
// 字的相对时间是相对本行起始的。

namespace kugou
{

// 解密 KRC。输入是歌词接口返回的 Base64 加密内容，失败返回空串。
std::string DecryptKrc(const std::string& base64_content);

// 把解出来的 KRC 文本转成播放器能解析的逐字歌词格式：
//   [mm:ss.xxx]<mm:ss.xxx>字<mm:ss.xxx>字 …
// 这里把逐字的相对时间换算成绝对时间（相对值 + 行起始）。
// 格式不对时返回空串，调用方可以退回普通歌词。
std::wstring KrcToExtendedLyric(const std::string& krc_utf8);

} // namespace kugou
