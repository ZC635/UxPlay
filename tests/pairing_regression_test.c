#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/bn.h>
#include <openssl/sha.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../lib/pairing.h"
#include "../lib/srp.h"

#define TEST_PROOF_SIZE SRP_PROOF_SIZE
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "assertion failed: %s (%s:%d)\n", \
                #condition, __FILE__, __LINE__); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const char srp_2048_prime[] =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

static void hash_number(const BIGNUM *number, unsigned char digest[TEST_PROOF_SIZE]) {
    int length = BN_num_bytes(number);
    unsigned char *bytes = malloc((size_t)length);

    assert(bytes != NULL);
    BN_bn2bin(number, bytes);
    SHA1(bytes, (size_t)length, digest);
    free(bytes);
}

static BIGNUM *hash_padded_pair(const BIGNUM *first, const BIGNUM *second) {
    unsigned char padded[512] = {0};
    unsigned char digest[TEST_PROOF_SIZE];
    int first_len = BN_num_bytes(first);
    int second_len = BN_num_bytes(second);

    assert(first_len <= 256 && second_len <= 256);
    BN_bn2bin(first, padded + 256 - first_len);
    BN_bn2bin(second, padded + 512 - second_len);
    SHA1(padded, sizeof(padded), digest);
    return BN_bin2bn(digest, sizeof(digest), NULL);
}

static void make_client_proof(const char *username, const char *pin,
                              const unsigned char *salt_bytes, int salt_len,
                              const unsigned char *server_B, int server_B_len,
                              unsigned char **client_A, int *client_A_len,
                              unsigned char proof[TEST_PROOF_SIZE]) {
    BIGNUM *N = NULL, *g = BN_new(), *salt = NULL, *B = NULL, *a = BN_new();
    BIGNUM *A = BN_new(), *k = NULL, *u = NULL, *x = NULL, *v = BN_new();
    BIGNUM *base = BN_new(), *exponent = BN_new(), *S = BN_new();
    BN_CTX *ctx = BN_CTX_new();
    unsigned char inner_hash[TEST_PROOF_SIZE], x_hash[TEST_PROOF_SIZE];
    unsigned char key[2 * TEST_PROOF_SIZE];
    unsigned char digest0[TEST_PROOF_SIZE], digest1[TEST_PROOF_SIZE];
    unsigned char h_N[TEST_PROOF_SIZE], h_g[TEST_PROOF_SIZE];
    unsigned char h_user[TEST_PROOF_SIZE], h_xor[TEST_PROOF_SIZE];
    unsigned char salt_encoded[32], A_encoded[256], B_encoded[256];
    unsigned int salt_encoded_len, A_encoded_len, B_encoded_len;
    SHA_CTX sha;

    assert(BN_hex2bn(&N, srp_2048_prime) != 0);
    assert(g != NULL && a != NULL && A != NULL);
    assert(k == NULL && u == NULL && x == NULL && v != NULL && base != NULL);
    assert(exponent != NULL && S != NULL && ctx != NULL);
    BN_set_word(g, 2);
    salt = BN_bin2bn(salt_bytes, salt_len, NULL);
    B = BN_bin2bn(server_B, server_B_len, NULL);
    assert(salt != NULL && B != NULL);
    assert(BN_rand(a, 256, -1, 0) == 1);
    assert(BN_mod_exp(A, g, a, N, ctx) == 1);
    *client_A_len = BN_num_bytes(A);
    *client_A = malloc((size_t)*client_A_len);
    assert(*client_A != NULL);
    BN_bn2bin(A, *client_A);

    k = hash_padded_pair(N, g);
    u = hash_padded_pair(A, B);
    assert(k != NULL && u != NULL);

    SHA1_Init(&sha);
    SHA1_Update(&sha, username, strlen(username));
    SHA1_Update(&sha, ":", 1);
    SHA1_Update(&sha, pin, strlen(pin));
    SHA1_Final(inner_hash, &sha);
    salt_encoded_len = (unsigned int)BN_num_bytes(salt);
    BN_bn2bin(salt, salt_encoded);
    SHA1_Init(&sha);
    SHA1_Update(&sha, salt_encoded, salt_encoded_len);
    SHA1_Update(&sha, inner_hash, sizeof(inner_hash));
    SHA1_Final(x_hash, &sha);
    x = BN_bin2bn(x_hash, sizeof(x_hash), NULL);
    assert(x != NULL);

    assert(BN_mod_exp(v, g, x, N, ctx) == 1);
    assert(BN_mod_mul(base, k, v, N, ctx) == 1);
    assert(BN_mod_sub(base, B, base, N, ctx) == 1);
    assert(BN_mul(exponent, u, x, ctx) == 1);
    assert(BN_add(exponent, exponent, a) == 1);
    assert(BN_mod_exp(S, base, exponent, N, ctx) == 1);

    unsigned int S_len = (unsigned int)BN_num_bytes(S);
    unsigned char *S_encoded = malloc(S_len);
    assert(S_encoded != NULL);
    BN_bn2bin(S, S_encoded);
    unsigned char suffix[4] = {0};
    SHA1_Init(&sha);
    SHA1_Update(&sha, S_encoded, S_len);
    SHA1_Update(&sha, suffix, sizeof(suffix));
    SHA1_Final(digest0, &sha);
    suffix[3] = 1;
    SHA1_Init(&sha);
    SHA1_Update(&sha, S_encoded, S_len);
    SHA1_Update(&sha, suffix, sizeof(suffix));
    SHA1_Final(digest1, &sha);
    memcpy(key, digest0, sizeof(digest0));
    memcpy(key + sizeof(digest0), digest1, sizeof(digest1));
    free(S_encoded);

    hash_number(N, h_N);
    hash_number(g, h_g);
    SHA1((const unsigned char *)username, strlen(username), h_user);
    for (size_t i = 0; i < sizeof(h_xor); i++) h_xor[i] = h_N[i] ^ h_g[i];
    A_encoded_len = (unsigned int)BN_num_bytes(A);
    B_encoded_len = (unsigned int)BN_num_bytes(B);
    BN_bn2bin(A, A_encoded);
    BN_bn2bin(B, B_encoded);
    SHA1_Init(&sha);
    SHA1_Update(&sha, h_xor, sizeof(h_xor));
    SHA1_Update(&sha, h_user, sizeof(h_user));
    SHA1_Update(&sha, salt_encoded, salt_encoded_len);
    SHA1_Update(&sha, A_encoded, A_encoded_len);
    SHA1_Update(&sha, B_encoded, B_encoded_len);
    SHA1_Update(&sha, key, sizeof(key));
    SHA1_Final(proof, &sha);

    BN_free(N); BN_free(g); BN_free(salt); BN_free(B); BN_free(a); BN_free(A);
    BN_free(k); BN_free(u); BN_free(x); BN_free(v); BN_free(base); BN_free(exponent);
    BN_free(S); BN_CTX_free(ctx);
}

static void new_test_user(pairing_session_t *session, pairing_t *pairing,
                          const char *device_id, const char *pin,
                          const char **salt, int *salt_len, char **B, int *B_len) {
    /* Existing SRP rejects randomly generated salt/verifier values whose
     * big-endian representation has a leading zero byte. Retry only that
     * documented error, so repeated leak tests are not probabilistic. */
    for (int attempt = 0; attempt < 16; attempt++) {
        int result = srp_new_user(session, pairing, device_id, pin, salt, salt_len, B, B_len);
        if (result == 0) return;
        assert(result == -3);
        assert(*B == NULL && *B_len == 0);
    }
    assert(!"repeated short SRP salt/verifier generation");
}

static void test_malformed_proof_lengths_are_rejected(void) {
    static const int invalid_lengths[] = {0, 1, 19, 21, 64};

    for (size_t i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); i++) {
        const char *device_id = "AA:BB:CC:DD:EE:FF";
        const char *pin = "1234";
        int result = 0;
        pairing_t *pairing = pairing_init_generate(device_id, "", &result);
        pairing_session_t *session = pairing_session_init(pairing);
        const char *salt = NULL;
        char *B = NULL;
        int salt_len = 0;
        int B_len = 0;
        unsigned char *A = NULL;
        int len_A = 0;
        unsigned char proof[TEST_PROOF_SIZE + 44] = {0};

        assert(pairing != NULL);
        assert(result == 0);
        assert(session != NULL);
        new_test_user(session, pairing, device_id, pin, &salt, &salt_len, &B, &B_len);
        make_client_proof(device_id, pin, (const unsigned char *)salt, salt_len,
                          (const unsigned char *)B, B_len, &A, &len_A, proof);
        memcpy(proof + TEST_PROOF_SIZE, "guard", 5);
        int validation = srp_validate_proof(session, pairing, A, len_A, proof,
                                            invalid_lengths[i]);
        if (validation >= 0) {
            fprintf(stderr, "accepted SRP proof length %d (result %d)\n",
                    invalid_lengths[i], validation);
            exit(1);
        }
        free(A);
        free((void *)B);
        pairing_session_destroy(session);
        pairing_destroy(pairing);
    }
}

static void test_valid_srp_proof_returns_sha1_response(void) {
    const char *device_id = "AA:BB:CC:DD:EE:FF";
    const char *pin = "1234";
    int result = 0;
    pairing_t *pairing = pairing_init_generate(device_id, "", &result);
    pairing_session_t *session;
    const char *salt = NULL;
    char *B = NULL;
    int salt_len = 0;
    int B_len = 0;
    unsigned char *A = NULL;
    int len_A = 0;
    unsigned char proof_buffer[TEST_PROOF_SIZE + 16];

    assert(pairing != NULL);
    assert(result == 0);
    session = pairing_session_init(pairing);
    assert(session != NULL);
    new_test_user(session, pairing, device_id, pin, &salt, &salt_len, &B, &B_len);

    make_client_proof(device_id, pin, (const unsigned char *)salt, salt_len,
                      (const unsigned char *)B, B_len, &A, &len_A, proof_buffer);
    memset(proof_buffer + TEST_PROOF_SIZE, 0xA5,
           sizeof(proof_buffer) - TEST_PROOF_SIZE);
    int validation = srp_validate_proof(session, pairing, A, len_A,
                                         proof_buffer, TEST_PROOF_SIZE);
    if (validation != 0) {
        fprintf(stderr, "valid SRP proof rejected: %d\n", validation);
        exit(1);
    }
    for (size_t i = TEST_PROOF_SIZE; i < sizeof(proof_buffer); i++) {
        assert(proof_buffer[i] == 0xA5);
    }

    free(A);
    free((void *)B);
    pairing_session_destroy(session);
    pairing_destroy(pairing);
}

static void test_out_of_order_proof_is_rejected(void) {
    int result = 0;
    pairing_t *pairing = pairing_init_generate("AA:BB:CC:DD:EE:FF", "",
                                               &result);
    pairing_session_t *session = pairing_session_init(pairing);
    unsigned char A[1] = {1};
    unsigned char proof[TEST_PROOF_SIZE] = {0};

    assert(pairing != NULL);
    assert(session != NULL);
    assert(srp_validate_proof(session, pairing, A, sizeof(A), proof,
                              sizeof(proof)) < 0);
    pairing_session_destroy(session);
    pairing_destroy(pairing);
}

int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    test_malformed_proof_lengths_are_rejected();
    test_valid_srp_proof_returns_sha1_response();
    test_out_of_order_proof_is_rejected();
    fprintf(stderr, "pairing regression tests passed\n");
    return 0;
}
