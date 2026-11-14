#include "origin.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define SEED_MULTIPLIER 48271
#define SEED_NAME_MAX 64
#define LIST_SQL_MAX 256
#define WAIT_MS 1000

struct origin {
    sqlite3 *database;
};

static bool products_insert(sqlite3 *database, int seed, int count);
static bool row_is_typed(sqlite3_stmt *statement);
static int list_parameters_bind(sqlite3_stmt *statement,
                                const struct list_query *list);
static int product_change(struct origin *origin, const char *sql,
                          int product_id, const char *name, size_t name_len,
                          int price);

struct origin *origin_create(const char *spec, bool seeding)
{
    struct origin *origin = calloc(1, sizeof *origin);
    if (origin == NULL) {
        return NULL;
    }
    int flags = SQLITE_OPEN_READWRITE | (seeding ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(spec, &origin->database, flags, NULL) != SQLITE_OK) {
        fprintf(stderr, "cache-demo: cannot open origin\n");
        origin_destroy(origin);
        return NULL;
    }
    sqlite3_busy_timeout(origin->database, WAIT_MS);
    if (!seeding && sqlite3_exec(origin->database,
                                 "SELECT id,name,price FROM products LIMIT 0",
                                 NULL, NULL, NULL) != SQLITE_OK) {
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
    sqlite3_close(origin->database);
    free(origin);
}

bool origin_seed(struct origin *origin, int seed, int count)
{
    /* 상한 매크로가 문자열 사이에 끼면 clang-format이 줄을 흩뜨리므로, SQL의
       줄 모양을 지키기 위해 이 문장만 포맷에서 뺍니다. */
    /* clang-format off */
    const char *schema =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS products ("
        "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND "
        SQL_NUMBER(PRODUCT_ID_MAX) "),"
        "name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND "
        SQL_NUMBER(NAME_MAX_BYTES) " AND instr(name,char(0))=0),"
        "price INTEGER NOT NULL CHECK(typeof(price)='integer' AND "
        "price BETWEEN 0 AND " SQL_NUMBER(PRICE_MAX) "));"
        "CREATE INDEX IF NOT EXISTS products_price_id ON products(price,id);"
        "DELETE FROM products;";
    /* clang-format on */
    bool success =
        sqlite3_exec(origin->database, schema, NULL, NULL, NULL) == SQLITE_OK &&
        products_insert(origin->database, seed, count) &&
        sqlite3_exec(origin->database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!success) {
        sqlite3_exec(origin->database, "ROLLBACK", NULL, NULL, NULL);
        fprintf(stderr, "cache-demo: seed failed\n");
    }

    return success;
}

const char *origin_name(void)
{
    return "sqlite";
}

int origin_product_get(struct origin *origin, int product_id,
                       origin_row_handle handle, void *context, int *count)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        origin->database, "SELECT id,name,price FROM products WHERE id=?", -1,
        &statement, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 1, product_id);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }
    int status = 503;
    *count = 0;
    if (result == SQLITE_ROW) {
        status = row_is_typed(statement) &&
                         handle(context, sqlite3_column_int(statement, 0),
                                (const char *)sqlite3_column_text(statement, 1),
                                (size_t)sqlite3_column_bytes(statement, 1),
                                sqlite3_column_int(statement, 2))
                     ? 200
                     : 503;
        *count = status == 200 ? 1 : 0;
    } else if (result == SQLITE_DONE) {
        status = 404;
    }
    sqlite3_finalize(statement);

    return status;
}

int origin_list_get(struct origin *origin, const struct list_query *list,
                    origin_row_handle handle, void *context)
{
    static const char *order_sql[] = {"id", "price,id", "price DESC,id"};
    char sql[LIST_SQL_MAX];
    snprintf(sql, sizeof sql,
             "SELECT id,name,price FROM products WHERE price BETWEEN ?1 AND "
             "?2%s ORDER BY %s LIMIT ?3 OFFSET ?4",
             list->search_text_len != 0 ? " AND instr(name,?5)>0" : "",
             order_sql[list->sort]);
    sqlite3_stmt *statement = NULL;
    int result =
        sqlite3_prepare_v2(origin->database, sql, -1, &statement, NULL);
    if (result == SQLITE_OK) {
        result = list_parameters_bind(statement, list);
    }
    bool success = result == SQLITE_OK;
    int step = SQLITE_DONE;
    while (success && (step = sqlite3_step(statement)) == SQLITE_ROW) {
        success = row_is_typed(statement) &&
                  handle(context, sqlite3_column_int(statement, 0),
                         (const char *)sqlite3_column_text(statement, 1),
                         (size_t)sqlite3_column_bytes(statement, 1),
                         sqlite3_column_int(statement, 2));
    }
    success = success && step == SQLITE_DONE;
    sqlite3_finalize(statement);

    return success ? 200 : 503;
}

int origin_product_set(struct origin *origin, int product_id, const char *name,
                       size_t name_len, int price)
{
    return product_change(origin,
                          "UPDATE products SET name=?2,price=?3 WHERE id=?1",
                          product_id, name, name_len, price);
}

int origin_product_delete(struct origin *origin, int product_id)
{
    return product_change(origin, "DELETE FROM products WHERE id=?1",
                          product_id, NULL, 0, 0);
}

static bool products_insert(sqlite3 *database, int seed, int count)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "INSERT INTO products VALUES(?,?,?)", -1,
                           &statement, NULL) != SQLITE_OK) {
        return false;
    }
    int64_t state = seed;
    bool success = true;
    for (int64_t i = 1; i <= count && success; i++) {
        char name[SEED_NAME_MAX];
        snprintf(name, sizeof name, "상품 %lld", (long long)i);
        state = state * SEED_MULTIPLIER % PRODUCT_ID_MAX;
        success = sqlite3_bind_int64(statement, 1, i) == SQLITE_OK &&
                  sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT) ==
                      SQLITE_OK &&
                  sqlite3_bind_int64(statement, 3, state % (PRICE_MAX + 1)) ==
                      SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);

    return success;
}

static bool row_is_typed(sqlite3_stmt *statement)
{
    sqlite3_int64 product_id = sqlite3_column_int64(statement, 0);
    sqlite3_int64 price = sqlite3_column_int64(statement, 2);

    return sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
           product_id >= 1 && product_id <= PRODUCT_ID_MAX &&
           sqlite3_column_type(statement, 1) == SQLITE_TEXT &&
           sqlite3_column_type(statement, 2) == SQLITE_INTEGER && price >= 0 &&
           price <= PRICE_MAX;
}

/* 목록 SQL의 ?1부터 ?5까지 값을 넣습니다. 검색어가 없으면 ?5는 SQL에 없습니다. */
static int list_parameters_bind(sqlite3_stmt *statement,
                                const struct list_query *list)
{
    int result = sqlite3_bind_int(statement, 1, list->min_price);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 2, list->max_price);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 3, list->limit);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 4, list->offset);
    }
    if (result == SQLITE_OK && list->search_text_len != 0) {
        result =
            sqlite3_bind_text(statement, 5, list->search_text,
                              (int)list->search_text_len, SQLITE_TRANSIENT);
    }

    return result;
}

static int product_change(struct origin *origin, const char *sql,
                          int product_id, const char *name, size_t name_len,
                          int price)
{
    int result =
        sqlite3_exec(origin->database, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_stmt *statement = NULL;
    if (result == SQLITE_OK) {
        result =
            sqlite3_prepare_v2(origin->database, sql, -1, &statement, NULL);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 1, product_id);
    }
    if (result == SQLITE_OK && name != NULL) {
        result = sqlite3_bind_text(statement, 2, name, (int)name_len,
                                   SQLITE_TRANSIENT);
    }
    if (result == SQLITE_OK && name != NULL) {
        result = sqlite3_bind_int(statement, 3, price);
    }
    bool applied =
        result == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE;
    bool committed = applied && sqlite3_exec(origin->database, "COMMIT", NULL,
                                             NULL, NULL) == SQLITE_OK;
    int changes = applied ? sqlite3_changes(origin->database) : 0;
    if (!applied) {
        sqlite3_exec(origin->database, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_finalize(statement);

    return !committed ? 503 : changes > 0 ? 204 : 404;
}
