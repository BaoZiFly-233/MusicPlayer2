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
        // 皮肤可以用 <onlineMusic item_height="30" font_size="9"/> 覆盖这一页的列表密度，
        // 和皮肤里其它列表一个规矩（item_height 是基类的，只能在这里转一手）。
        void ApplySkinDensity(int height, int font)
        {
            if (height > 0) item_height = height;
            if (font > 0) font_size = font;
        }
        // 每行左侧按来源给一个图标：本地文件和在线曲目一眼分开
        bool HasIcon() override { return true; }
        IconMgr::IconType GetIcon(int row) override;
        // 鼠标指到一行标题上时给出「播放 / 下一首播放 / 加入队列 / 收藏」四个小按钮，
        // 和原生媒体库列表的用法一致，不必每次都去右键或工具栏。
        int GetHoverButtonCount(int row) override;
        int GetHoverButtonColumn() override { return 1; }
        IconMgr::IconType GetHoverButtonIcon(int index, int row) override;
        std::wstring GetHoverButtonTooltip(int index, int row) override;
        void OnHoverButtonClicked(int btn_index, int row) override;
        // 鼠标没指到时，已收藏到在线本地歌单的行显示一颗实心红心
        int GetUnHoverIconCount(int row) override;
        IconMgr::IconType GetUnHoverIcon(int index, int row) override;
        // 行提示：完整标题、歌手、专辑、来源与状态，以及播放失败的原因
        bool ShowTooltip() override { return true; }
        std::wstring GetToolTipText(int row) override;
        int GetToolTipIndex() const override { return TooltipIndex::ONLINE_LIST; }
        void OnDoubleClicked() override;
        bool RButtonUp(CPoint point) override;
        void Dispatch(COnlineMusicModel::Action action, int value = 0);
        // 对指定的一行执行动作（悬停按钮用），不改变当前选中
        void DispatchRow(COnlineMusicModel::Action action, int row);
        // 右键菜单和工具栏的「更多」共用这一个菜单定义，避免两处叫法不一致、
        // 一处有另一处没有。full 为 true 时带上页面、缓存和账号这些整页操作。
        void ShowMenu(bool full);
        // 当前选中的行里有没有可播放的曲目；有没有能直接在资源管理器里定位的本地文件。
        bool HasSongSelected() const;
        bool HasLocalFileSelected() const;
        // 右键点的这一行是不是歌曲（专辑、歌单、榜单行不能用歌曲的那些操作）
        bool IsSongRow(int row) const;
        // 键盘：上下移动选中、Home/End、翻页；返回 true 表示已处理
        bool MoveSelection(int delta, bool page, bool to_edge);
        // 把正在播放的那一行滚到可见并选中；没有在播的行返回 false
        bool LocateToCurrent();
    private:
        enum HoverButton { HB_PLAY, HB_PLAY_NEXT, HB_QUEUE, HB_SAVE, HB_MAX };
        void ShowStandardMenu(bool full, bool row_is_song);
        // 一行歌曲的来源短名（本地 / K源 / B源）
        std::wstring RowOrigin(int row) const;
        // 这一行是否已收藏到在线本地歌单
        bool IsSavedRow(int row) const;
        std::shared_ptr<const COnlineMusicModel::State> m_state;
        // 当前列表里是否混了多个来源。混着的时候标题列会带上来源前缀，
        // 只含一个来源时不加，免得满屏都是重复的标签。
        bool m_mixed_origins{};
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
        void FromXmlNode(tinyxml2::XMLElement* xml_node) override;
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
        void ShowImportMenu();
        // 只在实际状态或宽度变化时调用，避免每帧查找元素、重复设置文本。
        void SyncLayout(const COnlineMusicModel::State& state);
        std::recursive_mutex m_view_mutex;
        OnlineMusicList* m_list{};
        OnlineMusicDetail* m_detail{};
        // 皮肤在 <onlineMusic> 上给的列表密度，0 表示没给、用布局文件里的默认值
        int m_skin_item_height{};
        int m_skin_font_size{};
        std::shared_ptr<const COnlineMusicModel::State> m_last_state;
        int m_last_width{ -1 };
        bool m_activated{};
        // 上一次同步时正在播放的曲目地址：变了才刷新「正在播放」按钮的可用性
        std::wstring m_last_playing;
    };
}
