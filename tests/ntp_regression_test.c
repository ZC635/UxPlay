#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../lib/raop.h"
#include "../lib/raop_ntp.h"
#include "../lib/byteutils.h"
#include "../lib/compat.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void close_socket(int fd)
{
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

static int bind_loopback(unsigned short *port)
{
    int fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(*port);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &addrlen) != 0) {
        if (fd >= 0) close_socket(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

static int test_fallback_preserves_source_address(void)
{
    unsigned short port = 0;
    int receiver = bind_loopback(&port);
    unsigned short sender_port = 0;
    int sender = bind_loopback(&sender_port);
    logger_t *logger = NULL;
    raop_callbacks_t callbacks;
    timing_protocol_t protocol = NTP;
    raop_ntp_t *ntp = NULL;
    kernel_timestamp_session_t *session;
    struct sockaddr_in remote;
    struct sockaddr_in source;
    int source_len = sizeof(source);
    char sent[] = "ntp-fallback";
    char received[32] = {0};
#ifdef _WIN32
    DWORD timeout = 2000;
#else
    struct timeval timeout = { 2, 0 };
#endif
    int ok;

    check(receiver >= 0 && sender >= 0, "bind fallback loopback sockets");
    if (receiver < 0 || sender < 0) {
        if (receiver >= 0) close_socket(receiver);
        if (sender >= 0) close_socket(sender);
        return 1;
    }
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    memset(&callbacks, 0, sizeof(callbacks));
    logger = logger_init();
    check(logger != NULL, "initialize fallback test logger");
    if (logger == NULL) {
        close_socket(receiver);
        close_socket(sender);
        return 1;
    }
    ntp = raop_ntp_init(logger, &callbacks, "127.0.0.1", 4, port, &protocol);
    check(ntp != NULL, "initialize fallback test NTP context");
    if (ntp == NULL) {
        logger_destroy(logger);
        close_socket(receiver);
        close_socket(sender);
        return 1;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    remote.sin_port = htons(port);
    ok = sendto(sender, sent, (int)strlen(sent), 0,
                (struct sockaddr *)&remote, sizeof(remote)) == (int)strlen(sent);
    check(ok, "send fallback datagram");

    session = kernel_timestamp_session_create(ntp, receiver);
    check(session != NULL, "create fallback timestamp session");
    if (session == NULL) {
        raop_ntp_destroy(ntp);
        logger_destroy(logger);
        close_socket(receiver);
        close_socket(sender);
        return 1;
    }
#ifdef _WIN32
    check(session->qpc_frequency > 0,
          "timestamp session initializes its QPC frequency without global setup");
#endif
    ok = kernel_timestamp_session_recv(session, received, sizeof(received),
                                       &source, &source_len, NULL, NULL);
    check(ok == (int)strlen(sent), "fallback receives UDP datagram");
    check(strcmp(received, sent) == 0, "fallback returns datagram payload");
    check(source_len == (int)sizeof(source), "fallback returns source address length");
    check(source.sin_family == AF_INET && ntohs(source.sin_port) == sender_port,
          "fallback returns source address and port");
    kernel_timestamp_session_destroy(session);
    raop_ntp_destroy(ntp);
    logger_destroy(logger);
    close_socket(sender);
    return 0;
}

static int receive_with_timeout(int fd, unsigned char *data, size_t length,
                                struct sockaddr_in *source, socklen_t *source_len)
{
#ifdef _WIN32
    DWORD timeout = 8000;
#else
    struct timeval timeout = { 8, 0 };
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    return (int)recvfrom(fd, (char *)data, (int)length, 0,
                         (struct sockaddr *)source, source_len);
}

static int test_client_reference_keeps_full_q32_timestamp(void)
{
    unsigned short server_port = 0;
    unsigned short client_port = 0;
    int server = bind_loopback(&server_port);
    logger_t *logger = NULL;
    raop_callbacks_t callbacks;
    timing_protocol_t protocol = NTP;
    raop_ntp_t *ntp = NULL;
    unsigned char request[128] = {0};
    unsigned char response[32] = {0x80, 0xd3};
    struct sockaddr_in client;
    struct sockaddr_in source;
    socklen_t client_len = sizeof(client);
    socklen_t source_len = sizeof(source);
    const uint64_t client_t2 = UINT64_C(0x1234567801020304);
    const uint64_t client_t3 = UINT64_C(0x23456789a1b2c3d4);
    int received;

    check(server >= 0, "bind NTP loopback server");
    if (server < 0) return 1;
    memset(&callbacks, 0, sizeof(callbacks));
    logger = logger_init();
    check(logger != NULL, "initialize NTP test logger");
    if (logger == NULL) {
        close_socket(server);
        return 1;
    }

    ntp = raop_ntp_init(logger, &callbacks, "127.0.0.1", 4, server_port, &protocol);
    check(ntp != NULL, "initialize NTP client");
    if (ntp == NULL) goto cleanup;
    raop_ntp_start(ntp, &client_port);
    check(client_port != 0, "start NTP client on a loopback port");
    if (client_port == 0) goto cleanup;

    received = receive_with_timeout(server, request, sizeof(request), &client, &client_len);
    check(received == 32, "receive initial NTP request");
    if (received != 32) goto cleanup;

    memcpy(response + 8, request + 24, 8);
    byteutils_put_long_be(response, 16, client_t2);
    byteutils_put_long_be(response, 24, client_t3);
    received = sendto(server, (const char *)response, sizeof(response), 0,
                      (struct sockaddr *)&client, client_len);
    check(received == (int)sizeof(response),
          "send NTP response carrying full client timestamp");
    if (received != (int)sizeof(response)) goto cleanup;

    received = receive_with_timeout(server, request, sizeof(request), &source, &source_len);
    check(received == 32, "receive following NTP request");
    if (received == 32) {
        check(byteutils_get_long_be(request, 8) == client_t3,
              "next NTP request echoes the complete Q32.32 client transmit timestamp");
    }

cleanup:
    if (ntp != NULL) {
        raop_ntp_stop(ntp);
        raop_ntp_destroy(ntp);
    }
    logger_destroy(logger);
    close_socket(server);
    return 0;
}

int main(void)
{
#ifdef _WIN32
    WSADATA wsa_data;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "FAIL: initialize Winsock\n");
        return 1;
    }
#endif
    test_fallback_preserves_source_address();
    test_client_reference_keeps_full_q32_timestamp();
#ifdef _WIN32
    WSACleanup();
#endif
    if (failures) {
        fprintf(stderr, "%d NTP regression assertion(s) failed\n", failures);
        return 1;
    }
    puts("NTP regression tests passed");
    return 0;
}
