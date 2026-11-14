#include "origin.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRODUCT_ID_MAX 2147483647
#define SEED_MULTIPLIER 48271

struct origin {
    PGconn *connection;
};

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

static bool exec_ok(PGconn *connection, const char *sql)
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

struct origin *origin_open(const char *spec, bool create)
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
    if (!exec_ok(origin->connection, "SET statement_timeout = '1000ms'")) {
        origin_destroy(origin);
        return NULL;
    }
    if (!create && !exec_ok(origin->connection,
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
    static const char *schema =
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS products ("
        "id integer PRIMARY KEY CHECK(id BETWEEN 1 AND 2147483647),"
        "name text NOT NULL CHECK(octet_length(name) BETWEEN 1 AND 120),"
        "price integer NOT NULL CHECK(price BETWEEN 0 AND 1000000000));"
        "CREATE INDEX IF NOT EXISTS products_price_id ON products(price,id);"
        "DELETE FROM products;";
    static const char *insert = "INSERT INTO products VALUES($1,$2,$3)";
    bool success = exec_ok(origin->connection, schema);
    int64_t state = seed;
    for (int64_t i = 1; i <= count && success; i++) {
        char name[64];
        char id_text[24];
        char price_text[24];
        snprintf(name, sizeof name, "상품 %lld", (long long)i);
        state = state * SEED_MULTIPLIER % PRODUCT_ID_MAX;
        snprintf(id_text, sizeof id_text, "%lld", (long long)i);
        snprintf(price_text, sizeof price_text, "%lld",
                 (long long)(state % (PRICE_MAX + 1)));
        const char *parameters[] = {id_text, name, price_text};
        PGresult *result = PQexecParams(origin->connection, insert, 3, NULL,
                                        parameters, NULL, NULL, 0);
        success = PQresultStatus(result) == PGRES_COMMAND_OK;
        if (!success) {
            fprintf(stderr, "cache-demo: origin: %s",
                    PQerrorMessage(origin->connection));
        }
        PQclear(result);
    }
    success = success && exec_ok(origin->connection, "COMMIT");
    if (!success) {
        exec_ok(origin->connection, "ROLLBACK");
        fprintf(stderr, "cache-demo: seed failed\n");
    }

    return success;
}

const char *origin_name(void)
{
    return "postgresql";
}

int origin_product_read(struct origin *origin, int product_id,
                        origin_row_handle handle, void *context, int *count)
{
    char id_text[24];
    snprintf(id_text, sizeof id_text, "%d", product_id);
    const char *parameters[] = {id_text};
    PGresult *result =
        PQexecParams(origin->connection,
                     "SELECT id,name,price FROM products WHERE id=$1::integer",
                     1, NULL, parameters, NULL, NULL, 0);
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

int origin_list_read(struct origin *origin, const struct list_query *list,
                     origin_row_handle handle, void *context)
{
    static const char *order_sql[] = {"id", "price,id", "price DESC,id"};
    char sql[256];
    char low_text[24];
    char high_text[24];
    char limit_text[24];
    char offset_text[24];
    snprintf(
        sql, sizeof sql,
        "SELECT id,name,price FROM products WHERE price BETWEEN $1::integer "
        "AND $2::integer%s ORDER BY %s LIMIT $3::integer "
        "OFFSET $4::integer",
        list->q_len != 0 ? " AND strpos(name,$5::text)>0" : "",
        order_sql[list->sort]);
    snprintf(low_text, sizeof low_text, "%d", list->min_price);
    snprintf(high_text, sizeof high_text, "%d", list->max_price);
    snprintf(limit_text, sizeof limit_text, "%d", list->limit);
    snprintf(offset_text, sizeof offset_text, "%d", list->offset);
    const char *parameters[] = {low_text, high_text, limit_text, offset_text,
                                list->q};
    int parameter_count = list->q_len != 0 ? 5 : 4;
    PGresult *result = PQexecParams(origin->connection, sql, parameter_count,
                                    NULL, parameters, NULL, NULL, 0);
    bool success = PQresultStatus(result) == PGRES_TUPLES_OK;
    for (int row = 0; success && row < PQntuples(result); row++) {
        success = row_apply(result, row, handle, context);
    }
    PQclear(result);

    return success ? 200 : 503;
}

static int product_change(struct origin *origin, const char *sql,
                          int product_id, const char *name, int price)
{
    if (!exec_ok(origin->connection, "BEGIN")) {
        return 503;
    }
    char id_text[24];
    char price_text[24];
    snprintf(id_text, sizeof id_text, "%d", product_id);
    snprintf(price_text, sizeof price_text, "%d", price);
    const char *parameters[] = {id_text, name, price_text};
    int parameter_count = name == NULL ? 1 : 3;
    PGresult *result = PQexecParams(origin->connection, sql, parameter_count,
                                    NULL, parameters, NULL, NULL, 0);
    bool applied = PQresultStatus(result) == PGRES_COMMAND_OK;
    int changes = applied ? (int)strtol(PQcmdTuples(result), NULL, 10) : 0;
    PQclear(result);
    if (!applied || !exec_ok(origin->connection, "COMMIT")) {
        exec_ok(origin->connection, "ROLLBACK");
        return 503;
    }

    return changes > 0 ? 204 : 404;
}

int origin_product_write(struct origin *origin, int product_id,
                         const char *name, size_t name_len, int price)
{
    (void)name_len;

    return product_change(origin,
                          "UPDATE products SET name=$2,price=$3 WHERE id=$1",
                          product_id, name, price);
}

int origin_product_remove(struct origin *origin, int product_id)
{
    return product_change(origin, "DELETE FROM products WHERE id=$1",
                          product_id, NULL, 0);
}
