#include <stdlib.h>
#include <stdio.h>
/* Track only mirror-thread allocations, keeping dependencies real. */
static void *pending[16];
static int pending_count;
static void *mirror_malloc(size_t n) {
    void *p = malloc(n);
    if (p) {
        if (pending_count == 16) abort();
        pending[pending_count++] = p;
    }
    return p;
}
static void mirror_free(void *p) {
    for (int i = 0; i < pending_count; i++) {
        if (pending[i] == p) { pending[i] = pending[--pending_count]; break; }
    }
    free(p);
}
#define malloc mirror_malloc
#define free mirror_free
#include "../lib/raop_rtp_mirror.c"
#undef malloc
#undef free

static int codec_calls;
static int set_codec(void *cls, video_codec_t codec) {
    (void)cls;
    if (codec != VIDEO_CODEC_H264) abort();
    codec_calls++;
    return 0;
}
static void quiet_log(void *cls, int level, const char *message) {
    (void)cls; (void)level; (void)message;
}
int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
#endif
    logger_t *log = logger_init();
    logger_set_callback(log, quiet_log, NULL);
    raop_callbacks_t callbacks = {0};
    callbacks.video_set_codec = set_codec;
    const char remote[] = "127.0.0.1";
    unsigned char key[16] = {0};
    timing_protocol_t protocol = TP_NONE;
    raop_ntp_t *ntp = raop_ntp_init(log, &callbacks, remote, 4, 0, &protocol);
    raop_rtp_mirror_t *mirror = raop_rtp_mirror_init(log, &callbacks, ntp, remote, 4, key);
    if (!ntp || !mirror || raop_rtp_mirror_init_socket(mirror, 0)) return 1;
    int client = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(mirror->mirror_data_lport);
    if (connect(client, (struct sockaddr *)&addr, sizeof(addr))) return 1;
    /* A complete H264 config, then a partially received payload and EOF.
     * No renderer or AES decoder is needed for this teardown path. */
    unsigned char wire[128 + 13 + 128 + 1] = {0};
    wire[0] = 13; wire[4] = 1;
    wire[128 + 7] = 1; wire[128 + 8] = 0x67;
    wire[128 + 11] = 1; wire[128 + 12] = 0x68;
    wire[141] = 20;
    if (send(client, (const char *)wire, sizeof(wire), 0) != sizeof(wire)) return 1;
    shutdown(client, 1);
    mirror->running = 1;
    /* Run synchronously: input is already queued, and partial-payload EOF exits. */
    raop_rtp_mirror_thread(mirror);
    CLOSESOCKET(client);
    CLOSESOCKET(mirror->mirror_data_sock);
    mirror->mirror_data_sock = -1;
    raop_rtp_mirror_destroy(mirror);
    raop_ntp_destroy(ntp);
    logger_destroy(log);
#ifdef _WIN32
    WSACleanup();
#endif
    if (pending_count || codec_calls != 1) {
        fprintf(stderr, "FAIL: mirror retained %d allocations; codec callbacks %d\n", pending_count, codec_calls);
        return 1;
    }
    return 0;
}
