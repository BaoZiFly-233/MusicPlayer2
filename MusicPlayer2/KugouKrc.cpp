#include "stdafx.h"
#include "KugouKrc.h"
#include "KugouCrypto.h"
#include <sstream>
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

            // KRC 给的是相对本行起始的时间，扩展 LRC 要绝对时间
            converted_words += L"<" + FormatTime(line_start_ms + word_offset) + L">" + word;
            ++word_count;
        }

        if (word_count == 0) continue;

        out << L"[" << FormatTime(line_start_ms) << L"]" << converted_words << L"\n";
        ++converted_lines;
    }

    // 一行都没转出来说明不是预期的格式，让调用方退回普通歌词
    if (converted_lines == 0) return wstring();
    return out.str();
}

} // namespace kugou
