/* mini-redis 서버입니다. 연결과 요청 해석은 mongoose가 맡고, 이 파일은 세 가지
   일을 합니다.
   - mongoose의 요청 이벤트에서 메서드, 경로, TTL 헤더, body를 꺼내 api 층에
     넘기고 돌려받은 응답을 보냅니다. 보관 기간을 세기 시작하는 시각으로, 요청
     헤더를 다 받은 시각을 연결마다 기억해 함께 넘깁니다.
   - 동시 연결을 CONNECTION_MAX개까지만 받고, 넘친 연결은 503으로 거절합니다.
   - IDLE_TIMEOUT_MS 동안 바이트가 오가지 않은 연결을 닫습니다.
   - SWEEP_INTERVAL_MS마다 주기 청소로 만료된 key를 SWEEP_MAX_COUNT개까지
     지웁니다.
   스레드 하나가 mg_mgr_poll을 반복해서 부르며 연결 받기, 요청 처리, 기한
   검사를 모두 맡습니다(ADR single-thread-event-loop). 요청을 하나씩 끝까지
   처리하므로, api 층이 빌려준 value를 다음 store 호출 전에 송신 버퍼로 복사할
   수 있습니다(ADR get-lends-value-until-next-change).

   사용법: mini-redis <port>
   SIGTERM이나 SIGINT를 받으면 루프를 빠져나와 mongoose와 store를 해제하고
   0으로 끝납니다. 그래야 AddressSanitizer가 누수를 끝에서 보고합니다. */

#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "mongoose.h"
#include "store.h"

#define PORT_MAX 65535
#define LISTEN_URL_MAX 64
/* 아래 세 값은 빌드할 때 정하는 상수입니다(spec Q19). make bench가 조합마다
   -D로 바꿔 다시 빌드합니다(Makefile의 SERVER_FLAGS). */
/* 요청이 없을 때 루프가 깨어나는 간격입니다. 청소 간격 S가 이보다 짧으면 실제
   간격이 이 값으로 늘어나므로 S와 같게 둡니다(spec Q17의 "다시 볼 것"). */
#ifndef POLL_INTERVAL_MS
#define POLL_INTERVAL_MS 25
#endif
/* 주기 청소의 간격 S(ms)와 한 번에 지우는 상한 K입니다(spec Q17, Q18). 잰 조합
   가운데 세 시나리오 모두에서 작은 GET의 p99가 10 ms 이하이고 회수 지연이 가장
   짧은 값입니다(spec Q21, "측정 결과"). S 100, K 20(초당 200개)은 초당 1,000개씩
   만료되는 key를 따라잡지 못했습니다. */
#ifndef SWEEP_INTERVAL_MS
#define SWEEP_INTERVAL_MS 25
#endif
#ifndef SWEEP_MAX_COUNT
#define SWEEP_MAX_COUNT 2000
#endif
/* 연결에서 이 시간 동안 바이트가 오가지 않으면 닫습니다(spec Q4). */
#define IDLE_TIMEOUT_MS 10000
/* 동시 연결의 상한입니다(spec Q8). 리스너는 세지 않습니다. 연결마다 수신 버퍼가
   최대 MG_MAX_RECV_SIZE(약 16 MiB)까지 크므로, 128개면 최악 약 2 GiB입니다. */
#define CONNECTION_MAX 128
#define TTL_HEADER_NAME "TTL"

/* mongoose가 연결마다 비워 둔 data 칸(MG_DATA_SIZE바이트)에 두는 값들입니다.
   data는 char 배열이라 uint64_t의 정렬에 맞지 않을 수 있으므로, 이 구조체를
   data에 직접 겹쳐 쓰지 않고 connection_state_get과 connection_state_set이
   통째로 memcpy해 옮깁니다. */
struct connection_state {
    /* 마지막으로 바이트가 오간 시각입니다(spec M3 Q4). */
    uint64_t last_activity;
    /* 지금 받는 요청의 헤더를 다 받은 시각입니다(spec Q11). */
    uint64_t headers_received_at;
    /* headers_received_at을 이 요청에서 이미 적었는지입니다. */
    bool has_headers_received_at;
};
_Static_assert(sizeof(struct connection_state) <= MG_DATA_SIZE,
               "connection_state는 mongoose의 연결 data 칸에 들어가야 합니다");

static volatile sig_atomic_t stop_requested = 0;

static int port_parse(const char *text);
static void stop_signals_install(void);
static void stop_signal_handle(int signum);
static int server_run(struct store *store, int port);
static bool listener_open(struct mg_mgr *manager, struct store *store,
                          int port);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void last_activity_set(struct mg_connection *connection,
                              uint64_t millis);
static struct connection_state
connection_state_get(const struct mg_connection *connection);
static void connection_state_set(struct mg_connection *connection,
                                 const struct connection_state *state);
static void connection_limit_check(struct mg_connection *connection);
static size_t open_connections_count(const struct mg_mgr *manager);
static void response_send_and_close(struct mg_connection *connection,
                                    const struct api_response *response);
static void response_send(struct mg_connection *connection,
                          const struct api_response *response);
static const char *status_reason(int status);
static void receive_buffer_grow(struct mg_connection *connection);
static void headers_received_note(struct mg_connection *connection);
static void headers_received_at_set(struct mg_connection *connection,
                                    bool is_noted, uint64_t millis);
static void headers_check(struct mg_connection *connection,
                          struct mg_http_message *message);
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message);
static struct api_ttl_header
ttl_header_read(const struct mg_connection *connection,
                const struct mg_http_message *message);
static size_t target_len(const struct mg_http_message *message);
static void connection_close_if_idle(struct mg_connection *connection);
static uint64_t sweep_if_due(struct store *store, uint64_t deadline);
static void sweep_trace(size_t deleted_count);

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
    uint64_t sweep_deadline = mg_millis() + SWEEP_INTERVAL_MS;
    while (exit_code == 0 && !stop_requested) {
        mg_mgr_poll(&manager, POLL_INTERVAL_MS);
        /* handler가 모두 끝난 뒤라, 빌려준 value를 가진 handler가 없습니다
           (spec "빌린 value"). */
        sweep_deadline = sweep_if_due(store, sweep_deadline);
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
    /* 바이트를 받거나 보낸 시각을 적습니다. 받기만 세면 느린 client가 16 MiB
       응답을 끝까지 받지 못하므로 보내기도 셉니다(spec Q4). 연결이 열릴 때도
       적어, 아무것도 보내지 않는 연결도 기한을 받게 합니다. */
    if (event == MG_EV_OPEN || event == MG_EV_READ || event == MG_EV_WRITE) {
        last_activity_set(connection, mg_millis());
    }
    if (event == MG_EV_ACCEPT) {
        connection_limit_check(connection);
    } else if (event == MG_EV_READ) {
        receive_buffer_grow(connection);
    } else if (event == MG_EV_HTTP_HDRS) {
        headers_received_note(connection);
        headers_check(connection, event_data);
    } else if (event == MG_EV_HTTP_MSG) {
        request_handle(connection, event_data);
    } else if (event == MG_EV_POLL) {
        connection_close_if_idle(connection);
    }
}

/* 연결에서 마지막으로 바이트가 오간 시각(mg_millis의 ms)을 적습니다. */
static void last_activity_set(struct mg_connection *connection, uint64_t millis)
{
    struct connection_state state = connection_state_get(connection);
    state.last_activity = millis;
    connection_state_set(connection, &state);
}

/* 연결의 data 칸에서 connection_state를 꺼냅니다. data 칸을 아는 함수는
   connection_state_get과 connection_state_set 둘뿐입니다. mongoose가 data 칸을
   0으로 채워 연결을 만들므로, 처음 꺼낸 값은 모든 필드가 0입니다. */
static struct connection_state
connection_state_get(const struct mg_connection *connection)
{
    struct connection_state state;
    memcpy(&state, connection->data, sizeof state);

    return state;
}

static void connection_state_set(struct mg_connection *connection,
                                 const struct connection_state *state)
{
    memcpy(connection->data, state, sizeof *state);
}

/* 새 연결을 받은 직후에 열린 연결을 셉니다. 새 연결까지 세어 CONNECTION_MAX를
   넘으면 503을 보내고, 송신 버퍼가 빈 뒤 mongoose가 연결을 닫게 합니다
   (is_draining). 받지 않고 두면 client에게 서버가 멈춘 것처럼 보이고, 응답 없이
   닫으면 네트워크 오류와 구분할 수 없기 때문입니다(spec Q8). */
static void connection_limit_check(struct mg_connection *connection)
{
    if (open_connections_count(connection->mgr) <= CONNECTION_MAX) {
        return;
    }
    struct api_response response = api_reject_connection();
    response_send_and_close(connection, &response);
}

/* 리스너와 이미 닫기로 한 연결(is_closing)은 세지 않습니다. 응답을 보내고 닫는
   중인 연결(is_draining)은 아직 fd와 버퍼를 쥐고 있으므로 셉니다. */
static size_t open_connections_count(const struct mg_mgr *manager)
{
    size_t count = 0;
    for (const struct mg_connection *connection = manager->conns;
         connection != NULL; connection = connection->next) {
        if (!connection->is_listening && !connection->is_closing) {
            count++;
        }
    }

    return count;
}

/* 오류 응답을 보내고, 송신 버퍼가 빈 뒤 mongoose가 연결을 닫게 합니다
   (is_draining). 바로 닫으면(is_closing) 보내지 못한 응답이 버려집니다. */
static void response_send_and_close(struct mg_connection *connection,
                                    const struct api_response *response)
{
    response_send(connection, response);
    connection->is_draining = 1;
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
    case 503:
        return "Service Unavailable";
    default:
        return "Unknown";
    }
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

/* 요청 헤더를 다 받은 시각을 적습니다. mongoose는 body를 기다리는 동안 데이터가
   올 때마다 MG_EV_HTTP_HDRS를 다시 부르므로, 요청마다 처음 한 번만 적습니다.
   그래야 16 MiB body를 느리게 올려도 보관 기간이 헤더를 받은 순간부터
   세어집니다(spec Q11). request_handle이 요청을 처리한 뒤 다음 요청을 위해
   지웁니다. */
static void headers_received_note(struct mg_connection *connection)
{
    if (!connection_state_get(connection).has_headers_received_at) {
        headers_received_at_set(connection, true, mg_millis());
    }
}

/* 헤더를 받은 시각과, 그 시각을 적었는지를 함께 씁니다. is_noted가 false면
   다음 요청이 시각을 새로 적습니다. */
static void headers_received_at_set(struct mg_connection *connection,
                                    bool is_noted, uint64_t millis)
{
    struct connection_state state = connection_state_get(connection);
    state.has_headers_received_at = is_noted;
    state.headers_received_at = millis;
    connection_state_set(connection, &state);
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
    response_send_and_close(connection, &response);
}

/* 이미 오류 응답(408, 411, 413, 503)을 보내고 닫는 중인 연결의 요청은 처리하지
   않습니다. mongoose는 연결을 닫을 때 받다 만 요청도 받은 데까지를 body로 삼아
   이 이벤트를 부르므로, 거르지 않으면 잘린 body가 저장됩니다. */
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message)
{
    if (connection->is_draining) {
        return;
    }
    struct store *store = connection->fn_data;
    struct api_ttl_header ttl_header = ttl_header_read(connection, message);
    struct api_response response =
        api_handle(store, mg_millis(), message->method.buf, message->method.len,
                   message->uri.buf, target_len(message), &ttl_header,
                   message->body.buf, message->body.len);
    /* 다음 store 호출 전에 응답을 송신 버퍼로 복사합니다. */
    response_send(connection, &response);
    /* mongoose는 이 이벤트 전에 is_resp를 세우고, 세워져 있는 동안 같은 연결의
       다음 요청을 해석하지 않습니다. is_resp를 내리는 것은 mg_http_reply뿐인데
       응답을 mg_printf와 mg_send로 보내므로, 여기서 직접 내립니다. 내리지 않으면
       연결 하나에서 두 번째 요청부터 응답이 오지 않습니다. */
    connection->is_resp = 0;
    /* 다음 요청이 헤더를 받은 시각을 새로 적게 합니다. */
    headers_received_at_set(connection, false, 0);
}

/* 요청의 TTL 헤더를 읽어, 첫 번째 값과 개수와 헤더를 다 받은 시각을 묶습니다.
   TTL 헤더가 없어도 count가 0인 값을 돌려줍니다. mg_http_get_header는 첫 번째
   값만 돌려주므로 개수는 직접 셉니다(spec Q15). 헤더 이름은 대소문자를 가리지
   않습니다. 값은 해석하지 않고 api 층에 넘깁니다. */
static struct api_ttl_header
ttl_header_read(const struct mg_connection *connection,
                const struct mg_http_message *message)
{
    struct api_ttl_header ttl_header = {
        .received_at = connection_state_get(connection).headers_received_at};
    size_t header_max = sizeof message->headers / sizeof message->headers[0];
    for (size_t i = 0; i < header_max && message->headers[i].name.len > 0;
         i++) {
        const struct mg_http_header *header = &message->headers[i];
        if (mg_strcasecmp(header->name, mg_str(TTL_HEADER_NAME)) != 0) {
            continue;
        }
        if (ttl_header.count == 0) {
            ttl_header.text = header->value.buf;
            ttl_header.text_len = header->value.len;
        }
        ttl_header.count++;
    }

    return ttl_header;
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

/* mongoose가 루프를 돌 때마다(POLL_INTERVAL_MS 이내) 연결마다 부릅니다. 기한이
   지났고 요청을 받는 중이면(수신 버퍼에 요청 조각이 남아 있으면) 408을 보내고,
   송신 버퍼가 빈 뒤 mongoose가 닫게 합니다. 이때 시각을 다시 적어 408을 보낼
   시간을 한 번 더 줍니다. 응답 뒤에 쉬는 연결, 응답을 받아 가지 않는 연결, 이미
   닫는 중인 연결은 응답 없이 바로 닫습니다. 스레드를 더하지 않고 같은 루프에서
   검사합니다(ADR single-thread-event-loop). */
static void connection_close_if_idle(struct mg_connection *connection)
{
    if (connection->is_listening ||
        mg_millis() - connection_state_get(connection).last_activity <
            IDLE_TIMEOUT_MS) {
        return;
    }
    if (connection->is_draining || connection->recv.len == 0) {
        connection->is_closing = 1;
        return;
    }
    struct api_response response = api_reject_timeout();
    response_send_and_close(connection, &response);
    last_activity_set(connection, mg_millis());
}

/* 기한이 지났으면 주기 청소를 한 번 하고 다음 기한 "기한 + S"를 돌려줍니다.
   기한에서 많이 늦었어도 한 번만 청소합니다. 다음 기한을 지금이 아니라 기한에서
   세어 간격의 격자를 지킵니다(spec Q17). */
static uint64_t sweep_if_due(struct store *store, uint64_t deadline)
{
    uint64_t now = mg_millis();
    if (now < deadline) {
        return deadline;
    }
    size_t deleted_count = store_delete_expired(store, now, SWEEP_MAX_COUNT);
    sweep_trace(deleted_count);

    return deadline + SWEEP_INTERVAL_MS;
}

/* 측정 빌드(make bench)만 SWEEP_TRACE를 켭니다. 청소가 key를 지웠으면 끝난
   시각(mg_millis)과 지운 개수를 stderr에 한 줄 적습니다. 만료된 key는 조회해도
   없는 key와 같은 404라서, 바깥에서는 이 줄로만 회수 지연을 잽니다
   (spec "측정 결과"). make verify의 빌드에서는 아무것도 하지 않습니다. */
static void sweep_trace(size_t deleted_count)
{
#ifdef SWEEP_TRACE
    if (deleted_count > 0) {
        fprintf(stderr, "sweep %" PRIu64 " %zu\n", mg_millis(), deleted_count);
    }
#else
    (void)deleted_count;
#endif
}
