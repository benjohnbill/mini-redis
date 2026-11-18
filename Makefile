# make verify: CI와 같은 명령입니다. 빌드, 테스트, ID 검사, 코드 규칙 검사를 차례로 실행합니다.

CC       ?= cc
# MG_MAX_HTTP_HEADERS는 mongoose가 읽는 헤더 수의 상한입니다(기본 30). 넘으면 서버가
# 431로 거절합니다(api spec). 이 값은 struct mg_http_message의 크기를 바꾸므로,
# mongoose.c와 mongoose.h를 읽는 모든 파일이 같은 값으로 컴파일되도록 여기에 둡니다.
CPPFLAGS := -Isrc -Ivendor/siphash -D_POSIX_C_SOURCE=200809L -DMG_MAX_HTTP_HEADERS=64
# Darwin 확장 선언은 Mongoose와 Acutest가 사용합니다. Linux 설정은 유지합니다.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif
# ASan과 UBSan은 메모리 오류와 정의되지 않은 동작을 테스트 중에 바로 멈춰 세웁니다.
CFLAGS   := -std=c11 -Wall -Wextra -Werror -g -fno-omit-frame-pointer \
            -fsanitize=address,undefined -fno-sanitize-recover=all
LDFLAGS  := -fsanitize=address,undefined

BUILD    := build

# 서버의 main이 없는 라이브러리 소스입니다. 테스트와 서버가 함께 링크합니다.
LIB_SRCS := $(filter-out src/server.c,$(wildcard src/*.c))
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o) $(BUILD)/siphash.o

# vendor/siphash: https://github.com/veorq/SipHash (CC0, vendor/siphash/LICENSE_CC0),
# commit 32d067603b93b47828700880649198e0bfbbcffa 의 siphash.c, siphash.h 그대로입니다.
# store가 key를 hash할 때 씁니다(ADR siphash-with-random-seed). 라운드 수가 매크로라서
# 파일을 고치지 않고 Redis와 같은 SipHash 1-2로 컴파일합니다.
SIPHASH_SRC   := vendor/siphash/siphash.c
SIPHASH_FLAGS := -DcROUNDS=1 -DdROUNDS=2

# tests/acutest.h: https://github.com/mity/acutest (MIT),
# commit 31751b4089c93b46a9fd8a8183a695f772de66de 의 include/acutest.h 그대로입니다.
TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/%)

# vendor/mongoose: https://github.com/cesanta/mongoose (GPLv2, 레포 루트 LICENSE),
# tag 7.23 = commit 02bdbb9ed6f0a8f7c42228c6e7cb35d748b60551 의 mongoose.c, mongoose.h 그대로입니다.
# 서버에만 링크하고 테스트에는 링크하지 않습니다. mongoose.c는 _POSIX_C_SOURCE만으로는
# alloca와 ip_mreq 선언이 숨어서 빌드되지 않으므로, 이 파일에만 _DEFAULT_SOURCE를 줍니다.
MONGOOSE_DIR   := vendor/mongoose
MONGOOSE_OBJ   := $(BUILD)/mongoose.o
# MG_MAX_RECV_SIZE는 수신 버퍼의 상한입니다. 기본 3 MiB이고 넘으면 응답 없이 연결을
# 끊으므로, body 상한 16,777,216바이트(API_BODY_MAX)에 여유분 65,536바이트를 더한
# 16,842,752바이트로 올립니다. 헤더도 같은 버퍼에 들어가기 때문입니다. 여유분은 헤더
# 49,152바이트와 mongoose가 수신 버퍼를 늘리는 단위 MG_IO_SIZE 16,384바이트를 덮습니다.
# 헤더가 이보다 길면(아주 긴 key) 상한 크기의 body에서 연결이 끊길 수 있고, spec Q10은
# 이를 받아들입니다.
MONGOOSE_FLAGS := -D_DEFAULT_SOURCE -DMG_MAX_RECV_SIZE=16842752

.PHONY: all test functional verify lint format clean bench idle-wakeup cache-demo cache-functional cache-bench cache-demo-postgres cache-functional-postgres cache-bench-postgres

# 서버에만 주는 -D 플래그입니다. src/server.c의 POLL_TIMEOUT_MAX_MS, SWEEP_INTERVAL_MS,
# SWEEP_MAX_COUNT를 빌드할 때 바꿉니다(spec Q19). 예:
#   make -B build/mini-redis SERVER_FLAGS='-DSWEEP_INTERVAL_MS=50 -DSWEEP_MAX_COUNT=5'
# 플래그만 바꾸면 make가 다시 빌드하지 않으므로 -B를 함께 씁니다.
SERVER_FLAGS ?=

SERVER   := $(BUILD)/mini-redis

all: $(SERVER) $(TEST_BINS)

$(SERVER): src/server.c $(LIB_OBJS) $(MONGOOSE_OBJ) | $(BUILD)
	$(CC) $(CPPFLAGS) $(SERVER_FLAGS) -I$(MONGOOSE_DIR) $(CFLAGS) $< $(LIB_OBJS) $(MONGOOSE_OBJ) \
	    $(LDFLAGS) -o $@

# Makefile도 의존합니다. MONGOOSE_FLAGS를 바꾸면 이전 빌드의 mongoose.o를 다시 만듭니다.
$(MONGOOSE_OBJ): $(MONGOOSE_DIR)/mongoose.c $(MONGOOSE_DIR)/mongoose.h Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(MONGOOSE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/siphash.o: $(SIPHASH_SRC) vendor/siphash/siphash.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(SIPHASH_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%_test: tests/%_test.c tests/acutest.h $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BUILD):
	mkdir -p $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# 기능 테스트는 실제 서버를 띄우고 바깥에서 요청을 보냅니다. -B는 tests/ 아래에
# .pyc를 만들지 않게 합니다(scripts/check-ids.py가 tests/의 모든 파일을 텍스트로 읽습니다).
functional: $(SERVER)
	python3 -B tests/functional/api_test.py $(SERVER)
	python3 -B tests/functional/concurrency_test.py $(SERVER)
	python3 -B tests/functional/expire_test.py $(SERVER)

verify: $(SERVER) test functional cache-functional demo-page-test
	python3 scripts/check-ids.py
	python3 scripts/convention.py lint

# 코드 규칙(CONVENTION.md의 코드 절)입니다. lint는 위반을 출력하고 실패로 끝나며,
# format은 모양 위반을 고치고 바꾼 줄을 보여 줍니다.
lint:
	@python3 scripts/convention.py lint

format:
	@python3 scripts/convention.py format

clean:
	rm -rf $(BUILD)

# make bench: 지연과 처리량을 잽니다(spec Q3). make verify와 CI에는 넣지 않습니다.
# 시간 기준의 숫자는 머신마다 흔들리기 때문입니다. AddressSanitizer 빌드는 몇 배
# 느려서 숫자에 의미가 없으므로, sanitizer를 끄고 최적화한 서버를 build/bench/에
# 따로 만듭니다. 목적 파일도 따로 두어 make verify의 빌드와 섞이지 않게 합니다.
# assert는 남깁니다(NDEBUG 없음). 테스트한 서버와 같은 검사를 하는 서버를 재기
# 위해서입니다. 측정 순서와 출력은 scripts/bench/bench.py가 정합니다.
# 측정 서버에만 SWEEP_TRACE를 켭니다. 주기 청소가 key를 지울 때마다 시각과 개수를
# stderr에 적고, bench.py가 그 줄로 회수 지연을 잽니다(M4 spec "측정 결과").
BENCH_BUILD    := $(BUILD)/bench
BENCH_CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2
BENCH_SERVER   := $(BENCH_BUILD)/mini-redis
BENCH_LIB_OBJS := $(LIB_SRCS:src/%.c=$(BENCH_BUILD)/%.o) $(BENCH_BUILD)/siphash.o
BENCH_MONGOOSE := $(BENCH_BUILD)/mongoose.o

bench: $(BENCH_SERVER)
	python3 -B scripts/bench/bench.py $(BENCH_SERVER)

# make idle-wakeup: 요청이 없는 서버가 1초에 몇 번 깨어나는지 잽니다(M7 perf).
# /proc를 읽으므로 Linux에서만 돕니다. make bench와 같은 서버를 씁니다.
idle-wakeup: $(BENCH_SERVER)
	python3 -B scripts/bench/idle_wakeup.py $(BENCH_SERVER)

$(BENCH_SERVER): src/server.c $(BENCH_LIB_OBJS) $(BENCH_MONGOOSE) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) -DSWEEP_TRACE $(SERVER_FLAGS) -I$(MONGOOSE_DIR) $(BENCH_CFLAGS) $< \
	    $(BENCH_LIB_OBJS) $(BENCH_MONGOOSE) -o $@

$(BENCH_MONGOOSE): $(MONGOOSE_DIR)/mongoose.c $(MONGOOSE_DIR)/mongoose.h Makefile | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) $(MONGOOSE_FLAGS) $(BENCH_CFLAGS) -c $< -o $@

$(BENCH_BUILD)/siphash.o: $(SIPHASH_SRC) vendor/siphash/siphash.h | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) $(SIPHASH_FLAGS) $(BENCH_CFLAGS) -c $< -o $@

$(BENCH_BUILD)/%.o: src/%.c | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -c $< -o $@

$(BENCH_BUILD):
	mkdir -p $@

# make rehash-micro: rehash 범위(위쪽 경계, 목표, 아래쪽 경계)를 바꾼 store의 저장, 조회
# 시간과 메모리를 서버 없이 잽니다. make verify와 CI에는 넣지 않습니다. 설정마다
# src/*.c를 -D로 따로 컴파일한 바이너리를 만들고, 회마다 설정을 번갈아 실행합니다.
# 설정 순서에 따른 차이가 한쪽에 몰리지 않게 하기 위해서입니다. 설정 하나는
# <위쪽 경계>_<목표>_<아래쪽 경계>이고, REHASH_KEYS는 넣을 key 수의 목록입니다.
# 기본값은 측정 기록 rehash-micro/001의 조건입니다. 예(두 설정만):
#   make rehash-micro REHASH_SETTINGS='4_2_1 1_0.5_0.25' REHASH_KEYS=4194304
REHASH_SETTINGS ?= 8_4_2 4_2_1 2_1_0.5 1_0.5_0.25 0.5_0.25_0.125
REHASH_KEYS     ?= 100000 1048576 4194304
REHASH_ROUNDS   ?= 3
REHASH_BINS     := $(REHASH_SETTINGS:%=$(BENCH_BUILD)/rehash_micro_%)
.PHONY: rehash-micro
rehash-micro: $(REHASH_BINS)
	@for keys in $(REHASH_KEYS); do \
	    for round in $$(seq $(REHASH_ROUNDS)); do \
	        for bin in $(REHASH_BINS); do printf 'round=%s ' $$round; ./$$bin $$keys; done; \
	    done; \
	done

$(BENCH_BUILD)/rehash_micro_%: scripts/bench/rehash_micro.c $(filter-out src/server.c src/api.c,$(LIB_SRCS)) $(SIPHASH_SRC) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) $(SIPHASH_FLAGS) $(BENCH_CFLAGS) \
	    -DMAX_ELEMS_PER_BUCKET=$(word 1,$(subst _, ,$*)) \
	    -DBEST_ELEMS_PER_BUCKET=$(word 2,$(subst _, ,$*)) \
	    -DMIN_ELEMS_PER_BUCKET=$(word 3,$(subst _, ,$*)) $^ -o $@

# SQLite는 별도 상품 데모에만 링크합니다.
CACHE_DEMO := $(BUILD)/cache-demo
.PHONY: cache-demo cache-functional
cache-demo: $(CACHE_DEMO)

# 상품 API는 demo/product_demo.c, 원본 접근은 demo/sqlite_origin.c가 맡습니다.
# PostgreSQL 원본은 demo/postgres_origin.c를 링크하는 선택 빌드입니다.
$(CACHE_DEMO): demo/product_demo.c demo/cache_client.c demo/sqlite_origin.c demo/origin.h demo/cache_client.h $(MONGOOSE_OBJ) | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) $(CFLAGS) demo/product_demo.c demo/cache_client.c \
	    demo/sqlite_origin.c $(MONGOOSE_OBJ) $(LDFLAGS) -lsqlite3 -o $@

# make cache-bench: 최적화한 데모와 mini-redis로 캐시 효과를 잽니다(M5 spec).
# sanitizer를 끈 빌드를 build/bench/에 따로 만들고, 시간 수치는 make verify와 CI에
# 넣지 않습니다. 측정 계획과 출력은 scripts/bench/cache_bench.py가 정합니다.
CACHE_BENCH_DEMO := $(BENCH_BUILD)/cache-demo
# 장비에 맞춘 부하나 기록 파일은 플래그로 넘깁니다. 예:
#   make cache-bench CACHE_BENCH_FLAGS='--rate 300 --connections 16 --output x.json'
CACHE_BENCH_FLAGS ?=
cache-bench: $(CACHE_BENCH_DEMO) $(BENCH_SERVER)
	python3 -B scripts/bench/cache_bench.py $(CACHE_BENCH_DEMO) $(BENCH_SERVER) $(CACHE_BENCH_FLAGS)

# make cache-stage-bench: cache-bench와 같은 측정을 STAGE_TRACE를 켠 데모로 돌려,
# 요청마다 묶음 안의 대기, 처리, 캐시 GET, 원본 조회, 캐시 PUT, 붙잡힘, 실제
# 붙잡힘 시간을 STAGE_TRACE_DIR에 실행마다 파일 하나로 남깁니다(perf 원인 쪼개기).
# 기록하는 비용이 지연에 섞이므로 cache-bench의 숫자와 나란히 비교하지 않습니다. 예:
#   make cache-stage-bench CACHE_BENCH_FLAGS='--arrival burst --kind detail'
#   python3 -B scripts/bench/stage_summary.py build/stage-trace
CACHE_STAGE_DEMO := $(BENCH_BUILD)/cache-demo-stage
STAGE_TRACE_DIR  ?= $(BUILD)/stage-trace
.PHONY: cache-stage-bench
cache-stage-bench: $(CACHE_STAGE_DEMO) $(BENCH_SERVER)
	python3 -B scripts/bench/cache_bench.py $(CACHE_STAGE_DEMO) $(BENCH_SERVER) --stage-trace $(STAGE_TRACE_DIR) $(CACHE_BENCH_FLAGS)

$(CACHE_STAGE_DEMO): demo/product_demo.c demo/cache_client.c demo/sqlite_origin.c demo/origin.h demo/cache_client.h $(BENCH_MONGOOSE) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) -DSTAGE_TRACE -I$(MONGOOSE_DIR) $(BENCH_CFLAGS) demo/product_demo.c demo/cache_client.c \
	    demo/sqlite_origin.c $(BENCH_MONGOOSE) -lsqlite3 -o $@

# make cache-stage-bench-postgres: cache-stage-bench와 같은 구간 기록을 PostgreSQL
# 원본으로 남깁니다. 원본 조회 구간이 PostgreSQL 왕복을 담습니다. 기록 파일 이름에는
# DB 종류가 없으므로, SQLite 기록을 덮어쓰지 않게 기본 폴더를 따로 둡니다. 예:
#   make cache-stage-bench-postgres POSTGRES_URL='postgresql://127.0.0.1/products' \
#        POSTGRES_BENCH_FLAGS='--arrival burst --kind detail'
CACHE_STAGE_POSTGRES := $(BENCH_BUILD)/cache-demo-postgres-stage
STAGE_TRACE_POSTGRES_DIR ?= $(BUILD)/stage-trace-postgres
.PHONY: cache-stage-bench-postgres
cache-stage-bench-postgres: $(CACHE_STAGE_POSTGRES) $(BENCH_SERVER)
	@test -n "$(POSTGRES_URL)" || { echo "POSTGRES_URL을 지정하세요"; exit 1; }
	python3 -B scripts/bench/cache_bench.py $(CACHE_STAGE_POSTGRES) $(BENCH_SERVER) --db "$(POSTGRES_URL)" --stage-trace $(STAGE_TRACE_POSTGRES_DIR) $(POSTGRES_BENCH_FLAGS)

$(CACHE_STAGE_POSTGRES): demo/product_demo.c demo/cache_client.c demo/postgres_origin.c demo/origin.h demo/cache_client.h $(BENCH_MONGOOSE) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) -DSTAGE_TRACE -I$(MONGOOSE_DIR) -I$(PG_INCLUDEDIR) $(BENCH_CFLAGS) demo/product_demo.c \
	    demo/cache_client.c demo/postgres_origin.c $(BENCH_MONGOOSE) -L$(PG_LIBDIR) -lpq -o $@

# 선택 PostgreSQL 원본의 같은 실험입니다. SQLite 측정과 별도 환경으로 다룹니다.
CACHE_BENCH_POSTGRES := $(BENCH_BUILD)/cache-demo-postgres
cache-bench-postgres: $(CACHE_BENCH_POSTGRES) $(BENCH_SERVER)
	@test -n "$(POSTGRES_URL)" || { echo "POSTGRES_URL을 지정하세요"; exit 1; }
	python3 -B scripts/bench/cache_bench.py $(CACHE_BENCH_POSTGRES) $(BENCH_SERVER) --db "$(POSTGRES_URL)" $(POSTGRES_BENCH_FLAGS)

$(CACHE_BENCH_POSTGRES): demo/product_demo.c demo/cache_client.c demo/postgres_origin.c demo/origin.h demo/cache_client.h $(BENCH_MONGOOSE) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) -I$(PG_INCLUDEDIR) $(BENCH_CFLAGS) demo/product_demo.c \
	    demo/cache_client.c demo/postgres_origin.c $(BENCH_MONGOOSE) -L$(PG_LIBDIR) -lpq -o $@

$(CACHE_BENCH_DEMO): demo/product_demo.c demo/cache_client.c demo/sqlite_origin.c demo/origin.h demo/cache_client.h $(BENCH_MONGOOSE) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) $(BENCH_CFLAGS) demo/product_demo.c demo/cache_client.c \
	    demo/sqlite_origin.c $(BENCH_MONGOOSE) -lsqlite3 -o $@

# 데모 페이지(demo/README.md의 "데모 페이지")는 같은 검색을 cache off와 on에
# 번갈아 보내 비교합니다. 기본 원본은 PostgreSQL이고, SQLite 파일 경로를 주면
# SQLite로 돕니다. 예:
#   make demo-page-prepare   # 처음 한 번. 상품 100만 개를 채웁니다
#   make demo-page           # http://127.0.0.1:8080/
#   make demo-page-prepare demo-page DEMO_PAGE_DB=/tmp/demo-products.sqlite
DEMO_PAGE_DB    ?= postgresql:///mini_redis_demo
DEMO_PAGE_COUNT ?= 1000000
DEMO_PAGE_BACKEND := $(if $(filter postgresql://%,$(DEMO_PAGE_DB)),$(BENCH_BUILD)/cache-demo-postgres,$(CACHE_BENCH_DEMO))
.PHONY: demo-page-prepare demo-page demo-page-test
demo-page-prepare: $(DEMO_PAGE_BACKEND)
	demo/page/prepare.sh $(DEMO_PAGE_BACKEND) '$(DEMO_PAGE_DB)' $(DEMO_PAGE_COUNT)

demo-page: $(DEMO_PAGE_BACKEND) $(BENCH_SERVER)
	demo/page/run.sh $(DEMO_PAGE_BACKEND) $(BENCH_SERVER) '$(DEMO_PAGE_DB)'

demo-page-test:
	python3 -B tests/functional/demo_page_test.py

# PostgreSQL 원본은 선택 빌드입니다. libpq 개발 파일이 있어야 만들 수 있으며,
# 기본 빌드·기능 검증·CI는 이것을 요구하지 않습니다(M5 spec). 예:
#   make cache-demo-postgres
#   ./build/cache-demo-postgres --db 'postgresql://user@host/db' --seed 1 --count 10000
PG_CONFIG     ?= pg_config
PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir 2>/dev/null)
PG_LIBDIR     := $(shell $(PG_CONFIG) --libdir 2>/dev/null)
CACHE_DEMO_POSTGRES := $(BUILD)/cache-demo-postgres
.PHONY: cache-demo-postgres
cache-demo-postgres: $(CACHE_DEMO_POSTGRES)

$(CACHE_DEMO_POSTGRES): demo/product_demo.c demo/cache_client.c demo/postgres_origin.c demo/origin.h demo/cache_client.h $(MONGOOSE_OBJ) | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) -I$(PG_INCLUDEDIR) $(CFLAGS) demo/product_demo.c \
	    demo/cache_client.c demo/postgres_origin.c $(MONGOOSE_OBJ) $(LDFLAGS) -L$(PG_LIBDIR) -lpq -o $@

# PostgreSQL 기능 검증은 선택입니다. 실행 중인 DB와 libpq 빌드가 필요합니다.
#   make cache-functional-postgres POSTGRES_URL='postgresql://user@host/db'
POSTGRES_URL ?=
POSTGRES_BENCH_FLAGS ?=
cache-functional-postgres: $(CACHE_DEMO_POSTGRES) $(SERVER)
	@test -n "$(POSTGRES_URL)" || { echo "POSTGRES_URL을 지정하세요"; exit 1; }
	python3 -B tests/functional/postgres_cache_test.py $(CACHE_DEMO_POSTGRES) $(SERVER) "$(POSTGRES_URL)"

cache-functional: $(CACHE_DEMO) $(SERVER)
	python3 -B tests/functional/cache_test.py $(CACHE_DEMO)
	python3 -B tests/functional/detail_cache_test.py $(CACHE_DEMO) $(SERVER)
	python3 -B tests/functional/list_cache_test.py $(CACHE_DEMO) $(SERVER)
	python3 -B tests/functional/mutation_cache_test.py $(CACHE_DEMO) $(SERVER)
	python3 -B tests/functional/cache_recovery_test.py $(CACHE_DEMO) $(SERVER)
	python3 -B tests/functional/detail_batch_test.py $(CACHE_DEMO) $(SERVER)
