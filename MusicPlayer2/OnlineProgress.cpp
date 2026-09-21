#include "stdafx.h"
#include "OnlineProgress.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace online
{
namespace
{
std::wstring Bytes(std::uint64_t bytes)
{
    std::wostringstream text;
    if (bytes < 1024) text << bytes << L" B";
    else if (bytes < 1024 * 1024) text << std::fixed << std::setprecision(1) << bytes / 1024.0 << L" KB";
    else text << std::fixed << std::setprecision(1) << bytes / (1024.0 * 1024.0) << L" MB";
    return text.str();
}
}
int ProgressSnapshot::Percent() const
{
    if (result == ProgressResult::Succeeded) return 100;
    if (total == 0) return -1;
    // 收到全部字节后仍可能需要校验和写盘，只有操作真正成功才显示 100%。
    return static_cast<int>((std::min)(99.0, 100.0 * completed / total));
}
std::wstring ProgressSnapshot::Counter() const
{
    if (!Running()) return result == ProgressResult::Succeeded ? L"已完成"
        : result == ProgressResult::Cancelled ? L"已取消" : L"未完成";
    if (cancelling) return L"正在取消";
    if (unit == ProgressUnit::Bytes)
        return total ? Bytes(completed) + L" / " + Bytes(total) : Bytes(completed);
    return total ? std::to_wstring(completed) + L" / " + std::to_wstring(total)
        : completed ? L"已处理 " + std::to_wstring(completed) + L" 项" : L"进行中";
}
std::wstring ProgressSnapshot::Timing(std::uint64_t now) const
{
    const auto end = finished ? finished : now;
    const auto seconds = (end >= started ? end - started : 0) / 1000;
    if (Running() && queued) return L"排队中 · " + std::to_wstring(seconds) + L" 秒";
    if (Running() && now >= updated && now - updated >= 15000)
        return L"仍在等待服务响应 · " + std::to_wstring(seconds) + L" 秒";
    return seconds < 60 ? std::to_wstring(seconds) + L" 秒"
        : std::to_wstring(seconds / 60) + L" 分 " + std::to_wstring(seconds % 60) + L" 秒";
}
OnlineProgress::Registry& OnlineProgress::Tasks()
{
    // 后台服务使用分离线程，退出时它们持有的句柄仍可安全结束。
    static auto* registry = new Registry;
    return *registry;
}
std::uint64_t OnlineProgress::Now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::shared_ptr<OnlineProgress> OnlineProgress::Start(const std::wstring& title,
    const std::wstring& detail, bool background, bool cancellable, bool queued)
{
    auto entry = std::make_shared<Entry>();
    entry->value.title = title; entry->value.detail = detail;
    entry->value.background = background; entry->value.cancellable = cancellable;
    entry->value.queued = queued;
    entry->value.started = entry->value.updated = Now();
    auto& registry = Tasks();
    std::lock_guard<std::mutex> lock(registry.mutex);
    entry->value.id = ++registry.next_id;
    // 批量排队时至多每秒清理一次，避免逐首扫描整个队列。
    if (entry->value.started - registry.cleaned >= 1000)
    {
        registry.cleaned = entry->value.started;
        std::erase_if(registry.entries, [&](const auto& old) {
            std::lock_guard<std::mutex> guard(old->mutex);
            // 先比大小再相减：两个时间戳都是无符号数，并发下 old 可能比本任务还晚结束，
            // 直接相减会下溢成一个很大的值，刚结束的任务会被立刻清掉。
            return old->value.finished && old->value.finished <= entry->value.started
                && entry->value.started - old->value.finished >= 8000;
        });
    }
    registry.entries.push_back(entry);
    return std::shared_ptr<OnlineProgress>(new OnlineProgress(std::move(entry)));
}
OnlineProgress::~OnlineProgress() { Finish(ProgressResult::Cancelled, L"操作已取消"); }
void OnlineProgress::Update(const std::wstring& detail, std::uint64_t completed, std::uint64_t total, ProgressUnit unit)
{
    std::lock_guard<std::mutex> lock(m_entry->mutex);
    auto& value = m_entry->value;
    if (!value.Running()) return;
    value.detail = detail; value.completed = total ? (std::min)(completed, total) : completed;
    value.total = total; value.unit = unit; value.updated = Now();
    value.queued = false;
}
void OnlineProgress::Transfer(std::uint64_t received, std::uint64_t total)
{
    std::lock_guard<std::mutex> lock(m_entry->mutex);
    auto& value = m_entry->value;
    if (!value.Running()) return;
    value.completed = total ? (std::min)(received, total) : received;
    value.total = total; value.unit = ProgressUnit::Bytes; value.updated = Now();
}
void OnlineProgress::Finish(ProgressResult result, const std::wstring& detail)
{
    std::lock_guard<std::mutex> lock(m_entry->mutex);
    auto& value = m_entry->value;
    if (!value.Running() || result == ProgressResult::Running) return;
    value.result = result;
    if (!detail.empty()) value.detail = detail;
    value.finished = value.updated = Now();
    value.cancellable = false;
}
bool OnlineProgress::Cancelled() const { return m_entry->cancel.load(); }
void OnlineProgress::Dismiss(std::uint64_t id)
{
    auto& registry = Tasks();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::erase_if(registry.entries, [id](const auto& entry) {
        std::lock_guard<std::mutex> guard(entry->mutex);
        return entry->value.id == id && !entry->value.Running();
    });
}
std::uint64_t OnlineProgress::Id() const { return m_entry->value.id; }
void OnlineProgress::RequestCancel(std::uint64_t id)
{
    auto& registry = Tasks();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (const auto& entry : registry.entries)
    {
        std::lock_guard<std::mutex> guard(entry->mutex);
        if (entry->value.id == id && entry->value.Running() && entry->value.cancellable)
        { entry->cancel = true; entry->value.cancelling = true; }
    }
}
std::vector<ProgressSnapshot> OnlineProgress::Snapshot()
{
    std::vector<ProgressSnapshot> result;
    auto& registry = Tasks();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto now = Now();
    std::erase_if(registry.entries, [&](const auto& entry) {
        std::lock_guard<std::mutex> guard(entry->mutex);
        const auto& value = entry->value;
        // 同理先比大小再相减，避免并发完成的任务因下溢被当成「早就结束」而立刻移出列表
        if (value.finished && value.finished <= now && now - value.finished >= 8000) return true;
        result.push_back(value);
        return false;
    });
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.Running() != b.Running()) return a.Running();
        if (a.queued != b.queued) return !a.queued;
        if (a.background != b.background) return !a.background;
        return a.id > b.id;
    });
    return result;
}
}
