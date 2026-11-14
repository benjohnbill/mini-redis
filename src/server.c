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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "mongoose.h"
#include "store.h"

#define PORT_MAX 65535
#define LISTEN_URL_MAX 64
#define POLL_INTERVAL_MS 100
/* 연결에서 이 시간 동안 바이트가 오가지 않으면 닫습니다(spec Q4). */
#define IDLE_TIMEOUT_MS 10000

static volatile sig_atomic_t stop_requested = 0;

static int port_parse(const char *text);
static void stop_signals_install(void);
static void stop_signal_handle(int signum);
static int server_run(struct store *store, int port);
static bool listener_open(struct mg_mgr *manager, struct store *store,
                          int port);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void activity_mark(struct mg_connection *connection);
static void receive_buffer_grow(struct mg_connection *connection);
static void headers_check(struct mg_connection *connection,
                          struct mg_http_message *message);
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message);
static size_t target_len(const struct mg_http_message *message);
static void response_send(struct mg_connection *connection,
                          const struct api_response *response);
static const char *status_reason(int status);
static void idle_connection_close(struct mg_connection *connection);

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
    action.sa_handler = stop_signal_handle;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

/* 신호 처리기에서는 플래그만 세웁니다. 루프는 늦어도 POLL_INTERVAL_MS 뒤에 봅니다. */
static void stop_signal_handle(int signum)
{
    (void)signum;
    stop_requested = 1;
}

/* 신호를 받을 때까지 요청을 처리합니다. 연결을 받을 수 없으면 1을 돌려줍니다. */
static int server_run(struct store *store, int port)
{
    /* mongoose의 기본 수준은 연결의 읽기와 쓰기마다 줄을 찍어, 큰 body 하나가
       출력을 수천 줄로 채웁니다. 오류만 찍게 합니다. */
    mg_log_set(MG_LL_ERROR);
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
    if (event == MG_EV_OPEN || event == MG_EV_READ || event == MG_EV_WRITE) {
        activity_mark(connection);
    }
    if (event == MG_EV_READ) {
        receive_buffer_grow(connection);
    } else if (event == MG_EV_HTTP_HDRS) {
        headers_check(connection, event_data);
    } else if (event == MG_EV_HTTP_MSG) {
        request_handle(connection, event_data);
    } else if (event == MG_EV_POLL) {
        idle_connection_close(connection);
    }
}

/* 바이트를 받거나 보낸 시각을 연결의 data 칸 맨 앞에 적습니다. 받기만 세면
   느린 client가 16 MiB 응답을 끝까지 받지 못하므로 보내기도 셉니다(spec Q4).
   연결이 열릴 때도 적어, 아무것도 보내지 않는 연결도 기한을 받게 합니다. */
static void activity_mark(struct mg_connection *connection)
{
    uint64_t now = mg_millis();
    memcpy(connection->data, &now, sizeof now);
}

/* mongoose는 수신 버퍼가 차면 MG_IO_SIZE(16 KiB)만큼만 늘리고, 늘릴 때마다 새
   버퍼를 할당해 복사합니다. 16 MiB body면 약 1,000번 복사해 ASan 빌드에서 PUT
   하나에 30초가 걸렸습니다. 그래서 버퍼가 차면 두 배로 늘리되, 헤더가 알린 요청
   길이보다 크게는 늘리지 않습니다. 메모리는 실제로 받은 바이트의 두 배를 넘지
   않습니다. MG_EV_READ는 mongoose의 HTTP 처리가 끝난 뒤에 오므로, 버퍼를 옮겨도
   mongoose가 들고 있는 옛 주소가 없습니다. 상한을 넘는 요청은 headers_check가
   이미 닫는 중으로 바꿨으므로 늘리지 않습니다. */
static void receive_buffer_grow(struct mg_connection *connection)
{
    struct mg_iobuf *received = &connection->recv;
    struct mg_http_message message;
    if (connection->is_draining || received->len < received->size ||
        mg_http_parse((const char *)received->buf, received->len, &message) <=
            0) {
        return;
    }
    size_t doubled = received->size * 2;
    size_t request_len = message.message.len;
    /* chunked 요청은 요청 길이가 헤더 길이뿐이므로, 줄이지 않게 거릅니다. */
    if (request_len > received->size) {
        mg_iobuf_resize(received,
                        doubled < request_len ? doubled : request_len);
    }
}

/* body가 오기 전에 chunked 요청인지, Content-Length가 상한을 넘는지 봅니다(spec
   Q10, "오류의 우선순위"). 걸리면 411이나 413을 보내고, 송신 버퍼가 빈 뒤
   mongoose가 연결을 닫게 합니다(is_draining). mongoose는 body를 기다리는 동안
   데이터가 올 때마다 이 이벤트를 다시 부르므로, 이미 닫는 중인 연결에는 응답을
   다시 보내지 않습니다. */
static void headers_check(struct mg_connection *connection,
                          struct mg_http_message *message)
{
    bool has_transfer_encoding =
        mg_http_get_header(message, "Transfer-Encoding") != NULL;
    struct api_response response;
    if (connection->is_draining ||
        !api_reject_headers(has_transfer_encoding, message->body.len,
                            &response)) {
        return;
    }
    response_send(connection, &response);
    connection->is_draining = 1;
}

/* 이미 오류 응답(408, 411, 413)을 보내고 닫는 중인 연결의 요청은 처리하지
   않습니다. mongoose는 연결을 닫을 때 받다 만 요청도 받은 데까지를 body로 삼아
   이 이벤트를 부르므로, 거르지 않으면 잘린 body가 저장됩니다. */
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message)
{
    if (connection->is_draining) {
        return;
    }
    struct store *store = connection->fn_data;
    struct api_response response = api_handle(
        store, message->method.buf, message->method.len, message->uri.buf,
        target_len(message), message->body.buf, message->body.len);
    /* 다음 store 호출 전에 응답을 송신 버퍼로 복사합니다. */
    response_send(connection, &response);
    /* mongoose는 이 이벤트 전에 is_resp를 세우고, 세워져 있는 동안 같은 연결의
       다음 요청을 해석하지 않습니다. is_resp를 내리는 것은 mg_http_reply뿐인데
       응답을 mg_printf와 mg_send로 보내므로, 여기서 직접 내립니다. 내리지 않으면
       연결 하나에서 두 번째 요청부터 응답이 오지 않습니다. */
    connection->is_resp = 0;
}

/* mongoose는 요청 줄의 대상을 ? 앞(uri)과 뒤(query)로 나눕니다. api 층은 ?를
   직접 판단하므로 둘을 합친 대상 전부의 길이를 돌려줍니다. 두 조각은 수신
   버퍼에서 ?를 사이에 두고 이어져 있고, ?가 없으면 query.buf가 NULL입니다. */
static size_t target_len(const struct mg_http_message *message)
{
    if (message->query.buf == NULL) {
        return message->uri.len;
    }

    return (size_t)(message->query.buf + message->query.len - message->uri.buf);
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
    if (response->allow != NULL) {
        mg_printf(connection, "Allow: %s\r\n", response->allow);
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
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

/* mongoose가 루프를 돌 때마다(POLL_INTERVAL_MS 이내) 연결마다 부릅니다. 기한이
   지났고 요청을 받는 중이면(수신 버퍼에 요청 조각이 남아 있으면) 408을 보내고,
   송신 버퍼가 빈 뒤 mongoose가 닫게 합니다. 이때 시각을 다시 적어 408을 보낼
   시간을 한 번 더 줍니다. 응답 뒤에 쉬는 연결, 응답을 받아 가지 않는 연결, 이미
   닫는 중인 연결은 응답 없이 바로 닫습니다. 스레드를 더하지 않고 같은 루프에서
   검사합니다(ADR single-thread-event-loop). */
static void idle_connection_close(struct mg_connection *connection)
{
    uint64_t last_activity = 0;
    memcpy(&last_activity, connection->data, sizeof last_activity);
    if (connection->is_listening ||
        mg_millis() - last_activity < IDLE_TIMEOUT_MS) {
        return;
    }
    if (connection->is_draining || connection->recv.len == 0) {
        connection->is_closing = 1;
        return;
    }
    struct api_response response = api_request_timeout();
    response_send(connection, &response);
    connection->is_draining = 1;
    activity_mark(connection);
}
