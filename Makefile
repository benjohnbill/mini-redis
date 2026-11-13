# make verify: CI와 같은 명령입니다. 빌드, 테스트, ID 검사, 코드 규칙 검사를 차례로 실행합니다.

CC       ?= cc
CPPFLAGS := -Isrc -D_POSIX_C_SOURCE=200809L
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
MONGOOSE_FLAGS := -D_DEFAULT_SOURCE

.PHONY: all test verify lint format clean

SERVER   := $(BUILD)/mini-redis

all: $(SERVER) $(TEST_BINS)

$(SERVER): src/server.c $(LIB_OBJS) $(MONGOOSE_OBJ) | $(BUILD)
	$(CC) $(CPPFLAGS) -I$(MONGOOSE_DIR) $(CFLAGS) $< $(LIB_OBJS) $(MONGOOSE_OBJ) \
	    $(LDFLAGS) -o $@

$(MONGOOSE_OBJ): $(MONGOOSE_DIR)/mongoose.c $(MONGOOSE_DIR)/mongoose.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(MONGOOSE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%_test: tests/%_test.c tests/acutest.h $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BUILD):
	mkdir -p $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

verify: $(SERVER) test
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
