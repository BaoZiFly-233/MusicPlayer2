#pragma once
#include "KugouCrypto.h"
#include <winhttp.h>
#include <atomic>
#include <memory>
#pragma comment(lib, "winhttp.lib")

namespace online
{
// 请求撤销标记的存放处。用内联函数里的 thread_local 而不是全局变量，
// 这样每个线程各有一份、所有翻译单元又共用同一个定义（内联函数的静态局部变量）。
inline const std::atomic<bool>*& RequestCancelSlot()
{
    static thread_local const std::atomic<bool>* slot = nullptr;
    return slot;
}

// 后台工作线程在跑自己的任务前把取消标志放到这个槽里，收发过程中检查；
// 这样用户换了关键词或切了页面时，上一个还在飞的请求会尽早结束，
// 新的搜索不必排在它后面等超时。窗口线程和绘制线程不设，保持空，
// 不会误伤界面上的同步请求。
class RequestCancelScope
{
public:
    explicit RequestCancelScope(const std::atomic<bool>* flag) : m_previous(RequestCancelSlot()) { RequestCancelSlot() = flag; }
    ~RequestCancelScope() { RequestCancelSlot() = m_previous; }
    RequestCancelScope(const RequestCancelScope&) = delete;
    RequestCancelScope& operator=(const RequestCancelScope&) = delete;
private:
    const std::atomic<bool>* m_previous;
};

inline bool RequestCancelled()
{
    const std::atomic<bool>* flag = RequestCancelSlot();
    return flag != nullptr && flag->load();
}

// 在线接口携带账号凭据，不经过会记录完整 URL 的通用下载日志。
// 按原始字节收齐响应后统一解码，避免分块切断 UTF-8 汉字。
// retryable 告诉调用方这次失败值不值得重发：网络层的偶发问题值得，
// 地址不对、响应超限、服务端明确拒绝这些再发一次也是同样的结果。
inline bool HttpRequestOnce(const std::wstring& url, const std::string& body, const std::wstring& headers,
    std::wstring& result, std::wstring& error, const wchar_t* method, bool& retryable)
{
    retryable = false;
    result.clear();
    if (RequestCancelled()) { error = L"请求已取消"; return false; }
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
    // 解析超时给到 15 秒：域名解析慢到十几秒的网络（实测某校园网 DNS 冷查询要 11 秒多）
    // 会卡在 10 秒的默认值上直接失败；多给几秒就能等到解析结果。
    WinHttpSetTimeouts(session.get(), 15000, 10000, 15000, 15000);
    Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), close);
    Handle request(connection ? WinHttpOpenRequest(connection.get(), method ? method : body.empty() ? L"GET" : L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr, close);
    if (!request || !WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
        || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        error = L"网络连接失败或超时，请稍后重试"; retryable = true; return false;
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
    {
        // 5xx 是服务端自己出问题，隔一下再来通常就好了；4xx 是请求本身的问题，重发没意义。
        error = L"服务暂不可用（HTTP " + std::to_wstring(status) + L"）";
        retryable = status == 408 || status == 429 || status >= 500;
        return false;
    }
    std::string bytes;
    char buffer[16384];
    for (;;)
    {
        // 收包过程中也要能停下：换关键词时旧请求不必把整份响应读完
        if (RequestCancelled()) { error = L"请求已取消"; return false; }
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read))
        {
            error = L"网络响应未完整接收，请重试"; retryable = true; return false;
        }
        if (read == 0) break;
        if (bytes.size() + read > 8 * 1024 * 1024)
        {
            error = L"接口响应超出大小限制"; return false;
        }
        bytes.append(buffer, read);
    }
    result = kugou::FromUtf8(bytes);
    if (result.empty()) { error = L"服务返回了空响应"; retryable = true; return false; }
    return true;
}

// 网络抖动的重试：搜索、榜单、歌词、封面地址这些读接口重发一次没有副作用，
// 所以跟着一次重试；带请求体的 POST 会改服务端状态（签到、领会员、上报），
// 重复发送可能多领一次或少算一次，那几条链路各有自己的幂等处理，不在这里重试。
inline bool HttpRequest(const std::wstring& url, const std::string& body, const std::wstring& headers,
    std::wstring& result, std::wstring& error, const wchar_t* method = nullptr)
{
    const bool idempotent = body.empty() && (method == nullptr || _wcsicmp(method, L"GET") == 0);
    bool retryable = false;
    if (HttpRequestOnce(url, body, headers, result, error, method, retryable)) return true;
    if (!idempotent || !retryable) return false;
    if (RequestCancelled()) { error = L"请求已取消"; return false; }
    // 隔一小会儿再发：立刻重发多半还是撞在同一阵抖动上
    Sleep(300);
    return HttpRequestOnce(url, body, headers, result, error, method, retryable);
}
}
