#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*gip_auth_send_fn)(void *ctx, const void *data, size_t len,
                                bool acknowledge);

struct gip_auth {
    uint8_t last_cmd;
    uint8_t random_host[32], random_client[32];
    /* The PDP returns a complete X.509 certificate (815 bytes in the
     * Windows trace), not a standalone SubjectPublicKeyInfo blob. */
    uint8_t client_certificate[1024];
    size_t client_certificate_len;
    uint8_t client_pubkey[270];
    uint8_t client_pubkey2[64];
    uint8_t master_secret[48];
    uint8_t transcript[32];
    uint8_t transcript_buf[8192];
    size_t transcript_len;
    bool started;
    bool authenticated;
    void *send_ctx;
    gip_auth_send_fn send;
};

void gip_auth_init(struct gip_auth *a, gip_auth_send_fn send, void *ctx);
int gip_auth_start(struct gip_auth *a);
int gip_auth_process(struct gip_auth *a, const uint8_t *data, size_t len);
int gip_auth_complete_without_client_finish(struct gip_auth *a);
