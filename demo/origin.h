#ifndef ORIGIN_H
#define ORIGIN_H

#include <stdbool.h>
#include <stddef.h>

#define PRODUCT_ID_MAX 2147483647
#define NAME_MAX_BYTES 120
#define PRICE_MAX 1000000000

/* 숫자 상수를 SQL 문자열 리터럴 안에 넣습니다. 스키마의 CHECK 범위가 C의 상한과
   어긋나지 않게 하기 위해서입니다. */
#define SQL_NUMBER(constant) SQL_NUMBER_TEXT(constant)
#define SQL_NUMBER_TEXT(constant) #constant

enum list_sort { LIST_SORT_ID, LIST_SORT_PRICE_ASC, LIST_SORT_PRICE_DESC };

/* 검증된 목록 조건입니다. 캐시 key와 SQL은 이 값만 씁니다. */
struct list_query {
    char search_text[NAME_MAX_BYTES + 1];
    size_t search_text_len;
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

/* spec은 SQLite 파일 경로이거나 PostgreSQL 연결 문자열입니다. seeding이 참이면
   상품 표가 아직 없어도 되고, origin_seed가 표를 만듭니다. seeding이 거짓인데
   상품 표가 없으면 NULL입니다. */
struct origin *origin_create(const char *spec, bool seeding);
void origin_destroy(struct origin *origin);
bool origin_seed(struct origin *origin, int seed, int count);
const char *origin_name(void);

/* callback이 false를 돌려주거나 질의가 실패하면 503입니다. 상품 조회는 행이
   없으면 404이고, 목록은 조건에 맞는 행이 없어도 200입니다. */
int origin_product_get(struct origin *origin, int product_id,
                       origin_row_handle handle, void *context, int *count);
int origin_list_get(struct origin *origin, const struct list_query *list,
                    origin_row_handle handle, void *context);
int origin_product_set(struct origin *origin, int product_id, const char *name,
                       size_t name_len, int price);
int origin_product_delete(struct origin *origin, int product_id);

#endif
