/* rehash 범위(MAX_ELEMS_PER_BUCKET, BEST_ELEMS_PER_BUCKET)에 따른 저장, 조회
 * 시간과 버킷 배열 메모리를 서버 없이 잽니다.
 *
 * 실험 브랜치 CJG-hash-bench 전용입니다(mini-redis-ideas/rehash-thresholds.md).
 * 설정마다 src/hash.c를 -D로 따로 빌드한 바이너리가 하나씩 있고, make
 * rehash-micro가 회마다 세 설정을 번갈아 실행합니다. 실행 한 번이 한 줄을
 * 출력합니다.
 *
 *   put_avg  store_set 하나의 평균 시간(ns)
 *   put_max  store_set 하나의 최대 시간(ms), 곧 가장 긴 rehash 멈춤
 *   rehash   버킷 수가 바뀐 store_set의 수
 *   get_avg  store_get 하나의 평균 시간(ns), key를 무작위 순서로 조회
 *   buckets  다 넣은 뒤의 버킷 수와 버킷 배열 메모리(MiB)
 *
 * 사용법: build/bench/rehash_micro_<MAX>_<BEST> */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"
#include "list.h"
#include "store.h"

#define KEY_COUNT 4194304u
#define KEY_NAME_MAX 32
#define VALUE_SIZE 100
#define NOW 1
#define SHUFFLE_SEED 0x9E3779B97F4A7C15ULL

struct put_result {
    double average_ns;
    double max_ms;
    size_t rehash_count;
};

static struct put_result put_all(struct store *store);
static double get_all(struct store *store);
static size_t bucket_count(struct store *store);
static uint32_t *shuffled_indexes(void);
static size_t key_name(char *buffer, size_t index);
static double now_ns(void);

int main(void)
{
    struct store *store = store_create();
    struct put_result put;
    double get_average_ns;
    size_t buckets;

    assert(store != NULL);
    put = put_all(store);
    get_average_ns = get_all(store);
    buckets = bucket_count(store);
    printf("max=%d best=%d put_avg=%.1f put_max=%.3f rehash=%zu "
           "get_avg=%.1f buckets=%zu bucket_mib=%.1f\n",
           MAX_ELEMS_PER_BUCKET, BEST_ELEMS_PER_BUCKET, put.average_ns,
           put.max_ms, put.rehash_count, get_average_ns, buckets,
           (double)(buckets * sizeof(struct list)) / (1 << 20));

    return 0;
}

static struct put_result put_all(struct store *store)
{
    struct put_result result = {0};
    unsigned char value[VALUE_SIZE];
    char name[KEY_NAME_MAX];
    size_t buckets_before = bucket_count(store);
    double total_ns = 0;

    memset(value, 'v', sizeof value);
    for (size_t i = 0; i < KEY_COUNT; i++) {
        size_t name_len = key_name(name, i);
        double start = now_ns();
        bool stored = store_set(store, name, name_len, value, sizeof value,
                                STORE_NO_EXPIRY);
        double elapsed = now_ns() - start;
        size_t buckets_after = bucket_count(store);

        assert(stored);
        total_ns += elapsed;
        if (elapsed / 1e6 > result.max_ms) {
            result.max_ms = elapsed / 1e6;
        }
        result.rehash_count += buckets_after != buckets_before;
        buckets_before = buckets_after;
    }
    result.average_ns = total_ns / KEY_COUNT;

    return result;
}

static double get_all(struct store *store)
{
    uint32_t *indexes = shuffled_indexes();
    char name[KEY_NAME_MAX];
    const void *value;
    size_t value_len;
    double start = now_ns();

    for (size_t i = 0; i < KEY_COUNT; i++) {
        size_t name_len = key_name(name, indexes[i]);
        bool found = store_get(store, NOW, name, name_len, &value, &value_len);
        assert(found);
    }
    double elapsed = now_ns() - start;
    free(indexes);

    return elapsed / KEY_COUNT;
}

/* struct store의 첫 멤버가 struct hash table이므로(src/store.c), store를
 * 가리키는 포인터는 그 table을 가리키는 포인터로 바꿔 읽을 수 있습니다. */
static size_t bucket_count(struct store *store)
{
    return ((struct hash *)store)->bucket_cnt;
}

/* 0부터 KEY_COUNT-1까지를 Fisher-Yates로 섞습니다. 실행마다 같은 순서입니다. */
static uint32_t *shuffled_indexes(void)
{
    uint32_t *indexes = malloc(KEY_COUNT * sizeof *indexes);
    uint64_t state = SHUFFLE_SEED;

    assert(indexes != NULL);
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        indexes[i] = i;
    }
    for (size_t i = KEY_COUNT - 1; i > 0; i--) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        size_t other = (size_t)(state % (i + 1));
        uint32_t saved = indexes[i];
        indexes[i] = indexes[other];
        indexes[other] = saved;
    }

    return indexes;
}

/* bench.py와 prefill.lua가 채우는 key와 같은 이름입니다. */
static size_t key_name(char *buffer, size_t index)
{
    return (size_t)snprintf(buffer, KEY_NAME_MAX, "prefill-%zu", index);
}

static double now_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1e9 + (double)now.tv_nsec;
}
