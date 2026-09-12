#include "stdafx.h"
#include "KugouCrypto.h"
#include "md5.h"
#include <random>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <wincrypt.h>

using namespace std;

namespace kugou
{

// Web 版签名用的 salt，登录相关接口（获取二维码等）用它
static const char* WEB_SALT = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";

string ToUtf8(const wstring& wstr)
{
    if (wstr.empty())
        return string();

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return string();

    string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()),
        &result[0], len, nullptr, nullptr);
    return result;
}

wstring FromUtf8(const string& str)
{
    if (str.empty())
        return wstring();

    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0)
        return wstring();

    wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &result[0], len);
    return result;
}

string Md5Hex(const string& input)
{
    MD5 md5;
    md5.Update(input);
    md5.Finalize();
    return md5.HexDigest();
}

string GenerateUuid()
{
    // UUID v4：xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    static thread_local std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(0, 15);
    std::uniform_int_distribution<int> dist2(8, 11);

    const char* hex = "0123456789abcdef";
    string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 36; ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            uuid.push_back('-');
        else if (i == 14)
            uuid.push_back('4');
        else if (i == 19)
            uuid.push_back(hex[dist2(rng)]);
        else
            uuid.push_back(hex[dist(rng)]);
    }
    return uuid;
}

string GetLocalMac()
{
    PIP_ADAPTER_INFO adapter_info = nullptr;
    ULONG buffer_len = 0;

    if (GetAdaptersInfo(nullptr, &buffer_len) != ERROR_BUFFER_OVERFLOW || buffer_len == 0)
        return DEFAULT_MAC;

    vector<BYTE> buffer(buffer_len);
    adapter_info = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());

    if (GetAdaptersInfo(adapter_info, &buffer_len) != ERROR_SUCCESS)
        return DEFAULT_MAC;

    for (PIP_ADAPTER_INFO adapter = adapter_info; adapter != nullptr; adapter = adapter->Next)
    {
        // 跳过全零地址
        bool all_zero = true;
        for (UINT i = 0; i < adapter->AddressLength; ++i)
        {
            if (adapter->Address[i] != 0)
            {
                all_zero = false;
                break;
            }
        }
        if (all_zero || adapter->AddressLength < 6)
            continue;

        char mac[32]{};
        sprintf_s(mac, "%02X:%02X:%02X:%02X:%02X:%02X",
            adapter->Address[0], adapter->Address[1], adapter->Address[2],
            adapter->Address[3], adapter->Address[4], adapter->Address[5]);
        return string(mac);
    }

    return DEFAULT_MAC;
}

string HexToDecimal(const string& hex)
{
    if (hex.empty())
        return "0";

    // 把十六进制串当作大整数，用「十进制字节数组」逐位乘 16 再进位的方式转换。
    // 这样不必引入大整数库，几百位也没问题。
    vector<BYTE> decimal;       // 十进制各位，低位在前
    decimal.push_back(0);

    for (char ch : hex)
    {
        int value = 0;
        if (ch >= '0' && ch <= '9')
            value = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            value = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            value = ch - 'A' + 10;
        else
            continue;           // 非法字符直接跳过

        int carry = value;
        for (size_t i = 0; i < decimal.size(); ++i)
        {
            int cur = decimal[i] * 16 + carry;
            decimal[i] = static_cast<BYTE>(cur % 10);
            carry = cur / 10;
        }
        while (carry > 0)
        {
            decimal.push_back(static_cast<BYTE>(carry % 10));
            carry /= 10;
        }
    }

    // 去掉前导零
    size_t last = decimal.size();
    while (last > 1 && decimal[last - 1] == 0)
        --last;

    string result;
    result.reserve(last);
    for (size_t i = last; i-- > 0;)
        result.push_back(static_cast<char>('0' + decimal[i]));

    return result;
}

string CalculateMid(const string& guid)
{
    return HexToDecimal(Md5Hex(guid));
}

// 签名算法：把 salt 前后各夹一次，中间放「按 key 排序后的 k=v 拼接」和请求体。
// Android 版和 Web 版只差一个 salt，所以共用这里的实现。
static string SignatureWithSalt(const vector<SignParam>& params, const string& body, const char* salt)
{
    // 按 key 的字节序排序
    vector<SignParam> sorted = params;
    sort(sorted.begin(), sorted.end(), [](const SignParam& a, const SignParam& b) {
        return a.key < b.key;
    });

    string joined;
    for (const SignParam& p : sorted)
    {
        joined += p.key;
        joined += '=';
        joined += p.value;
    }

    string raw;
    raw.reserve(joined.size() + body.size() + 64);
    raw += salt;
    raw += joined;
    raw += body;
    raw += salt;

    return Md5Hex(raw);
}

string SignatureAndroid(const vector<SignParam>& params, const string& body)
{
    return SignatureWithSalt(params, body, LITE_ANDROID_SALT);
}

string SignatureWeb(const vector<SignParam>& params)
{
    return SignatureWithSalt(params, string(), WEB_SALT);
}

string CalcV5Key(const string& hash, const string& mid, const string& userid)
{
    string raw = hash;
    raw += LITE_SIGN_KEY_SALT;
    raw += LITE_APPID;
    raw += mid;
    raw += userid.empty() ? "0" : userid;
    return Md5Hex(raw);
}

string UrlEncode(const string& str)
{
    static const char* hex = "0123456789ABCDEF";
    string result;
    result.reserve(str.size() * 3);

    for (unsigned char ch : str)
    {
        // 字母、数字和 -_.~ 不编码，其余按 %XX 处理
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            result.push_back(static_cast<char>(ch));
        }
        else
        {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0F]);
        }
    }
    return result;
}

string DecodeBase64(const string& encoded)
{
    static const signed char table[256] = {
        // 用一个查表把 Base64 字符映射成 0~63，-1 表示非法字符
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    string result;
    result.reserve(encoded.size() * 3 / 4 + 3);

    int buffer = 0;
    int bits = 0;
    for (unsigned char ch : encoded)
    {
        if (ch == '=')              // 填充，结束
            break;
        signed char value = table[ch];
        if (value < 0)              // 跳过换行、空格等非法字符
            continue;

        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }

    return result;
}

string EncodeBase64(const string& raw)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string result;
    result.reserve((raw.size() + 2) / 3 * 4);

    size_t i = 0;
    while (i + 2 < raw.size())
    {
        unsigned int v = (static_cast<unsigned char>(raw[i]) << 16)
            | (static_cast<unsigned char>(raw[i + 1]) << 8)
            | static_cast<unsigned char>(raw[i + 2]);
        result.push_back(table[(v >> 18) & 0x3F]);
        result.push_back(table[(v >> 12) & 0x3F]);
        result.push_back(table[(v >> 6) & 0x3F]);
        result.push_back(table[v & 0x3F]);
        i += 3;
    }

    // 处理尾巴，不足的用 = 补齐
    const size_t remain = raw.size() - i;
    if (remain == 1)
    {
        unsigned int v = static_cast<unsigned char>(raw[i]) << 16;
        result.push_back(table[(v >> 18) & 0x3F]);
        result.push_back(table[(v >> 12) & 0x3F]);
        result.push_back('=');
        result.push_back('=');
    }
    else if (remain == 2)
    {
        unsigned int v = (static_cast<unsigned char>(raw[i]) << 16)
            | (static_cast<unsigned char>(raw[i + 1]) << 8);
        result.push_back(table[(v >> 18) & 0x3F]);
        result.push_back(table[(v >> 12) & 0x3F]);
        result.push_back(table[(v >> 6) & 0x3F]);
        result.push_back('=');
    }

    return result;
}

string RandomKey6()
{
    // 只取小写字母，和官方实现一致
    static thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(0, 25);
    string key;
    key.reserve(6);
    for (int i = 0; i < 6; ++i)
        key.push_back(static_cast<char>('a' + dist(rng)));
    return key;
}

// key 和 IV 都来自 MD5(key6) 的十六进制字符串：前 16 字符作 key，后 16 字符作 IV
static void DeriveRegisterKeyIv(const string& key6, string& key, string& iv)
{
    string h = Md5Hex(key6);
    key = h.substr(0, 16);
    iv = h.substr(16, 16);
}

string AesEncryptForRegister(const string& plain, const string& key6)
{
    string key, iv;
    DeriveRegisterKeyIv(key6, key, iv);

    // CryptoAPI 的 AES 默认用 PKCS7 补位，和 CryptoJS 的 Pkcs7 一致
    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    string result;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return result;

    do
    {
        struct AesBlob
        {
            BLOBHEADER header;
            DWORD key_size;
            BYTE key_data[16];
        } blob{};
        blob.header.bType = PLAINTEXTKEYBLOB;
        blob.header.bVersion = CUR_BLOB_VERSION;
        blob.header.reserved = 0;
        blob.header.aiKeyAlg = CALG_AES_128;
        blob.key_size = 16;
        memcpy(blob.key_data, key.data(), 16);

        if (!CryptImportKey(hProv, reinterpret_cast<BYTE*>(&blob), sizeof(blob), 0, 0, &hKey))
            break;

        // 设置 CBC 模式和 IV
        DWORD mode = CRYPT_MODE_CBC;
        if (!CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&mode), 0))
            break;
        if (!CryptSetKeyParam(hKey, KP_IV, reinterpret_cast<const BYTE*>(iv.data()), 0))
            break;

        DWORD len = static_cast<DWORD>(plain.size());
        vector<BYTE> buf(plain.begin(), plain.end());
        buf.resize(len + 32);       // 留出补位空间

        if (!CryptEncrypt(hKey, 0, TRUE, 0, buf.data(), &len, static_cast<DWORD>(buf.size())))
            break;

        result.assign(reinterpret_cast<char*>(buf.data()), len);
    } while (false);

    if (hKey != 0)
        CryptDestroyKey(hKey);
    if (hProv != 0)
        CryptReleaseContext(hProv, 0);

    return result;
}

string AesDecryptForRegister(const string& cipher_base64, const string& key6)
{
    string cipher = DecodeBase64(cipher_base64);
    if (cipher.empty())
        return string();

    string key, iv;
    DeriveRegisterKeyIv(key6, key, iv);

    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    string result;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return result;

    do
    {
        struct AesBlob
        {
            BLOBHEADER header;
            DWORD key_size;
            BYTE key_data[16];
        } blob{};
        blob.header.bType = PLAINTEXTKEYBLOB;
        blob.header.bVersion = CUR_BLOB_VERSION;
        blob.header.reserved = 0;
        blob.header.aiKeyAlg = CALG_AES_128;
        blob.key_size = 16;
        memcpy(blob.key_data, key.data(), 16);

        if (!CryptImportKey(hProv, reinterpret_cast<BYTE*>(&blob), sizeof(blob), 0, 0, &hKey))
            break;

        DWORD mode = CRYPT_MODE_CBC;
        if (!CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&mode), 0))
            break;
        if (!CryptSetKeyParam(hKey, KP_IV, reinterpret_cast<const BYTE*>(iv.data()), 0))
            break;

        vector<BYTE> buf(cipher.begin(), cipher.end());
        DWORD len = static_cast<DWORD>(buf.size());
        if (!CryptDecrypt(hKey, 0, TRUE, 0, buf.data(), &len))
            break;

        result.assign(reinterpret_cast<char*>(buf.data()), len);
    } while (false);

    if (hKey != 0)
        CryptDestroyKey(hKey);
    if (hProv != 0)
        CryptReleaseContext(hProv, 0);

    return result;
}

string RsaEncryptPkcs1(const string& plain, const string& public_key_base64)
{
    string key_bytes = DecodeBase64(public_key_base64);
    if (key_bytes.empty())
        return string();

    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    CERT_PUBLIC_KEY_INFO* pub_info = nullptr;
    string result;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return result;

    do
    {
        // Base64 -> DER
        DWORD der_len = 0;
        if (!CryptStringToBinaryA(public_key_base64.c_str(), static_cast<DWORD>(public_key_base64.size()),
            CRYPT_STRING_BASE64, nullptr, &der_len, nullptr, nullptr))
            break;

        vector<BYTE> der(der_len);
        if (!CryptStringToBinaryA(public_key_base64.c_str(), static_cast<DWORD>(public_key_base64.size()),
            CRYPT_STRING_BASE64, der.data(), &der_len, nullptr, nullptr))
            break;
        der.resize(der_len);

        // DER -> CERT_PUBLIC_KEY_INFO 结构
        DWORD info_len = 0;
        if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
            der.data(), der_len, CRYPT_DECODE_ALLOC_FLAG, nullptr, &pub_info, &info_len))
            break;

        // 用经典 API 导入，得到 HCRYPTKEY（新版的 Ex2 返回 CNG 句柄，CryptEncrypt 用不了）
        if (!CryptImportPublicKeyInfo(hProv, X509_ASN_ENCODING, pub_info, &hKey))
            break;

        DWORD len = static_cast<DWORD>(plain.size());
        vector<BYTE> buf(plain.begin(), plain.end());
        buf.resize(len + 256);

        if (!CryptEncrypt(hKey, 0, TRUE, 0, buf.data(), &len, static_cast<DWORD>(buf.size())))
            break;

        // 输出十六进制小写
        static const char* hex = "0123456789abcdef";
        result.reserve(len * 2);
        for (DWORD i = 0; i < len; ++i)
        {
            result.push_back(hex[buf[i] >> 4]);
            result.push_back(hex[buf[i] & 0x0F]);
        }
    } while (false);

    if (pub_info != nullptr)
        LocalFree(pub_info);
    if (hKey != 0)
        CryptDestroyKey(hKey);
    if (hProv != 0)
        CryptReleaseContext(hProv, 0);

    return result;
}

} // namespace kugou
