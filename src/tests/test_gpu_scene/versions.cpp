#include "../../kernel/src/gpu_scene_versions.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#define VERIFY(x) do { if (!(x)) { std::fprintf(stderr,"SCENE_VERSIONS line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)

using namespace brook;

static void SynchronizedChild() {
    SceneImageVersions<3> versions(3*4096);
    SceneImageInfo a{},b{},c{},seen{};
    VERIFY(versions.Reserve(32,32,1,a)==SceneReserveResult::Ready && !a.texture);
    VERIFY(versions.Complete(a.version,11)==SceneCompleteResult::Published);
    VERIFY(versions.Reserve(32,32,1,b)==SceneReserveResult::Ready && !b.texture);
    VERIFY(versions.Complete(b.version,12)==SceneCompleteResult::Published);
    VERIFY(versions.Reserve(32,32,1,c)==SceneReserveResult::Ready && !c.texture);
    // A is committed, B cached, and C fails before allocating its image.
    VERIFY(versions.Reject(c.version));
    VERIFY(versions.GetPublished(a.version,seen) && seen.texture==11);
    VERIFY(versions.GetPublished(b.version,seen) && seen.texture==12);
    VERIFY(!versions.GetPublished(c.version,seen));
    VERIFY(versions.Stats().bytes==8192);
    // Parent-only repaint pins committed A, independently of cached B.
    VERIFY(versions.Pin(&a.version,1));
    VERIFY(versions.Release(a.version));
    VERIFY(!versions.GetPublished(a.version,seen));
    VERIFY(versions.GetPinned(a.version,seen) && seen.texture==11);
    VERIFY(versions.Reserve(32,32,1,c)==SceneReserveResult::Ready && !c.texture);
    VERIFY(versions.Complete(c.version,13)==SceneCompleteResult::Published);
    SceneImageInfo denied{};
    VERIFY(versions.Reserve(32,32,1,denied)==SceneReserveResult::Capacity);
    VERIFY(versions.Unpin(&a.version,1));
    VERIFY(versions.Reserve(32,32,1,denied)==SceneReserveResult::Ready && denied.texture==11);
    VERIFY(denied.version>a.version && denied.version>c.version);
    VERIFY(!versions.Pin(&a.version,1) && !versions.Release(a.version));
    VERIFY(versions.Complete(denied.version,12)==SceneCompleteResult::Invalid); // cannot alias B's allocation
    VERIFY(versions.Complete(denied.version,77)==SceneCompleteResult::Invalid); // cannot change a cached handle
    VERIFY(versions.Complete(denied.version,11)==SceneCompleteResult::Published);
    VERIFY(versions.GetPublished(b.version,seen) && seen.texture==12);
    VERIFY(versions.GetPublished(c.version,seen) && seen.texture==13);
}

static void AtomicPinsAndClose() {
    SceneImageVersions<4> versions(1ull<<30);
    SceneImageInfo a{},b{},pending{},unused{},seen{};
    VERIFY(versions.Reserve(3,5,1,a)==SceneReserveResult::Ready && versions.Complete(a.version,1)==SceneCompleteResult::Published);
    VERIFY(versions.Reserve(5,3,2,b)==SceneReserveResult::Ready && versions.Complete(b.version,2)==SceneCompleteResult::Published);
    const uint64_t invalid[]={a.version,b.version,999};
    VERIFY(!versions.Pin(invalid,3) && !versions.Stats().pinned);
    VERIFY(!versions.Pin(nullptr,1) && !versions.Unpin(nullptr,1));
    VERIFY(!versions.Pin(invalid,65) && !versions.Unpin(invalid,65));
    const uint64_t duplicate[]={a.version,b.version,a.version};
    VERIFY(versions.Pin(duplicate,3) && versions.Stats().pinned==3);
    VERIFY(!versions.Unpin(invalid,3) && versions.Stats().pinned==3);
    const uint64_t excess[]={b.version,b.version};
    VERIFY(!versions.Unpin(excess,2) && versions.Stats().pinned==3);
    VERIFY(versions.Reserve(8,8,1,pending)==SceneReserveResult::Ready);
    VERIFY(versions.Reserve(7,7,2,unused)==SceneReserveResult::Ready);
    versions.Close();versions.Close();
    VERIFY(!versions.GetPublished(a.version,seen) && !versions.Pin(duplicate,3));
    VERIFY(versions.GetPinned(a.version,seen) && seen.texture==1);
    VERIFY(versions.GetPinned(b.version,seen) && seen.texture==2);
    VERIFY(versions.Unpin(duplicate,3) && !versions.Stats().pinned);
    VERIFY(!versions.Unpin(duplicate,3));
    // A running upload may complete after the final FD closes. It becomes
    // cleanup work, never a newly published image or an early-free candidate.
    VERIFY(versions.Complete(pending.version,3)==SceneCompleteResult::Discarded);
    VERIFY(!versions.GetPublished(pending.version,seen));
    VERIFY(versions.Reject(unused.version));
    VERIFY(!versions.Drained());
    VERIFY(versions.Reserve(1,1,1,seen)==SceneReserveResult::Closing);
    uint32_t slot;SceneImageInfo retired{};
    unsigned retiredCount=0;
    while (versions.BeginEviction(slot,retired)) {
        VERIFY(!versions.EndEviction(slot,retired.version+1,true));
        VERIFY(versions.EndEviction(slot,retired.version,true));++retiredCount;
        VERIFY(!versions.EndEviction(slot,retired.version,true));
    }
    VERIFY(retiredCount==3 && versions.Drained() && !versions.Stats().bytes);
}

static void BudgetEvictionAndUncertainty() {
    SceneImageVersions<2> versions(256);
    SceneImageInfo a{},b{},seen{};
    VERIFY(versions.Reserve(0,8,1,a)==SceneReserveResult::Invalid);
    VERIFY(versions.Reserve(16385,1,1,a)==SceneReserveResult::Invalid);
    VERIFY(versions.Reserve(8,8,3,a)==SceneReserveResult::Invalid);
    VERIFY(versions.Reserve(8,8,1,a)==SceneReserveResult::Ready);
    VERIFY(versions.Stats().bytes==256);
    VERIFY(versions.Reserve(1,1,1,b)==SceneReserveResult::Capacity);
    VERIFY(versions.Complete(a.version,7)==SceneCompleteResult::Published && versions.Release(a.version));
    VERIFY(versions.Reserve(8,8,1,b)==SceneReserveResult::Ready && b.texture==7);
    VERIFY(versions.Reject(b.version)); // no commands: cached image reusable
    VERIFY(versions.Reserve(4,4,1,b)==SceneReserveResult::Capacity);
    uint32_t slot;SceneImageInfo retired{};
    VERIFY(versions.BeginEviction(slot,retired) && retired.texture==7);
    const uint32_t oldSlot=slot;const uint64_t oldTicket=retired.version;
    VERIFY(versions.Stats().bytes==256 && versions.Stats().evicting==1);
    VERIFY(versions.Reserve(1,1,1,b)==SceneReserveResult::Capacity);
    VERIFY(versions.EndEviction(slot,retired.version,true));
    VERIFY(versions.Reserve(4,4,1,b)==SceneReserveResult::Ready);
    VERIFY(versions.Complete(b.version,8)==SceneCompleteResult::Published && versions.Release(b.version));
    VERIFY(versions.BeginEviction(slot,retired));
    VERIFY(slot==oldSlot && !versions.EndEviction(oldSlot,oldTicket,true));
    VERIFY(versions.EndEviction(slot,retired.version,false));
    versions.Close();
    VERIFY(versions.Stats().quarantined==1 && versions.Stats().bytes==64 && !versions.Drained());
    VERIFY(!versions.GetPublished(b.version,seen));

    SceneImageVersions<2> uncertain(128);
    VERIFY(uncertain.Reserve(4,4,1,a)==SceneReserveResult::Ready);
    VERIFY(uncertain.Quarantine(a.version,17));
    VERIFY(!uncertain.Reject(a.version) && uncertain.Complete(a.version,17)==SceneCompleteResult::Invalid);
    VERIFY(uncertain.Reserve(4,4,2,b)==SceneReserveResult::Ready && uncertain.Complete(b.version,18)==SceneCompleteResult::Published);
    VERIFY(uncertain.Pin(&b.version,1) && uncertain.Release(b.version));
    VERIFY(uncertain.Quarantine(b.version));
    VERIFY(!uncertain.Unpin(&b.version,1));
    uncertain.Close();
    VERIFY(uncertain.Stats().quarantined==2 && uncertain.Stats().bytes==128 && uncertain.Stats().pinned==1);
    VERIFY(!uncertain.Drained() && !uncertain.BeginEviction(slot,retired));

    SceneImageVersions<1> exhausted(4,0x7fffffffffffffffull);
    VERIFY(exhausted.Reserve(1,1,1,a)==SceneReserveResult::Ready);
    VERIFY(a.version==0x7fffffffffffffffull && exhausted.Reject(a.version));
    VERIFY(exhausted.Reserve(1,1,1,b)==SceneReserveResult::IdExhausted);
}

static void InterleavedOperations() {
    SceneImageVersions<3> versions(192);
    SceneImageInfo a{},b{},replacement{},seen{};
    VERIFY(versions.Reserve(4,4,1,a)==SceneReserveResult::Ready);
    VERIFY(versions.Reserve(4,4,1,b)==SceneReserveResult::Ready);
    VERIFY(versions.Complete(b.version,31)==SceneCompleteResult::Published);
    VERIFY(!versions.GetPublished(a.version,seen));
    VERIFY(versions.Complete(a.version,32)==SceneCompleteResult::Published);
    VERIFY(versions.Release(a.version));
    VERIFY(versions.Reserve(4,4,1,replacement)==SceneReserveResult::Ready && replacement.texture==32);
    VERIFY(versions.Complete(a.version,32)==SceneCompleteResult::Invalid);
    VERIFY(!versions.Reject(a.version) && !versions.Quarantine(a.version));
    VERIFY(versions.Complete(replacement.version,32)==SceneCompleteResult::Published);
    const uint64_t batch[]={b.version,replacement.version};
    VERIFY(versions.Pin(batch,2));
    VERIFY(versions.Release(b.version) && versions.Release(replacement.version));
    VERIFY(versions.Quarantine(b.version));
    VERIFY(!versions.Unpin(batch,2) && versions.Stats().pinned==2);
    // A mixed unpin fails atomically. A caller retaining only uncertain inputs
    // may release the healthy image separately after verified GPU completion.
    VERIFY(versions.Unpin(&replacement.version,1) && versions.Stats().pinned==1);
    uint32_t slot;SceneImageInfo retired{};
    VERIFY(versions.BeginEviction(slot,retired) && retired.texture==32);
    versions.Close();
    VERIFY(versions.Stats().evicting==1 && versions.Stats().bytes==128);
    VERIFY(versions.EndEviction(slot,retired.version,true));
    VERIFY(versions.Stats().bytes==64 && versions.Stats().quarantined==1 && !versions.Drained());
}

// Independent public/queued-reference model across repeated replacement and
// recycling. Backend allocation IDs are deliberately different from versions.
static void RandomizedLifetime() {
    SceneImageVersions<8> versions(8*64);
    struct Ref { uint32_t pins,texture;bool publicId; };
    std::map<uint64_t,Ref> model;
    uint32_t random=0x42524f4f,texture=100;
    auto next=[&](unsigned n) {
        random^=random<<13;random^=random>>17;random^=random<<5;return random%n;
    };
    unsigned replacements=0,reuses=0,pinsTaken=0,releases=0;
    uint64_t previous=0;
    for (unsigned step=0;step<20000;++step) {
        const unsigned action=next(4);
        if (model.empty() || action==0) {
            SceneImageInfo image{};
            const auto result=versions.Reserve(4,4,1,image);
            if (result==SceneReserveResult::Ready) {
                VERIFY(image.version>previous);previous=image.version;
                if (next(5)==0) VERIFY(versions.Reject(image.version));
                else {
                    if (!image.texture) image.texture=texture++;else ++reuses;
                    VERIFY(versions.Complete(image.version,image.texture)==SceneCompleteResult::Published);
                    model[image.version]={0,image.texture,true};++replacements;
                }
            } else VERIFY(result==SceneReserveResult::Capacity);
        } else {
            auto it=model.begin();std::advance(it,next(model.size()));
            const uint64_t id=it->first;auto& ref=it->second;
            if (action==1 && ref.publicId) { VERIFY(versions.Pin(&id,1));++ref.pins;++pinsTaken; }
            if (action==2 && ref.publicId) { VERIFY(versions.Release(id));ref.publicId=false;++releases; }
            if (action==3 && ref.pins) { VERIFY(versions.Unpin(&id,1));--ref.pins; }
            if (!ref.publicId && !ref.pins) model.erase(it);
        }
        uint64_t pins=0;unsigned publicIds=0;
        for (const auto& item:model) {
            SceneImageInfo actual{};const auto& ref=item.second;
            VERIFY(versions.GetPublished(item.first,actual)==ref.publicId);
            if (ref.publicId) { VERIFY(actual.texture==ref.texture);++publicIds; }
            VERIFY(versions.GetPinned(item.first,actual)==bool(ref.pins));
            if (ref.pins) VERIFY(actual.texture==ref.texture);
            pins+=ref.pins;
        }
        const auto stats=versions.Stats();
        VERIFY(stats.pinned==pins && stats.published==publicIds && stats.bytes<=stats.byteLimit);
        VERIFY(stats.bytes==64*(model.size()+stats.idle));
    }
    VERIFY(replacements>1000 && reuses>1000 && pinsTaken>1000 && releases>1000);
    versions.Close();
    for (const auto& item:model)
        for (unsigned i=0;i<item.second.pins;++i) VERIFY(versions.Unpin(&item.first,1));
    uint32_t slot;SceneImageInfo retired{};
    while (versions.BeginEviction(slot,retired)) VERIFY(versions.EndEviction(slot,retired.version,true));
    VERIFY(versions.Drained());
}

void TestSceneVersions() {
    SynchronizedChild();AtomicPinsAndClose();BudgetEvictionAndUncertainty();InterleavedOperations();RandomizedLifetime();
    std::puts("SCENE_VERSIONS_PASS immutable A/B/C, queued pins, close, bounds, reuse, eviction tickets, quarantine, randomized lifetimes");
}
