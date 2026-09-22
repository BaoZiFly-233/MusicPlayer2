#include "stdafx.h"
#include "KugouKrc.h"
#include "KugouCrypto.h"
#include "nlohmann/json.hpp"
#include <sstream>
#include <string>
#include <vector>

extern "C"
{
#include "puff/puff.h"
}

using namespace std;

namespace kugou
{
namespace
{

// 解密用的固定密钥，来自官方客户端。
// 逐字节循环 XOR，参考开源实现 WXRIW/Kugou-Lyrics-Decoder（MIT）。
const unsigned char KRC_KEY[16] = {
    0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47,
    0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69
};

// 解压 zlib 流。输入要包含完整的 zlib 头（2 字节）和尾部校验（4 字节）。
bool InflateZlib(const vector<unsigned char>& input, string& output)
{
    output.clear();
    if (input.size() < 8) return false;

    // puff 处理的是裸 deflate 数据，zlib 的 2 字节头要跳过
    const unsigned char* source = input.data() + 2;
    const unsigned long source_len = static_cast<unsigned long>(input.size() - 2);

    // 先问一次解压后有多大，避免缓冲区估错
    unsigned long dest_len = 0;
    unsigned long probe_len = source_len;
    if (puff(nullptr, &dest_len, source, &probe_len) != 0 || dest_len == 0)
        return false;

    vector<unsigned char> buffer(dest_len);
    unsigned long out_len = dest_len;
    unsigned long in_len = source_len;
    if (puff(buffer.data(), &out_len, source, &in_len) != 0)
        return false;

    output.assign(buffer.begin(), buffer.begin() + static_cast<size_t>(out_len));
    return true;
}

// 毫秒转 [mm:ss.xxx] 里的时间部分
wstring FormatTime(int ms)
{
    if (ms < 0) ms = 0;
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return buffer;
}

// 从 text 的 pos 处读一个十进制整数。读不到返回 false。
bool ReadInt(const wstring& text, size_t& pos, int& value)
{
    while (pos < text.size() && (text[pos] == L' ' || text[pos] == L'\t')) ++pos;
    const size_t start = pos;
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') ++pos;
    if (pos == start) return false;
    value = _wtoi(text.substr(start, pos - start).c_str());
    return true;
}

// 解析行标签 [起始,时长]。成功时 pos 停在 ']' 之后。
bool ParseLineTag(const wstring& line, size_t& pos, int& start_ms, int& span_ms)
{
    pos = 0;
    if (pos >= line.size() || line[pos] != L'[') return false;
    ++pos;
    if (!ReadInt(line, pos, start_ms)) return false;
    if (pos >= line.size() || line[pos] != L',') return false;
    ++pos;
    if (!ReadInt(line, pos, span_ms)) return false;
    if (pos >= line.size() || line[pos] != L']') return false;
    ++pos;
    return true;
}

// krc 把逐字歌词之外的附加文本放在 [language:<base64>] 里，解出来是
//   {"content":[{"type":0,"lyricContent":[...]},{"type":1,"lyricContent":[...]}]}
// type 0 是罗马音，type 1 是中文翻译。每个 lyricContent 是一行一个词数组，
// 行序与正文字数行一一对应 —— 中文歌没有翻译时这里是空的，正好自然跳过。
vector<wstring> ParseKrcTranslations(const wstring& krc)
{
    vector<wstring> translations;
    const wstring marker = L"[language:";
    const size_t begin = krc.find(marker);
    if (begin == wstring::npos) return translations;
    const size_t end = krc.find(L']', begin);
    if (end == wstring::npos || end <= begin + marker.size()) return translations;

    const wstring encoded = krc.substr(begin + marker.size(), end - begin - marker.size());
    const string decoded = DecodeBase64(ToUtf8(encoded));
    if (decoded.empty()) return translations;

    nlohmann::json doc;
    try { doc = nlohmann::json::parse(decoded); }
    catch (const nlohmann::json::exception&) { return translations; }
    if (!doc.contains("content") || !doc["content"].is_array()) return translations;

    for (const auto& item : doc["content"])
    {
        // 只要翻译那一份；罗马音对中文用户没什么用
        if (!item.contains("type") || !item["type"].is_number() || item["type"].get<int>() != 1) continue;
        if (!item.contains("lyricContent") || !item["lyricContent"].is_array()) continue;
        for (const auto& row : item["lyricContent"])
        {
            string line;
            if (row.is_array())
            {
                for (const auto& word : row)
                    if (word.is_string()) line += word.get<string>();
            }
            else if (row.is_string())
            {
                line = row.get<string>();
            }
            translations.push_back(FromUtf8(line));
        }
        break;
    }
    return translations;
}

} // namespace

string DecryptKrc(const string& base64_content)
{
    if (base64_content.empty()) return string();

    string raw = DecodeBase64(base64_content);
    // 开头 4 字节是 "krc1" 标识
    if (raw.size() <= 4) return string();

    vector<unsigned char> data(raw.begin() + 4, raw.end());
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<unsigned char>(data[i] ^ KRC_KEY[i % sizeof(KRC_KEY)]);

    string result;
    if (!InflateZlib(data, result)) return string();
    return result;
}

wstring KrcToExtendedLyric(const string& krc_utf8)
{
    if (krc_utf8.empty()) return wstring();

    const wstring krc = FromUtf8(krc_utf8);
    // 外文歌的翻译藏在这个块里，按行与正文对齐
    const vector<wstring> translations = ParseKrcTranslations(krc);
    std::wostringstream out;
    int converted_lines = 0;

    size_t line_start = 0;
    while (line_start < krc.size())
    {
        size_t line_end = krc.find(L'\n', line_start);
        wstring line = krc.substr(line_start,
            line_end == wstring::npos ? wstring::npos : line_end - line_start);
        line_start = (line_end == wstring::npos) ? krc.size() : line_end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;

        // 只处理歌词行：以 [数字 开头的才是，[ti:xxx] 这类元数据跳过
        size_t pos = 0;
        int line_start_ms = 0, line_span_ms = 0;
        if (!ParseLineTag(line, pos, line_start_ms, line_span_ms)) continue;

        // 逐字标签 <相对起始ms,字时长ms,0>文字 …
        wstring converted_words;
        int word_count = 0;
        int last_word_end = -1;     // 上一个字的结束时间（绝对 ms）
        while (pos < line.size() && line[pos] == L'<')
        {
            const size_t tag_begin = pos;
            ++pos;
            int word_offset = 0, word_span = 0, reserved = 0;
            if (!ReadInt(line, pos, word_offset)) break;
            if (pos >= line.size() || line[pos] != L',') break;
            ++pos;
            if (!ReadInt(line, pos, word_span)) break;
            if (pos < line.size() && line[pos] == L',')
            {
                ++pos;
                ReadInt(line, pos, reserved);       // 第三个字段暂时没用，读出即可
            }
            if (pos >= line.size() || line[pos] != L'>') { pos = tag_begin; break; }
            ++pos;

            // 标签后面到下一个 '<' 之间就是这段的字
            const size_t text_begin = pos;
            while (pos < line.size() && line[pos] != L'<') ++pos;
            const wstring word = line.substr(text_begin, pos - text_begin);

            // KRC 给的是相对本行起始的时间，扩展 LRC 要绝对时间。
            // 和 B 源的 LRCX 转换同口径：字与字之间有停顿就补结束标签，
            // 让卡拉OK填色停在字尾，而不是把字间隙一起填满。
            const int word_start = line_start_ms + word_offset;
            int word_end = word_start + word_span;
            if (word_end < word_start) word_end = word_start;
            if (last_word_end >= 0 && word_start > last_word_end)
                converted_words += L"<" + FormatTime(last_word_end) + L">";
            converted_words += L"<" + FormatTime(word_start) + L">" + word;
            last_word_end = word_end;
            ++word_count;
        }

        // 句尾补一个结束标签：最后一个字唱完到行尾之间的留白也要让填色停住
        if (last_word_end >= 0)
        {
            int line_end = line_start_ms + line_span_ms;
            if (line_end < last_word_end) line_end = last_word_end;
            converted_words += L"<" + FormatTime(line_end) + L">";
        }

        if (word_count == 0) continue;

        out << L"[" << FormatTime(line_start_ms) << L"]" << converted_words << L"\n";
        // 紧跟一行同时间戳的翻译。播放器的 CLyrics 会把「时间戳相同的两行」
        // 认成原文加译文（前原文、后译文），这样双语就出来了。
        if (converted_lines < static_cast<int>(translations.size()) && !translations[converted_lines].empty())
            out << L"[" << FormatTime(line_start_ms) << L"]" << translations[converted_lines] << L"\n";
        ++converted_lines;
    }

    // 一行都没转出来说明不是预期的格式，让调用方退回普通歌词
    if (converted_lines == 0) return wstring();
    return out.str();
}

} // namespace kugou
