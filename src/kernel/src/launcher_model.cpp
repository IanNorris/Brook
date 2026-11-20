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
constexpr const char* IconNames[] = {"folder", "sliders", "globe", "play", "doc", "brush", "pad", "spark"};

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
    layoutEpoch_ = 0;
    viewport_ = {36, 48, 40, 96, 400, 20};
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
    if (view_ >= int(categoryCount_)) view_ = LauncherViewAll;
    if (selected_ >= int(appCount_)) selected_ = -1;
    if (menuApp_ >= int(appCount_)) menuApp_ = -1;
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
    if (count > LL::Apps) count = LL::Apps;
    appCount_ = count;
    for (uint32_t i = 0; i < count; ++i) {
        apps_[i] = apps[i];
        apps_[i].key[LL::KeyLength - 1] = apps_[i].name[LL::NameLength - 1] = 0;
        apps_[i].desc[LL::DescLength - 1] = apps_[i].hint[LL::CategoryId - 1] = 0;
        const int key = Intern(apps_[i].key);
        appKey_[i] = key < 0 ? 0xffff : uint16_t(key);
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
    for (const auto& d : Defaults) AddCategory(d.id, d.name, uint8_t(d.color), uint8_t(d.icon));
    top_ = LauncherTop::Pinned;
    layoutLoaded_ = false;
    Changed();
    Recompute();
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
            Changed();
            return;
        }
    // Pinning adds to the end; the oldest pin gives way when the row is full.
    if (pinCount_ == LL::Pins) {
        for (uint32_t j = 1; j < pinCount_; ++j) pins_[j - 1] = pins_[j];
        --pinCount_;
    }
    pins_[pinCount_++] = key;
    top_ = LauncherTop::Pinned;
    Changed();
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
    Changed();
    Recompute();
    EnsureVisible();
    return true;
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
            const bool oldFiles = Equal(key, "brook-files");
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
                const int icon = Lookup(IconNames, 8, fields[2], uint32_t(fieldEnd[2] - fields[2]));
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
        const int rank = Rank(apps_[a].name, c >= 0 ? categories_[c].name : "Unsorted", apps_[a].desc, query_);
        if (rank >= 9) continue;
        // Insertion by (rank, name).
        uint32_t at = n;
        while (at > 0 && (ranks[at - 1] > rank ||
                          (ranks[at - 1] == rank && CompareI(apps_[out[at - 1]].name, apps_[a].name) > 0))) {
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
            while (at > 0 && CompareI(apps_[list[at - 1]].name, apps_[a].name) > 0) { list[at] = list[at - 1]; --at; }
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
    scroll_ = 0;
    menuApp_ = -1;
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
    NoteLaunch(uint32_t(app));
    return {LauncherCommand::Launch, app};
}

LauncherResult LauncherModel::Key(LauncherKey key, char ch, uint8_t mods) {
    const LauncherResult changed{LauncherCommand::Changed, -1};
    const bool alt = mods & LauncherModAlt;
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
            return setView(uint32_t(((rail_ + d) % rows + rows) % rows));
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
            rail_ = view_ == LauncherViewAll ? 0 : view_ == LauncherViewUnsorted ? int(categoryCount_) + 1 : view_ + 1;
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
        if (secondary || t.index >= RailRows()) return {};
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
        rail_ = t.index + 1;
        scroll_ = 0;
        selected_ = -1;
        return changed;
    case LauncherTargetKind::Settings: return secondary ? LauncherResult{} : LauncherResult{LauncherCommand::OpenSettings, -1};
    default: return {};
    }
}

} // namespace brook
