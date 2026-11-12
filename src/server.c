/* 요청을 하나씩 받는 HTTP 서버입니다.
   한 요청을 끝까지 처리한 뒤에야 다음 연결을 accept합니다. 그래서 두 번째
   클라이언트는 첫 요청이 끝날 때까지 기다립니다. 이 구조는 일부러 둔 것이고,
   M3 concurrency가 바꿀 출발점입니다. 응답의 형태는 M2 api가 정합니다. */

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define REQUEST_MAX 8192

/* 헤더 끝(빈 줄)까지 읽습니다. 본문은 아직 다루지 않습니다. */
static int read_request(int fd, char *buf, size_t cap) {
    size_t len = 0;
    while (len < cap - 1) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) return 0;
    }
    return -1;
}

static void write_all(int fd, const char *s, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, s, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        s += n;
        len -= (size_t)n;
    }
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;

    /* 클라이언트가 먼저 연결을 끊으면 write가 SIGPIPE를 받아 서버가 죽기 때문에 무시합니다. */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    /* 서버를 다시 켤 때 TIME_WAIT에 남은 포트를 바로 쓰기 위해서입니다. */
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 16) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "mini-redis: listening on port %d\n", port);

    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n"
        "ok\n";

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        char buf[REQUEST_MAX];
        if (read_request(client_fd, buf, sizeof buf) == 0)
            write_all(client_fd, response, sizeof response - 1);
        close(client_fd);
    }
}
