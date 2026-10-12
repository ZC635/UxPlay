#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../lib/raop_rtp_mirror_nal.h"

static int failures;

static void
check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void
test_truncated_length_prefixes_are_rejected(void)
{
    static const unsigned char input[] = {0x00, 0x00, 0x00};

    for (int size = 1; size <= 3; size++) {
#ifdef _WIN32
        SYSTEM_INFO system_info;
        unsigned char *allocation;
        unsigned char *payload;
        DWORD previous_protection;

        GetSystemInfo(&system_info);
        allocation = VirtualAlloc(NULL, 2 * system_info.dwPageSize,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(allocation != NULL, "allocate guarded truncated-prefix pages");
        if (!allocation) {
            return;
        }
        check(VirtualProtect(allocation + system_info.dwPageSize,
                             system_info.dwPageSize, PAGE_NOACCESS,
                             &previous_protection) != 0,
              "protect the page after a truncated prefix");
        payload = allocation + system_info.dwPageSize - size;
#else
        unsigned char *payload = malloc((size_t)size);
#endif
        int offset = 0;
        int nal_length = 123;

#ifndef _WIN32
        check(payload != NULL, "allocate an exact truncated-prefix buffer");
        if (!payload) {
            return;
        }
#endif
        memcpy(payload, input, (size_t)size);
        check(!raop_rtp_mirror_next_nal(payload, size, &offset, &nal_length),
              "reject a truncated NAL length prefix");
        check(offset == 0, "leave the cursor unchanged for a truncated prefix");
        check(nal_length == 123,
              "leave the output length unchanged for a truncated prefix");
#ifdef _WIN32
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        free(payload);
#endif
    }
}

static void
test_zero_length_unit_at_payload_end_is_rejected(void)
{
    unsigned char payload[5] = {0x00, 0x00, 0x00, 0x00, 0xa5};
    int offset = 0;
    int nal_length = 123;

    check(!raop_rtp_mirror_next_nal(payload, 4, &offset, &nal_length),
          "reject a zero-length NAL unit at the payload boundary");
    check(offset == 0, "leave the cursor unchanged for a zero-length unit");
    check(nal_length == 123,
          "leave the output length unchanged for a zero-length unit");
    check(payload[4] == 0xa5,
          "do not read or write beyond a zero-length unit");
}

static void
test_oversized_unit_is_rejected_without_mutating_prefix(void)
{
    unsigned char payload[] = {0x00, 0x00, 0x00, 0x02, 0x65};
    const unsigned char original[] = {0x00, 0x00, 0x00, 0x02, 0x65};
    int offset = 0;
    int nal_length = 123;

    check(!raop_rtp_mirror_next_nal(payload, (int)sizeof(payload),
                                     &offset, &nal_length),
          "reject a NAL length larger than the remaining payload");
    check(offset == 0, "leave the cursor unchanged for an oversized unit");
    check(nal_length == 123,
          "leave the output length unchanged for an oversized unit");
    check(memcmp(payload, original, sizeof(payload)) == 0,
          "do not replace an oversized unit prefix with a start code");
}

static void
test_exact_h264_boundary_is_accepted(void)
{
    unsigned char payload[] = {0x00, 0x00, 0x00, 0x01, 0x65};
    static const unsigned char start_code[] = {0x00, 0x00, 0x00, 0x01};
    int offset = 0;
    int nal_length = 0;

    check(raop_rtp_mirror_next_nal(payload, (int)sizeof(payload),
                                    &offset, &nal_length),
          "accept an H.264 unit ending exactly at the payload boundary");
    check(offset == 4 && nal_length == 1,
          "return the H.264 header offset and payload length");
    check(memcmp(payload, start_code, sizeof(start_code)) == 0,
          "replace the H.264 length prefix with an Annex B start code");
    check(payload[offset] == 0x65,
          "preserve the H.264 IDR NAL header");
    offset += nal_length;
    check(offset == (int)sizeof(payload),
          "consume the exact H.264 payload boundary");
}

static void
test_exact_h265_boundary_is_accepted(void)
{
    unsigned char payload[] = {0x00, 0x00, 0x00, 0x02, 0x26, 0x01};
    static const unsigned char start_code[] = {0x00, 0x00, 0x00, 0x01};
    int offset = 0;
    int nal_length = 0;

    check(raop_rtp_mirror_next_nal(payload, (int)sizeof(payload),
                                    &offset, &nal_length),
          "accept an H.265 unit ending exactly at the payload boundary");
    check(offset == 4 && nal_length == 2,
          "return the H.265 header offset and payload length");
    check(memcmp(payload, start_code, sizeof(start_code)) == 0,
          "replace the H.265 length prefix with an Annex B start code");
    check(payload[offset] == 0x26 && payload[offset + 1] == 0x01,
          "preserve the two-byte H.265 NAL header");
    offset += nal_length;
    check(offset == (int)sizeof(payload),
          "consume the exact H.265 payload boundary");
}

int
main(void)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    test_truncated_length_prefixes_are_rejected();
    test_zero_length_unit_at_payload_end_is_rejected();
    test_oversized_unit_is_rejected_without_mutating_prefix();
    test_exact_h264_boundary_is_accepted();
    test_exact_h265_boundary_is_accepted();

    if (failures) {
        fprintf(stderr, "%d mirror NAL regression assertion(s) failed\n",
                failures);
        return 1;
    }
    puts("mirror NAL regression tests passed");
    return 0;
}
