# make verify: CI와 같은 명령입니다. 빌드, 테스트, ID 검사, 코드 규칙 검사를 차례로 실행합니다.

CC       ?= cc
CPPFLAGS := -Isrc -D_POSIX_C_SOURCE=200809L
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
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

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

.PHONY: all test functional verify lint format clean bench

# 서버에만 주는 -D 플래그입니다. src/server.c의 POLL_INTERVAL_MS, SWEEP_INTERVAL_MS,
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

verify: $(SERVER) test functional cache-functional
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
BENCH_LIB_OBJS := $(LIB_SRCS:src/%.c=$(BENCH_BUILD)/%.o)
BENCH_MONGOOSE := $(BENCH_BUILD)/mongoose.o

bench: $(BENCH_SERVER)
	python3 -B scripts/bench/bench.py $(BENCH_SERVER)

$(BENCH_SERVER): src/server.c $(BENCH_LIB_OBJS) $(BENCH_MONGOOSE) | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) -DSWEEP_TRACE $(SERVER_FLAGS) -I$(MONGOOSE_DIR) $(BENCH_CFLAGS) $< \
	    $(BENCH_LIB_OBJS) $(BENCH_MONGOOSE) -o $@

$(BENCH_MONGOOSE): $(MONGOOSE_DIR)/mongoose.c $(MONGOOSE_DIR)/mongoose.h Makefile | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) $(MONGOOSE_FLAGS) $(BENCH_CFLAGS) -c $< -o $@

$(BENCH_BUILD)/%.o: src/%.c | $(BENCH_BUILD)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -c $< -o $@

$(BENCH_BUILD):
	mkdir -p $@

# SQLite는 별도 상품 데모에만 링크합니다.
CACHE_DEMO := $(BUILD)/cache-demo
.PHONY: cache-demo cache-functional
cache-demo: $(CACHE_DEMO)

$(CACHE_DEMO): demo/product_demo.c $(MONGOOSE_OBJ) | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) $(CFLAGS) $< $(MONGOOSE_OBJ) \
	    $(LDFLAGS) -lsqlite3 -o $@

cache-functional: $(CACHE_DEMO)
	python3 -B tests/functional/cache_test.py $(CACHE_DEMO)
