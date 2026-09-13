#pragma once
#include "KugouCrypto.h"
#include <winhttp.h>
#include <memory>
#pragma comment(lib, "winhttp.lib")

namespace online
{
// 在线接口携带账号凭据，不经过会记录完整 URL 的通用下载日志。
// 按原始字节收齐响应后统一解码，避免分块切断 UTF-8 汉字。
inline bool HttpRequest(const std::wstring& url, const std::string& body, const std::wstring& headers,
    std::wstring& result, std::wstring& error, const wchar_t* method = nullptr)
{
    result.clear();
    URL_COMPONENTS parts{sizeof(URL_COMPONENTS)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    // WinINet 与 WinHTTP 的同名 scheme 枚举值不同，MFC 预编译头会先引入前者。
    if (url.compare(0, 8, L"https://") != 0 || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        error = L"在线服务地址无效"; return false;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    auto close = [](void* handle) { if (handle) WinHttpCloseHandle(handle); };
    using Handle = std::unique_ptr<void, decltype(close)>;
    Handle session(WinHttpOpen(L"MusicPlayer2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), close);
    if (!session) { error = L"无法初始化网络连接"; return false; }
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 15000);
    Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), close);
    Handle request(connection ? WinHttpOpenRequest(connection.get(), method ? method : body.empty() ? L"GET" : L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr, close);
    if (!request || !WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
        || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        error = L"网络连接失败或超时，请稍后重试"; return false;
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
    {
        error = L"服务暂不可用（HTTP " + std::to_wstring(status) + L"）"; return false;
    }
    std::string bytes;
    char buffer[16384];
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read))
        {
            error = L"网络响应未完整接收，请重试"; return false;
        }
        if (read == 0) break;
        if (bytes.size() + read > 8 * 1024 * 1024)
        {
            error = L"接口响应超出大小限制"; return false;
        }
        bytes.append(buffer, read);
    }
    result = kugou::FromUtf8(bytes);
    if (result.empty()) { error = L"服务返回了空响应"; return false; }
    return true;
}
}
