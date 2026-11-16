#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "launcher_power.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}} while(0)
using T=LauncherPowerTarget;
using C=LauncherPowerCommand;

static bool overlap(const DesktopRect& a,const DesktopRect& b) {
    return a.w>0&&a.h>0&&b.w>0&&b.h>0&&a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
static bool inside(const DesktopRect& inner,const DesktopRect& outer) {
    return inner.x>=outer.x&&inner.y>=outer.y&&inner.x+inner.w<=outer.x+outer.w&&inner.y+inner.h<=outer.y+outer.h;
}
static int cx(const DesktopRect& r){return r.x+r.w/2;}
static int cy(const DesktopRect& r){return r.y+r.h/2;}

// Window-manager values: labels measured at roughly 8 px per glyph.
static LauncherPowerMetrics metrics(const DesktopMetrics& m) {
    return {m.pixels(40),m.pixels(6),m.pixels(6),m.pixels(28),m.pixels(12),
            m.pixels(9*8)+2*m.pixels(14),m.pixels(13*8)+2*m.pixels(14)};
}

static void geometry() {
    for(const DesktopMetrics* dm:{&DesktopMetric100,&DesktopMetric150,&DesktopMetric200}) {
        const auto m=metrics(*dm);const int minimum=LauncherPowerMinimumWidth(m);
        for(int w:{minimum,minimum+1,minimum+137,1400,minimum-40,2*m.pad+2*m.buttonW+m.gap,200}) {
            const DesktopRect panel{4,300,w,600};
            const auto l=LauncherPowerFooter(panel,m);
            CHECK(inside(l.footer,panel)&&inside(l.zone,panel));
            CHECK(l.footer.y+l.footer.h==panel.y+panel.h-m.pad);
            // The separator sits in the gap above the band, apart from the buttons.
            CHECK(inside(l.separator,l.zone)&&!overlap(l.separator,l.footer)&&l.separator.y<l.footer.y);
            CHECK(inside(l.restart,l.footer)&&inside(l.shutdown,l.footer)&&!overlap(l.restart,l.shutdown));
            CHECK(l.shutdown.x+l.shutdown.w==l.footer.x+l.footer.w); // right-aligned
            CHECK(inside(l.restart,l.dismiss)&&inside(l.shutdown,l.dismiss));
            // The confirm button never covers where either power button was.
            CHECK(!overlap(l.confirm,l.restart)&&!overlap(l.confirm,l.shutdown)&&!overlap(l.confirm,l.cluster));
            if(l.confirm.w>0)CHECK(inside(l.confirm,l.footer)&&l.confirm.x+l.confirm.w+m.separation<=l.cluster.x);
            if(w>=minimum)CHECK(l.confirm.w==m.confirmW&&l.restart.w==m.buttonW);
        }
    }
    // Degenerate panels yield nothing hittable.
    const auto m=metrics(DesktopMetric100);
    const auto none=LauncherPowerFooter({0,0,8,8},m);
    CHECK(LauncherPowerHit(none,false,false,4,4)==T::None);
}

static void hits() {
    const auto m=metrics(DesktopMetric100);
    const DesktopRect panel{4,200,LauncherPowerMinimumWidth(m)+60,500};
    const auto l=LauncherPowerFooter(panel,m);
    CHECK(LauncherPowerHit(l,false,false,cx(l.restart),cy(l.restart))==T::Restart);
    CHECK(LauncherPowerHit(l,false,false,cx(l.shutdown),cy(l.shutdown))==T::Shutdown);
    CHECK(LauncherPowerHit(l,false,false,l.footer.x+2,cy(l.footer))==T::Footer);
    CHECK(LauncherPowerHit(l,false,false,cx(l.restart),l.separator.y)==T::Footer);  // slip onto the separator
    CHECK(LauncherPowerHit(l,false,false,cx(l.restart),l.zone.y-1)==T::None);       // the app grid above
    CHECK(LauncherPowerHit(l,false,false,cx(l.confirm),cy(l.confirm))==T::Footer);  // no confirm while idle
    CHECK(LauncherPowerHit(l,true,false,cx(l.restart),cy(l.restart))==T::Dismiss);
    CHECK(LauncherPowerHit(l,true,false,cx(l.shutdown),cy(l.shutdown))==T::Dismiss);
    CHECK(LauncherPowerHit(l,true,false,cx(l.confirm),cy(l.confirm))==T::Confirm);
    CHECK(LauncherPowerHit(l,true,true,cx(l.confirm),cy(l.confirm))==T::Footer);
    CHECK(LauncherPowerHit(l,false,true,cx(l.restart),cy(l.restart))==T::CancelShutdown);
    // Exhaustive: wherever a first press arms, a second press at the same
    // point dismisses. A double-click or touchpad bounce cannot confirm.
    for(const DesktopMetrics* dm:{&DesktopMetric100,&DesktopMetric150,&DesktopMetric200}) {
        const auto mm=metrics(*dm);
        for(int w:{LauncherPowerMinimumWidth(mm),LauncherPowerMinimumWidth(mm)-50,900}) {
            const auto f=LauncherPowerFooter({0,0,w,400},mm);int armed=0;
            for(int y=f.zone.y-2;y<f.zone.y+f.zone.h+2;++y)for(int x=-2;x<w+2;++x) {
                const T first=LauncherPowerHit(f,false,false,x,y);
                if(first!=T::Restart&&first!=T::Shutdown)continue;
                ++armed;CHECK(LauncherPowerHit(f,true,false,x,y)==T::Dismiss);
            }
            CHECK(armed>0);
        }
    }
}

static void doubleClick() {
    LauncherPowerGuard g;
    CHECK(g.Activate(T::Restart,1000)==C::None&&g.pending()==LauncherPowerAction::Restart);
    // The second click of a double-click lands on Cancel (same position).
    CHECK(g.Activate(T::Dismiss,1150)==C::None&&!g.prompt());
    // Inert footer presses do nothing in either state.
    CHECK(g.Activate(T::Footer,1200)==C::None&&!g.prompt());
    CHECK(g.Activate(T::Confirm,5000)==C::None&&!g.prompt()); // a stale confirm without a prompt
}

static void confirm() {
    LauncherPowerGuard g;
    CHECK(g.Activate(T::Restart,1000)==C::None);
    CHECK(g.Activate(T::Shutdown,1010)==C::None&&g.pending()==LauncherPowerAction::Restart); // not re-armed
    CHECK(g.Activate(T::Confirm,1000+LauncherPowerGuard::SettleMs-1)==C::None&&g.prompt()); // settling
    CHECK(g.Activate(T::Confirm,1000+LauncherPowerGuard::SettleMs)==C::Restart&&!g.prompt());
    CHECK(g.Activate(T::Shutdown,9000)==C::None&&g.pending()==LauncherPowerAction::Shutdown);
    CHECK(g.Activate(T::Confirm,9900)==C::Shutdown&&!g.prompt());
    // Closing the launcher resets the prompt.
    CHECK(g.Activate(T::Restart,10000)==C::None);g.Reset();CHECK(!g.prompt());
    CHECK(g.Activate(T::CancelShutdown,600)==C::CancelShutdown);
}

static void keyboard() {
    LauncherPowerGuard g;
    CHECK(g.Key(LauncherPowerKey::Enter,100)==C::None&&!g.prompt()); // no prompt: keys are inert
    // Enter on a focused power button arms, like a click; focus starts on Cancel.
    CHECK(g.Activate(T::Restart,1000)==C::None&&g.prompt()&&!g.confirmFocused());
    CHECK(g.Key(LauncherPowerKey::Enter,2000)==C::None&&!g.prompt()); // Enter again cancels
    CHECK(g.Activate(T::Shutdown,3000)==C::None);
    CHECK(g.Key(LauncherPowerKey::Escape,3100)==C::None&&!g.prompt());
    CHECK(g.Activate(T::Restart,4000)==C::None);
    CHECK(g.Key(LauncherPowerKey::Next,4100)==C::None&&g.confirmFocused());
    CHECK(g.Key(LauncherPowerKey::Enter,4200)==C::None&&g.prompt()); // still settling
    CHECK(g.Key(LauncherPowerKey::Previous,4300)==C::None&&!g.confirmFocused());
    CHECK(g.Key(LauncherPowerKey::Next,4400)==C::None&&g.confirmFocused());
    CHECK(g.Key(LauncherPowerKey::Enter,4500)==C::Restart&&!g.prompt()&&!g.confirmFocused());
}

int main() {
    geometry();hits();doubleClick();confirm();keyboard();
    std::puts("launcher power: ok");
    return 0;
}
