#include "stdafx.h"
#include "OnlineSource.h"

using namespace std;

namespace online
{

// 这些是真实可播放的协议，不算虚拟路径
static bool IsRealProtocol(const wstring& scheme)
{
    return scheme == L"http" || scheme == L"https" || scheme == L"ftp" || scheme == L"mms";
}

wstring CSourceRegistry::GetScheme(const wstring& path)
{
    if (path.empty())
        return wstring();

    size_t pos = path.find(L"://");
    if (pos == wstring::npos || pos == 0)
        return wstring();

    wstring scheme = path.substr(0, pos);
    // 统一转小写，便于比较
    for (wchar_t& ch : scheme)
        ch = static_cast<wchar_t>(towlower(ch));

    if (IsRealProtocol(scheme))
        return wstring();

    return scheme;
}

bool CSourceRegistry::IsVirtualPath(const wstring& path)
{
    return !GetScheme(path).empty();
}

CSourceRegistry& CSourceRegistry::Instance()
{
    static CSourceRegistry instance;
    return instance;
}

void CSourceRegistry::Register(IOnlineSource* source)
{
    if (source == nullptr)
        return;
    // 避免重复注册同一个 scheme
    if (FindByScheme(source->GetScheme()) != nullptr)
        return;
    m_sources.push_back(source);
}

IOnlineSource* CSourceRegistry::FindByScheme(const wstring& scheme)
{
    if (scheme.empty())
        return nullptr;

    wstring lower = scheme;
    for (wchar_t& ch : lower)
        ch = static_cast<wchar_t>(towlower(ch));

    for (IOnlineSource* source : m_sources)
    {
        if (source != nullptr && source->GetScheme() == lower)
            return source;
    }
    return nullptr;
}

IOnlineSource* CSourceRegistry::FindByPath(const wstring& path)
{
    return FindByScheme(GetScheme(path));
}

wstring CSourceRegistry::ResolvePlayUrl(const wstring& path)
{
    if (path.empty())
        return wstring();

    // 本地文件或已经是真实地址，原样返回，调用方不必自己判断
    if (!IsVirtualPath(path))
        return path;

    IOnlineSource* source = FindByPath(path);
    if (source == nullptr)
        return wstring();

    return source->ResolvePlayUrl(path);
}

// 内置音源在这里注册。酷狗概念版与波点音乐的实现在各自文件中，
// 它们完成前这里只保留注册骨架，注册表为空也能正常工作。
void InitOnlineSources()
{
    // 注册顺序即界面上的显示顺序
    // Register(new CKugouLiteSource());
    // Register(new CBodianSource());
}

} // namespace online
