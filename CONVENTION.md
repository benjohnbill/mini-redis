# 팀 규칙

1. **브랜치:** `main`은 PR로만 변경합니다. 개인 브랜치는 이니셜로 만들고 병합 후에도 유지합니다. 임시 feature 브랜치는 개인 브랜치끼리 먼저 합칠 때만 만듭니다. fork 대신 팀 레포 브랜치를 쓰는 이유는 모두가 한 레포의 PR과 보드를 보기 때문입니다.
2. **PR 전:** `main`의 최신 내용을 개인 브랜치에 반영합니다.

   ```bash
   git fetch origin
   git merge origin/main
   ```

3. **병합:** 승인 1건을 받은 후에 merge commit으로 합니다. 승인 후에 diff가 바뀌면 다시 승인을 받습니다. 예외로, 오너가 PR에 승인 없이 병합한다고 밝히면 승인 없이 병합할 수 있습니다. 오너가 팀원에게 먼저 이야기한 뒤에 하는 병합이기 때문입니다.
4. **`main` 실패:** `main`의 CI가 실패하면 본 사람이 되돌리거나 고치는 일을 먼저 합니다.
5. **커밋:** 첫 줄에 변경 목적과 결과를 한국어 문장으로 적습니다. 매일 커밋 이력을 남깁니다.

   ```text
   저장소가 같은 key에 두 번 넣으면 값 하나만 남기도록 구현합니다
   ```

6. **설명:** 작성자는 PR을 올리기 전에 본문과 diff를 읽고, 생성 파일과 비밀 정보가 없는지 확인합니다. 병합된 코드는 AI가 작성했든 팀원이 작성했든 원리를 말로 설명할 수 있어야 합니다.
7. **기록 위치:** spec과 할 일은 `docs/milestones/<이름>/`에, 회의 안건은 회의록에, 확정된 규칙은 이 파일에 적습니다. 보드에는 마일스톤 카드만 두고 그 폴더를 링크합니다.
   - **카드:** 마일스톤마다 보드 전용 카드(draft item) 하나를 둡니다. 레포의 Issues에는 올리지 않습니다. 제목은 `mini-redis M1 store: 저장, 조회, 삭제`처럼 레포, 순서, 이름, 주제를 적습니다. 본문에는 6단계 체크리스트를 두고, 각 줄에 그 단계의 결과물 링크와 맡은 사람을 적습니다. 내용은 옮기지 않습니다. 원본이 두 곳에 생기지 않게 하면서, 보드에서 진행률과 절차가 보이게 하기 위해서입니다.
   - **Status:** `Todo`는 `overview.md`가 쓰였고 1단계 전입니다. `In Progress`는 1단계부터 6단계까지 진행 중입니다. `Done`은 6단계가 끝났습니다. 6단계 중 어디인지는 마일스톤 폴더의 문서가 말하므로 따로 적지 않습니다.
   - **`레포` 필드:** 보드는 세 팀 레포가 함께 씁니다. 라벨은 보드 전용 카드에 붙일 수 없으므로, 단일 선택 필드 `레포`로 카드를 나누고 보드 화면을 이 필드로 묶습니다.
8. **규칙 변경:** 이 파일도 PR과 승인 1건으로 변경합니다. GitHub 설정은 PR로 바꿀 수 없으므로, PR에 적어 승인받은 뒤에 오너가 반영합니다.
9. **판정:** 빌드와 테스트 결과는 PR 하단의 CI에서 확인합니다. 검증 명령은 `make verify`입니다. 경고를 오류로 보는 빌드, AddressSanitizer와 UBSan을 켠 테스트, ID 검사(`scripts/check-ids.py`), 코드 규칙 검사(`make lint`)를 실행합니다. CI도 같은 명령을 실행하므로, PR 전에 로컬에서 먼저 실행합니다.

## 코드

모양은 Pintos 팀 레포와 같은 `.clang-format`이 정합니다. Linux 스타일에 스페이스 4칸 들여쓰기, 항상 쓰는 중괄호, 80열을 더했습니다. 이름의 형식(snake_case, 상수는 대문자)과 길이는 `.clang-tidy`가 검사합니다. 아래 `이름`과 `함수` 규칙은 도구가 잡지 못하므로 리뷰에서 봅니다.

```bash
make lint     # 위반을 출력하고, 위반이 있으면 실패로 끝납니다. make verify도 실행합니다
make format   # 모양 위반을 고치고, 바꾼 줄을 보여 줍니다
```

- **설치:** `pip install clang-format==21.1.8 clang-tidy==21.1.6`. 버전이 다르면 서로의 줄을 계속 다시 포맷하게 되므로 CI와 같은 버전을 씁니다. 에디터의 clangd는 `compile_flags.txt`를 읽고 같은 경고를 보여 줍니다.
- **가져온 코드:** `.clang-format-ignore`에 적은 파일(Pintos의 `hash`와 `list`, `acutest.h`)은 포맷과 이름 검사에서 뺍니다. 원본과 대조할 수 있게 원래 모양을 지킵니다.
- **blame:** 처음 전체를 포맷한 커밋은 `.git-blame-ignore-revs`에 있습니다. `git config blame.ignoreRevsFile .git-blame-ignore-revs`를 한 번 실행하면 blame이 그 커밋을 건너뜁니다.

### 이름

1. **변수 이름은 담는 것의 역할을 단어로 적습니다.** 줄인 이름은 읽는 사람이 선언까지 거슬러 올라가 뜻을 찾게 만들기 때문입니다. 도구는 3글자 미만만 막으므로 `ent`, `tmp`처럼 3글자 이상인 줄임말은 리뷰에서 봅니다. 예외는 반복 변수 `i`, `j`, `k`, 매개변수 `n`, 파일 디스크립터 `fd`입니다.

   ```c
   /* 오답 */
   struct entry *en = malloc(sizeof *en + key_len + value_len);
   size_t n = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;

   /* 정답 */
   struct entry *entry = malloc(sizeof *entry + key_len + value_len);
   size_t shorter_len = left_entry->key_len < right_entry->key_len
                            ? left_entry->key_len
                            : right_entry->key_len;
   ```

2. **다른 파일에서 부르는 함수는 `모듈_동사`로 짓습니다.** 이름만 보고 어느 파일에 있는지 알 수 있습니다(`store_set`, `hash_find`). 파일 안의 `static` 함수는 다루는 대상을 앞에 둡니다(`entry_create`, `entry_free`).
3. **동사의 뜻은 하나로 고정합니다.**

   | 동사 | 뜻 | 예 |
   |---|---|---|
   | `create` / `destroy` | 할당해서 만듭니다 / 해제합니다. 짝으로 씁니다 | `store_create`, `store_destroy` |
   | `init` | 이미 있는 메모리를 초기화합니다. 할당하지 않습니다 | `hash_init` |
   | `set` / `get` | 값을 씁니다 / 읽습니다. `get`은 바꾸지 않습니다 | `store_set`, `store_get` |
   | `find` | 찾습니다. 없으면 `NULL`이나 `false`를 돌려줍니다 | `hash_find` |
   | `delete` | 모음에서 뺍니다 | `store_delete`, `hash_delete` |
   | `is_` / `has_` | 참과 거짓을 묻고 `bool`을 돌려줍니다 | |

4. **같은 개념은 모든 파일에서 같은 이름을 씁니다.** 새 개념이 생기면 이 표에 한 줄 더합니다.

   | 개념 | 이름 | 쓰지 않는 이름 |
   |---|---|---|
   | store 하나 | `struct store *store` | `s`, `st` |
   | entry 하나 | `struct entry *entry` | `e`, `en` |
   | key와 그 바이트 길이 | `key`, `key_len` | `k`, `klen` |
   | value와 그 바이트 길이 | `value`, `value_len` | `v`, `val`, `vlen` |
   | api 층의 응답 하나 | `struct api_response *response` | `resp`, `res` |
   | mongoose의 연결 하나 | `struct mg_connection *connection` | `c`, `conn` |
   | 메서드와 그 바이트 길이 | `method`, `method_len` | `m`, `mlen` |
   | 경로와 그 바이트 길이 | `path`, `path_len` | `p`, `plen` |
   | 파일 디스크립터 | `fd` | `file_descriptor` |
   | 지금 시각 (ms) | `now` | `time`, `current_time`, `now_ms` |
   | 만료 시각 | `expires_at` | `ts`, `t`, `expire_time` |
   | 만료 없음 | `STORE_NO_EXPIRY` | `0`, `NO_TTL` |
   | api 층이 받는 TTL 헤더 | `struct api_ttl_header *ttl_header` | `ttl`, `header` |

5. **구조체에는 `typedef`를 붙이지 않고, 숫자 상수에는 이름을 붙입니다**(`REQUEST_MAX`). `struct`가 보여야 값인지 구조체인지 바로 알 수 있기 때문입니다.

### 함수

1. **함수는 단계의 나열로 읽히게 씁니다.** 목적이 하나인 덩어리가 3줄을 넘으면 동사 이름의 `static` 함수로 뺍니다. 함수 하나가 큰 단계와 세부 작업을 섞지 않게 하기 위해서입니다. `store_set`은 `entry_create`로 만들고 `hash_replace`로 넣는 두 단계로 읽히고, `entry_create`의 필드 대입 4줄은 `entry_init`이 맡습니다.
2. **파일은 위에서 아래로 읽히게 배치합니다.** 파일 위쪽에 `static` 함수 선언을 모으고, 공개 함수(서버는 `main`)를 먼저 둡니다. helper는 공개 함수가 처음 부르는 순서대로 그 뒤에 둡니다. 읽는 사람이 큰 흐름을 먼저 보고, 필요할 때만 세부로 내려가게 하기 위해서입니다.
3. **함수의 마지막 `return` 위에 빈 줄 없이 이어진 줄이 3줄 이상이면, 그 `return` 앞에 빈 줄을 하나 둡니다.** 일하는 부분과 결과를 돌려주는 부분을 눈으로 나누기 위해서입니다. 중간에서 빠져나가는 `return`과 본문이 짧은 함수에는 두지 않습니다. clang-format은 이 빈 줄을 지우지 않지만 강제하지도 않으므로 리뷰에서 봅니다.

   ```c
   static struct entry *entry_create(const void *key, size_t key_len,
                                     const void *value, size_t value_len)
   {
       ...
       if (entry == NULL) {
           return NULL;          /* 중간에서 빠져나감: 빈 줄 없음 */
       }
       memcpy(entry->bytes, bytes_or_empty(key, key_len), key_len);
       memcpy(entry->bytes + key_len, bytes_or_empty(value, value_len), value_len);
       entry_init(entry, key_len, value_len);

       return entry;             /* 위에 3줄 이상: 빈 줄 하나 */
   }
   ```

## 마일스톤

마일스톤 하나는 아래 6단계를 한 바퀴 돕니다. 마일스톤은 시간이 아니라 개념이나 필요 하나로 나누고, 순서는 `README.md`에만 적습니다.

| 단계 | 주체 | 할 일 |
| ---- | ---- | ----- |
| 1 | 사람, 각자 | `overview.md`의 문제를 공부하고 만나서 공유합니다 |
| 2 | 사람, 함께 | `/grill-with-docs`로 AI와 질의응답합니다. AI는 문제를 먼저 내고, Redis의 실제 방식은 이 단계 끝에 공개합니다 |
| 3 | AI, 함께 확인 | `/to-spec`으로 `spec.md`를 쓰고, ADR을 씁니다 |
| 4 | AI, 드라이버 1명 | `/to-tickets`(작으면 생략)와 테스트를 만듭니다. 시나리오 목록은 팀이 함께 확인합니다 |
| 5 | AI, 드라이버 1명 | `/implement-spec`으로 코드를 만듭니다 |
| 6 | 사람, 함께 | `overview.md`, `spec.md`, ADR을 읽고 Redis와 대조해 배운 것을 요약합니다. 다음 마일스톤의 `overview.md`를 씁니다 |

- **`overview.md`:** 다룰 요구사항 ID(`docs/requirements.md`), 탐구할 문제, 앞 마일스톤과의 의존, 끝나면 생기는 문서의 링크를 적습니다. Redis의 실제 방식은 적지 않습니다. 미리 알면 2단계에서 우리 답을 낼 이유가 없어지기 때문입니다.
- **`spec.md`의 `## Redis와 대조`:** 우리 생각, Redis의 방식, 우리가 고른 것을 이 순서로 적습니다. `README.md`가 이 절들을 링크합니다.
- **기록:** 끝난 마일스톤 폴더는 고치지 않습니다. 결정이 바뀌면 현재 마일스톤의 `spec.md`에 밝힙니다. 기능의 현재 모습은 `src/`, `tests/`, `README.md`, `docs/api.md`, `demo/README.md`가 말합니다.
- **문서 이름:** 레포의 모든 문서에서 Redis의 답을 아직 다루지 않은 마일스톤의 이름이나 예시로 쓰지 않습니다. grill의 AI도 이 문서들을 읽습니다.

## 문서 역할

| 내용 | 남는 곳 |
| ---- | ------- |
| 발표 순서의 요약과 각 문서로 가는 링크 | `README.md` |
| mini-redis의 HTTP 계약 (요청, 응답, 오류 이유, 상한) | `docs/api.md` |
| 상품 데모의 실행 방법과 HTTP 계약 | `demo/README.md` |
| 측정 결과 | 그 마일스톤 `spec.md`의 `## 측정 결과` |
| 요구사항과 우리 해석 | `docs/requirements.md`의 `해석`, `근거` 칸 |
| 큰 이유 (구조, 의존성, 인터페이스) | `docs/adr/` |
| 작은 이유 | 그 코드 줄의 주석 |
| 엣지 케이스 | 테스트 이름 |
| 일부러 하지 않는 것 | 표의 `남은 일`, LMS 문장이 없으면 `README.md`의 "알려진 한계"에 한 문장과 링크, 세부는 링크한 문서 |
| 용어 | `GLOSSARY.md` |

## ADR

- `docs/adr/<결정을 줄인 영어 이름>.md`에 평평하게 둡니다. 파일 이름에 번호를 붙이지 않습니다. 브랜치마다 쓴 번호가 병합 때 겹치기 때문입니다.
- frontmatter에 `status`와 `milestone`(마일스톤 이름)을 적습니다.
- 본문은 맥락, 결정, 이유를 1~3문장으로 씁니다. 버린 대안과 결과는 쓸 가치가 있을 때만 씁니다.
- 아래 두 기준을 모두 통과해야 ADR입니다. 통과하지 못한 결정은 `spec.md`나 코드 주석에 둡니다.
  - 되돌리기 어렵고, 맥락 없이 코드를 보는 사람이 "왜?"라고 묻고, 실제 대안이 있었습니다.
  - 구조, 비기능 요구, 의존성, 인터페이스, 만드는 방법에 닿습니다.
- Redis와 같은 답이어도 위 기준을 통과하면 ADR입니다.
- 이유 칸에는 2단계에서 팀원이 말한 문장만 씁니다. AI가 추론한 이유는 쓰지 않습니다.
- 결정을 바꿀 때는 새 ADR을 쓰고, 옛 ADR의 `status`를 `superseded by <새 ADR 파일 이름>`으로 바꿉니다.

## 테스트

- 테스트 파일 하나는 마일스톤 하나입니다: `tests/<이름>_test.c`. 외부에서 쓰는 기능 테스트는 `tests/functional/`에 둡니다.
- 테스트 이름은 동작을 말하는 한국어 문장입니다. 엣지 케이스 목록은 따로 두지 않고 테스트 이름이 맡습니다.
- 테스트 도구는 `tests/acutest.h`입니다. `TEST_LIST`에 `{"한국어 문장", 함수}`로 적으면 그 문장이 테스트 이름으로 출력됩니다. C 함수 이름에는 한국어 문장을 쓸 수 없기 때문에 이 도구를 골랐습니다.
- ID(`T1` 등)는 ADR의 `결정:` 줄이나 `docs/requirements.md`의 `근거` 칸이 인용하는 테스트에만 붙입니다. ID는 테스트 이름 문자열 맨 앞에 `"[T1] 같은 key에 ..."`처럼 적고, 레포 전체에서 하나만 씁니다. `scripts/check-ids.py`가 인용한 ID와 ADR 이름이 실제로 있는지 확인합니다.
- 테스트는 공개 함수를 호출해 동작을 확인합니다. 기대값은 검사 대상 함수로 계산하지 않고 직접 적습니다.
