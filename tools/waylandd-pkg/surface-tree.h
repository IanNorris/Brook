#ifndef BROOK_SURFACE_TREE_H
#define BROOK_SURFACE_TREE_H
#include <stdint.h>
#include <stdlib.h>

/* Single-event-loop commit dependency graph. Values are immutable application
 * state (images/geometry). Frame callbacks belong to the node, tagged with
 * its commit sequence; apply may release callbacks through current->commit.
 * An applied callback must not mutate tree topology or dispatch client requests.
 * Snapshot child entries retain BOTH the node and its exact committed state.
 * The adapter cancels all pending/waiting/eligible callbacks at protocol-node
 * destruction, even when snapshots defer free_node. No allocation happens
 * while applying or destroying a snapshot. */
struct bt_node;
struct bt_state;
struct bt_ops {
    void (*value_ref)(void *);
    void (*value_unref)(void *);
    void (*apply)(struct bt_node *,void *);
    void (*free_node)(struct bt_node *);
};
struct bt_child {
    struct bt_node *node;
    struct bt_state *state;
    uint64_t association;
    int32_t x,y;
    int below;
};
struct bt_state {
    unsigned refs;
    const struct bt_ops *ops;
    void *value;
    struct bt_child *children;
    size_t count;
    uint64_t commit;
    struct bt_state *garbage;
};
struct bt_node {
    unsigned refs;
    int dead,sync;
    uint64_t association,sequence;
    const struct bt_ops *ops;
    void *user;
    struct bt_node *parent,*children,*sibling;
    struct bt_state *current,*latest,*cached;
    int32_t pending_x,pending_y,x,y;
    int pending_below;
};
static void bt_node_ref(struct bt_node *node) { if (node) ++node->refs; }
static void bt_node_unref(struct bt_node *node) {
    if (node && !--node->refs) node->ops->free_node(node);
}
static struct bt_state *bt_state_ref(struct bt_state *state) {
    if (state) ++state->refs;
    return state;
}
static void bt_state_unref(struct bt_state *state) {
    if (!state || --state->refs) return;
    state->garbage=NULL;
    while (state) {
        struct bt_state *next=state->garbage;
        if (state->value) state->ops->value_unref(state->value);
        for (size_t i=0;i<state->count;++i) {
            struct bt_child *child=&state->children[i];
            if (child->state && !--child->state->refs) {
                child->state->garbage=next;next=child->state;
            }
            bt_node_unref(child->node);
        }
        free(state->children);free(state);state=next;
    }
}
static void bt_state_set(struct bt_state **slot,struct bt_state *state) {
    struct bt_state *old=*slot;*slot=bt_state_ref(state);bt_state_unref(old);
}
static void bt_node_init(struct bt_node *node,const struct bt_ops *ops,void *user) {
    *node=(struct bt_node){.refs=1,.sync=1,.association=1,.ops=ops,.user=user};
}
static int bt_effective_sync(const struct bt_node *node) {
    /* A root is never synchronized merely because its default mode is sync. */
    for (;node && node->parent;node=node->parent) if (node->sync) return 1;
    return 0;
}
static int bt_child_valid(const struct bt_node *parent,const struct bt_child *entry) {
    return !entry->node->dead && entry->node->parent==parent &&
           entry->node->association==entry->association;
}
static struct bt_child *bt_find_child(struct bt_state *state,const struct bt_node *node) {
    if (state) for (size_t i=0;i<state->count;++i)
        if (state->children[i].node==node && state->children[i].association==node->association)
            return &state->children[i];
    return NULL;
}
static struct bt_state *bt_state_alloc(const struct bt_ops *ops,void *value,size_t count) {
    struct bt_state *state=calloc(1,sizeof(*state));
    if (!state) return NULL;
    if (count) {
        state->children=calloc(count,sizeof(*state->children));
        if (!state->children) { free(state);return NULL; }
    }
    state->refs=1;state->ops=ops;state->value=value;
    if (value) ops->value_ref(value);
    return state;
}
static void bt_add_child(struct bt_state *state,const struct bt_child *entry) {
    state->children[state->count++]=*entry;
    bt_node_ref(entry->node);bt_state_ref(entry->state);
}
static struct bt_state *bt_build_commit(struct bt_node *node,void *value) {
    size_t count=0;
    for (struct bt_node *child=node->children;child;child=child->sibling) ++count;
    struct bt_state *state=bt_state_alloc(node->ops,value,count);
    if (!state) return NULL;
    for (struct bt_node *child=node->children;child;child=child->sibling) {
        struct bt_state *selected=child->cached;
        struct bt_child *previous=bt_find_child(node->latest,child);
        if (!selected) selected=bt_effective_sync(child) && previous?previous->state:child->current;
        const struct bt_child entry={child,selected,child->association,
                                    child->pending_x,child->pending_y,child->pending_below};
        bt_add_child(state,&entry);
    }
    return state;
}
static struct bt_state *bt_replace_child(struct bt_node *parent,struct bt_node *child,struct bt_state *replacement) {
    struct bt_state *old=parent->current;
    if (!bt_find_child(old,child)) return NULL;
    struct bt_state *state=bt_state_alloc(parent->ops,old->value,old->count);
    if (!state) return NULL;
    state->commit=old->commit;
    for (size_t i=0;i<old->count;++i) {
        struct bt_child entry=old->children[i];
        if (entry.node==child && entry.association==child->association) entry.state=replacement;
        bt_add_child(state,&entry);
    }
    return state;
}
/* Prepare every ancestor copy before changing visible state. An association
 * not yet applied by its parent is intentionally absent from that parent's
 * current snapshot; a desynchronized child cannot make that link visible. */
static int bt_build_path(struct bt_node *node,struct bt_state *state,
                          struct bt_node **top,struct bt_state **out) {
    *top=node;*out=bt_state_ref(state);
    while ((*top)->parent && bt_find_child((*top)->parent->current,*top)) {
        struct bt_state *copy=bt_replace_child((*top)->parent,*top,*out);
        if (!copy) { bt_state_unref(*out);*out=NULL;return 0; }
        bt_state_unref(*out);*out=copy;*top=(*top)->parent;
    }
    return 1;
}
static void bt_apply_one(struct bt_node *node,struct bt_state *state) {
    bt_state_set(&node->current,state);
    if (state) {
        node->ops->apply(node,state->value);
    }
}
static void bt_apply(struct bt_node *root,struct bt_state *state) {
    bt_apply_one(root,state);
    struct bt_node *node=root;size_t index=0;
    for (;;) {
        struct bt_state *current=node->current;
        while (current && index<current->count && !bt_child_valid(node,&current->children[index])) ++index;
        if (current && index<current->count) {
            const struct bt_child entry=current->children[index];
            entry.node->x=entry.x;entry.node->y=entry.y;
            bt_apply_one(entry.node,entry.state);node=entry.node;index=0;continue;
        }
        if (node==root) break;
        struct bt_node *child=node;node=node->parent;
        current=node->current;index=0;
        while (current && index<current->count && current->children[index].node!=child) ++index;
        ++index;
    }
}
static void bt_update_path_latest(struct bt_node *node,struct bt_node *top) {
    while (node!=top) { node=node->parent;bt_state_set(&node->latest,node->current); }
}
/* Retains value on success. Failure does not consume a child cache or alter any
 * current/latest state. Callers capture a candidate image before this step. */
static int bt_commit(struct bt_node *node,void *value) {
    if (!node || node->dead || node->sequence==UINT64_MAX) return 0;
    struct bt_state *state=bt_build_commit(node,value),*path=NULL;
    struct bt_node *top=node;
    if (!state) return 0;
    const int sync=bt_effective_sync(node);
    if (!sync && !bt_build_path(node,state,&top,&path)) { bt_state_unref(state);return 0; }
    state->commit=++node->sequence;
    for (size_t i=0;i<state->count;++i) {
        struct bt_child *child=&state->children[i];
        if (child->state && child->node->cached==child->state)
            bt_state_set(&child->node->cached,NULL);
    }
    bt_state_set(&node->latest,state);
    if (sync) bt_state_set(&node->cached,state);
    else {
        bt_state_set(&node->cached,NULL);bt_apply(top,path);bt_update_path_latest(node,top);
    }
    bt_state_unref(path);bt_state_unref(state);return 1;
}
static int bt_flush(struct bt_node *node) {
    if (!node->cached || bt_effective_sync(node)) return 1;
    struct bt_node *top=NULL;struct bt_state *path=NULL;
    if (!bt_build_path(node,node->cached,&top,&path)) return 0;
    bt_state_set(&node->cached,NULL);bt_apply(top,path);bt_update_path_latest(node,top);
    bt_state_unref(path);return 1;
}
/* Desynchronizing can apply several nodes before a later allocation fails.
 * Failure requires fatal client OOM teardown, not retry/transaction rollback. */
static int bt_set_sync(struct bt_node *root,int sync) {
    root->sync=!!sync;
    if (sync) return 1;
    struct bt_node *node=root;
    for (;;) {
        if (!bt_flush(node)) return 0;
        if (node->children) { node=node->children;continue; }
        while (node!=root && !node->sibling) node=node->parent;
        if (node==root) break;
        node=node->sibling;
    }
    return 1;
}
static int bt_attach(struct bt_node *node,struct bt_node *parent) {
    if (!node || !parent || node->dead || parent->dead || node->parent || node->association==UINT64_MAX) return 0;
    for (struct bt_node *p=parent;p;p=p->parent) if (p==node) return 0;
    ++node->association;node->parent=parent;node->sync=1;
    node->pending_x=node->pending_y=0;node->pending_below=0;
    struct bt_node **tail=&parent->children;
    while (*tail) tail=&(*tail)->sibling;
    *tail=node;node->sibling=NULL;return 1;
}
static void bt_unlink(struct bt_node *node) {
    if (!node->parent) return;
    struct bt_node **link=&node->parent->children;
    while (*link && *link!=node) link=&(*link)->sibling;
    if (*link) *link=node->sibling;
    node->parent=node->sibling=NULL;
    /* Saturated associations cannot be attached again. */
    if (node->association<UINT64_MAX) ++node->association;
}
static int bt_place(struct bt_node *node,struct bt_node *sibling,int above) {
    if (!node || !sibling || node->dead || sibling->dead) return 0;
    struct bt_node *parent=node->parent;
    if (!parent || !sibling || sibling==node || (sibling!=parent && sibling->parent!=parent)) return 0;
    struct bt_node **link=&parent->children;
    while (*link && *link!=node) link=&(*link)->sibling;
    if (!*link) return 0;
    *link=node->sibling;
    if (sibling==parent) {
        node->pending_below=!above;link=&parent->children;
        while (*link && (*link)->pending_below) link=&(*link)->sibling;
    } else {
        node->pending_below=sibling->pending_below;link=&parent->children;
        while (*link && *link!=sibling) link=&(*link)->sibling;
        if (above) link=&(*link)->sibling;
    }
    node->sibling=*link;*link=node;return 1;
}
static void bt_destroy(struct bt_node *node) {
    if (!node || node->dead) return;
    node->dead=1;bt_unlink(node);
    while (node->children) bt_unlink(node->children);
    bt_state_set(&node->current,NULL);bt_state_set(&node->latest,NULL);bt_state_set(&node->cached,NULL);
    bt_node_unref(node);
}
#endif
