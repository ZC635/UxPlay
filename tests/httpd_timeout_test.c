/* Exercise the real HTTP server with a controllable clock, without a minute-long test. */
#include <time.h>
#include <glib.h>
static gint test_seconds = 100;
static time_t test_time(time_t *out) {
    time_t now = (time_t)g_atomic_int_get(&test_seconds);
    if (out) *out = now;
    return now;
}
#define time test_time
#include "../lib/httpd.c"
#undef time

typedef struct {
    httpd_t *http;
    gint accepted;
    gint closed;
    gint requests;
} context_t;
static void log_message(void *opaque, int level, const char *message) { (void)opaque; (void)level; (void)message; }
static void *connected(void *opaque, unsigned char *l, int ll, unsigned char *r, int rl, unsigned int z) {
    context_t *ctx = opaque;
    g_atomic_int_inc(&ctx->accepted);
    return opaque;
}
static void disconnected(void *opaque) { g_atomic_int_inc(&((context_t *)opaque)->closed); }
static void requested(void *opaque, http_request_t *request, http_response_t **response) {
    context_t *ctx = opaque;
    httpd_set_connection_type(ctx->http, ctx, CONNECTION_TYPE_RAOP);
    *response = http_response_create();
    http_response_init(*response, "RTSP/1.0", 200, "OK");
    http_response_add_header(*response, "Content-Length", "0");
    http_response_finish(*response, NULL, 0);
    g_atomic_int_inc(&ctx->requests);
}
static int wait_count(gint *counter, int expected) {
    gint64 until = g_get_monotonic_time() + 2500000;
    while (g_atomic_int_get(counter) < expected && g_get_monotonic_time() < until) g_usleep(1000);
    return g_atomic_int_get(counter);
}
static void test_timeout(gboolean establish) {
    g_atomic_int_set(&test_seconds, 100);
    context_t ctx = {0};
    logger_t *log = logger_init();
    logger_set_callback(log, log_message, NULL);
    httpd_callbacks_t cb = { &ctx, connected, requested, disconnected };
    ctx.http = httpd_init(log, &cb, 1);
    unsigned short port = 0;
    g_assert_cmpint(httpd_start(ctx.http, &port), ==, 1);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_assert_cmpint(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    g_assert_cmpint(wait_count(&ctx.accepted, 1), ==, 1);
    const char *request = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    if (establish) {
        send(fd, request, (int)strlen(request), 0);
        g_assert_cmpint(wait_count(&ctx.requests, 1), ==, 1);
    }
    /* The accept callback precedes storing last_active; wake a full select cycle. */
    g_usleep(50000);
    g_atomic_int_set(&test_seconds, 162);
    if (establish) {
        send(fd, request, (int)strlen(request), 0);
        int requests = wait_count(&ctx.requests, 2);
        int closed = g_atomic_int_get(&ctx.closed);
        CLOSESOCKET(fd);
        httpd_destroy(ctx.http);
        logger_destroy(log);
        g_assert_cmpint(requests, ==, 2);
        g_assert_cmpint(closed, ==, 0);
    } else {
        int closed = wait_count(&ctx.closed, 1);
        CLOSESOCKET(fd);
        httpd_destroy(ctx.http);
        logger_destroy(log);
        g_assert_cmpint(closed, ==, 1);
    }
}
static void silent_client_expires(void) { test_timeout(FALSE); }
static void established_control_channel_survives(void) { test_timeout(TRUE); }
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_assert_cmpint(netutils_init(), ==, 0);
    g_test_add_func("/httpd/silent-client-expires", silent_client_expires);
    g_test_add_func("/httpd/established-control-survives", established_control_channel_survives);
    int result = g_test_run();
    netutils_cleanup();
    return result;
}
