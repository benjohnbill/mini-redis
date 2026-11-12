# make verify: CI와 같은 명령입니다. 빌드, 테스트, ID 검사를 차례로 실행합니다.

CC       ?= cc
CPPFLAGS := -Isrc -D_POSIX_C_SOURCE=200809L
# ASan과 UBSan은 메모리 오류와 정의되지 않은 동작을 테스트 중에 바로 멈춰 세웁니다.
CFLAGS   := -std=c11 -Wall -Wextra -Werror -g -fno-omit-frame-pointer \
            -fsanitize=address,undefined -fno-sanitize-recover=all
LDFLAGS  := -fsanitize=address,undefined

BUILD    := build

# 서버의 main이 없는 라이브러리 소스입니다. 테스트와 서버가 함께 링크합니다.
LIB_SRCS := src/hash.c src/list.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

# tests/acutest.h: https://github.com/mity/acutest (MIT),
# commit 31751b4089c93b46a9fd8a8183a695f772de66de 의 include/acutest.h 그대로입니다.
TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/%)

.PHONY: all test verify clean

SERVER   := $(BUILD)/mini-redis

all: $(SERVER) $(TEST_BINS)

$(SERVER): src/server.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) $(LDFLAGS) -o $@

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

clean:
	rm -rf $(BUILD)
