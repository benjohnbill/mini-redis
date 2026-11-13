#include "store.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hash.h"

struct store {
    struct hash table;
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
    unsigned char bytes[];
};

/* hash_bytes는 NULL 주소에서 assert로 멈추고, memcmp에 NULL을 넘기면 길이가
   0이어도 정의되지 않은 동작입니다. 0바이트 key를 NULL로 받으면 이 주소로 바꿉니다. */
static const unsigned char empty_bytes[1];

static const unsigned char *bytes_or_empty(const void *bytes, size_t len)
{
    assert(bytes != NULL || len == 0);
    return bytes != NULL ? bytes : empty_bytes;
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
                                  const void *value, size_t value_len)
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
    entry->key = entry->bytes;
    entry->key_len = key_len;
    entry->value = entry->bytes + key_len;
    entry->value_len = value_len;
    return entry;
}

/* 조회용 임시 entry입니다. key만 채우면 hash와 less가 동작합니다. */
static struct entry probe_for(const void *key, size_t key_len)
{
    return (struct entry){.key = bytes_or_empty(key, key_len),
                          .key_len = key_len};
}

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
    return store;
}

void store_destroy(struct store *store)
{
    assert(store != NULL);
    hash_destroy(&store->table, entry_free);
    free(store);
}

bool store_set(struct store *store, const void *key, size_t key_len,
               const void *value, size_t value_len)
{
    assert(store != NULL);

    /* 새 entry를 먼저 다 만든 뒤에 테이블에 넣습니다. 만들기가 실패하면
       테이블은 그대로입니다. */
    struct entry *entry = entry_create(key, key_len, value, value_len);
    if (entry == NULL) {
        return false;
    }

    /* 같은 key가 있으면 hash_replace가 옛 entry를 빼서 돌려줍니다. */
    struct hash_elem *replaced = hash_replace(&store->table, &entry->elem);
    if (replaced != NULL) {
        entry_free(replaced, NULL);
    }
    return true;
}

bool store_get(struct store *store, const void *key, size_t key_len,
               const void **value, size_t *value_len)
{
    assert(store != NULL && value != NULL && value_len != NULL);

    struct entry probe = probe_for(key, key_len);
    struct hash_elem *found = hash_find(&store->table, &probe.elem);
    if (found == NULL) {
        return false;
    }

    const struct entry *entry = hash_entry(found, struct entry, elem);
    *value = entry->value;
    *value_len = entry->value_len;
    return true;
}

bool store_delete(struct store *store, const void *key, size_t key_len)
{
    assert(store != NULL);

    /* hash_delete는 테이블에서 뺀 entry를 돌려줄 뿐 해제하지 않으므로, 해제는
       store가 합니다. */
    struct entry probe = probe_for(key, key_len);
    struct hash_elem *removed = hash_delete(&store->table, &probe.elem);
    if (removed == NULL) {
        return false;
    }
    entry_free(removed, NULL);
    return true;
}
