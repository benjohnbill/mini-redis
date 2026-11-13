#include "store.h"

#include <assert.h>
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
    const unsigned char *key; /* 조회용 임시 entry는 호출한 쪽 key를 가리킵니다. */
    size_t key_len;
    const unsigned char *value;
    size_t value_len;
    unsigned char bytes[];
};

/* hash_bytes는 NULL 주소에서 assert로 멈추고, memcmp에 NULL을 넘기면 길이가
   0이어도 정의되지 않은 동작입니다. 0바이트 key를 NULL로 받으면 이 주소로 바꿉니다. */
static const unsigned char empty_bytes[1];

static const unsigned char *bytes_or_empty(const void *p, size_t len) {
    assert(p != NULL || len == 0);
    return p != NULL ? p : empty_bytes;
}

/* value는 해시하지 않습니다. 같은 key는 value와 상관없이 같은 버킷에 가야 합니다. */
static uint64_t entry_hash(const struct hash_elem *e, void *aux) {
    (void)aux;
    const struct entry *en = hash_entry(e, struct entry, elem);
    return hash_bytes(en->key, en->key_len);
}

/* hash.c는 less(a, b)와 less(b, a)가 모두 거짓이면 같은 key로 봅니다.
   길이 비교가 없으면 "abc"와 "abcd"를 같은 key로 보게 됩니다. */
static bool entry_less(const struct hash_elem *a, const struct hash_elem *b,
                       void *aux) {
    (void)aux;
    const struct entry *ea = hash_entry(a, struct entry, elem);
    const struct entry *eb = hash_entry(b, struct entry, elem);
    size_t n = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;
    int cmp = memcmp(ea->key, eb->key, n);
    if (cmp != 0) return cmp < 0;
    return ea->key_len < eb->key_len;
}

static void entry_free(struct hash_elem *e, void *aux) {
    (void)aux;
    free(hash_entry(e, struct entry, elem));
}

struct store *store_create(void) {
    struct store *s = malloc(sizeof *s);
    if (s == NULL) return NULL;
    if (!hash_init(&s->table, entry_hash, entry_less, NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

void store_destroy(struct store *s) {
    assert(s != NULL);
    hash_destroy(&s->table, entry_free);
    free(s);
}

bool store_set(struct store *s, const void *key, size_t key_len,
               const void *value, size_t value_len) {
    assert(s != NULL);
    const unsigned char *k = bytes_or_empty(key, key_len);
    const unsigned char *v = bytes_or_empty(value, value_len);

    /* 새 entry를 먼저 다 만든 뒤에 테이블에 넣습니다. 할당이 실패하면 테이블은
       그대로입니다. */
    struct entry *en = malloc(sizeof *en + key_len + value_len);
    if (en == NULL) return false;
    memcpy(en->bytes, k, key_len);
    memcpy(en->bytes + key_len, v, value_len);
    en->key = en->bytes;
    en->key_len = key_len;
    en->value = en->bytes + key_len;
    en->value_len = value_len;

    /* 같은 key가 있으면 hash_replace가 옛 entry를 빼서 돌려줍니다. */
    struct hash_elem *old = hash_replace(&s->table, &en->elem);
    if (old != NULL) entry_free(old, NULL);
    return true;
}

bool store_get(struct store *s, const void *key, size_t key_len,
               const void **value, size_t *value_len) {
    assert(s != NULL && value != NULL && value_len != NULL);

    /* 조회용 임시 entry입니다. key만 채우면 hash와 less가 동작합니다. */
    struct entry probe = {.key = bytes_or_empty(key, key_len), .key_len = key_len};
    struct hash_elem *found = hash_find(&s->table, &probe.elem);
    if (found == NULL) return false;

    const struct entry *en = hash_entry(found, struct entry, elem);
    *value = en->value;
    *value_len = en->value_len;
    return true;
}

bool store_delete(struct store *s, const void *key, size_t key_len) {
    assert(s != NULL);

    /* store_get과 같은 조회용 임시 entry입니다. hash_delete는 테이블에서 뺀
       entry를 돌려줄 뿐 해제하지 않으므로, 해제는 store가 합니다. */
    struct entry probe = {.key = bytes_or_empty(key, key_len), .key_len = key_len};
    struct hash_elem *removed = hash_delete(&s->table, &probe.elem);
    if (removed == NULL) return false;
    entry_free(removed, NULL);
    return true;
}
