"""cache-demo --seed로 채운 products의 name을 names.product_name으로 바꿉니다.

id와 가격은 데모 백엔드의 준비 명령이 정한 그대로 둡니다. --db가
postgresql://로 시작하면 psql의 COPY로, 아니면 SQLite 파일로 다룹니다.
이름을 바꾼 뒤 VACUUM으로 옛 행을 치워, 검색이 읽는 페이지 수를 준비 직후와
같게 둡니다.

make demo-page-prepare가 부릅니다.
"""

import argparse
import sqlite3
import subprocess

from names import product_name


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", required=True)
    database = parser.parse_args().db
    if database.startswith("postgresql://"):
        count = fill_postgres(database)
    else:
        count = fill_sqlite(database)
    print(f"renamed {count} products in {database}")


def fill_sqlite(path):
    connection = sqlite3.connect(path)
    with connection:
        ids = [row[0] for row in connection.execute("SELECT id FROM products")]
        connection.executemany(
            "UPDATE products SET name = ? WHERE id = ?",
            ((product_name(product_id), product_id) for product_id in ids))
    connection.execute("VACUUM")
    connection.close()

    return len(ids)


def fill_postgres(url):
    ids = psql(url, "SELECT id FROM products ORDER BY id").split()
    rows = "".join(f"{product_id}\t{product_name(int(product_id))}\n"
                   for product_id in ids)
    script = (
        "BEGIN;\n"
        "CREATE TEMP TABLE new_names (id bigint PRIMARY KEY, name text)"
        " ON COMMIT DROP;\n"
        "COPY new_names FROM STDIN;\n" + rows + "\\.\n"
        "UPDATE products SET name = new_names.name FROM new_names"
        " WHERE products.id = new_names.id;\n"
        "COMMIT;\n"
        "VACUUM FULL ANALYZE products;\n")
    subprocess.run(["psql", url, "-q", "-v", "ON_ERROR_STOP=1"],
                   input=script, text=True, check=True)

    return len(ids)


def psql(url, query):
    result = subprocess.run(
        ["psql", url, "-At", "-v", "ON_ERROR_STOP=1", "-c", query],
        capture_output=True, text=True, check=True)

    return result.stdout


if __name__ == "__main__":
    main()
