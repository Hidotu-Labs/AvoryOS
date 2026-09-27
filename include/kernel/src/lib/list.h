#ifndef LIB_LIST_H
#define LIB_LIST_H

#include <stddef.h>
#include <stdint.h>

struct list_head {
    struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *newNode, struct list_head *prev, struct list_head *next) {
    next->prev = newNode;
    newNode->next = next;
    newNode->prev = prev;
    prev->next = newNode;
}

static inline void list_add(struct list_head *newNode, struct list_head *head) {
    __list_add(newNode, head, head->next);
}

static inline void list_add_tail(struct list_head *newNode, struct list_head *head) {
    __list_add(newNode, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    if (!entry || !entry->next || !entry->prev) return;
    // Validate pointers are in kernel higher-half space to catch corruption
    if ((uint64_t)entry->next < 0xFFFF800000000000ULL ||
        (uint64_t)entry->prev < 0xFFFF800000000000ULL) return;
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member)                          \
    for (pos = list_entry((head)->next, typeof(*pos), member);          \
         &pos->member != (head);                                        \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)                  \
    for (pos = list_entry((head)->next, typeof(*pos), member),          \
         n   = list_entry(pos->member.next, typeof(*pos), member);      \
         &pos->member != (head);                                        \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

#endif
