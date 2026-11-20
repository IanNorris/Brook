#pragma once
// Clearwater launcher model (DESIGN.md "Launcher organisation"): the app
// catalog, the user's layout (categories with one home per app, pins, recent
// apps, the top-row choice), search with ranking, and the open launcher's
// view state (query, selected section, keyboard selection, scroll, context
// menu). Pure data and logic: no kernel, display or I/O dependencies, and
// trivially copyable, so the kernel WM and the stage 3 user-mode shell can
// both own one. The glue feeds it the catalog and input and persists the
// text that Serialize() returns; chrome_launcher builds the surface from it.
#include <stddef.h>
#include <stdint.h>

namespace brook {

struct LauncherLimits {
    static constexpr uint32_t Apps = 128;       // WM_LAUNCHER_MAX_ITEMS
    static constexpr uint32_t Keys = 256;       // installed plus remembered (unknown) IDs
    static constexpr uint32_t KeyLength = 48;
    static constexpr uint32_t NameLength = 48;
    static constexpr uint32_t DescLength = 64;
    static constexpr uint32_t Categories = 16;
    static constexpr uint32_t CategoryId = 16;
    static constexpr uint32_t CategoryName = 24;
    static constexpr uint32_t Members = 96;     // per category
    static constexpr uint32_t Pins = 10;        // two rows of five tiles
    static constexpr uint32_t Recent = 5;       // one row of tiles
    static constexpr uint32_t Query = 40;
    static constexpr uint32_t Lines = 192;
    static constexpr uint32_t Results = Apps;
};

inline constexpr char TerminalAppKey[] = "terminal";

struct TaskbarPins {
    static constexpr uint32_t Capacity = 8;
    static constexpr uint32_t TextCapacity = 32 + Capacity * LauncherLimits::KeyLength;
    uint32_t Count() const { return count_; }
    const char* Key(uint32_t i) const { return i < count_ ? keys_[i] : ""; }
    int Find(const char* key) const;
    bool Toggle(const char* key);
    void SetDefault();
    bool Parse(const char* text);
    uint32_t Serialize(char* out, uint32_t capacity) const;

private:
    char keys_[Capacity][LauncherLimits::KeyLength];
    uint32_t count_;
};

// The eight category colours and the category icons (DESIGN.md "Token
// sheet": the same in every theme).
enum class LauncherColor : uint8_t { Blue, Teal, Green, Amber, Coral, Pink, Violet, Slate, Count };
enum class LauncherCategoryIcon : uint8_t { Folder, Sliders, Globe, Play, Doc, Brush, Pad, Spark, Bolt, Temperature, Network, Volume, Count };

struct LauncherApp {
    char key[LauncherLimits::KeyLength];    // stable ID: desktop app_id, or shortcut name
    char name[LauncherLimits::NameLength];
    char desc[LauncherLimits::DescLength];  // may be empty
    char hint[LauncherLimits::CategoryId];  // declared default category ID, or empty
    bool system;                            // part of Brook: Uninstall is never offered
    char taskbarKey[LauncherLimits::KeyLength]; // exact catalog identity; empty when unavailable
};

// Lines of the app list, in display order.
enum class LauncherLineKind : uint8_t {
    Heading,  // category (or Unsorted / results) caption with a count
    Apps,     // one or two app rows side by side
    Chips,    // matching categories, during search
    Empty,    // an empty-state message
};
struct LauncherLine {
    LauncherLineKind kind;
    uint8_t columns;     // Apps: 1 (search results, Unsorted) or 2
    int16_t category;    // Heading/Empty: category index, -2 Unsorted, -3 results, -4 chip caption
    uint16_t apps[2];    // catalog indices (Apps)
    uint16_t count;      // Heading: apps in it; Apps: apps on the line; Chips: chips
};

enum class LauncherEdit : uint8_t { None, MoveApps, MoveCategory, CategorySaved, CategoryDeleted, Name, Sorted, Pinned, Reset };
enum class LauncherSection : uint8_t { List, Tiles, Rail };
enum class LauncherTop : uint8_t { Pinned, Recent };
// A rail view: All apps, a category, or Unsorted.
static constexpr int LauncherViewAll = -1, LauncherViewUnsorted = -2;

enum class LauncherKey : uint8_t {
    Char, Backspace, Up, Down, Left, Right, Tab, BackTab, Enter, Escape, Menu, Home, End, PageUp, PageDown, Rename, Delete,
};
static constexpr uint8_t LauncherModAlt = 1, LauncherModCtrl = 2, LauncherModShift = 4;

// What a press or a key asks the owner to do. Redraw is implied by Changed.
enum class LauncherCommand : uint8_t { None, Changed, Launch, Close, OpenSettings, Undo, TaskbarChanged };
struct LauncherResult {
    LauncherCommand command = LauncherCommand::None;
    int app = -1;  // Launch: catalog index
};

// What a pointer press landed on (from the builder's layout).
enum class LauncherTargetKind : uint8_t {
    None, Panel, Search, TopTab, Tile, Rail, Row, Chip, MenuItem, Menu,
    Settings, Restart, Shutdown, Confirm, Dismiss, CancelShutdown, Footer, ForceApps,
    Organise, NewCategory, EditCategory, RenameApp, MoveSuggested, SortSuggested,
    Editor, EditorSave, EditorCancel, EditorDelete, EditorColor, EditorIcon, Undo, ResetOrganisation, ClearSelection, CategorySection,
};
struct LauncherTarget {
    LauncherTargetKind kind;
    bool operator==(const LauncherTarget& other) const { return kind == other.kind && index == other.index; }
    uint16_t index; // TopTab: LauncherTop; Tile: tile slot; Rail: rail row; Row: catalog index; Chip: category; MenuItem: item
};

// Heights the list is laid out with (device pixels), from the builder.
struct LauncherViewport {
    int heading, row, chips, empty, height;
    int step; // pixels a wheel tick scrolls
    int railRows; // rows visible at once, excluding a drag-only drop target
};

inline constexpr char LauncherFilesAppId[] = "org.gnome.Nautilus";

// Trivially constructible (it lives in kernel static storage, which runs no
// constructors): call Init() once before anything else.
class LauncherModel {
public:
    void Init();

    // --- catalog -----------------------------------------------------------
    // Replace the installed apps. Keeps the layout; an app the layout has
    // never seen lands in Unsorted, or, before any layout exists (first run,
    // or a missing or bad file), in its declared default category.
    void SetCatalog(const LauncherApp* apps, uint32_t count);
    uint32_t AppCount() const { return appCount_; }
    const LauncherApp& App(uint32_t i) const { return apps_[i]; }
    int FindApp(const char* key) const;

    // --- layout ------------------------------------------------------------
    void ResetLayout();                // the seven default categories, default pins
    bool RestoreLayout(const char* text);
    LauncherEdit LastEdit() const { return lastEdit_; }
    const char* EditNotice() const { return editNotice_; }
    uint32_t EditEpoch() const { return editEpoch_; }
    void SetUndoVisible(bool visible) { undoVisible_ = visible; }
    bool UndoVisible() const { return undoVisible_; }
    bool Parse(const char* text, bool* migrated = nullptr);      // [Launcher] section; false: defaults kept
    uint32_t Serialize(char* out, uint32_t capacity) const; // 0: did not fit
    uint32_t LayoutEpoch() const { return layoutEpoch_; }   // changes on every saved-state change

    uint32_t CategoryCount() const { return categoryCount_; }
    const char* CategoryName(uint32_t c) const { return categories_[c].name; }
    LauncherColor CategoryColor(uint32_t c) const { return LauncherColor(categories_[c].color); }
    LauncherCategoryIcon CategoryIcon(uint32_t c) const { return LauncherCategoryIcon(categories_[c].icon); }
    uint32_t CategoryAppCount(uint32_t c) const;  // installed members
    int CategoryOf(uint32_t app) const { return appCategory_[app]; } // -1: Unsorted
    uint32_t UnsortedCount() const;

    uint32_t PinCount() const { return pinCount_; }
    const char* PinKey(uint32_t index) const { return keys_[pins_[index]]; }
    bool Pinned(uint32_t app) const;
    void TogglePin(uint32_t app);
    bool CanToggleTaskbarPin(uint32_t app) const;
    int FindTaskbarApp(const char* key) const;
    TaskbarPins& DesktopPins() { return taskbarPins_; }
    const TaskbarPins& DesktopPins() const { return taskbarPins_; }
    void NoteLaunch(uint32_t app);     // front of Recent
    // Alt+Up / Alt+Down: reorder within the category, crossing into the
    // neighbouring category past either end. An Unsorted app moves into the
    // last category.
    bool Step(uint32_t app, int direction);

    // Organisation edits use catalog indices and retain the stable keys on disk.
    bool Organising() const { return organising_; }
    bool IsNew(uint32_t app) const { return newApp_[app]; }
    bool DraggingApp() const { return dragging_ && dragApp_ >= 0; }
    int DragApp() const { return dragApp_; }
    bool Marked(uint32_t app) const { return marked_[app]; }
    uint32_t MarkedCount() const;
    bool MoveMarked(uint32_t source, int category, int anchor = -1, bool after = false);
    bool MoveCategory(uint32_t category, uint32_t anchor, bool after);
    int CreateCategory();
    bool DeleteCategory(uint32_t category);
    bool RenameCategory(uint32_t category, const char* name);
    bool RenameApp(uint32_t app, const char* name);
    const char* Name(uint32_t app) const;
    int Suggestion(uint32_t app) const;
    LauncherColor IconColor(uint32_t app) const;
    bool SortSuggested();
    int RailRowFor(int view) const;

    bool EditorOpen() const { return editorIndex_ >= 0; }
    bool EditingApp() const { return editingApp_; }
    const char* EditName() const { return editName_; }
    uint8_t EditColor() const { return editColor_; }
    uint8_t EditIcon() const { return editIcon_; }

    bool PointerHeld() const { return held_.kind != LauncherTargetKind::None; }
    bool Dragging() const { return dragging_; }
    LauncherTarget DragSource() const { return held_; }
    LauncherTarget DropTarget() const { return drop_; }
    bool DropAfter() const { return dropAfter_; }
    bool BeginPointer(LauncherTarget target, int32_t x, int32_t y, bool after = false);
    bool MovePointer(LauncherTarget target, int32_t x, int32_t y, int threshold, bool after, uint64_t now = 0);
    LauncherResult ReleasePointer(LauncherTarget target, bool after);
    void CancelPointer();

    // Tiles of the top row (installed apps only).
    uint32_t TopTiles(uint16_t* out, uint32_t capacity) const;
    LauncherTop Top() const { return top_; }
    void SetTop(LauncherTop top);

    // --- open launcher -----------------------------------------------------
    void Open();                       // fresh session: empty query, All apps
    void SetViewport(const LauncherViewport& v);
    const char* Query() const { return query_; }
    int View() const { return view_; }
    LauncherSection Section() const { return section_; }
    int Selected() const { return selected_; }       // List: catalog index, or -1
    int SelectedTile() const { return tile_; }       // Tiles: slot
    int SelectedRail() const { return rail_; }       // Rail: row (0 All, 1.. categories, last Unsorted)
    bool KeyboardActive() const { return keyboard_; } // draw the keyboard selection
    uint32_t Scroll() const { return scroll_; } // pixels from the top of the list
    // Rail rows: All apps, each category, then Unsorted while it has apps.
    uint32_t RailRows() const;
    int RailView(uint32_t row) const;
    uint32_t RailScroll() const { return railScroll_; }
    bool ScrollRail(int rows);

    // The app list for the current query and view.
    uint32_t Lines(LauncherLine* out, uint32_t capacity) const;
    // Search: the ranked results and matching categories.
    uint32_t SearchResults(uint16_t* out, uint32_t capacity) const;
    uint32_t SearchCategories(uint16_t* out, uint32_t capacity) const;
    // Pixels of list below the visible page at the top (0: it all fits).
    uint32_t MaxScroll() const;
    // Total list height, in pixels.
    uint32_t ContentHeight() const;

    // Context menu (Pin or Unpin, Uninstall) for one app.
    bool MenuOpen() const { return menuApp_ >= 0; }
    int MenuApp() const { return menuApp_; }
    int MenuItem() const { return menuItem_; }
    int32_t MenuX() const { return menuX_; }
    int32_t MenuY() const { return menuY_; }
    void OpenMenu(uint32_t app, int32_t x, int32_t y, bool keyboard);
    void CloseMenu() { menuApp_ = -1; }

    LauncherResult Key(LauncherKey key, char ch = 0, uint8_t mods = 0);
    LauncherResult Press(const LauncherTarget& target, bool secondary, int32_t x, int32_t y);
    bool ScrollBy(int pixels);
    // Wheel ticks, positive up (the step a tick is from the viewport).
    bool ScrollTicks(int ticks) { return ScrollBy(-ticks * (viewport_.step > 0 ? viewport_.step : 1)); }

    static int Rank(const char* name, const char* category, const char* desc, const char* term);
    // Default category ID for a freedesktop Categories= list, or "".
    static const char* HintFromDesktopCategories(const char* categories);

private:
    struct Category {
        char id[LauncherLimits::CategoryId];
        char name[LauncherLimits::CategoryName];
        uint8_t color, icon;
        uint16_t count;
        uint16_t members[LauncherLimits::Members]; // key indices
    };
    int Intern(const char* key);
    int FindKey(const char* key) const;
    int AppOfKey(uint16_t key) const;
    void Recompute();
    void Changed() { ++layoutEpoch_; }
    void ClampScroll();
    void EnsureRailVisible();
    void EnsureVisible();
    uint32_t ItemOrder(uint16_t* out, uint32_t capacity) const;
    int SelectedLine() const;
    int LineHeight(const LauncherLine& line) const;
    LauncherResult Activate(int app);
    void RemoveMember(uint16_t key);
    bool AddCategory(const char* id, const char* name, uint8_t color, uint8_t icon);
    void FileDefault(uint32_t app);
    bool RailShows(uint32_t category) const;
    void ToggleMarked(uint32_t app);
    void SetOrganising(bool enabled);
    void BeginEditor(uint32_t index, bool app);
    LauncherResult EditorPress(const LauncherTarget& target);
    int TargetApp(LauncherTarget target) const;
    bool MovePin(uint32_t app, int anchor, bool after);
    void ResetOrganisation();
    void Edited(LauncherEdit edit) { lastEdit_ = edit; editNotice_[0] = 0; ++editEpoch_; Changed(); }
    void NoticeMove(const char* name, int category);

    LauncherApp apps_[LauncherLimits::Apps];
    uint16_t appKey_[LauncherLimits::Apps];
    int16_t appCategory_[LauncherLimits::Apps];
    uint32_t appCount_;

    char keys_[LauncherLimits::Keys][LauncherLimits::KeyLength];
    char aliases_[LauncherLimits::Keys][LauncherLimits::NameLength];
    uint32_t keyCount_;
    Category categories_[LauncherLimits::Categories];
    uint32_t categoryCount_;
    TaskbarPins taskbarPins_;
    uint16_t pins_[LauncherLimits::Pins];
    uint32_t pinCount_;
    uint16_t recent_[LauncherLimits::Recent];
    uint32_t recentCount_;
    LauncherTop top_;
    bool layoutLoaded_; // a layout exists (parsed, or filed since first run)
    uint32_t layoutEpoch_, editEpoch_;
    LauncherEdit lastEdit_;
    char editNotice_[96];
    bool undoVisible_;
    bool newApp_[LauncherLimits::Apps];

    char query_[LauncherLimits::Query];
    int view_;
    LauncherSection section_;
    int selected_, tile_, rail_;
    bool keyboard_;
    uint32_t scroll_, railScroll_;
    int menuApp_, menuItem_;
    int32_t menuX_, menuY_;
    LauncherViewport viewport_;
    bool organising_, marked_[LauncherLimits::Apps];
    bool editingApp_, editReplace_;
    int editorIndex_;
    char editName_[LauncherLimits::NameLength];
    uint8_t editColor_, editIcon_;
    LauncherTarget held_, drop_;
    int32_t pressX_, pressY_;
    int dragApp_, dragCategory_;
    bool dragging_, dropAfter_;
    uint64_t dropSince_;
};

static_assert(__is_trivially_copyable(LauncherModel) && __is_trivially_constructible(LauncherModel),
              "static storage, and the shell process copies the model");

} // namespace brook
