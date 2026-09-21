#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace online
{
enum class ProgressResult { Running, Succeeded, Failed, Cancelled };
enum class ProgressUnit { Items, Bytes };

struct ProgressSnapshot
{
    std::uint64_t id{}, started{}, updated{}, finished{};
    std::wstring title, detail;
    std::uint64_t completed{}, total{};
    ProgressUnit unit{ ProgressUnit::Items };
    ProgressResult result{ ProgressResult::Running };
    bool background{}, cancellable{}, cancelling{}, queued{};

    bool Running() const { return result == ProgressResult::Running; }
    int Percent() const;
    std::wstring Counter() const;
    std::wstring Timing(std::uint64_t now) const;
};

// 一个句柄对应一次操作。各服务独立更新，绘制线程只读取快照；排队任务被丢弃时自动结束。
class OnlineProgress
{
public:
    static std::shared_ptr<OnlineProgress> Start(const std::wstring& title,
        const std::wstring& detail = L"", bool background = false, bool cancellable = false, bool queued = false);
    static std::vector<ProgressSnapshot> Snapshot();
    static void RequestCancel(std::uint64_t id);
    static void Dismiss(std::uint64_t id);
    static std::uint64_t Now();

    ~OnlineProgress();
    void Update(const std::wstring& detail, std::uint64_t completed = 0, std::uint64_t total = 0,
        ProgressUnit unit = ProgressUnit::Items);
    void Transfer(std::uint64_t received, std::uint64_t total);
    void Finish(ProgressResult result, const std::wstring& detail = L"");
    bool Cancelled() const;
    const std::atomic<bool>* CancelFlag() const { return &m_entry->cancel; }
    std::uint64_t Id() const;

private:
    struct Entry
    {
        mutable std::mutex mutex;
        ProgressSnapshot value;
        std::atomic<bool> cancel{ false };
    };
    struct Registry
    {
        std::mutex mutex;
        std::uint64_t next_id{};
        std::uint64_t cleaned{};
        std::vector<std::shared_ptr<Entry>> entries;
    };
    explicit OnlineProgress(std::shared_ptr<Entry> entry) : m_entry(std::move(entry)) {}
    static Registry& Tasks();
    std::shared_ptr<Entry> m_entry;
};
}
