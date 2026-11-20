// Launcher model (DESIGN.md "Launcher organisation"): defaults, search
// ranking, keyboard navigation, pin/unpin, recent ordering, keyboard reorder
// and the [Launcher] persistence round trip.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include "launcher_model.h"
using namespace brook;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {

LauncherApp App(const char* key, const char* name, const char* desc, const char* hint, bool system = false) {
    LauncherApp a{};
    std::snprintf(a.key, sizeof(a.key), "%s", key);
    std::snprintf(a.name, sizeof(a.name), "%s", name);
    std::snprintf(a.desc, sizeof(a.desc), "%s", desc);
    std::snprintf(a.hint, sizeof(a.hint), "%s", hint);
    a.system = system;
    return a;
}

const LauncherApp Catalog[] = {
    App(LauncherFilesAppId, "Files", "Browse files and directories", "system", true),
    App("terminal", "Terminal", "Command line shell", "system", true),
    App("brook-settings", "Settings", "Appearance and taskbar", "system", true),
    App("firefox", "Firefox", "Web browser", "internet"),
    App("ladybird", "Ladybird", "Ladybird browser", "internet"),
    App("quake", "Quake", "First-person shooter", "games"),
    App("quake2", "Quake II", "id Software shooter", "games"),
    App("doom", "DOOM", "The classic", "games"),
    App("edit", "Text Editor", "Open, edit, save", "office"),
    App("calc", "Calculator", "Brook native calculator", "office"),
    App("fbtest", "FB Test", "Framebuffer stress test", "demos"),
    App("mystery", "Mystery Tool", "No declared category", ""),
};
constexpr uint32_t CatalogCount = sizeof(Catalog) / sizeof(Catalog[0]);

std::unique_ptr<LauncherModel> Fresh() {
    auto m = std::make_unique<LauncherModel>();
    m->Init();
    m->SetCatalog(Catalog, CatalogCount);
    m->SetViewport({36, 48, 40, 96, 48 * 6 + 36, 20, 0});
    m->Open();
    return m;
}
int Idx(const LauncherModel& m, const char* key) { const int i = m.FindApp(key); CHECK(i >= 0); return i; }
int Cat(const LauncherModel& m, const char* name) {
    for (uint32_t c = 0; c < m.CategoryCount(); ++c) if (!std::strcmp(m.CategoryName(c), name)) return int(c);
    return -9;
}
void Type(LauncherModel& m, const char* text) { for (; *text; ++text) m.Key(LauncherKey::Char, *text); }
std::string Names(const LauncherModel& m, const uint16_t* list, uint32_t n) {
    std::string s;
    for (uint32_t i = 0; i < n; ++i) { if (i) s += ","; s += m.App(list[i]).name; }
    return s;
}
std::string Results(const LauncherModel& m) {
    uint16_t r[LauncherLimits::Apps];
    return Names(m, r, m.SearchResults(r, LauncherLimits::Apps));
}

void Defaults() {
    auto m = Fresh();
    CHECK(m->CategoryCount() == 7);
    CHECK(m->CategoryOf(Idx(*m, "quake")) == Cat(*m, "Games"));
    CHECK(m->CategoryOf(Idx(*m, LauncherFilesAppId)) == Cat(*m, "System"));
    CHECK(m->CategoryOf(Idx(*m, "mystery")) == -1);
    CHECK(m->UnsortedCount() == 1);
    // Members are alphabetical among the defaults.
    LauncherLine lines[LauncherLimits::Lines];
    const uint32_t n = m->Lines(lines, LauncherLimits::Lines);
    CHECK(n > 4 && lines[0].kind == LauncherLineKind::Heading && lines[0].category == Cat(*m, "System"));
    CHECK(lines[1].kind == LauncherLineKind::Apps && lines[1].columns == 2 && lines[1].count == 2);
    CHECK(!std::strcmp(m->App(lines[1].apps[0]).name, "Files"));
    CHECK(!std::strcmp(m->App(lines[1].apps[1]).name, "Settings"));
    // The last section is Unsorted, one column.
    CHECK(lines[n - 2].kind == LauncherLineKind::Heading && lines[n - 2].category == LauncherViewUnsorted);
    CHECK(lines[n - 1].columns == 1);
    uint16_t tiles[LauncherLimits::Pins];
    CHECK(Names(*m, tiles, m->TopTiles(tiles, LauncherLimits::Pins)) == "Files,Terminal,Settings,Firefox,Quake");
    CHECK(m->RailRows() == 1 + 5 + 1);
    // An empty category is hidden until Organise exposes it for edits.
    CHECK(m->RailRowFor(Cat(*m, "Media")) == 0);
    m->Press({LauncherTargetKind::Organise, 0}, false, 0, 0);
    m->Press({LauncherTargetKind::Rail, uint16_t(m->RailRowFor(Cat(*m, "Media")))}, false, 0, 0);
    CHECK(m->View() == Cat(*m, "Media"));
    CHECK(m->Lines(lines, LauncherLimits::Lines) == 2 && lines[1].kind == LauncherLineKind::Empty);
}

void Search() {
    CHECK(LauncherModel::Rank("Quake II", "Games", "", "qua") == 0);
    CHECK(LauncherModel::Rank("Text Editor", "Office", "", "edi") == 1);
    CHECK(LauncherModel::Rank("Calculator", "Office", "", "cul") == 2);
    CHECK(LauncherModel::Rank("DOOM", "Games", "", "games") == 3);
    CHECK(LauncherModel::Rank("Firefox", "Internet", "Web browser", "brow") == 4);
    CHECK(LauncherModel::Rank("Firefox", "Internet", "Web browser", "zzz") == 9);
    auto m = Fresh();
    Type(*m, "qu");
    CHECK(Results(*m) == "Quake,Quake II");
    CHECK(m->Selected() == Idx(*m, "quake") && m->KeyboardActive());
    // Category names: typing "games" lists everything filed under Games.
    m->Key(LauncherKey::Escape);
    CHECK(!m->Query()[0] && m->Selected() == -1 && !m->KeyboardActive());
    Type(*m, "games");
    CHECK(Results(*m) == "DOOM,Quake,Quake II");
    uint16_t cats[4];
    CHECK(m->SearchCategories(cats, 4) == 1 && cats[0] == Cat(*m, "Games"));
    LauncherLine lines[LauncherLimits::Lines];
    const uint32_t n = m->Lines(lines, LauncherLimits::Lines);
    CHECK(n == 2 + 1 + 3 && lines[1].kind == LauncherLineKind::Chips && lines[3].columns == 1);
    // Name prefix beats a word start beats a description.
    m->Key(LauncherKey::Escape);
    Type(*m, "b");
    // Names containing "b" first, then descriptions.
    CHECK(Results(*m) == "FB Test,Ladybird,Calculator,Files,Firefox,Settings");
    while (m->Query()[0]) m->Key(LauncherKey::Backspace);
    Type(*m, "t");
    // "Terminal" (prefix), then "Text Editor" (prefix), then "FB Test" (word), then contains.
    CHECK(Results(*m).rfind("Terminal,Text Editor,FB Test,", 0) == 0);
    // Nothing matches: an empty-state line.
    m->Key(LauncherKey::Escape);
    Type(*m, "xyzzy");
    CHECK(m->Lines(lines, LauncherLimits::Lines) == 1 && lines[0].kind == LauncherLineKind::Empty);
    // Enter with no match does nothing; Escape clears, then closes.
    CHECK(m->Key(LauncherKey::Enter).command == LauncherCommand::None);
    CHECK(m->Key(LauncherKey::Escape).command == LauncherCommand::Changed);
    CHECK(m->Key(LauncherKey::Escape).command == LauncherCommand::Close);
    // Space presses the focused thing until there is a query.
    CHECK(m->Key(LauncherKey::Char, ' ').command == LauncherCommand::None);
    Type(*m, "quake ii");
    CHECK(Results(*m) == "Quake II");
    const auto r = m->Key(LauncherKey::Enter);
    CHECK(r.command == LauncherCommand::Launch && r.app == Idx(*m, "quake2"));
    // Hint mapping from freedesktop categories.
    CHECK(!std::strcmp(LauncherModel::HintFromDesktopCategories("Network;WebBrowser;"), "internet"));
    CHECK(!std::strcmp(LauncherModel::HintFromDesktopCategories("Game;ActionGame;"), "games"));
    CHECK(!std::strcmp(LauncherModel::HintFromDesktopCategories("AudioVideo;Player;"), "media"));
    CHECK(!std::strcmp(LauncherModel::HintFromDesktopCategories("Graphics;2DGraphics;"), "graphics"));
    CHECK(!std::strcmp(LauncherModel::HintFromDesktopCategories("Utility;TextEditor;"), "office"));
    CHECK(!std::strcmp(LauncherModel::HintFromDesktopCategories(""), ""));
}

void Navigation() {
    auto m = Fresh();
    uint16_t order[LauncherLimits::Apps];
    (void)order;
    // Down from nothing selects the first app; Up wraps to the last.
    CHECK(m->Key(LauncherKey::Down).command == LauncherCommand::Changed);
    CHECK(m->Selected() == Idx(*m, LauncherFilesAppId));
    m->Key(LauncherKey::Down);
    CHECK(m->Selected() == Idx(*m, "brook-settings"));
    m->Key(LauncherKey::Up); m->Key(LauncherKey::Up);
    CHECK(m->Selected() == Idx(*m, "mystery"));
    // The last item is scrolled into view.
    CHECK(m->Scroll() > 0 && m->Scroll() <= m->MaxScroll());
    m->Key(LauncherKey::Home);
    CHECK(m->Selected() == Idx(*m, LauncherFilesAppId) && m->Scroll() == 0);
    // Enter launches and records Recent.
    auto r = m->Key(LauncherKey::Enter);
    CHECK(r.command == LauncherCommand::Launch && r.app == Idx(*m, LauncherFilesAppId));
    // Tab: list -> tiles -> rail -> list.
    m->Key(LauncherKey::Tab);
    CHECK(m->Section() == LauncherSection::Tiles && m->SelectedTile() == 0);
    m->Key(LauncherKey::Right); m->Key(LauncherKey::Right);
    r = m->Key(LauncherKey::Enter);
    CHECK(r.command == LauncherCommand::Launch && r.app == Idx(*m, "brook-settings"));
    m->Key(LauncherKey::Tab);
    CHECK(m->Section() == LauncherSection::Rail);
    m->Key(LauncherKey::Down);
    CHECK(m->View() == 0); // System
    m->Key(LauncherKey::BackTab);
    CHECK(m->Section() == LauncherSection::Tiles);
    m->Key(LauncherKey::Tab); m->Key(LauncherKey::Tab);
    CHECK(m->Section() == LauncherSection::List);
    // Typing from any section searches.
    m->Key(LauncherKey::Tab);
    Type(*m, "fire");
    CHECK(m->Section() == LauncherSection::List && m->Selected() == Idx(*m, "firefox"));
    // Pointer presses: a rail row opens a view and clears the query.
    m->Press({LauncherTargetKind::Rail, 0}, false, 0, 0);
    CHECK(!m->Query()[0] && m->View() == LauncherViewAll);
    CHECK(m->Press({LauncherTargetKind::Row, uint16_t(Idx(*m, "doom"))}, false, 0, 0).app == Idx(*m, "doom"));
    // Scrolling is in pixels and stays within range; a wheel tick is the
    // viewport step, positive ticks scrolling up.
    CHECK(m->MaxScroll() == m->ContentHeight() - (48 * 6 + 36));
    CHECK(m->ScrollTicks(-1) && m->Scroll() == 20);
    CHECK(m->ScrollTicks(1) && m->Scroll() == 0);
    CHECK(m->ScrollBy(100000) && m->Scroll() == m->MaxScroll());
    CHECK(m->ScrollBy(-100000) && m->Scroll() == 0);
    CHECK(!m->ScrollBy(-1));
    // Keyboard selection scrolls the selected row fully into view.
    m->Key(LauncherKey::End);
    CHECK(m->Scroll() == m->MaxScroll());
}

void PinsAndRecent() {
    auto m = Fresh();
    const int doom = Idx(*m, "doom");
    CHECK(!m->Pinned(doom));
    // Right-click opens the two-item menu; item 0 pins.
    m->Press({LauncherTargetKind::Row, uint16_t(doom)}, true, 100, 200);
    CHECK(m->MenuOpen() && m->MenuApp() == doom && m->MenuX() == 100);
    // Uninstall (item 1) is disabled: nothing happens, the menu stays.
    CHECK(m->Press({LauncherTargetKind::MenuItem, 1}, false, 0, 0).command == LauncherCommand::None && m->MenuOpen());
    const uint32_t epoch = m->LayoutEpoch();
    m->Press({LauncherTargetKind::MenuItem, 0}, false, 0, 0);
    CHECK(!m->MenuOpen() && m->Pinned(doom) && m->LayoutEpoch() != epoch);
    uint16_t tiles[LauncherLimits::Pins];
    CHECK(Names(*m, tiles, m->TopTiles(tiles, LauncherLimits::Pins)) == "Files,Terminal,Settings,Firefox,Quake,DOOM");
    // Keyboard: Alt+Enter opens the menu on the selection, Enter unpins.
    m->Key(LauncherKey::Home);
    m->Key(LauncherKey::Enter, 0, LauncherModAlt);
    CHECK(m->MenuOpen() && m->MenuApp() == Idx(*m, LauncherFilesAppId));
    CHECK(m->Key(LauncherKey::Char, 'x').command == LauncherCommand::None); // the menu is modal
    m->Key(LauncherKey::Enter);
    CHECK(!m->Pinned(Idx(*m, LauncherFilesAppId)));
    // A press elsewhere only closes the menu.
    m->Press({LauncherTargetKind::Tile, 0}, true, 5, 5);
    CHECK(m->MenuOpen());
    CHECK(m->Press({LauncherTargetKind::Row, uint16_t(doom)}, false, 0, 0).command == LauncherCommand::Changed && !m->MenuOpen());
    // Pins are capped at ten; the oldest gives way.
    for (uint32_t i = 0; i < CatalogCount; ++i) if (!m->Pinned(i)) m->TogglePin(i);
    CHECK(m->TopTiles(tiles, LauncherLimits::Pins) == LauncherLimits::Pins);
    // Recent: most recent first, no duplicates, five at most.
    for (const char* key : {"quake", "doom", "edit", "calc", "fbtest", "firefox", "doom"}) m->NoteLaunch(Idx(*m, key));
    m->SetTop(LauncherTop::Recent);
    CHECK(Names(*m, tiles, m->TopTiles(tiles, LauncherLimits::Pins)) == "DOOM,Firefox,FB Test,Calculator,Text Editor");
    m->Press({LauncherTargetKind::TopTab, 0}, false, 0, 0);
    CHECK(m->Top() == LauncherTop::Pinned);
}

void Reorder() {
    auto m = Fresh();
    const int games = Cat(*m, "Games"), graphics = Cat(*m, "Graphics");
    const int doom = Idx(*m, "doom");
    // DOOM is first in Games; Alt+Up moves it to the end of the category
    // before it (Graphics, empty until now).
    CHECK(m->Step(doom, -1) && m->CategoryOf(doom) == graphics);
    CHECK(m->CategoryAppCount(games) == 2 && m->CategoryAppCount(graphics) == 1);
    CHECK(m->Step(doom, +1) && m->CategoryOf(doom) == games);
    CHECK(m->Step(doom, +1) && m->CategoryOf(doom) == games);
    LauncherLine lines[LauncherLimits::Lines];
    m->Press({LauncherTargetKind::Rail, uint16_t(m->RailRowFor(games))}, false, 0, 0);
    m->Lines(lines, LauncherLimits::Lines);
    CHECK(!std::strcmp(m->App(lines[1].apps[0]).name, "Quake") && !std::strcmp(m->App(lines[1].apps[1]).name, "DOOM"));
    // Unsorted apps move into the last category.
    const int mystery = Idx(*m, "mystery");
    CHECK(m->Step(mystery, -1) && m->CategoryOf(mystery) == int(m->CategoryCount()) - 1);
    CHECK(m->UnsortedCount() == 0 && m->RailRows() == 6);
    // Alt+Down through the keyboard.
    m->Press({LauncherTargetKind::Rail, 0}, false, 0, 0);
    m->Key(LauncherKey::Down);
    m->Key(LauncherKey::Down, 0, LauncherModAlt);
    CHECK(m->CategoryOf(Idx(*m, LauncherFilesAppId)) == Cat(*m, "System"));
}

void Organisation() {
    auto m = Fresh();
    using TK = LauncherTargetKind;
    const auto row = [&](const char* key) { return LauncherTarget{TK::Row, uint16_t(Idx(*m, key))}; };
    const auto rail = [&](int view) { return LauncherTarget{TK::Rail, uint16_t(m->RailRowFor(view))}; };
    const auto click = [&](LauncherTarget t) { return m->Press(t, false, 12, 12); };
    char layout[40 * 1024];
    CHECK(m->Serialize(layout, sizeof(layout)));
    const int games = Cat(*m, "Games"), office = Cat(*m, "Office");
    const auto doom = row("doom"), quake = row("quake"), calc = row("calc"), fire = row("firefox");
    CHECK(m->BeginPointer(doom, 12, 12));
    CHECK(!m->MovePointer(doom, 14, 13, 6, false));
    CHECK(m->ReleasePointer(doom, false).command == LauncherCommand::Launch);
    CHECK(m->BeginPointer(doom, 12, 12));
    CHECK(m->MovePointer({TK::None, 0}, 34, 34, 6, false));
    CHECK(m->ReleasePointer({TK::None, 0}, false).command == LauncherCommand::Changed);
    CHECK(m->CategoryOf(doom.index) == games);

    click({TK::Organise, 0});
    CHECK(m->Organising() && m->RailRows() == 9);
    CHECK(click(doom).command == LauncherCommand::Changed && m->Marked(doom.index));
    click(quake);
    CHECK(m->MarkedCount() == 2);
    const auto officeTarget = rail(office);
    CHECK(m->BeginPointer(doom, 12, 12));
    CHECK(m->MovePointer(officeTarget, 34, 34, 6, false, 1000));
    CHECK(!m->MovePointer(officeTarget, 34, 34, 6, false, 1699));
    CHECK(m->MovePointer(officeTarget, 34, 34, 6, false, 1700));
    CHECK(m->View() == office);
    CHECK(m->ReleasePointer(calc, true).command == LauncherCommand::Changed);
    CHECK(m->CategoryOf(doom.index) == office && m->CategoryOf(quake.index) == office);
    LauncherLine lines[LauncherLimits::Lines];
    CHECK(m->Lines(lines, LauncherLimits::Lines) >= 3);
    CHECK(lines[1].apps[0] == calc.index && lines[1].apps[1] == doom.index && lines[2].apps[0] == quake.index);

    CHECK(m->BeginPointer(doom, 12, 12));
    CHECK(m->MovePointer({TK::NewCategory, 0}, 34, 34, 6, false));
    m->ReleasePointer({TK::NewCategory, 0}, false);
    CHECK(m->EditorOpen() && !m->EditingApp());
    Type(*m, "Arcade");
    click({TK::EditorColor, uint16_t(LauncherColor::Pink)});
    click({TK::EditorIcon, uint16_t(LauncherCategoryIcon::Bolt)});
    m->Key(LauncherKey::Enter);
    const int arcade = Cat(*m, "Arcade");
    CHECK(arcade >= 0 && m->CategoryOf(doom.index) == arcade && m->CategoryOf(quake.index) == arcade);
    CHECK(m->CategoryColor(arcade) == LauncherColor::Pink && m->CategoryIcon(arcade) == LauncherCategoryIcon::Bolt);
    const auto before = rail(games), after = rail(arcade);
    m->BeginPointer(after, 12, 12);
    m->MovePointer(before, 34, 34, 6, false);
    m->ReleasePointer(before, false);
    CHECK(Cat(*m, "Arcade") == games);
    click({TK::EditCategory, uint16_t(Cat(*m, "Arcade"))});
    click({TK::EditorDelete, 0});
    CHECK(m->CategoryOf(doom.index) == -1 && m->CategoryOf(quake.index) == -1);
    CHECK(m->Pinned(quake.index)); // deletion leaves the pin references alone
    CHECK(m->Suggestion(doom.index) == Cat(*m, "Games"));
    click({TK::MoveSuggested, doom.index});
    CHECK(m->CategoryOf(doom.index) == Cat(*m, "Games") && m->Suggestion(quake.index) >= 0);
    CHECK(m->SortSuggested() && m->CategoryOf(quake.index) == Cat(*m, "Games"));

    CHECK(m->RenameApp(fire.index, "Internet Browser"));
    CHECK(!m->RenameApp(fire.index, "Bad|Name"));
    CHECK(!std::strcmp(m->Name(fire.index), "Internet Browser"));
    CHECK(!std::strcmp(m->App(fire.index).name, "Firefox"));
    Type(*m, "internet browser");
    CHECK(m->Selected() == fire.index);
    char alias[40 * 1024];
    CHECK(m->Serialize(alias, sizeof(alias)));
    auto again = Fresh();
    CHECK(again->Parse(alias));
    CHECK(!std::strcmp(again->Name(fire.index), "Internet Browser"));
    CHECK(again->RenameApp(fire.index, ""));
    CHECK(!std::strcmp(again->Name(fire.index), "Firefox"));
    m->SetTop(LauncherTop::Recent);
    CHECK(m->RestoreLayout(layout));
    CHECK(m->Top() == LauncherTop::Recent && !std::strcmp(m->Name(fire.index), "Firefox"));
    CHECK(m->CategoryOf(doom.index) == games && m->Pinned(quake.index));

    m->SetTop(LauncherTop::Pinned);
    CHECK(m->BeginPointer(doom, 12, 12));
    m->MovePointer({TK::Tile, 0}, 34, 34, 6, false);
    m->ReleasePointer({TK::Tile, 0}, false);
    CHECK(m->Pinned(doom.index) && !std::strcmp(m->PinKey(0), "doom"));
    CHECK(m->CategoryOf(doom.index) == games);
    const uint32_t count = m->PinCount();
    click({TK::ResetOrganisation, 0});
    CHECK(m->PinCount() == count && m->CategoryCount() == 7);
    const LauncherApp added[] = {Catalog[0], App("editor", "Editor", "Text tool", "office")};
    m->SetCatalog(added, 2);
    CHECK(m->IsNew(1) && m->Suggestion(1) == Cat(*m, "Office"));
    m->SetCatalog(added, 2);
    CHECK(m->IsNew(1));
    click({TK::MoveSuggested, 1});
    CHECK(!m->IsNew(1) && m->CategoryOf(1) == Cat(*m, "Office"));
    click(rail(Cat(*m, "Office")));
    m->Key(LauncherKey::BackTab); // focus the rail on the current category
    CHECK(m->Section() == LauncherSection::Rail);
    CHECK(m->Key(LauncherKey::Delete).command == LauncherCommand::Changed);
    CHECK(m->CategoryOf(1) == -1 && m->View() == LauncherViewUnsorted);
}

void Persistence() {
    auto m = Fresh();
    m->TogglePin(Idx(*m, "doom"));
    m->NoteLaunch(Idx(*m, "calc"));
    m->NoteLaunch(Idx(*m, "quake2"));
    m->Step(Idx(*m, "doom"), -1);
    m->SetTop(LauncherTop::Recent);
    static char text[16384];
    const uint32_t n = m->Serialize(text, sizeof(text));
    CHECK(n > 0 && n < sizeof(text));
    CHECK(std::strstr(text, "[Launcher]\nversion=1\ntop_row=recent\n"));
    CHECK(std::strstr(text, "pinned=org.gnome.Nautilus,terminal,brook-settings,firefox,quake,doom\n"));
    CHECK(std::strstr(text, "recent=quake2,calc\n"));
    CHECK(std::strstr(text, "category.office=Office|amber|doc|calc,edit\n"));
    CHECK(std::strstr(text, "category.graphics=Graphics|pink|brush|doom\n"));
    // Too small a buffer is reported, not truncated silently.
    char tiny[32];
    CHECK(m->Serialize(tiny, sizeof(tiny)) == 0);

    // Round trip, with an uninstalled app remembered in its place.
    auto loaded = std::make_unique<LauncherModel>();
    loaded->Init();
    CHECK(loaded->Parse(text));
    loaded->SetCatalog(Catalog, CatalogCount - 2); // fbtest and mystery are gone
    char again[16384];
    CHECK(loaded->Serialize(again, sizeof(again)) == n && !std::strcmp(text, again));
    CHECK(loaded->CategoryOf(Idx(*loaded, "doom")) == Cat(*loaded, "Graphics"));
    CHECK(loaded->Top() == LauncherTop::Recent);
    // Reinstalled, it returns to its place; a new app lands in Unsorted
    // (the layout exists, so nothing is filed behind the user's back).
    LauncherApp more[CatalogCount + 1];
    for (uint32_t i = 0; i < CatalogCount; ++i) more[i] = Catalog[i];
    more[CatalogCount] = App("inkscape", "Inkscape", "Vector drawing", "graphics");
    loaded->SetCatalog(more, CatalogCount + 1);
    CHECK(loaded->CategoryOf(Idx(*loaded, "fbtest")) == Cat(*loaded, "Demos"));
    CHECK(loaded->CategoryOf(Idx(*loaded, "inkscape")) == -1);

    // Existing layout keys follow Files across the package change, including
    // recent and category membership. Preserve order and collapse aliases.
    auto old = std::make_unique<LauncherModel>();
    old->Init();
    const uint32_t before = old->LayoutEpoch();
    bool migrated = false;
    CHECK(old->Parse("[Launcher]\nversion=1\npinned=terminal,brook-hardware-files,brook-files,org.gnome.Nautilus,firefox\n"
                     "recent=brook-hardware-files,calc,files,org.gnome.Nautilus\ncategories=system\n"
                     "category.system=System|blue|sliders|Files,terminal\n", &migrated));
    CHECK(migrated && old->LayoutEpoch() != before);
    old->SetCatalog(Catalog, CatalogCount);
    CHECK(old->Pinned(Idx(*old, LauncherFilesAppId)));
    CHECK(old->CategoryOf(Idx(*old, LauncherFilesAppId)) == Cat(*old, "System"));
    CHECK(old->Serialize(again, sizeof(again)) > 0);
    CHECK(std::strstr(again, "pinned=terminal,org.gnome.Nautilus,firefox\n"));
    CHECK(std::strstr(again, "recent=org.gnome.Nautilus,calc\n"));
    CHECK(std::strstr(again, "category.system=System|blue|sliders|org.gnome.Nautilus,terminal\n"));
    CHECK(!std::strstr(again, "brook-files") && !std::strstr(again, "brook-hardware-files"));
    CHECK(old->Parse(again, &migrated) && !migrated);

    // A bad or missing section falls back to the defaults.
    auto bad = std::make_unique<LauncherModel>();
    bad->Init();
    CHECK(!bad->Parse("[Launcher]\nversion=2\n"));
    CHECK(!bad->Parse("[Other]\nversion=1\n"));
    CHECK(!bad->Parse("[Launcher]\nversion=1\ncategory.x=broken\n"));
    bad->SetCatalog(Catalog, CatalogCount);
    CHECK(bad->CategoryCount() == 7 && bad->CategoryOf(Idx(*bad, "quake")) == Cat(*bad, "Games"));
    // One home per app even if the file lists it twice; unknown colours fall back.
    auto dup = std::make_unique<LauncherModel>();
    dup->Init();
    CHECK(dup->Parse("[Launcher]\nversion=1\ncategories=a,b\ncategory.a=A|nope|nope|doom\n"
                     "category.b=Bee|green|globe|doom,quake\n"));
    dup->SetCatalog(Catalog, CatalogCount);
    CHECK(dup->CategoryCount() == 2 && dup->CategoryOf(Idx(*dup, "doom")) == 0 && dup->CategoryOf(Idx(*dup, "quake")) == 1);
    CHECK(dup->CategoryColor(0) == LauncherColor::Blue && dup->CategoryColor(1) == LauncherColor::Green);
    CHECK(dup->CategoryAppCount(1) == 1 && dup->UnsortedCount() == CatalogCount - 2);
}

} // namespace

int main() {
    Defaults();
    Search();
    Navigation();
    PinsAndRecent();
    Reorder();
    Organisation();
    Persistence();
    std::printf("LAUNCHER_MODEL_OK\n");
    return 0;
}
