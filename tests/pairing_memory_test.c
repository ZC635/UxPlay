/* GNU ld wrapping accounts for allocations made by the static airplay library.
 * OpenSSL hooks cover its DLL allocations too; no RSS thresholds or sleeps. */
#include <openssl/crypto.h>
#include <pthread.h>
#define main pairing_behavior_main
#include "pairing_regression_test.c"
#undef main

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void __real_free(void *);
static pthread_mutex_t allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct allocation_s { void *ptr; struct allocation_s *next; } allocation_t;
static allocation_t *allocations;
static size_t outstanding;

static void remember(void *ptr) {
    if (!ptr) return;
    allocation_t *entry = __real_malloc(sizeof(*entry));
    assert(entry);
    entry->ptr = ptr;
    pthread_mutex_lock(&allocation_mutex);
    entry->next = allocations;
    allocations = entry;
    outstanding++;
    pthread_mutex_unlock(&allocation_mutex);
}
static void forget(void *ptr) {
    pthread_mutex_lock(&allocation_mutex);
    allocation_t **entry = &allocations;
    while (*entry && (*entry)->ptr != ptr) entry = &(*entry)->next;
    if (*entry) {
        allocation_t *old = *entry;
        *entry = old->next;
        outstanding--;
        __real_free(old);
    }
    pthread_mutex_unlock(&allocation_mutex);
}
void *__wrap_malloc(size_t size) { void *p = __real_malloc(size); remember(p); return p; }
void *__wrap_calloc(size_t n, size_t size) { void *p = __real_calloc(n, size); remember(p); return p; }
void __wrap_free(void *ptr) { forget(ptr); __real_free(ptr); }
static void *crypto_malloc(size_t n, const char *file, int line) {
    (void)file; (void)line; return __wrap_malloc(n);
}
static void crypto_free(void *ptr, const char *file, int line) {
    (void)file; (void)line; __wrap_free(ptr);
}
static void *crypto_realloc(void *ptr, size_t n, const char *file, int line) {
    (void)file; (void)line;
    if (!n) { __wrap_free(ptr); return NULL; }
    forget(ptr);
    void *p = realloc(ptr, n);
    remember(p ? p : ptr);
    return p;
}

static void cycle(void) {
    test_malformed_proof_lengths_are_rejected();
    test_valid_srp_proof_returns_sha1_response();
    test_out_of_order_proof_is_rejected();
    unsigned char key[32] = {0};
    char encoded[45] = {0};
    pk_to_base64(key, sizeof(key), encoded, sizeof(encoded));
    assert(strcmp(encoded, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=") == 0);
}
int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    assert(CRYPTO_set_mem_functions(crypto_malloc, crypto_realloc, crypto_free));
    cycle(); /* Warm OpenSSL providers, caches and RNG before measuring. */
    size_t before = outstanding;
    for (int i = 0; i < 5; i++) cycle();
    if (outstanding != before) {
        fprintf(stderr, "FAIL: repeated SRP/base64 operations retained %lld allocations\n",
                (long long)outstanding - (long long)before);
        return 1;
    }
    OPENSSL_cleanup();
    puts("SRP and base64 allocation balance passed");
    return 0;
}
