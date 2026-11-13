/* mini-redis 서버입니다. 연결과 요청 해석은 mongoose가 맡고, 이 파일은
   mongoose의 요청 이벤트에서 메서드, 경로, body를 꺼내 api 층에 넘기고
   돌려받은 응답을 보내는 일만 합니다.
   스레드 하나가 mg_mgr_poll을 반복해서 부르며 요청을 하나씩 처리합니다.
   그래서 api 층이 빌려준 value를 다음 store 호출 전에 송신 버퍼로 복사할 수
   있습니다. 이 근거는 M3 concurrency에서 다시 봅니다.

   사용법: mini-redis <port>
   SIGTERM이나 SIGINT를 받으면 루프를 빠져나와 mongoose와 store를 해제하고
   0으로 끝납니다. 그래야 AddressSanitizer가 누수를 끝에서 보고합니다. */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "api.h"
#include "mongoose.h"
#include "store.h"

#define PORT_MAX 65535
#define LISTEN_URL_MAX 64
#define POLL_INTERVAL_MS 100

static volatile sig_atomic_t stop_requested = 0;

static int port_parse(const char *text);
static void stop_signals_install(void);
static void stop_request(int signum);
static int server_run(struct store *store, int port);
static bool listener_open(struct mg_mgr *manager, struct store *store,
                          int port);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void response_send(struct mg_connection *connection,
                          const struct api_response *response);
static const char *status_reason(int status);

int main(int argc, char **argv)
{
    int port = argc == 2 ? port_parse(argv[1]) : -1;
    if (port < 0) {
        fprintf(stderr, "usage: %s <port 1-%d>\n", argv[0], PORT_MAX);
        return 2;
    }
    struct store *store = store_create();
    if (store == NULL) {
        fprintf(stderr, "mini-redis: store를 만들 메모리가 없습니다\n");
        return 1;
    }
    stop_signals_install();
    int exit_code = server_run(store, port);
    store_destroy(store);

    return exit_code;
}

/* 1부터 PORT_MAX까지의 10진수만 받고, 아니면 -1을 돌려줍니다. */
static int port_parse(const char *text)
{
    char *end = NULL;
    long port = strtol(text, &end, 10);
    if (end == text || *end != '\0' || port < 1 || port > PORT_MAX) {
        return -1;
    }

    return (int)port;
}

static void stop_signals_install(void)
{
    struct sigaction action = {0};
    action.sa_handler = stop_request;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

/* 신호 처리기에서는 플래그만 세웁니다. 루프는 늦어도 POLL_INTERVAL_MS 뒤에 봅니다. */
static void stop_request(int signum)
{
    (void)signum;
    stop_requested = 1;
}

/* 신호를 받을 때까지 요청을 처리합니다. 연결을 받을 수 없으면 1을 돌려줍니다. */
static int server_run(struct store *store, int port)
{
    struct mg_mgr manager;
    mg_mgr_init(&manager);
    int exit_code = listener_open(&manager, store, port) ? 0 : 1;
    while (exit_code == 0 && !stop_requested) {
        mg_mgr_poll(&manager, POLL_INTERVAL_MS);
    }
    mg_mgr_free(&manager);

    return exit_code;
}

static bool listener_open(struct mg_mgr *manager, struct store *store, int port)
{
    char listen_url[LISTEN_URL_MAX];
    snprintf(listen_url, sizeof listen_url, "http://0.0.0.0:%d", port);
    struct mg_connection *listener =
        mg_http_listen(manager, listen_url, http_event_handle, store);
    if (listener == NULL) {
        fprintf(stderr, "mini-redis: 포트 %d에서 연결을 받을 수 없습니다\n",
                port);
        return false;
    }
    fprintf(stderr, "mini-redis: listening on port %d\n", port);

    return true;
}

static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data)
{
    if (event != MG_EV_HTTP_MSG) {
        return;
    }
    struct mg_http_message *message = event_data;
    struct store *store = connection->fn_data;
    struct api_response response = api_handle(
        store, message->method.buf, message->method.len, message->uri.buf,
        message->uri.len, message->body.buf, message->body.len);
    /* 다음 store 호출 전에 응답을 송신 버퍼로 복사합니다. */
    response_send(connection, &response);
}

/* body는 mg_send로 보냅니다. mg_http_reply는 printf 형식이라 '\0'에서
   value가 잘리기 때문입니다. 204에는 body도 Content-Length도 없습니다. */
static void response_send(struct mg_connection *connection,
                          const struct api_response *response)
{
    mg_printf(connection, "HTTP/1.1 %d %s\r\n", response->status,
              status_reason(response->status));
    if (response->content_type != NULL) {
        mg_printf(connection, "Content-Type: %s\r\n", response->content_type);
    }
    if (response->status != 204) {
        mg_printf(connection, "Content-Length: %zu\r\n", response->body_len);
    }
    mg_printf(connection, "\r\n");
    if (response->body_len > 0) {
        mg_send(connection, response->body, response->body_len);
    }
}

static const char *status_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    default:
        return "Unknown";
    }
}
