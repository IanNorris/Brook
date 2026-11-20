// Clearwater launcher model: see launcher_model.h. Freestanding.
#include "launcher_model.h"

namespace brook {

namespace {

using LL = LauncherLimits;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
uint32_t Length(const char* s) { uint32_t n = 0; while (s && s[n]) ++n; return n; }
void Copy(char* dst, const char* src, uint32_t capacity) {
    uint32_t i = 0;
    while (src && src[i] && i + 1 < capacity) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}
bool ValidLayoutName(const char* name, uint32_t capacity, bool allowEmpty) {
    uint32_t n = 0;
    for (; name[n]; ++n) {
        const uint8_t c = uint8_t(name[n]);
        if (n + 1 >= capacity || c < 32 || c == 127 || c == '|') return false;
    }
    return (!n && allowEmpty) || (n && name[0] != ' ' && name[n - 1] != ' ');
}
bool Equal(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
bool EqualI(const char* a, const char* b) {
    while (*a && Lower(*a) == Lower(*b)) { ++a; ++b; }
    return !*a && !*b;
}
// Case-insensitive substring: the offset of the first match, or -1.
int Find(const char* hay, const char* needle) {
    const uint32_t n = Length(needle);
    if (!n) return 0;
    for (int i = 0; hay[i]; ++i) {
        uint32_t j = 0;
        while (j < n && hay[i + j] && Lower(hay[i + j]) == Lower(needle[j])) ++j;
        if (j == n) return i;
    }
    return -1;
}
bool WordStart(const char* hay, const char* needle) {
    const uint32_t n = Length(needle);
    for (int i = 0; hay[i]; ++i) {
        if (i && hay[i - 1] != ' ' && hay[i - 1] != '-' && hay[i - 1] != '(') continue;
        uint32_t j = 0;
        while (j < n && hay[i + j] && Lower(hay[i + j]) == Lower(needle[j])) ++j;
        if (j == n) return true;
    }
    return false;
}
int CompareI(const char* a, const char* b) {
    while (*a && Lower(*a) == Lower(*b)) { ++a; ++b; }
    return int(uint8_t(Lower(*a))) - int(uint8_t(Lower(*b)));
}

struct DefaultCategory { const char* id; const char* name; LauncherColor color; LauncherCategoryIcon icon; };
constexpr DefaultCategory Defaults[] = {
    {"system", "System", LauncherColor::Blue, LauncherCategoryIcon::Sliders},
    {"internet", "Internet", LauncherColor::Teal, LauncherCategoryIcon::Globe},
    {"media", "Media", LauncherColor::Violet, LauncherCategoryIcon::Play},
    {"office", "Office", LauncherColor::Amber, LauncherCategoryIcon::Doc},
    {"graphics", "Graphics", LauncherColor::Pink, LauncherCategoryIcon::Brush},
    {"games", "Games", LauncherColor::Coral, LauncherCategoryIcon::Pad},
    {"demos", "Demos", LauncherColor::Slate, LauncherCategoryIcon::Spark},
};
// The prototype's default pins, by name, among the apps installed at first run.
constexpr const char* DefaultPins[] = {"Files", "Terminal", "Settings", "Firefox", "Quake"};
constexpr const char* ColorNames[] = {"blue", "teal", "green", "amber", "coral", "pink", "violet", "slate"};
constexpr const char* IconNames[] = {"folder", "sliders", "globe", "play", "doc", "brush", "pad", "spark", "bolt", "temperature", "network", "volume"};

int Lookup(const char* const* names, uint32_t count, const char* value, uint32_t length) {
    for (uint32_t i = 0; i < count; ++i)
        if (Length(names[i]) == length) {
            uint32_t j = 0;
            while (j < length && names[i][j] == value[j]) ++j;
            if (j == length) return int(i);
        }
    return -1;
}

class Writer {
public:
    Writer(char* out, uint32_t capacity) : out_(out), capacity_(capacity) {}
    void Put(const char* s) { while (*s) Char(*s++); }
    void Char(char c) { if (used_ + 1 < capacity_) out_[used_++] = c; else full_ = true; }
    uint32_t Finish() { if (capacity_) out_[used_ < capacity_ ? used_ : capacity_ - 1] = 0; return full_ ? 0 : used_; }
private:
    char* out_;
    uint32_t capacity_, used_ = 0;
    bool full_ = false;
};

} // namespace

void LauncherModel::Init() {
    appCount_ = keyCount_ = 0;
    layoutEpoch_ = editEpoch_ = 0;
    undoVisible_ = false;
    lastEdit_ = LauncherEdit::None;
    editNotice_[0] = 0;
    viewport_ = {36, 48, 40, 96, 400, 20, 0};
    menuItem_ = 0; menuX_ = menuY_ = 0;
    Open();
    ResetLayout();
}

// --- catalog -----------------------------------------------------------------

int LauncherModel::FindKey(const char* key) const {
    for (uint32_t i = 0; i < keyCount_; ++i) if (Equal(keys_[i], key)) return int(i);
    return -1;
}

int LauncherModel::Intern(const char* key) {
    if (!key || !*key) return -1;
    const int found = FindKey(key);
    if (found >= 0) return found;
    if (keyCount_ == LL::Keys) return -1;
    Copy(keys_[keyCount_], key, LL::KeyLength);
    aliases_[keyCount_][0] = 0;
    return int(keyCount_++);
}

int LauncherModel::AppOfKey(uint16_t key) const {
    for (uint32_t i = 0; i < appCount_; ++i) if (appKey_[i] == key) return int(i);
    return -1;
}

int LauncherModel::FindApp(const char* key) const {
    const int k = FindKey(key);
    return k < 0 ? -1 : AppOfKey(uint16_t(k));
}

void LauncherModel::Recompute() {
    for (uint32_t a = 0; a < appCount_; ++a) {
        appCategory_[a] = -1;
        for (uint32_t c = 0; c < categoryCount_ && appCategory_[a] < 0; ++c)
            for (uint32_t m = 0; m < categories_[c].count; ++m)
                if (categories_[c].members[m] == appKey_[a]) { appCategory_[a] = int16_t(c); break; }
    }
    for (uint32_t a = 0; a < appCount_; ++a) if (appCategory_[a] >= 0) newApp_[a] = false;
    if (view_ >= int(categoryCount_)) view_ = LauncherViewAll;
    if (selected_ >= int(appCount_)) selected_ = -1;
    if (menuApp_ >= int(appCount_)) menuApp_ = -1;
    rail_ = RailRowFor(view_);
    EnsureRailVisible();
    ClampScroll();
}

void LauncherModel::FileDefault(uint32_t app) {
    if (appCategory_[app] >= 0 || appKey_[app] == 0xffff) return;
    for (uint32_t c = 0; c < categoryCount_; ++c) {
        Category& cat = categories_[c];
        if (!Equal(cat.id, apps_[app].hint) || cat.count == LL::Members) continue;
        // Alphabetical among the defaults.
        uint32_t at = cat.count;
        for (uint32_t m = 0; m < cat.count; ++m) {
            const int other = AppOfKey(cat.members[m]);
            if (other >= 0 && CompareI(apps_[app].name, apps_[other].name) < 0) { at = m; break; }
        }
        for (uint32_t m = cat.count; m > at; --m) cat.members[m] = cat.members[m - 1];
        cat.members[at] = appKey_[app];
        ++cat.count;
        appCategory_[app] = int16_t(c);
        return;
    }
}

void LauncherModel::SetCatalog(const LauncherApp* apps, uint32_t count) {
    CancelPointer();
    CloseMenu();
    editorIndex_ = -1;
    for (bool& mark : marked_) mark = false;
    if (count > LL::Apps) count = LL::Apps;
    bool wasNew[LL::Keys]{};
    for (uint32_t app = 0; app < appCount_; ++app)
        if (appKey_[app] != 0xffff) wasNew[appKey_[app]] = newApp_[app];
    appCount_ = count;
    for (uint32_t i = 0; i < count; ++i) {
        apps_[i] = apps[i];
        apps_[i].key[LL::KeyLength - 1] = apps_[i].name[LL::NameLength - 1] = 0;
        apps_[i].desc[LL::DescLength - 1] = apps_[i].hint[LL::CategoryId - 1] = 0;
        newApp_[i] = layoutLoaded_ && FindKey(apps_[i].key) < 0;
        const int key = Intern(apps_[i].key);
        appKey_[i] = key < 0 ? 0xffff : uint16_t(key);
        if (key >= 0) newApp_[i] |= wasNew[key];
    }
    Recompute();
    if (!layoutLoaded_) {
        // First run: every installed app in its declared category, the
        // default pins, and a layout to save from now on.
        for (uint32_t i = 0; i < count; ++i) FileDefault(i);
        pinCount_ = 0;
        for (const char* name : DefaultPins)
            for (uint32_t i = 0; i < count && pinCount_ < LL::Pins; ++i)
                if (EqualI(apps_[i].name, name) && appKey_[i] != 0xffff && !Pinned(i)) { pins_[pinCount_++] = appKey_[i]; break; }
        layoutLoaded_ = true;
        Changed();
        Recompute();
    }
}

// --- layout -------------------------------------------------------------------

bool LauncherModel::AddCategory(const char* id, const char* name, uint8_t color, uint8_t icon) {
    if (categoryCount_ == LL::Categories || !*id) return false;
    for (uint32_t c = 0; c < categoryCount_; ++c) if (Equal(categories_[c].id, id)) return false;
    Category& c = categories_[categoryCount_++];
    Copy(c.id, id, sizeof(c.id));
    Copy(c.name, *name ? name : id, sizeof(c.name));
    c.color = color < uint8_t(LauncherColor::Count) ? color : 0;
    c.icon = icon < uint8_t(LauncherCategoryIcon::Count) ? icon : 0;
    c.count = 0;
    return true;
}

void LauncherModel::ResetLayout() {
    categoryCount_ = pinCount_ = recentCount_ = 0;
    for (uint32_t key = 0; key < keyCount_; ++key) aliases_[key][0] = 0;
    for (const auto& d : Defaults) AddCategory(d.id, d.name, uint8_t(d.color), uint8_t(d.icon));
    top_ = LauncherTop::Pinned;
    layoutLoaded_ = false;
    Changed();
    Recompute();
}

void LauncherModel::ResetOrganisation() {
    categoryCount_ = 0;
    for (const auto& d : Defaults) AddCategory(d.id, d.name, uint8_t(d.color), uint8_t(d.icon));
    view_ = LauncherViewAll;
    Recompute();
    for (uint32_t app = 0; app < appCount_; ++app) FileDefault(app);
    Edited(LauncherEdit::Reset);
    Recompute();
}

bool LauncherModel::RestoreLayout(const char* text) {
    const LauncherTop top = top_;
    const uint32_t count = recentCount_;
    uint16_t recent[LL::Recent];
    for (uint32_t i = 0; i < count; ++i) recent[i] = recent_[i];
    if (!Parse(text)) return false;
    top_ = top;
    recentCount_ = count;
    for (uint32_t i = 0; i < count; ++i) recent_[i] = recent[i];
    CancelPointer();
    CloseMenu();
    editorIndex_ = -1;
    view_ = LauncherViewAll;
    rail_ = 0;
    scroll_ = 0;
    lastEdit_ = LauncherEdit::None;
    editNotice_[0] = 0;
    for (bool& mark : marked_) mark = false;
    Changed();
    return true;
}

uint32_t LauncherModel::CategoryAppCount(uint32_t c) const {
    uint32_t n = 0;
    for (uint32_t a = 0; a < appCount_; ++a) n += appCategory_[a] == int(c);
    return n;
}

uint32_t LauncherModel::UnsortedCount() const {
    uint32_t n = 0;
    for (uint32_t a = 0; a < appCount_; ++a) n += appCategory_[a] < 0;
    return n;
}

bool LauncherModel::Pinned(uint32_t app) const {
    for (uint32_t i = 0; i < pinCount_; ++i) if (pins_[i] == appKey_[app]) return true;
    return false;
}

void LauncherModel::TogglePin(uint32_t app) {
    if (app >= appCount_ || appKey_[app] == 0xffff) return;
    const uint16_t key = appKey_[app];
    for (uint32_t i = 0; i < pinCount_; ++i)
        if (pins_[i] == key) {
            for (uint32_t j = i + 1; j < pinCount_; ++j) pins_[j - 1] = pins_[j];
            --pinCount_;
            Edited(LauncherEdit::Pinned);
            return;
        }
    // Pinning adds to the end; the oldest pin gives way when the row is full.
    if (pinCount_ == LL::Pins) {
        for (uint32_t j = 1; j < pinCount_; ++j) pins_[j - 1] = pins_[j];
        --pinCount_;
    }
    pins_[pinCount_++] = key;
    top_ = LauncherTop::Pinned;
    Edited(LauncherEdit::Pinned);
}

void LauncherModel::NoteLaunch(uint32_t app) {
    if (app >= appCount_ || appKey_[app] == 0xffff) return;
    const uint16_t key = appKey_[app];
    uint32_t at = recentCount_ < LL::Recent ? recentCount_ : LL::Recent - 1;
    for (uint32_t i = 0; i < recentCount_; ++i) if (recent_[i] == key) { at = i; break; }
    if (at == 0 && recentCount_ && recent_[0] == key) return;
    for (uint32_t i = at; i > 0; --i) recent_[i] = recent_[i - 1];
    recent_[0] = key;
    if (recentCount_ < LL::Recent && at == recentCount_) ++recentCount_;
    Changed();
}

void LauncherModel::RemoveMember(uint16_t key) {
    for (uint32_t c = 0; c < categoryCount_; ++c) {
        Category& cat = categories_[c];
        for (uint32_t m = 0; m < cat.count; ++m)
            if (cat.members[m] == key) {
                for (uint32_t j = m + 1; j < cat.count; ++j) cat.members[j - 1] = cat.members[j];
                --cat.count;
                return;
            }
    }
}

bool LauncherModel::Step(uint32_t app, int direction) {
    if (app >= appCount_ || appKey_[app] == 0xffff || !categoryCount_ || !direction) return false;
    const uint16_t key = appKey_[app];
    const int home = appCategory_[app];
    if (home < 0) {
        // Unsorted: into the last category, at its end.
        Category& last = categories_[categoryCount_ - 1];
        if (last.count == LL::Members) return false;
        last.members[last.count++] = key;
    } else {
        Category& cat = categories_[home];
        // Neighbours among installed members, so unknown IDs never swallow a step.
        int index = -1;
        for (uint32_t m = 0; m < cat.count; ++m) if (cat.members[m] == key) index = int(m);
        int neighbour = index + direction;
        while (neighbour >= 0 && neighbour < int(cat.count) && AppOfKey(cat.members[neighbour]) < 0) neighbour += direction;
        if (neighbour >= 0 && neighbour < int(cat.count)) {
            cat.members[index] = cat.members[neighbour];
            cat.members[neighbour] = key;
        } else {
            const int next = home + direction;
            if (next < 0 || next >= int(categoryCount_)) return false;
            Category& to = categories_[next];
            if (to.count == LL::Members) return false;
            RemoveMember(key);
            if (direction > 0) {
                for (uint32_t m = to.count; m > 0; --m) to.members[m] = to.members[m - 1];
                to.members[0] = key;
            } else to.members[to.count] = key;
            ++to.count;
            if (view_ == home) view_ = next;
        }
    }
    Edited(LauncherEdit::MoveApps);
    Recompute();
    EnsureVisible();
    return true;
}

bool LauncherModel::RailShows(uint32_t category) const {
    return organising_ || dragging_ || view_ == int(category) || CategoryAppCount(category) != 0;
}

uint32_t LauncherModel::RailRows() const {
    uint32_t count = 1;
    for (uint32_t c = 0; c < categoryCount_; ++c) if (RailShows(c)) ++count;
    return count + (organising_ || dragging_ || UnsortedCount() || view_ == LauncherViewUnsorted);
}

int LauncherModel::RailView(uint32_t row) const {
    if (!row) return LauncherViewAll;
    for (uint32_t c = 0; c < categoryCount_; ++c)
        if (RailShows(c) && !--row) return int(c);
    return LauncherViewUnsorted;
}

int LauncherModel::RailRowFor(int view) const {
    if (view == LauncherViewAll) return 0;
    const uint32_t rows = RailRows();
    for (uint32_t row = 0; row < rows; ++row) if (RailView(row) == view) return int(row);
    return 0;
}

const char* LauncherModel::Name(uint32_t app) const {
    const uint16_t key = appKey_[app];
    return key != 0xffff && aliases_[key][0] ? aliases_[key] : apps_[app].name;
}

bool LauncherModel::RenameApp(uint32_t app, const char* name) {
    if (app >= appCount_ || !ValidLayoutName(name, LL::NameLength, true)) return false;
    const uint16_t key = appKey_[app];
    if (key == 0xffff || Find(keys_[key], "=") >= 0 || Find(keys_[key], "\n") >= 0) return false;
    if (Equal(name, apps_[app].name)) name = "";
    if (Equal(aliases_[key], name)) return false;
    Copy(aliases_[key], name, LL::NameLength);
    Edited(LauncherEdit::Name);
    return true;
}

bool LauncherModel::RenameCategory(uint32_t category, const char* name) {
    if (category >= categoryCount_ || !ValidLayoutName(name, LL::CategoryName, false) ||
        Equal(categories_[category].name, name)) return false;
    Copy(categories_[category].name, name, LL::CategoryName);
    Edited(LauncherEdit::CategorySaved);
    return true;
}

int LauncherModel::CreateCategory() {
    if (categoryCount_ == LL::Categories) return -1;
    for (unsigned n = 1; n <= LL::Categories; ++n) {
        char id[] = "custom-00";
        id[7] = char('0' + n / 10);
        id[8] = char('0' + n % 10);
        if (!AddCategory(id, "New category", uint8_t(LauncherColor::Blue), uint8_t(LauncherCategoryIcon::Folder))) continue;
        Changed();
        return int(categoryCount_ - 1);
    }
    return -1;
}

bool LauncherModel::DeleteCategory(uint32_t category) {
    if (category >= categoryCount_) return false;
    for (uint32_t c = category + 1; c < categoryCount_; ++c) categories_[c - 1] = categories_[c];
    --categoryCount_;
    if (view_ == int(category)) view_ = LauncherViewUnsorted;
    else if (view_ > int(category)) --view_;
    Edited(LauncherEdit::CategoryDeleted);
    Recompute();
    return true;
}

bool LauncherModel::MoveCategory(uint32_t category, uint32_t anchor, bool after) {
    if (category >= categoryCount_ || anchor >= categoryCount_) return false;
    const uint32_t slot = anchor + after;
    const uint32_t dest = slot - (category < slot);
    if (category == dest) return false;
    const Category moving = categories_[category];
    for (uint32_t i = category; i < dest; ++i) categories_[i] = categories_[i + 1];
    for (uint32_t i = category; i > dest; --i) categories_[i] = categories_[i - 1];
    categories_[dest] = moving;
    if (view_ == int(category)) view_ = int(dest);
    else if (view_ > int(category) && view_ <= int(dest)) --view_;
    else if (view_ < int(category) && view_ >= int(dest)) ++view_;
    Edited(LauncherEdit::MoveCategory);
    Recompute();
    return true;
}

uint32_t LauncherModel::MarkedCount() const {
    uint32_t count = 0;
    for (uint32_t app = 0; app < appCount_; ++app) count += marked_[app];
    return count;
}

void LauncherModel::ToggleMarked(uint32_t app) {
    if (app < appCount_) marked_[app] = !marked_[app];
}

void LauncherModel::SetOrganising(bool enabled) {
    organising_ = enabled;
    if (!enabled && view_ >= 0 && !CategoryAppCount(uint32_t(view_))) view_ = LauncherViewAll;
    for (bool& mark : marked_) mark = false;
    rail_ = RailRowFor(view_);
    EnsureRailVisible();
}

void LauncherModel::NoticeMove(const char* name, int category) {
    Writer notice(editNotice_, sizeof(editNotice_));
    notice.Put("Moved ");
    notice.Put(name);
    notice.Put(" to ");
    notice.Put(category < 0 ? "Unsorted" : CategoryName(uint32_t(category)));
    notice.Finish();
}

bool LauncherModel::MoveMarked(uint32_t source, int category, int anchor, bool after) {
    if (source >= appCount_ || (category != LauncherViewUnsorted && (category < 0 || category >= int(categoryCount_)))) return false;
    if (anchor >= int(appCount_) || (anchor >= 0 && (marked_[source] ? marked_[anchor] : anchor == int(source)))) return false;
    uint16_t moving[LL::Apps];
    uint32_t count = 0, coming = 0;
    auto selected = [&](uint32_t app) { return marked_[source] ? marked_[app] : app == source; };
    // Preserve the selection's current relative order, including offscreen rows.
    for (uint32_t c = 0; c < categoryCount_; ++c) {
        const Category& current = categories_[c];
        for (uint32_t m = 0; m < current.count; ++m) {
            const int app = AppOfKey(current.members[m]);
            if (app >= 0 && selected(uint32_t(app))) {
                moving[count++] = current.members[m];
                coming += int(c) != category;
            }
        }
    }
    for (uint32_t a = 0; a < appCount_; ++a)
        if (selected(a) && appCategory_[a] < 0 && appKey_[a] != 0xffff) {
            moving[count++] = appKey_[a];
            ++coming;
        }
    if (!count) return false;
    if (category == LauncherViewUnsorted) {
        bool filed = false;
        for (uint32_t a = 0; a < appCount_; ++a) filed |= selected(a) && appCategory_[a] >= 0;
        if (!filed) return false;
    }
    if (category >= 0 && categories_[category].count + coming > LL::Members) return false;
    for (uint32_t i = 0; i < count; ++i) RemoveMember(moving[i]);
    if (category >= 0) {
        Category& dest = categories_[category];
        uint32_t at = dest.count;
        if (anchor >= 0)
            for (uint32_t m = 0; m < dest.count; ++m)
                if (dest.members[m] == appKey_[anchor]) { at = m + after; break; }
        for (uint32_t m = dest.count; m > at; --m) dest.members[m + count - 1] = dest.members[m - 1];
        for (uint32_t i = 0; i < count; ++i) dest.members[at + i] = moving[i];
        dest.count += count;
    }
    Edited(LauncherEdit::MoveApps);
    NoticeMove(count > 1 ? "selected apps" : Name(source), category);
    Recompute();
    return true;
}

int LauncherModel::Suggestion(uint32_t app) const {
    if (app >= appCount_ || appCategory_[app] >= 0 || appKey_[app] == 0xffff) return -1;
    for (uint32_t c = 0; c < categoryCount_; ++c)
        if (categories_[c].count < LL::Members && Equal(categories_[c].id, apps_[app].hint)) return int(c);
    return -1;
}

LauncherColor LauncherModel::IconColor(uint32_t app) const {
    int category = CategoryOf(app);
    if (category < 0) category = Suggestion(app);
    return category >= 0 ? CategoryColor(uint32_t(category)) : LauncherColor::Slate;
}

bool LauncherModel::SortSuggested() {
    bool changed = false;
    for (uint32_t a = 0; a < appCount_; ++a) {
        if (Suggestion(a) < 0) continue;
        FileDefault(a);
        changed = true;
    }
    if (changed) { Edited(LauncherEdit::Sorted); Recompute(); }
    return changed;
}

bool LauncherModel::MovePin(uint32_t app, int anchor, bool after) {
    if (app >= appCount_ || appKey_[app] == 0xffff || anchor == int(app) ||
        (pinCount_ == LL::Pins && !Pinned(app))) return false;
    const uint16_t key = appKey_[app];
    for (uint32_t i = 0; i < pinCount_; ++i) {
        if (pins_[i] != key) continue;
        for (--pinCount_; i < pinCount_; ++i) pins_[i] = pins_[i + 1];
        break;
    }
    uint32_t at = pinCount_;
    if (anchor >= 0 && anchor < int(appCount_))
        for (uint32_t i = 0; i < pinCount_; ++i)
            if (pins_[i] == appKey_[anchor]) { at = i + after; break; }
    for (uint32_t i = pinCount_++; i > at; --i) pins_[i] = pins_[i - 1];
    pins_[at] = key;
    Edited(LauncherEdit::Pinned);
    return true;
}

void LauncherModel::BeginEditor(uint32_t index, bool app) {
    CloseMenu();
    editorIndex_ = int(index);
    editingApp_ = app;
    editReplace_ = true;
    if (app) Copy(editName_, Name(index), LL::NameLength);
    else {
        Copy(editName_, categories_[index].name, LL::CategoryName);
        editColor_ = categories_[index].color;
        editIcon_ = categories_[index].icon;
    }
}

LauncherResult LauncherModel::EditorPress(const LauncherTarget& t) {
    switch (t.kind) {
    case LauncherTargetKind::EditorSave:
        if (!ValidLayoutName(editName_, editingApp_ ? LL::NameLength : LL::CategoryName, editingApp_)) return {};
        if (editingApp_) RenameApp(uint32_t(editorIndex_), editName_);
        else {
            Category& category = categories_[editorIndex_];
            RenameCategory(uint32_t(editorIndex_), editName_);
            if (category.color != editColor_ || category.icon != editIcon_) {
                category.color = editColor_;
                category.icon = editIcon_;
                Edited(LauncherEdit::CategorySaved);
            }
        }
        editorIndex_ = -1;
        break;
    case LauncherTargetKind::EditorCancel:
        editorIndex_ = -1;
        break;
    case LauncherTargetKind::EditorDelete:
        if (editingApp_) RenameApp(uint32_t(editorIndex_), "");
        else DeleteCategory(uint32_t(editorIndex_));
        editorIndex_ = -1;
        break;
    case LauncherTargetKind::EditorColor:
        if (editingApp_ || t.index >= uint8_t(LauncherColor::Count)) return {};
        editColor_ = uint8_t(t.index);
        break;
    case LauncherTargetKind::EditorIcon:
        if (editingApp_ || t.index >= uint8_t(LauncherCategoryIcon::Count)) return {};
        editIcon_ = uint8_t(t.index);
        break;
    default: return {};
    }
    return {LauncherCommand::Changed, -1};
}

int LauncherModel::TargetApp(LauncherTarget target) const {
    if (target.kind == LauncherTargetKind::Row)
        return target.index < appCount_ ? int(target.index) : -1;
    if (target.kind != LauncherTargetKind::Tile) return -1;
    uint16_t tiles[LL::Pins];
    return target.index < TopTiles(tiles, LL::Pins) ? int(tiles[target.index]) : -1;
}

void LauncherModel::CancelPointer() {
    held_ = drop_ = {LauncherTargetKind::None, 0};
    dragging_ = dropAfter_ = false;
    dropSince_ = 0;
    dragApp_ = dragCategory_ = -1;
}

bool LauncherModel::BeginPointer(LauncherTarget target, int32_t x, int32_t y, bool after) {
    if (MenuOpen() || EditorOpen()) return false;
    dragApp_ = TargetApp(target);
    dragCategory_ = target.kind == LauncherTargetKind::Rail ? RailView(target.index) : LauncherViewAll;
    if (dragApp_ < 0 && dragCategory_ < 0) return false;
    pressX_ = x;
    pressY_ = y;
    held_ = drop_ = target;
    dropAfter_ = after;
    return true;
}

bool LauncherModel::MovePointer(LauncherTarget target, int32_t x, int32_t y, int threshold, bool after, uint64_t now) {
    if (!PointerHeld()) return false;
    const int dx = x - pressX_, dy = y - pressY_;
    if (!dragging_ && dx > -threshold && dx < threshold && dy > -threshold && dy < threshold) return false;
    const bool newTarget = !dragging_ || !(drop_ == target);
    const bool changed = newTarget || dropAfter_ != after;
    if (newTarget) dropSince_ = now;
    dragging_ = true;
    drop_ = target;
    dropAfter_ = after;
    if (dragApp_ >= 0 && !query_[0] && target.kind == LauncherTargetKind::Rail &&
        target.index < RailRows() && now - dropSince_ >= 700) {
        const int view = RailView(target.index);
        if (view_ != view) {
            view_ = view;
            rail_ = target.index;
            scroll_ = 0;
            selected_ = -1;
            return true;
        }
    }
    return changed;
}

LauncherResult LauncherModel::ReleasePointer(LauncherTarget target, bool after) {
    if (!PointerHeld()) return {};
    using TK = LauncherTargetKind;
    if (!dragging_) {
        const bool same = target == held_;
        const auto x = pressX_, y = pressY_;
        CancelPointer();
        return same ? Press(target, false, x, y) : LauncherResult{};
    }
    // Resolve the rail against the drag layout, which also exposes empty destinations.
    const int rail = target.kind == TK::Rail ? RailView(target.index) : LauncherViewAll;
    const int anchor = TargetApp(target);
    const int category = dragCategory_, app = dragApp_;
    CancelPointer();
    if (category >= 0 && rail >= 0) MoveCategory(uint32_t(category), uint32_t(rail), after);
    if (app >= 0) {
        if ((target.kind == TK::Tile && top_ == LauncherTop::Pinned) ||
            (target.kind == TK::TopTab && target.index == uint16_t(LauncherTop::Pinned))) {
            MovePin(uint32_t(app), anchor, after);
        } else if (target.kind == TK::Rail) MoveMarked(uint32_t(app), rail);
        else if (target.kind == TK::CategorySection && !query_[0])
            MoveMarked(uint32_t(app), target.index == LL::Categories ? LauncherViewUnsorted : int(target.index));
        else if (target.kind == TK::Row && anchor >= 0 && !query_[0]) {
            const int home = CategoryOf(uint32_t(anchor));
            MoveMarked(uint32_t(app), home < 0 ? LauncherViewUnsorted : home, anchor, after);
        } else if (target.kind == TK::NewCategory) {
            const int created = CreateCategory();
            if (created >= 0) {
                if (MoveMarked(uint32_t(app), created)) {
                    view_ = created;
                    scroll_ = 0;
                    rail_ = RailRowFor(view_);
                    BeginEditor(uint32_t(created), false);
                } else DeleteCategory(uint32_t(created));
            }
        }
    }
    rail_ = RailRowFor(view_);
    EnsureRailVisible();
    return {LauncherCommand::Changed, -1};
}

uint32_t LauncherModel::TopTiles(uint16_t* out, uint32_t capacity) const {
    const uint16_t* list = top_ == LauncherTop::Pinned ? pins_ : recent_;
    const uint32_t count = top_ == LauncherTop::Pinned ? pinCount_ : recentCount_;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < capacity; ++i) {
        const int app = AppOfKey(list[i]);
        if (app >= 0) out[n++] = uint16_t(app);
    }
    return n;
}

void LauncherModel::SetTop(LauncherTop top) {
    if (top_ == top) return;
    top_ = top;
    tile_ = 0;
    Changed();
}

// --- persistence -------------------------------------------------------------

uint32_t LauncherModel::Serialize(char* out, uint32_t capacity) const {
    Writer w(out, capacity);
    auto keyList = [&](const uint16_t* list, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) { if (i) w.Char(','); w.Put(keys_[list[i]]); }
    };
    w.Put("[Launcher]\nversion=1\ntop_row=");
    w.Put(top_ == LauncherTop::Pinned ? "pinned" : "recent");
    w.Put("\npinned="); keyList(pins_, pinCount_);
    w.Put("\nrecent="); keyList(recent_, recentCount_);
    w.Put("\ncategories=");
    for (uint32_t c = 0; c < categoryCount_; ++c) { if (c) w.Char(','); w.Put(categories_[c].id); }
    w.Char('\n');
    for (uint32_t c = 0; c < categoryCount_; ++c) {
        const Category& cat = categories_[c];
        w.Put("category."); w.Put(cat.id); w.Char('='); w.Put(cat.name); w.Char('|');
        w.Put(ColorNames[cat.color]); w.Char('|'); w.Put(IconNames[cat.icon]); w.Char('|');
        keyList(cat.members, cat.count);
        w.Char('\n');
    }
    for (uint32_t key = 0; key < keyCount_; ++key) {
        if (!aliases_[key][0]) continue;
        w.Put("name."); w.Put(keys_[key]); w.Char('='); w.Put(aliases_[key]); w.Char('\n');
    }
    return w.Finish();
}

bool LauncherModel::Parse(const char* text, bool* migrated) {
    if (migrated) *migrated = false;
    bool remapFiles = false;
    // Parse into a copy; any structural error keeps the defaults (never an error).
    LauncherModel& self = *this;
    const uint32_t savedKeys = keyCount_;
    uint32_t catCount = 0;
    Category cats[LL::Categories];
    uint16_t pins[LL::Pins], recent[LL::Recent];
    uint32_t pinN = 0, recentN = 0;
    LauncherTop top = LauncherTop::Pinned;
    bool section = false, version = false, ok = true;
    char order[LL::Categories][LL::CategoryId];
    uint32_t orderN = 0;
    auto keyList = [&](const char* v, const char* end, uint16_t* list, uint32_t capacity, uint32_t& n) {
        while (v < end) {
            const char* comma = v;
            while (comma < end && *comma != ',') ++comma;
            char key[LL::KeyLength];
            uint32_t len = uint32_t(comma - v) < LL::KeyLength - 1 ? uint32_t(comma - v) : LL::KeyLength - 1;
            for (uint32_t i = 0; i < len; ++i) key[i] = v[i];
            key[len] = 0;
            // The file browser had a desktop ID in the hardware profile and
            // a shortcut basename before native shortcuts carried app IDs.
            const bool oldFiles = Equal(key, "brook-files") || Equal(key, "brook-hardware-files") ||
                                  Equal(key, "files") || Equal(key, "Files");
            remapFiles |= oldFiles;
            const int k = len ? self.Intern(oldFiles ? LauncherFilesAppId : key) : -1;
            bool dup = false;
            for (uint32_t i = 0; i < n; ++i) dup |= list[i] == k;
            if (k >= 0 && !dup && n < capacity) list[n++] = uint16_t(k);
            v = comma + 1;
        }
    };
    for (const char* line = text; line && *line;) {
        const char* end = line;
        while (*end && *end != '\n') ++end;
        const char* stop = end;
        while (stop > line && (stop[-1] == '\r' || stop[-1] == ' ')) --stop;
        const char* p = line;
        while (p < stop && (*p == ' ' || *p == '\t')) ++p;
        if (p < stop && *p == '[') {
            section = uint32_t(stop - p) == 10 && p[1] == 'L' && Find(p, "[Launcher]") == 0;
        } else if (section && p < stop && *p != '#' && *p != ';') {
            const char* eq = p;
            while (eq < stop && *eq != '=') ++eq;
            if (eq == stop) { ok = false; break; }
            const uint32_t keyLen = uint32_t(eq - p);
            const char* v = eq + 1;
            auto is = [&](const char* k) { return Length(k) == keyLen && Find(p, k) == 0; };
            if (is("version")) version = stop - v == 1 && *v == '1';
            else if (is("top_row")) top = stop - v == 6 && Find(v, "recent") == 0 ? LauncherTop::Recent : LauncherTop::Pinned;
            else if (is("pinned")) keyList(v, stop, pins, LL::Pins, pinN);
            else if (is("recent")) keyList(v, stop, recent, LL::Recent, recentN);
            else if (is("categories")) {
                while (v < stop && orderN < LL::Categories) {
                    const char* comma = v;
                    while (comma < stop && *comma != ',') ++comma;
                    const uint32_t len = uint32_t(comma - v);
                    if (len && len < LL::CategoryId) {
                        for (uint32_t i = 0; i < len; ++i) order[orderN][i] = v[i];
                        order[orderN++][len] = 0;
                    }
                    v = comma + 1;
                }
            } else if (keyLen > 9 && Find(p, "category.") == 0 && catCount < LL::Categories) {
                Category& c = cats[catCount];
                const uint32_t idLen = keyLen - 9;
                if (idLen >= LL::CategoryId) { ok = false; break; }
                for (uint32_t i = 0; i < idLen; ++i) c.id[i] = p[9 + i];
                c.id[idLen] = 0;
                const char* fields[4] = {v, nullptr, nullptr, nullptr};
                const char* fieldEnd[4] = {stop, stop, stop, stop};
                uint32_t f = 1;
                for (const char* q = v; q < stop && f < 4; ++q)
                    if (*q == '|') { fieldEnd[f - 1] = q; fields[f++] = q + 1; }
                if (f != 4) { ok = false; break; }
                uint32_t nameLen = uint32_t(fieldEnd[0] - fields[0]);
                if (nameLen >= LL::CategoryName) nameLen = LL::CategoryName - 1;
                for (uint32_t i = 0; i < nameLen; ++i) c.name[i] = fields[0][i];
                c.name[nameLen] = 0;
                const int color = Lookup(ColorNames, 8, fields[1], uint32_t(fieldEnd[1] - fields[1]));
                const int icon = Lookup(IconNames, uint32_t(LauncherCategoryIcon::Count), fields[2], uint32_t(fieldEnd[2] - fields[2]));
                c.color = uint8_t(color < 0 ? 0 : color);
                c.icon = uint8_t(icon < 0 ? 0 : icon);
                uint32_t n = 0;
                keyList(fields[3], fieldEnd[3], c.members, LL::Members, n);
                c.count = uint16_t(n);
                ++catCount;
            }
        }
        if (!*end) break;
        line = end + 1;
    }
    if (!ok || !version) { keyCount_ = savedKeys; return false; }
    // Apply aliases only after the layout header and structure are accepted.
    for (uint32_t key = 0; key < keyCount_; ++key) aliases_[key][0] = 0;
    section = false;
    for (const char* line = text; *line;) {
        const char* end = line;
        while (*end && *end != '\n') ++end;
        const char* stop = end;
        while (stop > line && (stop[-1] == '\r' || stop[-1] == ' ')) --stop;
        while (line < stop && (*line == ' ' || *line == '\t')) ++line;
        if (line < stop && *line == '[') {
            section = stop - line == 10 && Find(line, "[Launcher]") == 0;
        } else if (section && Find(line, "name.") == 0) {
            const char* eq = line + 5;
            while (eq < stop && *eq != '=') ++eq;
            const auto keyLen = eq - line - 5;
            const auto nameLen = stop - eq - 1;
            if (keyLen > 0 && keyLen < LL::KeyLength && nameLen > 0 && nameLen < LL::NameLength) {
                char key[LL::KeyLength], name[LL::NameLength];
                for (int i = 0; i < keyLen; ++i) key[i] = line[5 + i];
                key[keyLen] = 0;
                for (int i = 0; i < nameLen; ++i) name[i] = eq[1 + i];
                name[nameLen] = 0;
                const int k = ValidLayoutName(name, LL::NameLength, false) ? Intern(key) : -1;
                if (k >= 0) Copy(aliases_[k], name, LL::NameLength);
            }
        }
        line = *end ? end + 1 : end;
    }
    // Categories in the listed order; a listed ID without its line is empty.
    categoryCount_ = 0;
    for (uint32_t o = 0; o < orderN; ++o) {
        bool found = false;
        for (uint32_t c = 0; c < catCount && !found; ++c)
            if (Equal(cats[c].id, order[o])) {
                bool dup = false;
                for (uint32_t d = 0; d < categoryCount_; ++d) dup |= Equal(categories_[d].id, order[o]);
                if (!dup && categoryCount_ < LL::Categories) categories_[categoryCount_++] = cats[c];
                found = true;
            }
        if (!found) AddCategory(order[o], order[o], 0, 0);
    }
    // One home per app: a later category never claims an app twice.
    for (uint32_t c = 1; c < categoryCount_; ++c) {
        Category& cat = categories_[c];
        uint32_t kept = 0;
        for (uint32_t m = 0; m < cat.count; ++m) {
            bool earlier = false;
            for (uint32_t d = 0; d < c && !earlier; ++d)
                for (uint32_t e = 0; e < categories_[d].count; ++e) earlier |= categories_[d].members[e] == cat.members[m];
            if (!earlier) cat.members[kept++] = cat.members[m];
        }
        cat.count = uint16_t(kept);
    }
    for (uint32_t i = 0; i < pinN; ++i) pins_[i] = pins[i];
    for (uint32_t i = 0; i < recentN; ++i) recent_[i] = recent[i];
    pinCount_ = pinN; recentCount_ = recentN; top_ = top;
    layoutLoaded_ = true;
    if (remapFiles) Changed();
    if (migrated) *migrated = remapFiles;
    Recompute();
    return true;
}

// --- search ---------------------------------------------------------------------

int LauncherModel::Rank(const char* name, const char* category, const char* desc, const char* term) {
    if (!term || !*term) return 9;
    const int at = Find(name, term);
    if (at == 0) return 0;
    if (WordStart(name, term)) return 1;
    if (at > 0) return 2;
    if (category && Find(category, term) >= 0) return 3;
    if (desc && Find(desc, term) >= 0) return 4;
    return 9;
}

const char* LauncherModel::HintFromDesktopCategories(const char* list) {
    struct Map { const char* token; const char* id; };
    // First match wins, most specific first.
    static constexpr Map map[] = {
        {"Game", "games"}, {"WebBrowser", "internet"}, {"Network", "internet"}, {"Email", "internet"},
        {"AudioVideo", "media"}, {"Audio", "media"}, {"Video", "media"}, {"Player", "media"},
        {"Graphics", "graphics"}, {"Office", "office"}, {"TextEditor", "office"}, {"Calculator", "office"},
        {"WordProcessor", "office"}, {"Spreadsheet", "office"}, {"Development", "system"},
        {"TerminalEmulator", "system"}, {"FileManager", "system"}, {"Settings", "system"}, {"System", "system"},
        {"Utility", "system"},
    };
    if (!list) return "";
    for (const Map& m : map) {
        const uint32_t n = Length(m.token);
        for (const char* p = list; *p;) {
            const char* end = p;
            while (*end && *end != ';') ++end;
            if (uint32_t(end - p) == n && Find(p, m.token) == 0) return m.id;
            p = *end ? end + 1 : end;
        }
    }
    return "";
}

uint32_t LauncherModel::SearchResults(uint16_t* out, uint32_t capacity) const {
    uint32_t n = 0;
    int ranks[LL::Apps];
    for (uint32_t a = 0; a < appCount_ && n < capacity; ++a) {
        const int c = appCategory_[a];
        const int rank = Rank(Name(a), c >= 0 ? categories_[c].name : "Unsorted", apps_[a].desc, query_);
        if (rank >= 9) continue;
        // Insertion by (rank, name).
        uint32_t at = n;
        while (at > 0 && (ranks[at - 1] > rank ||
                          (ranks[at - 1] == rank && CompareI(Name(out[at - 1]), Name(a)) > 0))) {
            out[at] = out[at - 1]; ranks[at] = ranks[at - 1]; --at;
        }
        out[at] = uint16_t(a); ranks[at] = rank;
        ++n;
    }
    return n;
}

uint32_t LauncherModel::SearchCategories(uint16_t* out, uint32_t capacity) const {
    uint32_t n = 0;
    if (!query_[0]) return 0;
    for (uint32_t c = 0; c < categoryCount_ && n < capacity; ++c)
        if (Find(categories_[c].name, query_) >= 0) out[n++] = uint16_t(c);
    return n;
}

// --- lines --------------------------------------------------------------------

uint32_t LauncherModel::Lines(LauncherLine* out, uint32_t capacity) const {
    uint32_t n = 0;
    auto push = [&](LauncherLineKind kind, int category, uint16_t count, uint8_t columns = 0) {
        if (n == capacity) return (LauncherLine*)nullptr;
        out[n] = {kind, columns, int16_t(category), {0, 0}, count};
        return &out[n++];
    };
    auto rows = [&](const uint16_t* apps, uint32_t count, uint8_t columns) {
        for (uint32_t i = 0; i < count; i += columns) {
            LauncherLine* line = push(LauncherLineKind::Apps, 0, 0, columns);
            if (!line) return;
            for (uint32_t j = 0; j < columns && i + j < count; ++j) { line->apps[j] = apps[i + j]; ++line->count; }
        }
    };
    uint16_t list[LL::Apps];
    auto members = [&](int c) {
        uint32_t count = 0;
        if (c >= 0) {
            for (uint32_t m = 0; m < categories_[c].count; ++m) {
                const int app = AppOfKey(categories_[c].members[m]);
                if (app >= 0 && appCategory_[app] == c) list[count++] = uint16_t(app);
            }
            return count;
        }
        // Unsorted, alphabetical.
        for (uint32_t a = 0; a < appCount_; ++a) {
            if (appCategory_[a] >= 0) continue;
            uint32_t at = count;
            while (at > 0 && CompareI(Name(list[at - 1]), Name(a)) > 0) { list[at] = list[at - 1]; --at; }
            list[at] = uint16_t(a);
            ++count;
        }
        return count;
    };
    if (query_[0]) {
        uint16_t cats[LL::Categories];
        const uint32_t catN = SearchCategories(cats, LL::Categories);
        if (catN) { push(LauncherLineKind::Heading, -4, uint16_t(catN)); push(LauncherLineKind::Chips, -4, uint16_t(catN)); }
        const uint32_t hits = SearchResults(list, LL::Apps);
        if (hits) { push(LauncherLineKind::Heading, -3, uint16_t(hits)); rows(list, hits, 1); }
        else if (!catN) push(LauncherLineKind::Empty, -3, 0);
        return n;
    }
    auto section = [&](int c, bool always) {
        const uint32_t count = members(c);
        if (!count && !always) return;
        push(LauncherLineKind::Heading, c < 0 ? LauncherViewUnsorted : c, uint16_t(count));
        if (count) rows(list, count, c < 0 ? 1 : 2);
        else push(LauncherLineKind::Empty, c < 0 ? LauncherViewUnsorted : c, 0);
    };
    if (view_ == LauncherViewAll) {
        for (uint32_t c = 0; c < categoryCount_; ++c) section(int(c), false);
        section(-1, false);
        if (!n) push(LauncherLineKind::Empty, LauncherViewAll, 0);
    } else section(view_ == LauncherViewUnsorted ? -1 : view_, true);
    return n;
}

uint32_t LauncherModel::ItemOrder(uint16_t* out, uint32_t capacity) const {
    LauncherLine lines[LL::Lines];
    const uint32_t n = Lines(lines, LL::Lines);
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (lines[i].kind == LauncherLineKind::Apps)
            for (uint32_t j = 0; j < lines[i].count && count < capacity; ++j) out[count++] = lines[i].apps[j];
    return count;
}

int LauncherModel::LineHeight(const LauncherLine& line) const {
    switch (line.kind) {
    case LauncherLineKind::Heading: return viewport_.heading;
    case LauncherLineKind::Apps: return viewport_.row;
    case LauncherLineKind::Chips: return viewport_.chips;
    case LauncherLineKind::Empty: return viewport_.empty;
    }
    return viewport_.row;
}

uint32_t LauncherModel::ContentHeight() const {
    LauncherLine lines[LL::Lines];
    const uint32_t n = Lines(lines, LL::Lines);
    int used = 0;
    for (uint32_t i = 0; i < n; ++i) used += LineHeight(lines[i]);
    return used > 0 ? uint32_t(used) : 0;
}

uint32_t LauncherModel::MaxScroll() const {
    const uint32_t total = ContentHeight();
    const uint32_t page = viewport_.height > 0 ? uint32_t(viewport_.height) : 0;
    return total > page ? total - page : 0;
}

void LauncherModel::SetViewport(const LauncherViewport& v) {
    const bool railResized = viewport_.railRows != v.railRows;
    viewport_ = v;
    if (railResized) EnsureRailVisible();
    ClampScroll();
}

bool LauncherModel::ScrollRail(int rows) {
    if (viewport_.railRows <= 0) return false;
    const int limit = int(RailRows()) - viewport_.railRows;
    const int offset = int(railScroll_) + rows;
    const uint32_t next = uint32_t(offset <= 0 || limit <= 0 ? 0 : offset > limit ? limit : offset);
    if (next == railScroll_) return false;
    railScroll_ = next;
    return true;
}

void LauncherModel::EnsureRailVisible() {
    if (viewport_.railRows > 0 && (rail_ < int(railScroll_) || rail_ >= int(railScroll_) + viewport_.railRows))
        ScrollRail(rail_ < int(railScroll_) ? rail_ - int(railScroll_) : rail_ - int(railScroll_) - viewport_.railRows + 1);
    else ScrollRail(0);
}

void LauncherModel::ClampScroll() {
    const uint32_t max = MaxScroll();
    if (scroll_ > max) scroll_ = max;
}

bool LauncherModel::ScrollBy(int pixels) {
    const uint32_t before = scroll_;
    if (pixels < 0) scroll_ = uint32_t(-pixels) > scroll_ ? 0 : scroll_ - uint32_t(-pixels);
    else scroll_ += uint32_t(pixels);
    ClampScroll();
    return scroll_ != before;
}

int LauncherModel::SelectedLine() const {
    if (selected_ < 0) return -1;
    LauncherLine lines[LL::Lines];
    const uint32_t n = Lines(lines, LL::Lines);
    for (uint32_t i = 0; i < n; ++i)
        if (lines[i].kind == LauncherLineKind::Apps)
            for (uint32_t j = 0; j < lines[i].count; ++j) if (lines[i].apps[j] == selected_) return int(i);
    return -1;
}

void LauncherModel::EnsureVisible() {
    const int line = SelectedLine();
    if (line < 0) { ClampScroll(); return; }
    LauncherLine lines[LL::Lines];
    const uint32_t n = Lines(lines, LL::Lines);
    // Keep the section heading in view with its first row.
    uint32_t first = uint32_t(line);
    if (first > 0 && lines[first - 1].kind == LauncherLineKind::Heading) --first;
    int top = 0, y = 0, bottom = 0;
    for (uint32_t i = 0; i < n && i <= uint32_t(line); ++i) {
        if (i == first) top = y;
        y += LineHeight(lines[i]);
        if (i == uint32_t(line)) bottom = y;
    }
    if (bottom - int(scroll_) > viewport_.height) scroll_ = uint32_t(bottom - viewport_.height > 0 ? bottom - viewport_.height : 0);
    if (top < int(scroll_)) scroll_ = uint32_t(top);
    ClampScroll();
}

// --- interaction -----------------------------------------------------------------

void LauncherModel::Open() {
    query_[0] = 0;
    view_ = LauncherViewAll;
    section_ = LauncherSection::List;
    selected_ = -1; tile_ = 0; rail_ = 0;
    keyboard_ = false;
    scroll_ = railScroll_ = 0;
    menuApp_ = editorIndex_ = -1;
    SetOrganising(false);
    CancelPointer();
}

void LauncherModel::OpenMenu(uint32_t app, int32_t x, int32_t y, bool keyboard) {
    if (app >= appCount_) return;
    menuApp_ = int(app);
    menuItem_ = 0;
    menuX_ = x; menuY_ = y;
    keyboard_ = keyboard;
}

LauncherResult LauncherModel::Activate(int app) {
    if (app < 0 || app >= int(appCount_)) return {};
    if (organising_) {
        selected_ = app;
        ToggleMarked(uint32_t(app));
        return {LauncherCommand::Changed, -1};
    }
    NoteLaunch(uint32_t(app));
    return {LauncherCommand::Launch, app};
}

LauncherResult LauncherModel::Key(LauncherKey key, char ch, uint8_t mods) {
    const LauncherResult changed{LauncherCommand::Changed, -1};
    const bool alt = mods & LauncherModAlt;
    if (EditorOpen()) {
        if (key == LauncherKey::Escape) return EditorPress({LauncherTargetKind::EditorCancel, 0});
        if (key == LauncherKey::Enter) return EditorPress({LauncherTargetKind::EditorSave, 0});
        if (key == LauncherKey::Backspace) {
            const uint32_t n = Length(editName_);
            if (n) editName_[editReplace_ ? 0 : n - 1] = 0;
            editReplace_ = false;
            return changed;
        }
        if (key == LauncherKey::Char && !(mods & LauncherModCtrl) && uint8_t(ch) >= 32 && uint8_t(ch) < 127 && ch != '|') {
            const uint32_t n = editReplace_ ? 0 : Length(editName_);
            if (n + 1 < (editingApp_ ? LL::NameLength : LL::CategoryName)) {
                editName_[n] = ch;
                editName_[n + 1] = 0;
                editReplace_ = false;
            }
            return changed;
        }
        return {};
    }
    if (key == LauncherKey::Char && (ch == 'z' || ch == 'Z') && (mods & LauncherModCtrl))
        return {LauncherCommand::Undo, -1};
    if (key == LauncherKey::Escape && PointerHeld()) { CancelPointer(); return changed; }
    if (key == LauncherKey::Char && ch == ' ' && (mods & LauncherModCtrl) && organising_ && selected_ >= 0) {
        ToggleMarked(uint32_t(selected_));
        return changed;
    }
    if (key == LauncherKey::Rename) {
        const int category = RailView(uint32_t(rail_));
        if (section_ == LauncherSection::Rail && category >= 0) BeginEditor(uint32_t(category), false);
        else if (section_ == LauncherSection::List && selected_ >= 0) BeginEditor(uint32_t(selected_), true);
        else return {};
        return changed;
    }
    if (MenuOpen()) {
        // The menu is modal: two items, Uninstall disabled.
        switch (key) {
        case LauncherKey::Escape: case LauncherKey::Menu: CloseMenu(); return changed;
        case LauncherKey::Enter: {
            const int app = menuApp_;
            CloseMenu();
            if (menuItem_ == 0) TogglePin(uint32_t(app));
            return changed;
        }
        default: return {};
        }
    }
    uint16_t order[LL::Apps];
    const uint32_t items = ItemOrder(order, LL::Apps);
    uint16_t tiles[LL::Pins];
    const uint32_t tileCount = query_[0] ? 0 : TopTiles(tiles, LL::Pins);
    auto requery = [&]() {
        scroll_ = 0;
        section_ = LauncherSection::List;
        // The best match is selected (Enter opens it); a cleared query
        // starts the list afresh.
        uint16_t first[LL::Apps];
        selected_ = query_[0] && ItemOrder(first, LL::Apps) ? first[0] : -1;
        keyboard_ = query_[0] != 0;
        return changed;
    };
    auto position = [&]() {
        for (uint32_t i = 0; i < items; ++i) if (order[i] == selected_) return int(i);
        return -1;
    };
    auto select = [&](int i) {
        if (!items) return changed;
        selected_ = order[(i % int(items) + int(items)) % int(items)];
        keyboard_ = true;
        EnsureVisible();
        return changed;
    };
    auto setView = [&](uint32_t row) {
        rail_ = int(row);
        EnsureRailVisible();
        view_ = RailView(row);
        scroll_ = 0;
        selected_ = -1;
        keyboard_ = true;
        return changed;
    };
    switch (key) {
    case LauncherKey::Char: {
        if (ch == ' ' && !query_[0]) {
            if (section_ == LauncherSection::Tiles && tileCount) return Activate(tiles[tile_ % tileCount]);
            if (section_ == LauncherSection::Rail) { section_ = LauncherSection::List; return changed; }
            return {};
        }
        if (uint8_t(ch) < 32 || uint8_t(ch) > 126) return {};
        const uint32_t n = Length(query_);
        if (n + 1 >= LL::Query) return {};
        query_[n] = ch; query_[n + 1] = 0;
        return requery();
    }
    case LauncherKey::Backspace: {
        const uint32_t n = Length(query_);
        if (!n) return {};
        query_[n - 1] = 0;
        return requery();
    }
    case LauncherKey::Escape:
        if (query_[0]) { query_[0] = 0; return requery(); }
        if (organising_) { SetOrganising(false); return changed; }
        return {LauncherCommand::Close, -1};
    case LauncherKey::Tab: case LauncherKey::BackTab: {
        const LauncherSection cycle[] = {LauncherSection::List, LauncherSection::Tiles, LauncherSection::Rail};
        int i = section_ == LauncherSection::List ? 0 : section_ == LauncherSection::Tiles ? 1 : 2;
        for (int step = 0; step < 3; ++step) {
            i = (i + (key == LauncherKey::Tab ? 1 : 2)) % 3;
            if (cycle[i] == LauncherSection::Tiles && !tileCount) continue;
            if (cycle[i] == LauncherSection::Rail && query_[0]) continue;
            break;
        }
        section_ = cycle[i];
        if (section_ == LauncherSection::Tiles && tile_ >= int(tileCount)) tile_ = 0;
        if (section_ == LauncherSection::List && selected_ < 0 && items) selected_ = order[0];
        keyboard_ = true;
        return changed;
    }
    case LauncherKey::Up: case LauncherKey::Down: {
        const int d = key == LauncherKey::Down ? 1 : -1;
        if (section_ == LauncherSection::Rail) {
            const int rows = int(RailRows());
            const uint32_t next = uint32_t(((rail_ + d) % rows + rows) % rows);
            if (alt && RailView(uint32_t(rail_)) >= 0 && RailView(next) >= 0) {
                MoveCategory(uint32_t(RailView(uint32_t(rail_))), uint32_t(RailView(next)), d > 0);
                return changed;
            }
            return setView(next);
        }
        if (section_ == LauncherSection::Tiles) {
            const int next = tile_ + 5 * d;
            if (next >= 0 && next < int(tileCount)) { tile_ = next; return changed; }
            if (d > 0) { section_ = LauncherSection::List; return select(0); }
            return {};
        }
        if (alt) {
            if (query_[0] || selected_ < 0) return {};
            return Step(uint32_t(selected_), d) ? changed : LauncherResult{};
        }
        const int at = position();
        return select(at < 0 ? (d > 0 ? 0 : -1) : at + d);
    }
    case LauncherKey::Left: case LauncherKey::Right: {
        const int d = key == LauncherKey::Right ? 1 : -1;
        if (section_ == LauncherSection::Tiles) {
            if (!tileCount) return {};
            tile_ = ((tile_ + d) % int(tileCount) + int(tileCount)) % int(tileCount);
            keyboard_ = true;
            return changed;
        }
        if (section_ == LauncherSection::Rail) {
            if (d > 0) { section_ = LauncherSection::List; if (selected_ < 0 && items) selected_ = order[0]; keyboard_ = true; return changed; }
            return {};
        }
        if (query_[0]) return {};
        if (d < 0) {
            // Left of the first column: the rail, on the current view.
            LauncherLine lines[LL::Lines];
            const uint32_t n = Lines(lines, LL::Lines);
            const int line = SelectedLine();
            if (line >= 0 && uint32_t(line) < n && lines[line].apps[0] != selected_) return select(position() - 1);
            section_ = LauncherSection::Rail;
            rail_ = RailRowFor(view_);
            keyboard_ = true;
            return changed;
        }
        return select(position() + 1);
    }
    case LauncherKey::Home: return section_ == LauncherSection::List ? select(0) : LauncherResult{};
    case LauncherKey::End: return section_ == LauncherSection::List ? select(int(items) - 1) : LauncherResult{};
    case LauncherKey::PageUp: case LauncherKey::PageDown: {
        if (section_ != LauncherSection::List) return {};
        const int page = viewport_.row > 0 ? viewport_.height / viewport_.row * 2 : 8;
        const int at = position() < 0 ? 0 : position();
        int to = at + (key == LauncherKey::PageDown ? page : -page);
        if (to < 0) to = 0;
        if (to >= int(items)) to = int(items) - 1;
        return select(to);
    }
    case LauncherKey::Rename: return {};
    case LauncherKey::Delete: {
        const int category = RailView(uint32_t(rail_));
        if (organising_ && section_ == LauncherSection::Rail && category >= 0) {
            BeginEditor(uint32_t(category), false);
            return changed;
        }
        return {};
    }
    case LauncherKey::Menu:
        if (section_ == LauncherSection::Tiles && tileCount) { OpenMenu(tiles[tile_ % tileCount], -1, -1, true); return changed; }
        if (section_ == LauncherSection::List && selected_ >= 0) { OpenMenu(uint32_t(selected_), -1, -1, true); return changed; }
        return {};
    case LauncherKey::Enter:
        if (alt) return Key(LauncherKey::Menu);
        if (section_ == LauncherSection::Tiles) return tileCount ? Activate(tiles[tile_ % tileCount]) : LauncherResult{};
        if (section_ == LauncherSection::Rail) { section_ = LauncherSection::List; selected_ = -1; return changed; }
        if (selected_ >= 0) return Activate(selected_);
        return items ? Activate(order[0]) : LauncherResult{};
    }
    return {};
}

LauncherResult LauncherModel::Press(const LauncherTarget& t, bool secondary, int32_t x, int32_t y) {
    const LauncherResult changed{LauncherCommand::Changed, -1};
    if (EditorOpen()) return secondary ? LauncherResult{} : EditorPress(t);
    if (MenuOpen()) {
        if (t.kind == LauncherTargetKind::Menu) return {};
        if (t.kind == LauncherTargetKind::MenuItem) {
            if (t.index != 0) return {}; // Uninstall is disabled
            const int app = menuApp_;
            CloseMenu();
            TogglePin(uint32_t(app));
            return changed;
        }
        CloseMenu();
        if (!secondary) return changed; // the press only dismisses the menu
    }
    keyboard_ = false;
    uint16_t tiles[LL::Pins];
    const uint32_t tileCount = TopTiles(tiles, LL::Pins);
    switch (t.kind) {
    case LauncherTargetKind::Organise:
        if (secondary) return {};
        SetOrganising(!organising_);
        return changed;
    case LauncherTargetKind::Undo:
        return secondary ? LauncherResult{} : LauncherResult{LauncherCommand::Undo, -1};
    case LauncherTargetKind::ResetOrganisation:
        if (secondary || !organising_) return {};
        ResetOrganisation();
        return changed;
    case LauncherTargetKind::EditCategory:
        if (t.index >= categoryCount_) return {};
        BeginEditor(t.index, false);
        return changed;
    case LauncherTargetKind::ClearSelection:
        if (secondary) return {};
        for (bool& mark : marked_) mark = false;
        return changed;
    case LauncherTargetKind::RenameApp:
        if (secondary || MarkedCount() != 1) return {};
        for (uint32_t a = 0; a < appCount_; ++a) {
            if (!marked_[a]) continue;
            BeginEditor(a, true);
            return changed;
        }
        return {};
    case LauncherTargetKind::MoveSuggested:
        if (!secondary && Suggestion(t.index) >= 0) {
            FileDefault(t.index);
            Edited(LauncherEdit::Sorted);
            NoticeMove(Name(t.index), CategoryOf(t.index));
            Recompute();
            return changed;
        }
        return {};
    case LauncherTargetKind::SortSuggested:
        return !secondary && SortSuggested() ? changed : LauncherResult{};
    case LauncherTargetKind::TopTab: SetTop(t.index ? LauncherTop::Recent : LauncherTop::Pinned); return changed;
    case LauncherTargetKind::Tile:
        if (t.index >= tileCount) return {};
        if (secondary) { OpenMenu(tiles[t.index], x, y, false); return changed; }
        return Activate(tiles[t.index]);
    case LauncherTargetKind::Row:
        if (t.index >= appCount_) return {};
        if (secondary) { selected_ = t.index; OpenMenu(t.index, x, y, false); return changed; }
        return Activate(t.index);
    case LauncherTargetKind::Rail:
        if (t.index >= RailRows()) return {};
        if (secondary) {
            const int category = RailView(t.index);
            if (category < 0) return {};
            BeginEditor(uint32_t(category), false);
            return changed;
        }
        query_[0] = 0;
        rail_ = t.index;
        view_ = RailView(t.index);
        scroll_ = 0;
        selected_ = -1;
        section_ = LauncherSection::List;
        return changed;
    case LauncherTargetKind::Chip:
        if (secondary || t.index >= categoryCount_) return {};
        query_[0] = 0;
        view_ = t.index;
        rail_ = RailRowFor(view_);
        EnsureRailVisible();
        scroll_ = 0;
        selected_ = -1;
        return changed;
    case LauncherTargetKind::Settings: return secondary ? LauncherResult{} : LauncherResult{LauncherCommand::OpenSettings, -1};
    default: return {};
    }
}

} // namespace brook
