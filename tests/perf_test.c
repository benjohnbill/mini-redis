#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "acutest.h"
#include "hash.h"

/* 버킷 수를 늘리는 옮기기가 시작될 때까지 넣는 key 수의 하한입니다. 이만큼
   넣은 뒤에 시작하는 옮기기는 옛 버킷이 수만 개라서, 옮기는 도중에 여러
   동작을 해 볼 수 있습니다. */
#define GROW_MIN_COUNT 100000
/* 옮기는 도중에 바꿔 보는 key 수입니다. 동작마다 옛 버킷을 하나씩 옮기므로,
   옛 버킷 수보다 충분히 작아야 바꾸는 내내 옮기는 중입니다. */
#define CHANGE_COUNT 1000
/* 정답표와 비교하는 무작위 시험의 key 범위, 단계 수, 씨앗입니다. */
#define RANDOM_KEY_RANGE 50000
#define RANDOM_GROW_STEPS 400000
#define RANDOM_SHRINK_STEPS 200000
#define RANDOM_SEED 12345

/* 테이블에 넣는 원소입니다. value는 교체한 원소를 구분하는 데 씁니다. */
struct item {
    int key;
    int value;
    struct hash_elem elem;
};

static uint64_t item_hash(const struct hash_elem *element, void *aux)
{
    (void)aux;
    return hash_int(hash_entry(element, struct item, elem)->key);
}

static bool item_less(const struct hash_elem *left,
                      const struct hash_elem *right, void *aux)
{
    (void)aux;
    return hash_entry(left, struct item, elem)->key <
           hash_entry(right, struct item, elem)->key;
}

static struct item *item_create(int key, int value)
{
    struct item *item = malloc(sizeof *item);
    if (!TEST_CHECK(item != NULL)) {
        abort();
    }
    item->key = key;
    item->value = value;

    return item;
}

/* hash_destroy와 hash_clear가 원소마다 부릅니다. aux가 NULL이 아니면 해제한
   원소 수를 셉니다. */
static void item_destroy(struct hash_elem *element, void *aux)
{
    if (aux != NULL) {
        (*(size_t *)aux)++;
    }
    free(hash_entry(element, struct item, elem));
}

/* key를 찾아 원소를 돌려줍니다. 없으면 NULL입니다. */
static struct item *table_find(struct hash *table, int key)
{
    struct item probe = {.key = key};
    struct hash_elem *found = hash_find(table, &probe.elem);

    return found == NULL ? NULL : hash_entry(found, struct item, elem);
}

/* key 0부터 차례로 넣다가, min_count개 이상 넣은 뒤 옮기기가 시작되면
   멈추고 넣은 수를 돌려줍니다. */
static int fill_until_rehashing(struct hash *table, int min_count)
{
    int count = 0;
    while (count < min_count || !hash_is_rehashing(table)) {
        TEST_CHECK(hash_insert(table, &item_create(count, 0)->elem) == NULL);
        count++;
    }

    return count;
}

/* 조회를 되풀이해 진행 중인 옮기기를 끝냅니다. */
static void finish_rehashing(struct hash *table)
{
    while (hash_is_rehashing(table)) {
        table_find(table, -1);
    }
}

static void grow_keeps_all_keys_findable(void)
{
    struct hash table;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, NULL));
    int count = fill_until_rehashing(&table, GROW_MIN_COUNT);

    TEST_CHECK(hash_is_rehashing(&table));
    TEST_CHECK(hash_size(&table) == (size_t)count);
    for (int key = 0; key < count; key++) {
        struct item *item = table_find(&table, key);
        TEST_CHECK(item != NULL && item->key == key);
    }
    TEST_CHECK(table_find(&table, count) == NULL);
    hash_destroy(&table, item_destroy);
}

static void rehash_finishes_with_lookups_only(void)
{
    struct hash table;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, NULL));
    int count = fill_until_rehashing(&table, GROW_MIN_COUNT);

    for (int key = 0; key < count; key++) {
        table_find(&table, key);
    }
    TEST_CHECK(!hash_is_rehashing(&table));
    hash_destroy(&table, item_destroy);
}

/* 옮기는 도중에 key 0..999를 지우고, 1000..1999를 value 1로 교체하고,
   count..count+999를 새로 넣고, 이미 있는 key 2000을 다시 넣어 봅니다. */
static void changes_during_rehash_are_kept(void)
{
    struct hash table;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, NULL));
    int count = fill_until_rehashing(&table, GROW_MIN_COUNT);

    for (int key = 0; key < CHANGE_COUNT; key++) {
        struct item probe = {.key = key};
        struct hash_elem *removed = hash_delete(&table, &probe.elem);
        if (TEST_CHECK(removed != NULL)) {
            item_destroy(removed, NULL);
        }
    }
    for (int key = CHANGE_COUNT; key < 2 * CHANGE_COUNT; key++) {
        struct hash_elem *replaced =
            hash_replace(&table, &item_create(key, 1)->elem);
        if (TEST_CHECK(replaced != NULL)) {
            item_destroy(replaced, NULL);
        }
    }
    for (int key = count; key < count + CHANGE_COUNT; key++) {
        TEST_CHECK(hash_insert(&table, &item_create(key, 0)->elem) == NULL);
    }
    struct item *duplicate = item_create(2 * CHANGE_COUNT, 1);
    TEST_CHECK(hash_insert(&table, &duplicate->elem) != NULL);
    free(duplicate);
    TEST_CHECK(hash_is_rehashing(&table));

    TEST_CHECK(hash_size(&table) == (size_t)count);
    for (int key = 0; key < count + CHANGE_COUNT; key++) {
        struct item *item = table_find(&table, key);
        if (key < CHANGE_COUNT) {
            TEST_CHECK(item == NULL);
            TEST_MSG("지운 key %d가 남아 있다", key);
        } else if (TEST_CHECK(item != NULL)) {
            int want = key < 2 * CHANGE_COUNT ? 1 : 0;
            TEST_CHECK(item->value == want);
            TEST_MSG("key %d의 value가 %d가 아니다", key, want);
        }
    }
    hash_destroy(&table, item_destroy);
}

/* key 100,000개를 넣은 뒤 0부터 지워, 버킷 수를 줄이는 옮기기가 시작되면
   남은 key를 확인합니다. */
static void shrink_keeps_remaining_keys_findable(void)
{
    struct hash table;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, NULL));
    int count = 0;
    for (; count < GROW_MIN_COUNT; count++) {
        hash_insert(&table, &item_create(count, 0)->elem);
    }
    finish_rehashing(&table);

    int removed = 0;
    while (!hash_is_rehashing(&table) && removed < count) {
        struct item probe = {.key = removed};
        item_destroy(hash_delete(&table, &probe.elem), NULL);
        removed++;
    }

    TEST_CHECK(hash_is_rehashing(&table));
    TEST_CHECK(hash_size(&table) == (size_t)(count - removed));
    /* 조회도 옮기기를 진행하므로, 옮기는 도중에 확인하려면 남은 key부터
       봅니다. 지운 key부터 보면 남은 key에 닿기 전에 옮기기가 끝납니다. */
    for (int key = removed; key < count; key++) {
        TEST_CHECK(table_find(&table, key) != NULL);
        TEST_MSG("남은 key %d를 찾지 못한다", key);
    }
    for (int key = 0; key < removed; key++) {
        TEST_CHECK(table_find(&table, key) == NULL);
        TEST_MSG("지운 key %d가 남아 있다", key);
    }
    hash_destroy(&table, item_destroy);
}

static void count_action(struct hash_elem *element, void *aux)
{
    (void)element;
    (*(size_t *)aux)++;
}

static void iteration_during_rehash_visits_each_once(void)
{
    struct hash table;
    size_t applied = 0;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, &applied));
    int count = fill_until_rehashing(&table, GROW_MIN_COUNT);
    unsigned char *seen = calloc((size_t)count, 1);
    TEST_ASSERT(seen != NULL);

    struct hash_iterator iterator;
    hash_first(&iterator, &table);
    size_t visited = 0;
    while (hash_next(&iterator) != NULL) {
        int key = hash_entry(hash_cur(&iterator), struct item, elem)->key;
        if (TEST_CHECK(key >= 0 && key < count)) {
            TEST_CHECK(seen[key] == 0);
            seen[key] = 1;
        }
        visited++;
    }
    TEST_CHECK(visited == (size_t)count);
    TEST_CHECK(!hash_is_rehashing(&table));

    hash_apply(&table, count_action);
    TEST_CHECK(applied == (size_t)count);
    free(seen);
    hash_destroy(&table, item_destroy);
}

static void destroy_during_rehash_frees_each_once(void)
{
    struct hash table;
    size_t destroyed = 0;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, &destroyed));
    int count = fill_until_rehashing(&table, GROW_MIN_COUNT);

    TEST_CHECK(hash_is_rehashing(&table));
    hash_destroy(&table, item_destroy);
    TEST_CHECK(destroyed == (size_t)count);
}

static uint64_t random_next(uint64_t *state)
{
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;

    return *state >> 33;
}

/* 동작 하나를 테이블과 정답표에 함께 하고, 테이블이 돌려준 원소가 정답표와
   같은지 확인합니다. action은 0 저장, 1 교체, 2 삭제, 3 조회입니다. */
static void random_apply(struct hash *table, struct item **expected, int action,
                         int key)
{
    struct item *want = expected[key];
    struct hash_elem *want_elem = want == NULL ? NULL : &want->elem;
    struct item probe = {.key = key};

    if (action == 0) {
        struct item *item = item_create(key, 0);
        TEST_CHECK(hash_insert(table, &item->elem) == want_elem);
        if (want == NULL) {
            expected[key] = item;
        } else {
            free(item);
        }
    } else if (action == 1) {
        struct item *item = item_create(key, 1);
        TEST_CHECK(hash_replace(table, &item->elem) == want_elem);
        free(want);
        expected[key] = item;
    } else if (action == 2) {
        TEST_CHECK(hash_delete(table, &probe.elem) == want_elem);
        free(want);
        expected[key] = NULL;
    } else {
        TEST_CHECK(hash_find(table, &probe.elem) == want_elem);
    }
}

/* 앞 단계는 저장, 교체, 삭제, 조회를 같은 비율로 해서 테이블을 키우고, 뒤
   단계는 삭제를 4번 중 3번으로 해서 줄입니다. 그 사이에 늘리기와 줄이기의
   옮기기를 모두 지납니다. */
static void random_operations_match_reference(void)
{
    struct hash table;
    TEST_ASSERT(hash_init(&table, item_hash, item_less, NULL));
    struct item **expected = calloc(RANDOM_KEY_RANGE, sizeof *expected);
    TEST_ASSERT(expected != NULL);
    uint64_t state = RANDOM_SEED;
    size_t rehashing_steps = 0;

    for (int step = 0; step < RANDOM_GROW_STEPS + RANDOM_SHRINK_STEPS; step++) {
        uint64_t random = random_next(&state);
        int key = (int)(random % RANDOM_KEY_RANGE);
        int action = (int)((random / RANDOM_KEY_RANGE) % 4);
        if (step >= RANDOM_GROW_STEPS && action != 3) {
            action = 2;
        }
        random_apply(&table, expected, action, key);
        if (hash_is_rehashing(&table)) {
            rehashing_steps++;
        }
    }

    size_t present = 0;
    for (int key = 0; key < RANDOM_KEY_RANGE; key++) {
        present += expected[key] != NULL;
    }
    TEST_CHECK(hash_size(&table) == present);
    TEST_CHECK(rehashing_steps > 0);
    TEST_MSG("옮기는 도중에 한 동작 수: %zu", rehashing_steps);
    free(expected);
    hash_destroy(&table, item_destroy);
}

TEST_LIST = {
    {"버킷 수를 늘리며 옮기는 도중에도 넣은 key를 모두 찾는다",
     grow_keeps_all_keys_findable},
    {"조회만 와도 옮기기가 끝난다", rehash_finishes_with_lookups_only},
    {"옮기는 도중에 지우고, 교체하고, 넣은 결과가 그대로 남는다",
     changes_during_rehash_are_kept},
    {"버킷 수를 줄이며 옮기는 도중에도 남은 key를 모두 찾고 지운 key는 "
     "찾지 못한다",
     shrink_keeps_remaining_keys_findable},
    {"옮기는 도중에 훑으면 모든 원소를 한 번씩 만난다",
     iteration_during_rehash_visits_each_once},
    {"옮기는 도중에 파괴하면 모든 원소를 한 번씩 해제한다",
     destroy_during_rehash_frees_each_once},
    {"무작위 저장, 교체, 삭제, 조회의 결과가 정답표와 같다",
     random_operations_match_reference},
    {NULL, NULL},
};
