#include "store.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hash.h"

/* 기간이 있는 entry만 만료 시각이 이른 순으로 담는 min-heap입니다(ADR
   expiry-heap-with-immediate-removal). entries[0]이 만료 시각이 가장 이른
   entry이고, 각 entry는 heap_index에 자기 자리를 기억합니다. */
struct heap {
    struct entry **entries;
    size_t count;
    size_t capacity;
};

/* heap 배열을 처음 만들 때의 칸 수입니다. 그 뒤로는 두 배씩 늘립니다. */
#define HEAP_CAPACITY_MIN 16

struct store {
    struct hash table;
    struct heap heap;
};

/* entry 하나는 malloc 한 번으로 만듭니다. key와 value의 바이트는 구조체 바로
   뒤 bytes에 이어서 둡니다. 길이가 둘 다 0이어도 할당 크기가 0이 되지 않으므로
   malloc(0)이 NULL을 돌려주는 경우를 따로 다루지 않아도 됩니다. */
struct entry {
    struct hash_elem elem;
    /* 조회용 임시 entry는 호출한 쪽 key를 가리킵니다. */
    const unsigned char *key;
    size_t key_len;
    const unsigned char *value;
    size_t value_len;
    /* 만료되지 않는 entry는 STORE_NO_EXPIRY입니다. */
    uint64_t expires_at;
    /* heap 안의 자리입니다. 기간이 있는 entry에서만 뜻이 있습니다. */
    size_t heap_index;
    unsigned char bytes[];
};

static uint64_t entry_hash(const struct hash_elem *element, void *aux);
static bool entry_less(const struct hash_elem *left,
                       const struct hash_elem *right, void *aux);
static void entry_free(struct hash_elem *element, void *aux);
static struct entry *entry_create(const void *key, size_t key_len,
                                  const void *value, size_t value_len,
                                  uint64_t expires_at);
static const unsigned char *bytes_or_empty(const void *bytes, size_t len);
static void entry_init(struct entry *entry, size_t key_len, size_t value_len,
                       uint64_t expires_at);
static bool entry_has_expiry(const struct entry *entry);
static bool heap_reserve(struct heap *heap);
static void entry_discard(struct store *store, struct entry *entry);
static void heap_delete(struct heap *heap, struct entry *entry);
static void heap_place(struct heap *heap, size_t index, struct entry *entry);
static void heap_sift_up(struct heap *heap, size_t index);
static bool entry_expires_before(const struct entry *left,
                                 const struct entry *right);
static void heap_swap(struct heap *heap, size_t first, size_t second);
static void heap_sift_down(struct heap *heap, size_t index);
static void heap_insert(struct heap *heap, struct entry *entry);
static struct entry *live_entry_find(struct store *store, uint64_t now,
                                     const void *key, size_t key_len);
static struct entry probe_for(const void *key, size_t key_len);
static bool entry_is_expired(const struct entry *entry, uint64_t now);

struct store *store_create(void)
{
    struct store *store = malloc(sizeof *store);
    if (store == NULL) {
        return NULL;
    }
    if (!hash_init(&store->table, entry_hash, entry_less, NULL)) {
        free(store);
        return NULL;
    }
    store->heap = (struct heap){0};

    return store;
}

void store_destroy(struct store *store)
{
    assert(store != NULL);
    hash_destroy(&store->table, entry_free);
    free(store->heap.entries);
    free(store);
}

bool store_set(struct store *store, const void *key, size_t key_len,
               const void *value, size_t value_len, uint64_t expires_at)
{
    assert(store != NULL);

    /* 새 entry를 먼저 다 만든 뒤에 테이블에 넣습니다. 만들기가 실패하면
       테이블은 그대로입니다. */
    struct entry *entry =
        entry_create(key, key_len, value, value_len, expires_at);
    if (entry == NULL) {
        return false;
    }
    /* heap의 칸도 테이블을 바꾸기 전에 확보합니다. 옛 entry를 heap에서 먼저
       빼므로, 덮어쓰기에도 칸 하나면 충분합니다. */
    if (entry_has_expiry(entry) && !heap_reserve(&store->heap)) {
        entry_free(&entry->elem, NULL);
        return false;
    }

    /* 같은 key가 있으면 hash_replace가 옛 entry를 빼서 돌려줍니다. */
    struct hash_elem *replaced = hash_replace(&store->table, &entry->elem);
    if (replaced != NULL) {
        entry_discard(store, hash_entry(replaced, struct entry, elem));
    }
    if (entry_has_expiry(entry)) {
        heap_insert(&store->heap, entry);
    }

    return true;
}

bool store_get(struct store *store, uint64_t now, const void *key,
               size_t key_len, const void **value, size_t *value_len)
{
    assert(store != NULL && value != NULL && value_len != NULL);
    assert(now != STORE_NO_EXPIRY);

    /* 만료된 entry는 "없음"으로 답하되 지우지 않습니다. get은 store를 바꾸지
       않기 때문입니다(spec "조회할 때의 검사"). 지우는 일은 삭제와 주기 청소가
       맡습니다. */
    const struct entry *entry = live_entry_find(store, now, key, key_len);
    if (entry == NULL) {
        return false;
    }
    *value = entry->value;
    *value_len = entry->value_len;

    return true;
}

bool store_delete(struct store *store, uint64_t now, const void *key,
                  size_t key_len)
{
    assert(store != NULL);
    assert(now != STORE_NO_EXPIRY);

    /* hash_delete는 테이블에서 뺀 entry를 돌려줄 뿐 해제하지 않으므로, 해제는
       store가 합니다. */
    struct entry probe = probe_for(key, key_len);
    struct hash_elem *removed = hash_delete(&store->table, &probe.elem);
    if (removed == NULL) {
        return false;
    }
    /* 만료된 entry도 빼서 해제하지만, 없는 key와 같게 "지운 것이 없음"으로
       답합니다(spec Q3, "조회할 때의 검사"). */
    struct entry *entry = hash_entry(removed, struct entry, elem);
    bool was_live = !entry_is_expired(entry, now);
    entry_discard(store, entry);

    return was_live;
}

size_t store_delete_expired(struct store *store, uint64_t now, size_t max_count)
{
    assert(store != NULL);
    assert(now != STORE_NO_EXPIRY);

    /* heap의 맨 앞이 만료 전이면 나머지도 만료 전이므로 거기서 멈춥니다(spec Q9). */
    size_t deleted_count = 0;
    while (deleted_count < max_count && store->heap.count > 0 &&
           entry_is_expired(store->heap.entries[0], now)) {
        struct entry *entry = store->heap.entries[0];
        hash_delete(&store->table, &entry->elem);
        entry_discard(store, entry);
        deleted_count++;
    }

    return deleted_count;
}

/* value는 해시하지 않습니다. 같은 key는 value와 상관없이 같은 버킷에 가야 합니다. */
static uint64_t entry_hash(const struct hash_elem *element, void *aux)
{
    (void)aux;
    const struct entry *entry = hash_entry(element, struct entry, elem);
    return hash_bytes(entry->key, entry->key_len);
}

/* hash.c는 less(a, b)와 less(b, a)가 모두 거짓이면 같은 key로 봅니다.
   길이 비교가 없으면 "abc"와 "abcd"를 같은 key로 보게 됩니다. */
static bool entry_less(const struct hash_elem *left,
                       const struct hash_elem *right, void *aux)
{
    (void)aux;
    const struct entry *left_entry = hash_entry(left, struct entry, elem);
    const struct entry *right_entry = hash_entry(right, struct entry, elem);
    size_t shorter_len = left_entry->key_len < right_entry->key_len
                             ? left_entry->key_len
                             : right_entry->key_len;
    int byte_order = memcmp(left_entry->key, right_entry->key, shorter_len);
    if (byte_order != 0) {
        return byte_order < 0;
    }

    return left_entry->key_len < right_entry->key_len;
}

static void entry_free(struct hash_elem *element, void *aux)
{
    (void)aux;
    free(hash_entry(element, struct entry, elem));
}

/* key와 value의 바이트를 복사한 새 entry를 만듭니다. 길이의 합이 size_t를
   넘거나 메모리가 부족하면 NULL을 돌려줍니다. */
static struct entry *entry_create(const void *key, size_t key_len,
                                  const void *value, size_t value_len,
                                  uint64_t expires_at)
{
    /* 길이의 합이 size_t를 넘으면 malloc 크기가 작게 감기므로 할당 실패로 봅니다. */
    if (key_len > SIZE_MAX - sizeof(struct entry) ||
        value_len > SIZE_MAX - sizeof(struct entry) - key_len) {
        return NULL;
    }
    struct entry *entry = malloc(sizeof *entry + key_len + value_len);
    if (entry == NULL) {
        return NULL;
    }
    memcpy(entry->bytes, bytes_or_empty(key, key_len), key_len);
    memcpy(entry->bytes + key_len, bytes_or_empty(value, value_len), value_len);
    entry_init(entry, key_len, value_len, expires_at);

    return entry;
}

/* hash_bytes는 NULL 주소에서 assert로 멈추고, memcmp에 NULL을 넘기면 길이가
   0이어도 정의되지 않은 동작입니다. 0바이트 key를 NULL로 받으면 이 주소로 바꿉니다. */
static const unsigned char empty_bytes[1];

static const unsigned char *bytes_or_empty(const void *bytes, size_t len)
{
    assert(bytes != NULL || len == 0);
    return bytes != NULL ? bytes : empty_bytes;
}

/* key와 value의 바이트가 이미 bytes에 있을 때, 주소와 길이 필드를 채웁니다. */
static void entry_init(struct entry *entry, size_t key_len, size_t value_len,
                       uint64_t expires_at)
{
    entry->key = entry->bytes;
    entry->key_len = key_len;
    entry->value = entry->bytes + key_len;
    entry->value_len = value_len;
    entry->expires_at = expires_at;
}

static bool entry_has_expiry(const struct entry *entry)
{
    return entry->expires_at != STORE_NO_EXPIRY;
}

/* heap에 entry 하나를 더 넣을 칸을 확보합니다. 메모리가 부족하면 false를
   돌려주고 heap은 그대로입니다. */
static bool heap_reserve(struct heap *heap)
{
    if (heap->count < heap->capacity) {
        return true;
    }
    /* 두 배로 늘린 크기가 size_t를 넘으면 할당 실패로 봅니다. */
    if (heap->capacity > SIZE_MAX / (2 * sizeof *heap->entries)) {
        return false;
    }
    size_t new_capacity =
        heap->capacity == 0 ? HEAP_CAPACITY_MIN : heap->capacity * 2;
    struct entry **entries =
        realloc(heap->entries, new_capacity * sizeof *entries);
    if (entries == NULL) {
        return false;
    }
    heap->entries = entries;
    heap->capacity = new_capacity;

    return true;
}

/* 테이블에서 이미 뺀 entry를 heap에서도 빼고 해제합니다. 지우거나 덮어쓴 key가
   heap에 남지 않게 합니다(spec Q16). */
static void entry_discard(struct store *store, struct entry *entry)
{
    if (entry_has_expiry(entry)) {
        heap_delete(&store->heap, entry);
    }
    entry_free(&entry->elem, NULL);
}

/* 맨 끝 entry를 빈 자리로 옮긴 뒤, 부모보다 이르면 올리고 아니면 내립니다. */
static void heap_delete(struct heap *heap, struct entry *entry)
{
    size_t index = entry->heap_index;
    assert(index < heap->count && heap->entries[index] == entry);
    heap->count--;
    if (index == heap->count) {
        return;
    }
    heap_place(heap, index, heap->entries[heap->count]);
    heap_sift_up(heap, index);
    heap_sift_down(heap, heap->entries[index]->heap_index);
}

/* heap의 index 칸에 entry를 두고, entry가 그 자리를 기억하게 합니다. */
static void heap_place(struct heap *heap, size_t index, struct entry *entry)
{
    heap->entries[index] = entry;
    entry->heap_index = index;
}

static void heap_sift_up(struct heap *heap, size_t index)
{
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!entry_expires_before(heap->entries[index],
                                  heap->entries[parent])) {
            return;
        }
        heap_swap(heap, index, parent);
        index = parent;
    }
}

static bool entry_expires_before(const struct entry *left,
                                 const struct entry *right)
{
    return left->expires_at < right->expires_at;
}

static void heap_swap(struct heap *heap, size_t first, size_t second)
{
    struct entry *first_entry = heap->entries[first];
    heap_place(heap, first, heap->entries[second]);
    heap_place(heap, second, first_entry);
}

static void heap_sift_down(struct heap *heap, size_t index)
{
    for (;;) {
        size_t earliest = index;
        size_t left_child = 2 * index + 1;
        size_t right_child = left_child + 1;
        if (left_child < heap->count &&
            entry_expires_before(heap->entries[left_child],
                                 heap->entries[earliest])) {
            earliest = left_child;
        }
        if (right_child < heap->count &&
            entry_expires_before(heap->entries[right_child],
                                 heap->entries[earliest])) {
            earliest = right_child;
        }
        if (earliest == index) {
            return;
        }
        heap_swap(heap, index, earliest);
        index = earliest;
    }
}

/* heap_reserve로 칸을 확보한 뒤에만 부릅니다. */
static void heap_insert(struct heap *heap, struct entry *entry)
{
    assert(heap->count < heap->capacity);
    heap_place(heap, heap->count, entry);
    heap->count++;
    heap_sift_up(heap, entry->heap_index);
}

/* now 시각에 만료되지 않은 entry를 찾고, 없거나 만료됐으면 NULL을 돌려줍니다. */
static struct entry *live_entry_find(struct store *store, uint64_t now,
                                     const void *key, size_t key_len)
{
    struct entry probe = probe_for(key, key_len);
    struct hash_elem *found = hash_find(&store->table, &probe.elem);
    if (found == NULL) {
        return NULL;
    }
    struct entry *entry = hash_entry(found, struct entry, elem);

    return entry_is_expired(entry, now) ? NULL : entry;
}

/* 조회용 임시 entry입니다. key만 채우면 hash와 less가 동작합니다. */
static struct entry probe_for(const void *key, size_t key_len)
{
    return (struct entry){.key = bytes_or_empty(key, key_len),
                          .key_len = key_len};
}

/* 만료 시각과 같은 순간부터 만료입니다(spec "경계"). now는 STORE_NO_EXPIRY가
   아니므로, 만료 시각이 STORE_NO_EXPIRY인 entry는 만료되지 않습니다. */
static bool entry_is_expired(const struct entry *entry, uint64_t now)
{
    return now >= entry->expires_at;
}
