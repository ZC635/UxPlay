/* Include the implementation to inspect ownership after stop, without exposing
 * test-only state through the public API. Sockets and worker threads are real. */
#include "../lib/raop_rtp.c"
#include <stdio.h>
static int failures;
static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); failures++; }
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
    unsigned char key[16] = {0};
    const char remote[] = "127.0.0.1";
    timing_protocol_t protocol = TP_NONE;
    raop_ntp_t *ntp = raop_ntp_init(log, &callbacks, remote, 4, 0, &protocol);
    raop_rtp_t *rtp = raop_rtp_init(log, &callbacks, ntp, remote, 4, key, key);
    check(rtp != NULL && ntp != NULL, "initialize RTP/NTP");
    if (!rtp || !ntp) return 1;
    for (int cycle = 0; cycle < 3; cycle++) {
        unsigned short remote_port = 0, control_port = 0, data_port = 0;
        unsigned char codec = 8;
        unsigned int rate = 44100;
        raop_rtp_start_audio(rtp, &remote_port, &control_port, &data_port, &codec, &rate);
        check(raop_rtp_is_running(rtp) && control_port && data_port, "start/restart audio");
        int control_socket = rtp->csock, data_socket = rtp->dsock;
        if (cycle == 1) {
            /* Model a worker which has stopped itself before its owner calls stop. */
            MUTEX_LOCK(rtp->run_mutex);
            rtp->running = 0;
            MUTEX_UNLOCK(rtp->run_mutex);
        }
        raop_rtp_stop(rtp);
        check(rtp->joined, "join a stopped worker before destroying its state");
        check(!rtp->rtp_session_csock && !rtp->rtp_session_dsock,
              "release both timestamp sessions and clear ownership");
        check(rtp->csock == -1 && rtp->dsock == -1, "invalidate both sockets");
        struct sockaddr_storage addr;
        socklen_t length = sizeof(addr);
        check(getsockname(control_socket, (struct sockaddr *)&addr, &length) != 0,
              "control socket is closed");
        length = sizeof(addr);
        check(getsockname(data_socket, (struct sockaddr *)&addr, &length) != 0,
              "data socket is closed");
        raop_rtp_stop(rtp); /* idempotent */
        if (failures) break;
    }
    raop_rtp_destroy(rtp);
    raop_ntp_destroy(ntp);
    logger_destroy(log);
#ifdef _WIN32
    WSACleanup();
#endif
    return failures ? 1 : 0;
}
