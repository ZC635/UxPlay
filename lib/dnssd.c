/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2022
 */

/* These defines allow us to compile on iOS */
#ifndef __has_feature
# define __has_feature(x) 0
#endif
#ifndef __has_extension
# define __has_extension __has_feature
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compat.h"
#if !defined(_WIN32)
#include <dns_sd.h>
#endif
#include "dnssd.h"

#include "dnssdint.h"
#include "global.h"
#include "utils.h"

#define MAX_DEVICEID 18
#define MAX_SERVNAME 256

#if defined(HAVE_LIBDL) && !defined(__APPLE__)
# define USE_LIBDL 1
#else
# define USE_LIBDL 0
#endif

#if defined(_WIN32) || USE_LIBDL
# ifdef _WIN32
#  include <stdint.h>
#  if !defined(EFI32) && !defined(EFI64)
#   define DNSSD_STDCALL __stdcall
#  else
#   define DNSSD_STDCALL
#  endif
# else
#  include <dlfcn.h>
#  define DNSSD_STDCALL
# endif

typedef struct _DNSServiceRef_t *DNSServiceRef;
#ifdef _WIN32
typedef struct { uint8_t *buffer; uint16_t length; size_t capacity; } TXTRecordRef;
#else
typedef union _TXTRecordRef_t { char PrivateData[16]; char *ForceNaturalAlignment; } TXTRecordRef;
#endif
typedef uint32_t DNSServiceFlags;
typedef int32_t  DNSServiceErrorType;

typedef void (DNSSD_STDCALL *DNSServiceRegisterReply)
    (
    DNSServiceRef                       sdRef,
    DNSServiceFlags                     flags,
    DNSServiceErrorType                 errorCode,
    const char                          *name,
    const char                          *regtype,
    const char                          *domain,
    void                                *context
    );

#else
//# include <dns_sd.h>
# define DNSSD_STDCALL
#endif

typedef DNSServiceErrorType (DNSSD_STDCALL *DNSServiceRegister_t)
        (
                DNSServiceRef                       *sdRef,
                DNSServiceFlags                     flags,
                uint32_t                            interfaceIndex,
                const char                          *name,
                const char                          *regtype,
                const char                          *domain,
                const char                          *host,
                uint16_t                            port,
                uint16_t                            txtLen,
                const void                          *txtRecord,
                DNSServiceRegisterReply             callBack,
                void                                *context
        );
typedef void (DNSSD_STDCALL *DNSServiceRefDeallocate_t)(DNSServiceRef sdRef);
typedef void (DNSSD_STDCALL *TXTRecordCreate_t)
        (
                TXTRecordRef     *txtRecord,
                uint16_t         bufferLen,
                void             *buffer
        );
typedef void (DNSSD_STDCALL *TXTRecordDeallocate_t)(TXTRecordRef *txtRecord);
typedef DNSServiceErrorType (DNSSD_STDCALL *TXTRecordSetValue_t)
        (
                TXTRecordRef     *txtRecord,
                const char       *key,
                uint8_t          valueSize,
                const void       *value
        );
typedef uint16_t (DNSSD_STDCALL *TXTRecordGetLength_t)(const TXTRecordRef *txtRecord);
typedef const void * (DNSSD_STDCALL *TXTRecordGetBytesPtr_t)(const TXTRecordRef *txtRecord);

#ifdef WIN32
static void DNSSD_STDCALL dnssd_win_txt_record_create(TXTRecordRef *txtRecord, uint16_t bufferLen, void *buffer) {
    (void)bufferLen;
    (void)buffer;
    memset(txtRecord, 0, sizeof(*txtRecord));
}

static void DNSSD_STDCALL dnssd_win_txt_record_deallocate(TXTRecordRef *txtRecord) {
    if (txtRecord) {
        free(txtRecord->buffer);
        memset(txtRecord, 0, sizeof(*txtRecord));
    }
}

static DNSServiceErrorType DNSSD_STDCALL dnssd_win_txt_record_set_value(
        TXTRecordRef *txtRecord, const char *key, uint8_t valueSize, const void *value) {
    if (!txtRecord || !key) return -1;

    size_t key_len = strlen(key);
    if (key_len + 1 + valueSize > 255) return -1;

    size_t entry_size = 1 + key_len + 1 + valueSize;
    uint8_t *entry = (uint8_t *)malloc(entry_size);
    if (!entry) return -1;

    entry[0] = (uint8_t)(key_len + 1 + valueSize);
    memcpy(entry + 1, key, key_len);
    entry[1 + key_len] = '=';
    if (valueSize > 0 && value) {
        memcpy(entry + 1 + key_len + 1, value, valueSize);
    }

    uint8_t *read = txtRecord->buffer;
    uint8_t *write = txtRecord->buffer;
    uint8_t *end = txtRecord->buffer + txtRecord->length;
    while (read < end) {
        uint8_t entry_len = *read;
        if (entry_len == 0 || read + 1 + entry_len > end) break;

        int is_same_key = 0;
        const uint8_t *equals = (const uint8_t *)memchr(read + 1, '=', entry_len);
        if (equals) {
            size_t existing_key_len = (size_t)(equals - (read + 1));
            if (existing_key_len == key_len && memcmp(read + 1, key, key_len) == 0) {
                is_same_key = 1;
            }
        }

        if (!is_same_key) {
            if (write != read) {
                memmove(write, read, 1 + entry_len);
            }
            write += 1 + entry_len;
        }
        read += 1 + entry_len;
    }
    txtRecord->length = (uint16_t)(write - txtRecord->buffer);

    size_t new_len = txtRecord->length + entry_size;
    if (new_len > UINT16_MAX) {
        free(entry);
        return -1;
    }
    if (new_len > txtRecord->capacity) {
        size_t new_cap = txtRecord->capacity ? txtRecord->capacity * 2 : 256;
        while (new_cap < new_len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)realloc(txtRecord->buffer, new_cap);
        if (!new_buf) {
            free(entry);
            return -1;
        }
        txtRecord->buffer = new_buf;
        txtRecord->capacity = new_cap;
    }

    memcpy(txtRecord->buffer + txtRecord->length, entry, entry_size);
    txtRecord->length = (uint16_t)new_len;
    free(entry);
    return 0;
}

static uint16_t DNSSD_STDCALL dnssd_win_txt_record_get_length(const TXTRecordRef *txtRecord) {
    return txtRecord ? txtRecord->length : 0;
}

static const void * DNSSD_STDCALL dnssd_win_txt_record_get_bytes_ptr(const TXTRecordRef *txtRecord) {
    return txtRecord ? txtRecord->buffer : NULL;
}

static DNSServiceErrorType DNSSD_STDCALL dnssd_win_service_register(
        DNSServiceRef *sdRef,
        DNSServiceFlags flags,
        uint32_t interfaceIndex,
        const char *name,
        const char *regtype,
        const char *domain,
        const char *host,
        uint16_t port,
        uint16_t txtLen,
        const void *txtRecord,
        DNSServiceRegisterReply callBack,
        void *context) {
    (void)flags; (void)interfaceIndex; (void)name; (void)regtype; (void)domain;
    (void)host; (void)port; (void)txtLen; (void)txtRecord; (void)callBack; (void)context;
    if (sdRef) *sdRef = (DNSServiceRef)1;
    return 0;
}

static void DNSSD_STDCALL dnssd_win_service_ref_deallocate(DNSServiceRef sdRef) {
    (void)sdRef;
}
#endif

struct dnssd_s {
#ifdef WIN32
    HMODULE module;
#elif USE_LIBDL
    void *module;
#endif

    DNSServiceRegister_t       DNSServiceRegister;
    DNSServiceRefDeallocate_t  DNSServiceRefDeallocate;
    TXTRecordCreate_t          TXTRecordCreate;
    TXTRecordSetValue_t        TXTRecordSetValue;
    TXTRecordGetLength_t       TXTRecordGetLength;
    TXTRecordGetBytesPtr_t     TXTRecordGetBytesPtr;
    TXTRecordDeallocate_t      TXTRecordDeallocate;

    TXTRecordRef raop_record;
    TXTRecordRef airplay_record;

    DNSServiceRef raop_service;
    DNSServiceRef airplay_service;

    char *name;
    int name_len;

    char *hw_addr;
    int hw_addr_len;

    char *pk;

    uint32_t features1;
    uint32_t features2;

    unsigned char pin_pw;
};



dnssd_t *
dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len, int *error, unsigned char pin_pw)
{
    /* pin_pw = 0: no pin or password
                1: use onscreen pin for client access control
                2 or 3: require password for client access control  
    */
    
    if (error) *error = DNSSD_ERROR_NOERROR;

    dnssd_t *dnssd = (dnssd_t *) calloc(1, sizeof(dnssd_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    dnssd->pin_pw = pin_pw;

    char *end = NULL;
    unsigned long features  = strtoul(FEATURES_1, &end, 16);
    if (!end || (features & 0xFFFFFFFF) != features) {
        free (dnssd);
        if (error) *error = DNSSD_ERROR_BADFEATURES;
        return NULL;
    } 
    dnssd->features1 = (uint32_t) features;

    features  = strtoul(FEATURES_2, &end, 16);
    if (!end || (features & 0xFFFFFFFF) != features) {
        free (dnssd);
        if (error) *error = DNSSD_ERROR_BADFEATURES;
        return NULL;
    } 
    dnssd->features2 = (uint32_t) features;

#ifdef WIN32
    dnssd->DNSServiceRegister = (DNSServiceRegister_t)dnssd_win_service_register;
    dnssd->DNSServiceRefDeallocate = (DNSServiceRefDeallocate_t)dnssd_win_service_ref_deallocate;
    dnssd->TXTRecordCreate = (TXTRecordCreate_t)dnssd_win_txt_record_create;
    dnssd->TXTRecordSetValue = (TXTRecordSetValue_t)dnssd_win_txt_record_set_value;
    dnssd->TXTRecordGetLength = (TXTRecordGetLength_t)dnssd_win_txt_record_get_length;
    dnssd->TXTRecordGetBytesPtr = (TXTRecordGetBytesPtr_t)dnssd_win_txt_record_get_bytes_ptr;
    dnssd->TXTRecordDeallocate = (TXTRecordDeallocate_t)dnssd_win_txt_record_deallocate;
#elif USE_LIBDL
    dnssd->module = dlopen("libdns_sd.so", RTLD_LAZY);
	if (!dnssd->module) {
		if (error) *error = DNSSD_ERROR_LIBNOTFOUND;
		free(dnssd);
		return NULL;
	}
	dnssd->DNSServiceRegister = (DNSServiceRegister_t)dlsym(dnssd->module, "DNSServiceRegister");
	dnssd->DNSServiceRefDeallocate = (DNSServiceRefDeallocate_t)dlsym(dnssd->module, "DNSServiceRefDeallocate");
	dnssd->TXTRecordCreate = (TXTRecordCreate_t)dlsym(dnssd->module, "TXTRecordCreate");
	dnssd->TXTRecordSetValue = (TXTRecordSetValue_t)dlsym(dnssd->module, "TXTRecordSetValue");
	dnssd->TXTRecordGetLength = (TXTRecordGetLength_t)dlsym(dnssd->module, "TXTRecordGetLength");
	dnssd->TXTRecordGetBytesPtr = (TXTRecordGetBytesPtr_t)dlsym(dnssd->module, "TXTRecordGetBytesPtr");
	dnssd->TXTRecordDeallocate = (TXTRecordDeallocate_t)dlsym(dnssd->module, "TXTRecordDeallocate");

	if (!dnssd->DNSServiceRegister || !dnssd->DNSServiceRefDeallocate || !dnssd->TXTRecordCreate ||
	    !dnssd->TXTRecordSetValue || !dnssd->TXTRecordGetLength || !dnssd->TXTRecordGetBytesPtr ||
	    !dnssd->TXTRecordDeallocate) {
		if (error) *error = DNSSD_ERROR_PROCNOTFOUND;
		dlclose(dnssd->module);
		free(dnssd);
		return NULL;
	}
#else
    dnssd->DNSServiceRegister = &DNSServiceRegister;
    dnssd->DNSServiceRefDeallocate = &DNSServiceRefDeallocate;
    dnssd->TXTRecordCreate = &TXTRecordCreate;
    dnssd->TXTRecordSetValue = &TXTRecordSetValue;
    dnssd->TXTRecordGetLength = &TXTRecordGetLength;
    dnssd->TXTRecordGetBytesPtr = &TXTRecordGetBytesPtr;
    dnssd->TXTRecordDeallocate = &TXTRecordDeallocate;
#endif

    dnssd->name_len = name_len;
    dnssd->name = calloc(1, name_len + 1);
    if (!dnssd->name) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->name, name, name_len);

    dnssd->hw_addr_len = hw_addr_len;
    dnssd->hw_addr = calloc(1, dnssd->hw_addr_len);
    if (!dnssd->hw_addr) {
        free(dnssd->name);
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    memcpy(dnssd->hw_addr, hw_addr, hw_addr_len);

    return dnssd;
}

void
dnssd_destroy(dnssd_t *dnssd)
{
    if (dnssd) {
        if (dnssd->raop_service) {
            dnssd->TXTRecordDeallocate(&dnssd->raop_record);
            dnssd->DNSServiceRefDeallocate(dnssd->raop_service);
        }
        if (dnssd->airplay_service) {
            dnssd->TXTRecordDeallocate(&dnssd->airplay_record);
            dnssd->DNSServiceRefDeallocate(dnssd->airplay_service);
        }
#if USE_LIBDL
        dlclose(dnssd->module);
#endif
        free(dnssd->name);
        free(dnssd->hw_addr);
        free(dnssd);
    }
}

int
dnssd_register_raop(dnssd_t *dnssd, unsigned short port)
{
    char servname[MAX_SERVNAME];

    char features[22] = {0};

    assert(dnssd);

    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd->features1, dnssd->features2);

    dnssd->TXTRecordCreate(&dnssd->raop_record, 0, NULL);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "ch", strlen(RAOP_CH), RAOP_CH);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "cn", strlen(RAOP_CN), RAOP_CN);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "da", strlen(RAOP_DA), RAOP_DA);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "et", strlen(RAOP_ET), RAOP_ET);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "vv", strlen(RAOP_VV), RAOP_VV);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "ft", strlen(features), features);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "am", strlen(GLOBAL_MODEL), GLOBAL_MODEL);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "md", strlen(RAOP_MD), RAOP_MD);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "rhd", strlen(RAOP_RHD), RAOP_RHD);
    switch (dnssd->pin_pw) {
    case 2:
    case 3:
        dnssd->TXTRecordSetValue(&dnssd->raop_record, "pw", strlen("true"), "true");
        dnssd->TXTRecordSetValue(&dnssd->raop_record, "sf", 4, "0x84");
        break;
    case 1:
        dnssd->TXTRecordSetValue(&dnssd->raop_record, "pw", strlen("true"), "true");
        dnssd->TXTRecordSetValue(&dnssd->raop_record, "sf", 3, "0x8c");
        break;
    default:
        dnssd->TXTRecordSetValue(&dnssd->raop_record, "pw", strlen("false"), "false");
        dnssd->TXTRecordSetValue(&dnssd->raop_record, "sf", strlen(RAOP_SF), RAOP_SF);
        break;
    }
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "sr", strlen(RAOP_SR), RAOP_SR);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "ss", strlen(RAOP_SS), RAOP_SS);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "sv", strlen(RAOP_SV), RAOP_SV);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "tp", strlen(RAOP_TP), RAOP_TP);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "txtvers", strlen(RAOP_TXTVERS), RAOP_TXTVERS);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "sf", strlen(RAOP_SF), RAOP_SF);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "vs", strlen(RAOP_VS), RAOP_VS);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "vn", strlen(RAOP_VN), RAOP_VN);
    dnssd->TXTRecordSetValue(&dnssd->raop_record, "pk", strlen(dnssd->pk), dnssd->pk);

    /* Convert hardware address to string */
    if (utils_hwaddr_raop(servname, sizeof(servname), dnssd->hw_addr, dnssd->hw_addr_len) < 0) {
        /* FIXME: handle better */
        return -1;
    }

    /* Check that we have bytes for 'hw@name' format */
    if (sizeof(servname) < strlen(servname) + 1 + dnssd->name_len + 1) {
        /* FIXME: handle better */
        return -2;
    }

    strncat(servname, "@", sizeof(servname)-strlen(servname)-1);
    strncat(servname, dnssd->name, sizeof(servname)-strlen(servname)-1);

    /* Register the service */
    DNSServiceErrorType retval = dnssd->DNSServiceRegister(&dnssd->raop_service, 0, 0,
                                                          servname, "_raop._tcp",
                                                          NULL, NULL,
                                                          htons(port),
                                                          dnssd->TXTRecordGetLength(&dnssd->raop_record),
                                                          dnssd->TXTRecordGetBytesPtr(&dnssd->raop_record),
                                                          NULL, NULL);

    return (int) retval;   /* error codes are listed in Apple's dns_sd.h */
}

int
dnssd_register_airplay(dnssd_t *dnssd, unsigned short port)
{
    char device_id[3 * MAX_HWADDR_LEN];
    char features[22] = {0};

    assert(dnssd);

    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd->features1, dnssd->features2);

    /* Convert hardware address to string */
    if (utils_hwaddr_airplay(device_id, sizeof(device_id), dnssd->hw_addr, dnssd->hw_addr_len) < 0) {
        /* FIXME: handle better */
        return -1;
    }

    // flags is a string representing a 20-bit flag (up to 3 hex digits)
    dnssd->TXTRecordCreate(&dnssd->airplay_record, 0, NULL);
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "deviceid", strlen(device_id), device_id);
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "features", strlen(features), features);
    switch (dnssd->pin_pw) {
    case 1:   // display onscreen pin
        dnssd->TXTRecordSetValue(&dnssd->airplay_record, "pw", strlen("true"), "true");
        dnssd->TXTRecordSetValue(&dnssd->airplay_record, "flags", 3, "0x4");
        break;  
    case 2:  // require password
    case 3:
        dnssd->TXTRecordSetValue(&dnssd->airplay_record, "pw", strlen("true"), "true");
        dnssd->TXTRecordSetValue(&dnssd->airplay_record, "flags", 3, "0x4");
        break;
    default:
        dnssd->TXTRecordSetValue(&dnssd->airplay_record, "pw", strlen("false"), "false");
        dnssd->TXTRecordSetValue(&dnssd->airplay_record, "flags", 3, "0x4");
        break;
    }
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "model", strlen(GLOBAL_MODEL), GLOBAL_MODEL);
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "pk", strlen(dnssd->pk), dnssd->pk);
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "pi", strlen(AIRPLAY_PI), AIRPLAY_PI);
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "srcvers", strlen(AIRPLAY_SRCVERS), AIRPLAY_SRCVERS);
    dnssd->TXTRecordSetValue(&dnssd->airplay_record, "vv", strlen(AIRPLAY_VV), AIRPLAY_VV);

    /* Register the service */
    DNSServiceErrorType retval = dnssd->DNSServiceRegister(&dnssd->airplay_service, 0, 0,
                                                           dnssd->name, "_airplay._tcp",
                                                           NULL, NULL,
                                                           htons(port),
                                                           dnssd->TXTRecordGetLength(&dnssd->airplay_record),
                                                           dnssd->TXTRecordGetBytesPtr(&dnssd->airplay_record),
                                                           NULL, NULL);

    return (int) retval;   /* error codes are listed in Apple's dns_sd.h */
}

const char *
dnssd_get_raop_txt(dnssd_t *dnssd, int *length)
{
    *length = dnssd->TXTRecordGetLength(&dnssd->raop_record);
    return dnssd->TXTRecordGetBytesPtr(&dnssd->raop_record);
}

const char *
dnssd_get_airplay_txt(dnssd_t *dnssd, int *length)
{
    *length = dnssd->TXTRecordGetLength(&dnssd->airplay_record);
    return dnssd->TXTRecordGetBytesPtr(&dnssd->airplay_record);
}

const char *
dnssd_get_name(dnssd_t *dnssd, int *length)
{
    *length = dnssd->name_len;
    return dnssd->name;
}

const char *
dnssd_get_hw_addr(dnssd_t *dnssd, int *length)
{
    *length = dnssd->hw_addr_len;
    return dnssd->hw_addr;
}

void
dnssd_unregister_raop(dnssd_t *dnssd)
{
    assert(dnssd);

    if (!dnssd->raop_service) {
        return;
    }

    /* Deallocate TXT record */
    dnssd->TXTRecordDeallocate(&dnssd->raop_record);

    dnssd->DNSServiceRefDeallocate(dnssd->raop_service);
    dnssd->raop_service = NULL;

    if (dnssd->airplay_service == NULL) {
        free(dnssd->name);
        dnssd->name = NULL;
        free(dnssd->hw_addr);
        dnssd->hw_addr = NULL;
    }
}

void
dnssd_unregister_airplay(dnssd_t *dnssd)
{
    assert(dnssd);

    if (!dnssd->airplay_service) {
        return;
    }

    /* Deallocate TXT record */
    dnssd->TXTRecordDeallocate(&dnssd->airplay_record);

    dnssd->DNSServiceRefDeallocate(dnssd->airplay_service);
    dnssd->airplay_service = NULL;

    if (dnssd->raop_service == NULL) {
        free(dnssd->name);
        dnssd->name = NULL;
        free(dnssd->hw_addr);
        dnssd->hw_addr = NULL;
    }
}

uint64_t dnssd_get_airplay_features(dnssd_t *dnssd) {
    uint64_t features = ((uint64_t) dnssd->features2) << 32;
    features += (uint64_t) dnssd->features1;
    return features;
}

void dnssd_set_pk(dnssd_t *dnssd, char * pk_str) {
    dnssd->pk = pk_str;
}

void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val) {
    uint32_t mask = 0;
    uint32_t *features = 0;
    if (bit < 0 || bit > 63) return;
    if (val < 0 || val > 1) return;
    if (bit >= 32) {
        mask = 0x1 << (bit - 32);
        features = &(dnssd->features2);
    } else {
        mask = 0x1 << bit;
        features = &(dnssd->features1);
    }
    if (val) {
        *features = *features | mask;
    } else {
        *features = *features & ~mask;
    }
}

int
dnssd_uses_external_runtime(void)
{
#ifdef _WIN32
    return 0;
#else
    return 1;
#endif
}
