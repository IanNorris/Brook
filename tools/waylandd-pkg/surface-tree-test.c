#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int fail_allocation;
static void *tree_calloc(size_t count,size_t bytes) {
    if (fail_allocation && !--fail_allocation) return NULL;
    return calloc(count,bytes);
}
#define calloc tree_calloc
#include "surface-tree.h"
#undef calloc

struct value { unsigned refs;int image; };
struct frame { struct frame *next;uint64_t commit;unsigned count; };
static unsigned live_values,live_nodes,eligible,cancelled;
static void value_ref(void *p) { ++((struct value *)p)->refs; }
static void value_unref(void *p) {
    struct value *v=p;
    if (!--v->refs) { --live_values;free(v); }
}
static void apply(struct bt_node *node,void *p) {
    (void)p;
    struct frame **link=(struct frame **)&node->user;
    while (*link) {
        struct frame *frame=*link;
        if (frame->commit<=node->current->commit) {
            eligible+=frame->count;*link=frame->next;free(frame);
        } else link=&frame->next;
    }
}
static void free_node(struct bt_node *node) {
    struct frame *frame=node->user;
    while (frame) { struct frame *next=frame->next;cancelled+=frame->count;free(frame);frame=next; }
    --live_nodes;free(node);
}
static const struct bt_ops ops={value_ref,value_unref,apply,free_node};
static struct bt_node *node(void) {
    struct bt_node *n=calloc(1,sizeof(*n));assert(n);++live_nodes;bt_node_init(n,&ops,NULL);return n;
}
static int commit(struct bt_node *n,int image,int frames) {
    struct value *v=calloc(1,sizeof(*v));assert(v);v->refs=1;v->image=image;++live_values;
    struct frame *frame=NULL;
    if (frames) {
        frame=calloc(1,sizeof(*frame));assert(frame);
        frame->commit=n->sequence+1;frame->count=frames;frame->next=n->user;n->user=frame;
    }
    const int ok=bt_commit(n,v);value_unref(v);
    if (!ok && frame) { assert(n->user==frame);n->user=frame->next;free(frame); }
    return ok;
}
static int image(struct bt_state *s) { return s && s->value?((struct value *)s->value)->image:0; }
static void empty(void) { assert(!live_nodes && !live_values); }

static void nested_dependencies(void) {
    struct bt_node *r=node(),*p=node(),*c=node();
    assert(bt_attach(p,r) && bt_attach(c,p));
    assert(commit(c,10,1) && image(c->cached)==10 && !c->current);
    assert(commit(p,20,1) && !c->cached && image(p->cached->children[0].state)==10);
    assert(commit(c,11,2) && image(c->cached)==11);
    unsigned before=eligible;
    assert(commit(r,30,1));
    assert(image(c->current)==10 && image(c->cached)==11 && image(c->latest)==11);
    assert(eligible==before+3); /* R, P and C=A; C=B is still ineligible */
    assert(commit(r,31,0));assert(image(c->current)==10 && image(c->cached)==11);
    assert(commit(p,21,0) && commit(r,32,0));
    assert(image(c->current)==11 && !c->cached && eligible==before+5);
    assert(bt_set_sync(c,0));assert(commit(c,12,3));
    assert(image(c->current)==11 && image(c->cached)==12); /* inherited sync */
    assert(bt_set_sync(p,0));
    assert(image(c->current)==12 && !c->cached && eligible==before+8);
    assert(bt_set_sync(c,1));assert(commit(r,33,0));
    assert(image(c->current)==12); /* resync must not roll back a desync update */
    bt_destroy(r);bt_destroy(p);bt_destroy(c);empty();
}
static void parent_owned_order(void) {
    struct bt_node *r=node(),*a=node(),*b=node();assert(bt_attach(a,r) && bt_attach(b,r));
    a->pending_x=4;a->pending_y=-7;
    assert(commit(a,1,0) && commit(b,2,0));assert(a->x==0 && a->y==0);
    assert(bt_place(b,r,0));assert(commit(r,3,0));
    assert(a->x==4 && a->y==-7);
    assert(r->current->children[0].node==b && r->current->children[0].below);
    assert(r->current->children[1].node==a && !r->current->children[1].below);
    assert(bt_place(a,b,0));a->pending_x=9;
    assert(commit(a,4,0));
    assert(a->x==4 && r->current->children[0].node==b);
    assert(commit(r,5,0));
    assert(a->x==9 && r->current->children[0].node==a && r->current->children[0].below);
    assert(bt_place(a,r,1));assert(commit(r,6,0));
    assert(r->current->children[0].node==b && r->current->children[1].node==a);
    assert(!bt_place(a,a,1));
    struct bt_node *stranger=node();assert(!bt_place(a,stranger,1));bt_destroy(stranger);
    uint64_t association=a->association;bt_unlink(a);
    assert(!bt_child_valid(r,&r->current->children[1]));
    struct bt_node *q=node();assert(bt_attach(a,q) && a->association>association);
    assert(commit(q,7,0) && image(a->current)==4);
    bt_destroy(r);bt_destroy(b);assert(a->parent==q);
    bt_destroy(q);bt_destroy(a);empty();
}
static void callbacks_and_failures(void) {
    struct bt_node *r=node(),*c=node();assert(bt_attach(c,r));
    unsigned before=eligible;
    assert(commit(c,1,1) && commit(c,2,2));assert(eligible==before);
    assert(c->sequence==2 && c->cached->commit==2);
    assert(commit(r,3,0) && eligible==before+3);assert(commit(r,3,0) && eligible==before+3);
    assert(bt_set_sync(c,0));
    for (int fail=1;fail<=3;++fail) {
        struct value *v=calloc(1,sizeof(*v));assert(v);v->refs=1;v->image=4;++live_values;
        struct bt_state *old_r=r->current,*old_c=c->current,*latest_r=r->latest,*latest_c=c->latest;
        uint64_t sequence=c->sequence;
        fail_allocation=fail;assert(!bt_commit(c,v));fail_allocation=0;value_unref(v);
        assert(c->sequence==sequence);
        assert(r->current==old_r && c->current==old_c && r->latest==latest_r && c->latest==latest_c);
    }
    bt_destroy(r);bt_destroy(c);empty();
    /* Failure before caching a newer update must retain the previous cache. */
    r=node();c=node();assert(bt_attach(c,r) && commit(c,5,2));
    struct bt_state *cached=c->cached;
    struct value *v=calloc(1,sizeof(*v));assert(v);v->refs=1;v->image=6;++live_values;
    fail_allocation=1;assert(!bt_commit(c,v));fail_allocation=0;value_unref(v);
    assert(c->cached==cached && image(cached)==5);
    unsigned was_cancelled=cancelled;bt_destroy(r);bt_destroy(c);
    assert(cancelled==was_cancelled+2);empty();
}
static void replaced_parent_callbacks(void) {
    struct bt_node *r=node(),*p=node(),*c=node();
    assert(bt_attach(p,r) && bt_attach(c,p));
    unsigned before=eligible;
    assert(commit(c,1,1) && commit(p,10,0));
    assert(commit(c,2,2) && commit(p,11,0));
    assert(eligible==before && commit(r,20,0));
    assert(image(c->current)==2 && eligible==before+3);
    assert(commit(r,21,0) && eligible==before+3);
    bt_destroy(r);bt_destroy(p);bt_destroy(c);empty();

    /* G holds the older parent dependency while P accumulates a newer one. */
    struct bt_node *g=node();r=node();p=node();c=node();
    assert(bt_attach(r,g) && bt_attach(p,r) && bt_attach(c,p));
    before=eligible;
    assert(commit(c,1,1) && commit(p,10,0) && commit(r,20,0));
    assert(commit(c,2,2) && commit(p,11,0));
    assert(commit(g,30,0) && image(c->current)==1 && eligible==before+1);
    assert(commit(r,21,0) && commit(g,31,0));
    assert(image(c->current)==2 && eligible==before+3);
    assert(commit(g,32,0) && eligible==before+3);
    bt_destroy(g);bt_destroy(r);bt_destroy(p);bt_destroy(c);empty();
}
static void failure_boundaries(void) {
    struct bt_node *r=node(),*p=node(),*c=node();
    assert(bt_attach(p,r) && bt_attach(c,p));
    assert(commit(c,1,1));
    struct bt_state *cache=c->cached;
    for (int fail=1;fail<=2;++fail) {
        struct value *v=calloc(1,sizeof(*v));assert(v);v->refs=1;++live_values;
        fail_allocation=fail;assert(!bt_commit(p,v));fail_allocation=0;value_unref(v);
        assert(c->cached==cache && !p->latest && !p->sequence);
    }
    assert(commit(p,2,0) && commit(r,3,0));
    uint64_t old_commit=c->current->commit;
    c->sequence=UINT64_MAX;
    assert(!commit(c,4,0) && c->current->commit==old_commit && !c->cached);
    bt_destroy(r);bt_destroy(p);bt_destroy(c);empty();

    r=node();c=node();assert(bt_attach(c,r) && commit(r,1,0) && commit(c,2,1));
    cache=c->cached;
    fail_allocation=1;assert(!bt_set_sync(c,0));fail_allocation=0;
    assert(!c->sync && c->cached==cache && !c->current);
    /* The caller must tear down on this failure, not assume rollback. */
    bt_destroy(r);bt_destroy(c);empty();

    r=node();c=node();assert(!bt_attach(r,r) && bt_attach(c,r));
    assert(!bt_attach(r,c));
    bt_unlink(c);c->association=UINT64_MAX;
    assert(!bt_attach(c,r));bt_destroy(r);bt_destroy(c);empty();
}
static void deep_lifetimes(void) {
    enum { count=4096 };
    struct bt_node **nodes=calloc(count,sizeof(*nodes));assert(nodes);
    for (unsigned i=0;i<count;++i) {
        nodes[i]=node();if (i) assert(bt_attach(nodes[i],nodes[i-1]));
    }
    for (unsigned i=count;i--;) assert(commit(nodes[i],i+1,0));
    assert(image(nodes[count-1]->current)==count);
    /* Parent destruction orphans only its directly associated children. Old
     * dependency snapshots must keep dead nodes valid until they are released. */
    for (unsigned i=count;i--;) bt_destroy(nodes[i]);
    free(nodes);empty();
}
int main(void) {
    nested_dependencies();parent_owned_order();callbacks_and_failures();replaced_parent_callbacks();failure_boundaries();deep_lifetimes();
    puts("SURFACE_TREE_PASS exact nested dependencies, sync inheritance, parent-owned stacking/position, callbacks, allocation rollback, deep lifetimes");
}
