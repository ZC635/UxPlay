/* Exercise real plist parsing and RTSP handlers; replace only the crypto
 * boundary so malformed sizes cannot hide behind authentication failure. */
#include "../lib/pairing.h"
#include <plist/plist.h>
#include <string.h>
#include "../lib/fairplay.h"
static int crypto_calls;
static int accept_confirmation(pairing_session_t *s, pairing_t *p,
                               unsigned char *epk, unsigned char *tag) {
    (void)s; (void)p; (void)epk; (void)tag;
    crypto_calls++;
    return 32;
}
static int accept_proof(pairing_session_t *s, pairing_t *p,
                        const unsigned char *pk, int n, unsigned char *proof, int len) {
    (void)s; (void)p; (void)pk; (void)n; (void)proof; (void)len;
    crypto_calls++;
    return 0;
}
static int accept_decrypt(fairplay_t *fp, const unsigned char input[72], unsigned char output[16]) {
    (void)fp; (void)input;
    memset(output, 0, 16);
    crypto_calls++;
    return 0;
}
#define srp_confirm_pair_setup accept_confirmation
#define srp_validate_proof accept_proof
#define fairplay_decrypt accept_decrypt
#include "../lib/raop.c"
#undef srp_confirm_pair_setup
#undef srp_validate_proof
#undef fairplay_decrypt

static int failures;
static void quiet_log(void *cls, int level, const char *message) {
    (void)cls; (void)level; (void)message;
}

static void check_setup_lengths(unsigned int iv_len, unsigned int key_len) {
    char bytes[80] = {0};
    plist_t input = plist_new_dict();
    plist_dict_set_item(input, "eiv", plist_new_data(bytes, iv_len));
    plist_dict_set_item(input, "ekey", plist_new_data(bytes, key_len));
    char *body = NULL, *reply = NULL;
    uint32_t body_len = 0;
    int reply_len = 0, result = 0;
    plist_to_bin(input, &body, &body_len);
    plist_free(input);
    char header[256];
    int n = snprintf(header, sizeof(header),
        "SETUP /stream RTSP/1.0\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: %u\r\n\r\n", body_len);
    http_request_t *request = http_request_init();
    http_request_add_data(request, header, n);
    http_request_add_data(request, body, body_len);
    plist_mem_free(body);
    raop_t receiver = {0};
    receiver.logger = logger_init();
    logger_set_callback(receiver.logger, quiet_log, NULL);
    receiver.pairing = pairing_init_generate("AA:BB:CC:DD:EE:FF", "", &result);
    raop_conn_t conn = {0};
    conn.raop = &receiver;
    conn.session = pairing_session_init(receiver.pairing);
    unsigned char remote[] = {127, 0, 0, 1};
    conn.remote = remote;
    conn.remotelen = sizeof(remote);
    /* Only decryption is replaced; exact lengths still construct real sessions. */
    http_response_t *response = http_response_create();
    http_response_init(response, "RTSP/1.0", 200, "OK");
    crypto_calls = 0;
    raop_handler_setup(&conn, request, response, &reply, &reply_len);
    http_response_finish(response, reply, reply_len);
    int wire_len = 0;
    const char *wire = http_response_get_data(response, &wire_len);
    bool valid = iv_len == 16 && key_len == 72;
    if (!wire || strncmp(wire, valid ? "RTSP/1.0 200" : "RTSP/1.0 400", 12) ||
        crypto_calls != (valid ? 1 : 0) ||
        (valid ? (!conn.raop_rtp || !conn.raop_rtp_mirror) : (conn.raop_rtp || conn.raop_rtp_mirror))) {
        fprintf(stderr, "FAIL SETUP lengths %u/%u: %.12s\n", iv_len, key_len, wire ? wire : "NULL");
        failures++;
    }
    free(reply);
    http_response_destroy(response);
    http_request_destroy(request);
    raop_rtp_destroy(conn.raop_rtp);
    raop_rtp_mirror_destroy(conn.raop_rtp_mirror);
    raop_ntp_destroy(conn.raop_ntp);
    pairing_session_destroy(conn.session);
    pairing_destroy(receiver.pairing);
    logger_destroy(receiver.logger);
}
static void check_lengths(int phase, unsigned int first, unsigned int second) {
    unsigned char bytes[300] = {0};
    plist_t input = plist_new_dict();
    plist_dict_set_item(input, phase == 2 ? "pk" : "epk", plist_new_data((char *)bytes, first));
    plist_dict_set_item(input, phase == 2 ? "proof" : "authTag", plist_new_data((char *)bytes, second));
    char *body = NULL, *reply = NULL;
    uint32_t body_len = 0;
    int reply_len = 0, result = 0;
    plist_to_bin(input, &body, &body_len);
    plist_free(input);
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "POST /pair-setup-pin RTSP/1.0\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: %u\r\n\r\n", body_len);
    http_request_t *request = http_request_init();
    http_request_add_data(request, header, header_len);
    http_request_add_data(request, body, body_len);
    plist_mem_free(body);
    raop_t receiver = {0};
    receiver.logger = logger_init();
    logger_set_callback(receiver.logger, quiet_log, NULL);
    logger_set_level(receiver.logger, LOGGER_ERR);
    receiver.pairing = pairing_init_generate("AA:BB:CC:DD:EE:FF", "", &result);
    raop_conn_t conn = {0};
    conn.raop = &receiver;
    conn.session = pairing_session_init(receiver.pairing);
    http_response_t *response = http_response_create();
    http_response_init(response, "RTSP/1.0", 200, "OK");
    crypto_calls = 0;
    raop_handler_pairsetup_pin(&conn, request, response, &reply, &reply_len);
    http_response_finish(response, reply, reply_len);
    int wire_len = 0;
    const char *wire = http_response_get_data(response, &wire_len);
    bool valid = phase == 2 ? first > 0 && first <= 256 && second == 20
                            : first == 32 && second == 16;
    if (!wire || strncmp(wire, valid ? "RTSP/1.0 200" : "RTSP/1.0 470", 12) ||
        crypto_calls != (valid ? 1 : 0)) {
        fprintf(stderr, "FAIL phase %d lengths %u/%u: status %.12s, crypto calls %d\n",
                phase, first, second, wire ? wire : "NULL", crypto_calls);
        failures++;
    }
    free(reply);
    http_response_destroy(response);
    http_request_destroy(request);
    pairing_session_destroy(conn.session);
    pairing_destroy(receiver.pairing);
    logger_destroy(receiver.logger);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc > 1 && !strcmp(argv[1], "setup")) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
#endif
        const unsigned int lengths[][2] = {{17, 72}, {16, 73}, {0, 72}, {1, 72}, {15, 72}, {16, 0}, {16, 1}, {16, 71}, {16, 72}};
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
            check_setup_lengths(lengths[i][0], lengths[i][1]);
#ifdef _WIN32
        WSACleanup();
#endif
        return failures ? 1 : 0;
    }
    /* Oversize cases fail deterministically on the old handler without relying
     * on allocator layout. Short/empty cases also run after the fix. */
    check_lengths(3, 33, 16);
    check_lengths(3, 32, 17);
    if (failures) return 1;
    const unsigned int sizes[] = {0, 1, 15, 16, 17, 19, 20, 21, 31, 32, 33, 64, 256, 257};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        check_lengths(2, 256, sizes[i]);
        check_lengths(2, sizes[i], 20);
        check_lengths(3, sizes[i], 16);
        check_lengths(3, 32, sizes[i]);
    }
    return failures ? 1 : 0;
}
