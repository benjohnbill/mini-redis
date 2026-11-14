#ifndef STORE_H
#define STORE_H

/* store: key와 value를 바이트 묶음(주소와 길이 한 쌍)으로 보관합니다.
   key와 value는 '\0'으로 끝나지 않아도 되고, 중간에 '\0'이 있어도 됩니다.
   길이가 0이면 주소로 NULL을 넘겨도 됩니다. 길이가 0이 아닌데 주소가 NULL이거나,
   store나 출력 인자가 NULL이면 assert로 멈춥니다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 시각은 모두 mg_millis와 같은 단조 시계의 ms입니다. store는 시계를 직접 읽지
   않고, 부르는 쪽이 "지금"과 만료 시각을 넘깁니다(spec "store와 시각").
   STORE_NO_EXPIRY는 "만료 없음"을 나타내는 만료 시각입니다. 어떤 "지금"도 이
   값에 닿지 않아야 하므로, "지금"으로 이 값을 넘기면 assert로 멈춥니다. */
#define STORE_NO_EXPIRY UINT64_MAX

struct store;

/* 빈 store를 만듭니다. 메모리가 부족하면 NULL을 돌려줍니다. */
struct store *store_create(void);

/* store와 그 안의 모든 entry를 해제합니다. */
void store_destroy(struct store *store);

/* key와 value의 바이트를 store의 메모리로 복사해 저장합니다.
   돌아온 뒤에 호출한 쪽 버퍼를 바꿔도 저장된 값은 그대로입니다.
   expires_at은 이 key의 만료 시각이고, 만료되지 않는 key는
   STORE_NO_EXPIRY입니다.
   false는 메모리 부족일 때만 돌려주며, 그때 store는 바뀌지 않습니다. */
bool store_set(struct store *store, const void *key, size_t key_len,
               const void *value, size_t value_len, uint64_t expires_at);

/* now 시각에 key를 찾으면 true를 돌려주고, *value와 *value_len에 저장된
   value를 빌려줍니다. 없거나 만료됐으면(now >= 만료 시각) false를 돌려주고
   *value와 *value_len을 바꾸지 않습니다. 만료된 entry를 지우지는 않습니다.
   빌려준 *value는 읽기 전용이며, 같은 store에 다음 store_set 또는
   store_delete를 부르기 전까지만 유효합니다. 그 뒤에 쓰면 해제된 메모리를
   가리킬 수 있습니다. */
bool store_get(struct store *store, uint64_t now, const void *key,
               size_t key_len, const void **value, size_t *value_len);

/* now 시각에 key가 있으면 그 entry를 빼서 해제하고 true를 돌려줍니다.
   없으면 false를 돌려주며, 이것은 오류가 아니라 "지운 것이 없음"입니다.
   만료된 entry는 빼서 해제하되, 없는 key와 같게 false를 돌려줍니다. */
bool store_delete(struct store *store, uint64_t now, const void *key,
                  size_t key_len);

#endif /* STORE_H */
