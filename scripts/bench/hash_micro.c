/* hash 함수 후보의 처리 시간과 버킷 분포를 서버 없이 잽니다.
 *
 * 실험 브랜치 CJG-hash-bench 전용입니다(mini-redis-ideas/hash-function.md).
 * 출력은 세 절입니다.
 *   time     key 길이마다 hash 한 번에 걸리는 시간(ns). 다음 key를 앞 hash
 *            값으로 고르므로, 호출이 겹치지 않고 한 번씩의 지연을 잽니다.
 *   buckets  key prefill-0부터 prefill-4194303까지를 hash & (bucket_count-1)로
 *            나눴을 때의 분포. 무작위로 흩어졌을 때의 기대값과 나란히 적습니다.
 *   seed     seed의 bit 40 하나만 바꿨을 때 버킷 번호가 그대로인 key의 비율.
 *            seed가 공격을 막는다면 0에 가까워야 합니다.
 *
 * 사용법: build/bench/hash_micro [반복 횟수, 기본 3] */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "hash.h"
#include "wyhash.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

/* vendor/siphash/siphash.c를 라운드 수만 바꿔 두 번 컴파일한 함수입니다(Makefile). */
int siphash_1_2(const void *input, size_t input_len, const void *key,
                uint8_t *output, size_t output_len);
int siphash_2_4(const void *input, size_t input_len, const void *key,
                uint8_t *output, size_t output_len);

#define SEED_SIZE 16
#define SEED_FLIP_BIT 40
#define KEY_POOL_COUNT 256
#define TIME_TOTAL_BYTES (256u << 20)
#define TIME_MIN_CALLS (1u << 22)
#define BUCKET_KEY_COUNT 4194304u
#define SEED_KEY_COUNT 100000u
#define SEED_BUCKET_BITS 21
#define KEY_NAME_MAX 32
#define DEFAULT_ROUNDS 3

struct candidate {
    const char *name;
    uint64_t (*hash)(const void *key, size_t key_len);
};

static const size_t key_lengths[] = {16, 256, 4096};
static const unsigned bucket_bits_list[] = {20, 21};

static unsigned char seed_bytes[SEED_SIZE];
static uint64_t seed_word;
static unsigned char xxh3_secret[XXH3_SECRET_DEFAULT_SIZE];
static volatile uint64_t sink;

static uint64_t fnv_current_hash(const void *key, size_t key_len);
static uint64_t fnv_seeded_hash(const void *key, size_t key_len);
static uint64_t wyhash_seeded_hash(const void *key, size_t key_len);
static uint64_t xxh3_seed_hash(const void *key, size_t key_len);
static uint64_t xxh3_secret_hash(const void *key, size_t key_len);
static uint64_t siphash_1_2_hash(const void *key, size_t key_len);
static uint64_t siphash_2_4_hash(const void *key, size_t key_len);
static void seed_set(const unsigned char *bytes);
static void time_section(int rounds);
static double time_per_call_ns(const struct candidate *candidate,
                               const unsigned char *keys, size_t key_len);
static double now_seconds(void);
static void buckets_section(void);
static void buckets_report(const struct candidate *candidate,
                           unsigned bucket_bits, uint32_t *counts);
static void seed_section(void);
static double seed_same_bucket_ratio(const struct candidate *candidate);
static size_t key_name(char *buffer, size_t index);

static const struct candidate candidates[] = {
    {"fnv-current", fnv_current_hash}, {"fnv-seed", fnv_seeded_hash},
    {"wyhash", wyhash_seeded_hash},    {"xxh3-seed", xxh3_seed_hash},
    {"xxh3-secret", xxh3_secret_hash}, {"siphash-1-2", siphash_1_2_hash},
    {"siphash-2-4", siphash_2_4_hash},
};
#define CANDIDATE_COUNT (sizeof candidates / sizeof candidates[0])

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : DEFAULT_ROUNDS;
    unsigned char bytes[SEED_SIZE];

    if (rounds < 1 || getrandom(bytes, sizeof bytes, 0) != sizeof bytes) {
        fprintf(stderr, "usage: hash_micro [rounds >= 1]\n");
        return 1;
    }
    seed_set(bytes);
    time_section(rounds);
    buckets_section();
    seed_section();

    return 0;
}

/* 지금 서버가 쓰는 함수 그대로입니다. seed가 없습니다. */
static uint64_t fnv_current_hash(const void *key, size_t key_len)
{
    return hash_bytes(key, key_len);
}

/* src/hash.c의 hash_bytes와 같은 반복문이고, 시작값만 seed입니다. */
static uint64_t fnv_seeded_hash(const void *key, size_t key_len)
{
    const unsigned char *bytes = key;
    uint64_t hash = seed_word;

    for (size_t i = 0; i < key_len; i++) {
        hash = (hash * 0x00000100000001B3UL) ^ bytes[i];
    }

    return hash;
}

static uint64_t wyhash_seeded_hash(const void *key, size_t key_len)
{
    return wyhash(key, key_len, seed_word, _wyp);
}

/* 입력이 240바이트를 넘으면 호출마다 secret을 seed로 다시 만듭니다
 * (xxhash.h의 XXH3_hashLong_64b_withSeed_internal). */
static uint64_t xxh3_seed_hash(const void *key, size_t key_len)
{
    return XXH3_64bits_withSeed(key, key_len, seed_word);
}

/* secret은 seed_set에서 한 번만 만듭니다. */
static uint64_t xxh3_secret_hash(const void *key, size_t key_len)
{
    return XXH3_64bits_withSecretandSeed(key, key_len, xxh3_secret,
                                         sizeof xxh3_secret, seed_word);
}

static uint64_t siphash_1_2_hash(const void *key, size_t key_len)
{
    uint64_t hash;

    siphash_1_2(key, key_len, seed_bytes, (uint8_t *)&hash, sizeof hash);
    return hash;
}

static uint64_t siphash_2_4_hash(const void *key, size_t key_len)
{
    uint64_t hash;

    siphash_2_4(key, key_len, seed_bytes, (uint8_t *)&hash, sizeof hash);
    return hash;
}

/* 모든 후보의 seed를 BYTES에서 정합니다. 64비트 seed는 앞 8바이트를
 * little endian으로 읽은 값이라서, SipHash의 k0와 같은 bit를 씁니다. */
static void seed_set(const unsigned char *bytes)
{
    memcpy(seed_bytes, bytes, SEED_SIZE);
    seed_word = 0;
    for (int i = 7; i >= 0; i--) {
        seed_word = (seed_word << 8) | bytes[i];
    }
    XXH3_generateSecret_fromSeed(xxh3_secret, seed_word);
}

static void time_section(int rounds)
{
    size_t length_count = sizeof key_lengths / sizeof key_lengths[0];
    unsigned char *keys = malloc(KEY_POOL_COUNT * 4096);

    if (keys == NULL || getrandom(keys, KEY_POOL_COUNT * 4096, 0) < 0) {
        fprintf(stderr, "key pool failed\n");
        exit(1);
    }
    printf("# time: ns per call, %d rounds\n", rounds);
    printf("%-12s %6s", "candidate", "len");
    for (int round = 0; round < rounds; round++) {
        printf(" %9s%d", "round", round + 1);
    }
    printf("\n");
    for (size_t length = 0; length < length_count; length++) {
        for (size_t c = 0; c < CANDIDATE_COUNT; c++) {
            printf("%-12s %6zu", candidates[c].name, key_lengths[length]);
            for (int round = 0; round < rounds; round++) {
                printf(" %10.1f", time_per_call_ns(&candidates[c], keys,
                                                   key_lengths[length]));
            }
            printf("\n");
        }
    }
    free(keys);
}

static double time_per_call_ns(const struct candidate *candidate,
                               const unsigned char *keys, size_t key_len)
{
    size_t calls = TIME_TOTAL_BYTES / key_len;
    size_t index = 0;
    uint64_t hash = 0;
    double start;

    if (calls < TIME_MIN_CALLS) {
        calls = TIME_MIN_CALLS;
    }
    for (size_t i = 0; i < KEY_POOL_COUNT; i++) { /* 캐시를 데웁니다 */
        hash ^= candidate->hash(keys + i * key_len, key_len);
    }
    start = now_seconds();
    for (size_t i = 0; i < calls; i++) {
        hash = candidate->hash(keys + index * key_len, key_len);
        index = (size_t)(hash ^ i) & (KEY_POOL_COUNT - 1);
    }
    sink ^= hash;

    return (now_seconds() - start) * 1e9 / (double)calls;
}

static double now_seconds(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void buckets_section(void)
{
    size_t bits_count = sizeof bucket_bits_list / sizeof bucket_bits_list[0];
    uint32_t *counts = malloc(sizeof *counts << 21);

    if (counts == NULL) {
        fprintf(stderr, "bucket counts failed\n");
        exit(1);
    }
    printf("\n# buckets: %u keys prefill-N\n", BUCKET_KEY_COUNT);
    printf("%-12s %4s %4s %9s %9s %9s %9s\n", "candidate", "bits", "max",
           "empty%", "expect%", "probe", "expect");
    for (size_t bits = 0; bits < bits_count; bits++) {
        for (size_t c = 0; c < CANDIDATE_COUNT; c++) {
            buckets_report(&candidates[c], bucket_bits_list[bits], counts);
        }
    }
    free(counts);
}

/* probe는 있는 key를 찾을 때 평균 몇 개의 entry를 비교하는지입니다.
 * 버킷의 entry 수를 c라 하면 그 버킷의 key들은 모두 c(c+1)/2번 비교합니다.
 * 무작위로 흩어지면 버킷당 entry 수는 평균 load인 포아송 분포를 따르므로,
 * 빈 버킷 비율은 e^-load, probe는 1 + load/2가 기대값입니다. */
static void buckets_report(const struct candidate *candidate,
                           unsigned bucket_bits, uint32_t *counts)
{
    size_t bucket_count = (size_t)1 << bucket_bits;
    double load = (double)BUCKET_KEY_COUNT / (double)bucket_count;
    char name[KEY_NAME_MAX];
    uint32_t max_count = 0;
    size_t empty_count = 0;
    double comparisons = 0;

    memset(counts, 0, bucket_count * sizeof *counts);
    for (size_t i = 0; i < BUCKET_KEY_COUNT; i++) {
        size_t name_len = key_name(name, i);
        counts[candidate->hash(name, name_len) & (bucket_count - 1)]++;
    }
    for (size_t bucket = 0; bucket < bucket_count; bucket++) {
        uint32_t count = counts[bucket];
        max_count = count > max_count ? count : max_count;
        empty_count += count == 0;
        comparisons += (double)count * (count + 1) / 2;
    }
    printf("%-12s %4u %4u %9.3f %9.3f %9.4f %9.4f\n", candidate->name,
           bucket_bits, max_count, 100.0 * empty_count / bucket_count,
           100.0 * exp(-load), comparisons / BUCKET_KEY_COUNT,
           1 + load / 2);
}

static void seed_section(void)
{
    printf("\n# seed: same bucket after flipping seed bit %d, %u keys, "
           "%d bucket bits (random: %.6f%%)\n",
           SEED_FLIP_BIT, SEED_KEY_COUNT, SEED_BUCKET_BITS,
           100.0 / (double)((size_t)1 << SEED_BUCKET_BITS));
    for (size_t c = 0; c < CANDIDATE_COUNT; c++) {
        printf("%-12s %9.3f%%\n", candidates[c].name,
               100.0 * seed_same_bucket_ratio(&candidates[c]));
    }
}

static double seed_same_bucket_ratio(const struct candidate *candidate)
{
    size_t mask = ((size_t)1 << SEED_BUCKET_BITS) - 1;
    unsigned char original[SEED_SIZE];
    unsigned char flipped[SEED_SIZE];
    char name[KEY_NAME_MAX];
    size_t same_count = 0;

    memcpy(original, seed_bytes, SEED_SIZE);
    memcpy(flipped, seed_bytes, SEED_SIZE);
    flipped[SEED_FLIP_BIT / 8] ^= (unsigned char)(1u << (SEED_FLIP_BIT % 8));
    for (size_t i = 0; i < SEED_KEY_COUNT; i++) {
        size_t name_len = key_name(name, i);
        seed_set(original);
        uint64_t before = candidate->hash(name, name_len) & mask;
        seed_set(flipped);
        uint64_t after = candidate->hash(name, name_len) & mask;
        same_count += before == after;
    }
    seed_set(original);

    return (double)same_count / SEED_KEY_COUNT;
}

/* bench.py와 prefill.lua가 채우는 key와 같은 이름입니다. */
static size_t key_name(char *buffer, size_t index)
{
    return (size_t)snprintf(buffer, KEY_NAME_MAX, "prefill-%zu", index);
}
