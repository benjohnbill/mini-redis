#ifndef ORIGIN_H
#define ORIGIN_H

#include <stdbool.h>
#include <stddef.h>

#define NAME_MAX_BYTES 120
#define PRICE_MAX 1000000000

enum list_sort { LIST_SORT_ID, LIST_SORT_PRICE_ASC, LIST_SORT_PRICE_DESC };

/* 검증된 목록 조건입니다. 캐시 key와 SQL은 이 값만 씁니다. */
struct list_query {
    char q[NAME_MAX_BYTES + 1];
    size_t q_len;
    int min_price;
    int max_price;
    int offset;
    int limit;
    enum list_sort sort;
};

struct origin;

/* name은 NUL로 끝난 UTF-8이고 name_len은 그 바이트 길이입니다. false를
   돌려주면 호출자는 원본 오류로 처리합니다. */
typedef bool (*origin_row_handle)(void *context, int product_id,
                                  const char *name, size_t name_len, int price);

/* spec은 SQLite 파일 경로이거나 PostgreSQL 연결 문자열입니다. create가 참이면
   상품 표와 인덱스를 만들고 기존 행을 지웁니다. schema가 없으면 NULL입니다. */
struct origin *origin_open(const char *spec, bool create);
void origin_destroy(struct origin *origin);
bool origin_seed(struct origin *origin, int seed, int count);
const char *origin_name(void);

/* callback이 false를 돌려주거나 질의가 실패하면 503입니다. 상품 조회는 행이
   없으면 404이고, 목록은 조건에 맞는 행이 없어도 200입니다. */
int origin_product_read(struct origin *origin, int product_id,
                        origin_row_handle handle, void *context, int *count);
int origin_list_read(struct origin *origin, const struct list_query *list,
                     origin_row_handle handle, void *context);
int origin_product_write(struct origin *origin, int product_id,
                         const char *name, size_t name_len, int price);
int origin_product_remove(struct origin *origin, int product_id);

#endif
