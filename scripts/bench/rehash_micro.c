/* rehash 범위(MAX, BEST, MIN_ELEMS_PER_BUCKET)에 따른 저장, 조회
 * 시간과 버킷 배열 메모리를 서버 없이 잽니다.
 *
 * 설정마다 src/hash.c를 -D로 따로 빌드한 바이너리가 하나씩 있고, make
 * rehash-micro가 회마다 설정을 번갈아 실행합니다(Makefile). 실행 한 번이 한
 * 줄을 출력합니다.
 *
 *   put_avg  store_set 하나의 평균 시간(ns)
 *   put_max  store_set 하나의 최대 시간(ms), 곧 가장 긴 rehash 멈춤
 *   rehash   버킷 수가 바뀐 store_set의 수
 *   get_avg  store_get 하나의 평균 시간(ns), key를 무작위 순서로 조회
 *   buckets  다 넣은 뒤의 버킷 수와 버킷 배열 메모리(MiB)
 *   rss_per_key  다 넣은 뒤 프로세스 RSS가 늘어난 양 ÷ key 수(바이트)
 *
 * 사용법: build/bench/rehash_micro_<MAX>_<BEST>_<MIN> [key 수, 기본 4194304] */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "list.h"
#include "store.h"

#define DEFAULT_KEY_COUNT 4194304u
#define KEY_NAME_MAX 32
#define VALUE_SIZE 100
#define NOW 1
#define SHUFFLE_SEED 0x9E3779B97F4A7C15ULL
/* 범위 상수는 0.5 같은 실수일 수 있으므로 빌드할 때 준 글자 그대로 출력합니다. */
#define TEXT(value) #value
#define MACRO_TEXT(value) TEXT(value)

struct put_result {
    double average_ns;
    double max_ms;
    size_t rehash_count;
};

static size_t key_count = DEFAULT_KEY_COUNT;

static struct put_result put_all(struct store *store);
static double get_all(struct store *store);
static size_t bucket_count(struct store *store);
static uint32_t *shuffled_indexes(void);
static size_t rss_bytes(void);
static size_t key_name(char *buffer, size_t index);
static double now_ns(void);

int main(int argc, char **argv)
{
    struct store *store = store_create();
    struct put_result put;
    double get_average_ns;
    size_t buckets;
    size_t rss_before;
    size_t rss_after;

    if (argc > 1) {
        key_count = strtoul(argv[1], NULL, 10);
    }
    assert(store != NULL && key_count > 0 && key_count <= UINT32_MAX);
    rss_before = rss_bytes();
    put = put_all(store);
    rss_after = rss_bytes();
    get_average_ns = get_all(store);
    buckets = bucket_count(store);
    printf("keys=%zu max=%s best=%s min=%s put_avg=%.1f put_max=%.3f rehash=%zu "
           "get_avg=%.1f buckets=%zu bucket_mib=%.1f rss_per_key=%.1f\n",
           key_count, MACRO_TEXT(MAX_ELEMS_PER_BUCKET),
           MACRO_TEXT(BEST_ELEMS_PER_BUCKET), MACRO_TEXT(MIN_ELEMS_PER_BUCKET),
           put.average_ns, put.max_ms, put.rehash_count, get_average_ns,
           buckets, (double)(buckets * sizeof(struct list)) / (1 << 20),
           (double)(rss_after - rss_before) / (double)key_count);

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
    for (size_t i = 0; i < key_count; i++) {
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
    result.average_ns = total_ns / (double)key_count;

    return result;
}

static double get_all(struct store *store)
{
    uint32_t *indexes = shuffled_indexes();
    char name[KEY_NAME_MAX];
    const void *value;
    size_t value_len;
    double start = now_ns();

    for (size_t i = 0; i < key_count; i++) {
        size_t name_len = key_name(name, indexes[i]);
        bool found = store_get(store, NOW, name, name_len, &value, &value_len);
        assert(found);
    }
    double elapsed = now_ns() - start;
    free(indexes);

    return elapsed / (double)key_count;
}

/* struct store의 첫 멤버가 struct hash table이므로(src/store.c), store를
 * 가리키는 포인터는 그 table을 가리키는 포인터로 바꿔 읽을 수 있습니다. */
static size_t bucket_count(struct store *store)
{
    return ((struct hash *)store)->bucket_cnt;
}

/* 0부터 key_count-1까지를 Fisher-Yates로 섞습니다. 실행마다 같은 순서입니다. */
static uint32_t *shuffled_indexes(void)
{
    uint32_t *indexes = malloc(key_count * sizeof *indexes);
    uint64_t state = SHUFFLE_SEED;

    assert(indexes != NULL);
    for (uint32_t i = 0; i < key_count; i++) {
        indexes[i] = i;
    }
    for (size_t i = key_count - 1; i > 0; i--) {
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

/* /proc/self/statm의 두 번째 값(상주 페이지 수)을 바이트로 바꿉니다. */
static size_t rss_bytes(void)
{
    FILE *statm = fopen("/proc/self/statm", "r");
    unsigned long total_pages = 0;
    unsigned long resident_pages = 0;
    int read_count;

    assert(statm != NULL);
    read_count = fscanf(statm, "%lu %lu", &total_pages, &resident_pages);
    assert(read_count == 2);
    (void)read_count;
    fclose(statm);

    return (size_t)resident_pages * (size_t)sysconf(_SC_PAGESIZE);
}

static double now_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1e9 + (double)now.tv_nsec;
}
