#pragma once
#include "UIElement.h"
#include "AbstractListElement.h"
#include "SearchBox.h"
#include "../OnlineMusicModel.h"

namespace UiElement
{
    class OnlineMusicList : public AbstractListElement
    {
    public:
        void SetSnapshot(std::shared_ptr<const COnlineMusicModel::State> state);
        std::wstring GetItemText(int row, int col) override;
        int GetRowCount() override;
        int GetColumnCount() override;
        int GetColumnWidth(int col, int total_width) override;
        bool IsMultipleSelectionEnable() override { return true; }
        bool IsHighlightRow(int row) override;
        bool IsItemEnabled(int row) override;   // 播放失败过的曲目显示为灰色
        std::wstring GetEmptyString() override;
        int GetColumnScrollTextWhenSelected() override { return 1; }
        void OnDoubleClicked() override;
        bool RButtonUp(CPoint point) override;
        void Dispatch(COnlineMusicModel::Action action);
    private:
        std::shared_ptr<const COnlineMusicModel::State> m_state;
    };

    class OnlineMusicSearch : public SearchBox
    {
    public:
        void OnKeyWordsChanged() override;
        void OnSubmit() override;
        void SyncQuery(const COnlineMusicModel::State& state);
    private:
        unsigned long long m_query_revision{};
    };

    class OnlineMusicDetail : public AbstractScrollArea
    {
    public:
        void SetSnapshot(std::shared_ptr<const COnlineMusicModel::State> state);
        void DrawScrollArea() override;
        int GetScrollAreaHeight() override;
    private:
        std::shared_ptr<const COnlineMusicModel::State> m_state;
        std::vector<std::wstring> m_lines;
    };

    // 由原生皮肤元素组成的在线工作区，不创建页面窗口、不绘制独立背景或播放条。
    class OnlineMusic : public Element
    {
    public:
        void InitComplete() override;
        void Draw() override;
        void DrawTopMost() override;
        bool LButtonUp(CPoint point) override;
        bool LButtonDown(CPoint point) override;
        bool RButtonUp(CPoint point) override;
        bool RButtonDown(CPoint point) override;
        bool DoubleClick(CPoint point) override;
        bool MouseMove(CPoint point) override;
        bool MouseLeave() override;
        bool MouseWheel(int delta, CPoint point) override;
        bool GlobalLButtonUp(CPoint point) override;
        bool GlobalLButtonDown(CPoint point) override;
        bool GlobalMouseMove(CPoint point) override;
        bool SetCursor() override;
        bool HandleKey(UINT key, bool control);
    private:
        void ShowMoreMenu();
        std::recursive_mutex m_view_mutex;
        OnlineMusicList* m_list{};
        OnlineMusicDetail* m_detail{};
        bool m_activated{};
    };
}
