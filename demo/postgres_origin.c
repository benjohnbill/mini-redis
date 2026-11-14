#include "origin.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SEED_MULTIPLIER 48271
#define SEED_NAME_MAX 64
#define LIST_SQL_MAX 256
#define INTEGER_TEXT_MAX 24
#define WAIT_MS 1000

#define ARRAY_COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))

struct origin {
    PGconn *connection;
};

/* libpq는 매개변수를 문자열로 받으므로, 목록 조건의 숫자를 글자로 바꿔 둡니다. */
struct list_parameters {
    char min_price[INTEGER_TEXT_MAX];
    char max_price[INTEGER_TEXT_MAX];
    char limit[INTEGER_TEXT_MAX];
    char offset[INTEGER_TEXT_MAX];
};

static bool sql_exec(PGconn *connection, const char *sql);
static bool products_insert(PGconn *connection, int seed, int count);
static bool row_apply(PGresult *result, int row, origin_row_handle handle,
                      void *context);
static bool number_parse(const char *text, int maximum, int *value);
static void list_parameters_format(const struct list_query *list,
                                   struct list_parameters *texts);
static int product_change(struct origin *origin, const char *sql,
                          int product_id, const char *name, int price);

struct origin *origin_create(const char *spec, bool seeding)
{
    struct origin *origin = calloc(1, sizeof *origin);
    if (origin == NULL) {
        return NULL;
    }
    origin->connection = PQconnectdb(spec);
    if (PQstatus(origin->connection) != CONNECTION_OK) {
        fprintf(stderr, "cache-demo: cannot open origin\n");
        origin_destroy(origin);
        return NULL;
    }
    if (!sql_exec(origin->connection,
                  "SET statement_timeout = '" SQL_NUMBER(WAIT_MS) "ms'")) {
        origin_destroy(origin);
        return NULL;
    }
    if (!seeding && !sql_exec(origin->connection,
                              "SELECT id,name,price FROM products LIMIT 0")) {
        fprintf(stderr, "cache-demo: origin schema unavailable\n");
        origin_destroy(origin);
        return NULL;
    }

    return origin;
}

void origin_destroy(struct origin *origin)
{
    if (origin == NULL) {
        return;
    }
    PQfinish(origin->connection);
    free(origin);
}

bool origin_seed(struct origin *origin, int seed, int count)
{
    /* 상한 매크로가 문자열 사이에 끼면 clang-format이 줄을 흩뜨리므로, SQL의
       줄 모양을 지키기 위해 이 문장만 포맷에서 뺍니다. */
    /* clang-format off */
    static const char *schema =
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS products ("
        "id integer PRIMARY KEY CHECK(id BETWEEN 1 AND "
        SQL_NUMBER(PRODUCT_ID_MAX) "),"
        "name text NOT NULL CHECK(octet_length(name) BETWEEN 1 AND "
        SQL_NUMBER(NAME_MAX_BYTES) "),"
        "price integer NOT NULL CHECK(price BETWEEN 0 AND "
        SQL_NUMBER(PRICE_MAX) "));"
        "CREATE INDEX IF NOT EXISTS products_price_id ON products(price,id);"
        "DELETE FROM products;";
    /* clang-format on */
    bool success = sql_exec(origin->connection, schema) &&
                   products_insert(origin->connection, seed, count) &&
                   sql_exec(origin->connection, "COMMIT");
    if (!success) {
        sql_exec(origin->connection, "ROLLBACK");
        fprintf(stderr, "cache-demo: seed failed\n");
    }

    return success;
}

const char *origin_name(void)
{
    return "postgresql";
}

int origin_product_get(struct origin *origin, int product_id,
                       origin_row_handle handle, void *context, int *count)
{
    char id_text[INTEGER_TEXT_MAX];
    snprintf(id_text, sizeof id_text, "%d", product_id);
    const char *parameters[] = {id_text};
    PGresult *result =
        PQexecParams(origin->connection,
                     "SELECT id,name,price FROM products WHERE id=$1::integer",
                     ARRAY_COUNT(parameters), NULL, parameters, NULL, NULL, 0);
    int status = PGRES_TUPLES_OK == PQresultStatus(result)
                     ? PQntuples(result) == 0 ? 404 : 503
                     : 503;
    *count = 0;
    if (status == 503 && PQntuples(result) == 1) {
        status = row_apply(result, 0, handle, context) ? 200 : 503;
        *count = status == 200 ? 1 : 0;
    }
    PQclear(result);

    return status;
}

int origin_list_get(struct origin *origin, const struct list_query *list,
                    origin_row_handle handle, void *context)
{
    static const char *order_sql[] = {"id", "price,id", "price DESC,id"};
    char sql[LIST_SQL_MAX];
    snprintf(
        sql, sizeof sql,
        "SELECT id,name,price FROM products WHERE price BETWEEN $1::integer "
        "AND $2::integer%s ORDER BY %s LIMIT $3::integer "
        "OFFSET $4::integer",
        list->search_text_len != 0 ? " AND strpos(name,$5::text)>0" : "",
        order_sql[list->sort]);
    struct list_parameters texts;
    list_parameters_format(list, &texts);
    const char *parameters[] = {texts.min_price, texts.max_price, texts.limit,
                                texts.offset, list->search_text};
    /* 검색어가 없으면 SQL에 $5가 없으므로 마지막 매개변수를 보내지 않습니다. */
    int parameter_count = list->search_text_len != 0
                              ? ARRAY_COUNT(parameters)
                              : ARRAY_COUNT(parameters) - 1;
    PGresult *result = PQexecParams(origin->connection, sql, parameter_count,
                                    NULL, parameters, NULL, NULL, 0);
    bool success = PQresultStatus(result) == PGRES_TUPLES_OK;
    for (int row = 0; success && row < PQntuples(result); row++) {
        success = row_apply(result, row, handle, context);
    }
    PQclear(result);

    return success ? 200 : 503;
}

int origin_product_set(struct origin *origin, int product_id, const char *name,
                       size_t name_len, int price)
{
    (void)name_len;

    return product_change(origin,
                          "UPDATE products SET name=$2,price=$3 WHERE id=$1",
                          product_id, name, price);
}

int origin_product_delete(struct origin *origin, int product_id)
{
    return product_change(origin, "DELETE FROM products WHERE id=$1",
                          product_id, NULL, 0);
}

static bool sql_exec(PGconn *connection, const char *sql)
{
    PGresult *result = PQexec(connection, sql);
    bool success = PQresultStatus(result) == PGRES_COMMAND_OK ||
                   PQresultStatus(result) == PGRES_TUPLES_OK;
    if (!success) {
        fprintf(stderr, "cache-demo: origin: %s", PQerrorMessage(connection));
    }
    PQclear(result);

    return success;
}

static bool products_insert(PGconn *connection, int seed, int count)
{
    static const char *insert = "INSERT INTO products VALUES($1,$2,$3)";
    int64_t state = seed;
    bool success = true;
    for (int64_t i = 1; i <= count && success; i++) {
        char name[SEED_NAME_MAX];
        char id_text[INTEGER_TEXT_MAX];
        char price_text[INTEGER_TEXT_MAX];
        snprintf(name, sizeof name, "상품 %lld", (long long)i);
        state = state * SEED_MULTIPLIER % PRODUCT_ID_MAX;
        snprintf(id_text, sizeof id_text, "%lld", (long long)i);
        snprintf(price_text, sizeof price_text, "%lld",
                 (long long)(state % (PRICE_MAX + 1)));
        const char *parameters[] = {id_text, name, price_text};
        PGresult *result =
            PQexecParams(connection, insert, ARRAY_COUNT(parameters), NULL,
                         parameters, NULL, NULL, 0);
        success = PQresultStatus(result) == PGRES_COMMAND_OK;
        if (!success) {
            fprintf(stderr, "cache-demo: origin: %s",
                    PQerrorMessage(connection));
        }
        PQclear(result);
    }

    return success;
}

static bool row_apply(PGresult *result, int row, origin_row_handle handle,
                      void *context)
{
    int product_id = 0;
    int price = 0;
    if (!number_parse(PQgetvalue(result, row, 0), PRODUCT_ID_MAX,
                      &product_id) ||
        product_id < 1 ||
        !number_parse(PQgetvalue(result, row, 2), PRICE_MAX, &price)) {
        return false;
    }

    return handle(context, product_id, PQgetvalue(result, row, 1),
                  (size_t)PQgetlength(result, row, 1), price);
}

static bool number_parse(const char *text, int maximum, int *value)
{
    int number = 0;
    if (text == NULL || *text == '\0') {
        return false;
    }
    for (const char *letter = text; *letter != '\0'; letter++) {
        int digit = *letter - '0';
        if (digit < 0 || digit > 9 || number > (maximum - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;

    return true;
}

static void list_parameters_format(const struct list_query *list,
                                   struct list_parameters *texts)
{
    snprintf(texts->min_price, sizeof texts->min_price, "%d", list->min_price);
    snprintf(texts->max_price, sizeof texts->max_price, "%d", list->max_price);
    snprintf(texts->limit, sizeof texts->limit, "%d", list->limit);
    snprintf(texts->offset, sizeof texts->offset, "%d", list->offset);
}

static int product_change(struct origin *origin, const char *sql,
                          int product_id, const char *name, int price)
{
    if (!sql_exec(origin->connection, "BEGIN")) {
        return 503;
    }
    char id_text[INTEGER_TEXT_MAX];
    char price_text[INTEGER_TEXT_MAX];
    snprintf(id_text, sizeof id_text, "%d", product_id);
    snprintf(price_text, sizeof price_text, "%d", price);
    const char *parameters[] = {id_text, name, price_text};
    /* 삭제는 name이 NULL이고, SQL에 $1(id)만 있습니다. */
    int parameter_count = name == NULL ? 1 : ARRAY_COUNT(parameters);
    PGresult *result = PQexecParams(origin->connection, sql, parameter_count,
                                    NULL, parameters, NULL, NULL, 0);
    bool applied = PQresultStatus(result) == PGRES_COMMAND_OK;
    int changes = applied ? (int)strtol(PQcmdTuples(result), NULL, 10) : 0;
    PQclear(result);
    if (!applied || !sql_exec(origin->connection, "COMMIT")) {
        sql_exec(origin->connection, "ROLLBACK");
        return 503;
    }

    return changes > 0 ? 204 : 404;
}
