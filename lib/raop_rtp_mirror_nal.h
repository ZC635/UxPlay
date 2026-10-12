#ifndef RAOP_RTP_MIRROR_NAL_H
#define RAOP_RTP_MIRROR_NAL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "byteutils.h"

/* Private NAL iterator shared by the mirror receiver and its boundary tests.
 * On success, nalu_offset points to the first byte of a non-empty NAL unit. */
static inline bool
raop_rtp_mirror_next_nal(unsigned char *payload, int payload_size,
                         int *nalu_offset, int *nalu_length)
{
    static const unsigned char nal_start_code[4] = {0x00, 0x00, 0x00, 0x01};
    int remaining;
    uint32_t length;

    if (!payload || !nalu_offset || !nalu_length || payload_size < 0 ||
        *nalu_offset < 0 || *nalu_offset > payload_size) {
        return false;
    }

    remaining = payload_size - *nalu_offset;
    if (remaining < 4) {
        return false;
    }

    length = byteutils_get_int_be(payload, *nalu_offset);
    if (length == 0 || length > (uint32_t)(remaining - 4)) {
        return false;
    }

    memcpy(payload + *nalu_offset, nal_start_code, sizeof(nal_start_code));
    *nalu_offset += 4;
    *nalu_length = (int)length;
    return true;
}

#endif
