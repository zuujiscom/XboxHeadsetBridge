#include "gip_auth.h"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <string.h>
#include <stdio.h>

#define TRAILER 8

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t rdbe16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }

static int prf(const uint8_t *key, size_t klen, const char *label,
               const uint8_t *seed, size_t slen, uint8_t *out, size_t olen)
{
    uint8_t h[32], block[32]; unsigned n = 0;
    HMAC_CTX *c = HMAC_CTX_new(); if (!c) return -1;
    HMAC_Init_ex(c, key, (int)klen, EVP_sha256(), NULL);
    HMAC_Update(c, (const unsigned char *)label, strlen(label));
    HMAC_Update(c, seed, slen); HMAC_Final(c, h, &n);
    while (olen) {
        HMAC_Init_ex(c, key, (int)klen, EVP_sha256(), NULL);
        HMAC_Update(c, h, 32); HMAC_Update(c, (const unsigned char *)label, strlen(label));
        HMAC_Update(c, seed, slen); HMAC_Final(c, block, &n);
        size_t take = olen < 32 ? olen : 32; memcpy(out, block, take);
        out += take; olen -= take;
        SHA256(h, 32, h);
    }
    HMAC_CTX_free(c); return 0;
}

static int transcript_update(struct gip_auth *a, const uint8_t *p, size_t n)
{
    if (a->transcript_len + n > sizeof(a->transcript_buf)) return -1;
    memcpy(a->transcript_buf + a->transcript_len, p, n);
    a->transcript_len += n;
    SHA256(a->transcript_buf, a->transcript_len, a->transcript);
    return 0;
}

static int send_full(struct gip_auth *a, uint8_t cmd, const uint8_t *body, size_t body_len)
{
    uint8_t pkt[4096]; size_t n = 10 + body_len + TRAILER;
    if (n > sizeof(pkt)) return -1;
    pkt[0] = 0; pkt[1] = 0x41; pkt[2] = 0; pkt[3] = cmd; be16(pkt + 4, (uint16_t)(body_len + 4));
    pkt[6] = cmd; pkt[7] = 1; be16(pkt + 8, (uint16_t)body_len);
    memcpy(pkt + 10, body, body_len); memset(pkt + 10 + body_len, 0, TRAILER);
    a->last_cmd = cmd; transcript_update(a, pkt + 6, body_len + 4);
    return a->send(a->send_ctx, pkt, n, true);
}

static int request(struct gip_auth *a, uint8_t cmd, uint16_t len)
{
	uint8_t p[14] = {0};
	fprintf(stderr, "  --> AUTH request 0x%02x (%u bytes)\n", cmd, len);
	p[0] = 0; p[1] = 0x42; p[2] = 0; p[3] = cmd; be16(p + 4, (uint16_t)(len + 4));
	a->last_cmd = cmd;
	return a->send(a->send_ctx, p, sizeof(p), true);
}

int gip_auth_complete_without_client_finish(struct gip_auth *a)
{
    uint8_t complete[2] = {1, 0};
    if (a->authenticated || a->last_cmd != 0x08)
        return 0;
    fprintf(stderr, "  <-- AUTH request 0x08 ACK; completing authentication\n");
    int ret = a->send(a->send_ctx, complete, sizeof(complete), false);
    if (!ret)
        a->authenticated = true;
    return ret;
}

void gip_auth_init(struct gip_auth *a, gip_auth_send_fn send, void *ctx)
{
    memset(a, 0, sizeof(*a)); a->send = send; a->send_ctx = ctx;
    RAND_bytes(a->random_host, sizeof(a->random_host));
    SHA256(NULL, 0, a->transcript);
}

int gip_auth_start(struct gip_auth *a)
{
    /* xone's v1 host hello is 58 bytes total: 10-byte full header,
     * 32-byte nonce, 8 bytes of device-reserved data, and 8-byte trailer. */
    uint8_t body[40] = {0};
    memcpy(body, a->random_host, 32);
    a->started = true;
    fprintf(stderr, "  --> AUTHENTICATE host hello (v1)\n");
    return send_full(a, 0x01, body, sizeof(body));
}

static int rsa_secret(struct gip_auth *a)
{
    const unsigned char *p = a->client_pubkey;
    /* The embedded sequence is PKCS#1 RSAPublicKey DER, rather than the
     * SubjectPublicKeyInfo structure expected by d2i_PUBKEY(). */
    RSA *rsa = d2i_RSAPublicKey(NULL, &p, sizeof(a->client_pubkey));
    EVP_PKEY *key = rsa ? EVP_PKEY_new() : NULL;
    if (!key) {
        RSA_free(rsa);
    } else if (EVP_PKEY_assign_RSA(key, rsa) != 1) {
        EVP_PKEY_free(key);
        RSA_free(rsa);
        key = NULL;
    }
    EVP_PKEY_CTX *ctx; uint8_t pms[48], enc[256], randoms[64]; size_t n = sizeof(enc);
    if (!key) {
        fprintf(stderr, "  AUTH: embedded RSA key parse failed\n");
        return -1;
    }
    RAND_bytes(pms, sizeof(pms)); memcpy(randoms, a->random_host, 32); memcpy(randoms + 32, a->random_client, 32);
    ctx = EVP_PKEY_CTX_new(key, NULL); if (!ctx) { EVP_PKEY_free(key); return -1; }
    if (EVP_PKEY_encrypt_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0 ||
        EVP_PKEY_encrypt(ctx, enc, &n, pms, sizeof(pms)) <= 0) { EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key); return -1; }
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);
    if (prf(pms, sizeof(pms), "Master Secret", randoms, sizeof(randoms), a->master_secret, sizeof(a->master_secret))) return -1;
    uint8_t body[256];
    if (n != sizeof(body)) return -1;
    memcpy(body, enc, n);
    fprintf(stderr, "  --> AUTH encrypted secret\n");
    return send_full(a, 0x05, body, sizeof(body));
}

static int ecdh_exchange(struct gip_auth *a)
{
    EC_KEY *host = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    const EC_GROUP *group; const EC_POINT *pub; EC_POINT *peer;
    EVP_PKEY *peer_key = NULL; EVP_PKEY_CTX *ctx = NULL;
    uint8_t peer_oct[65], shared[32], randoms[64], body[64] = {0};
    size_t shared_len = sizeof(shared); int n;
    if (!host || EC_KEY_generate_key(host) != 1) return -1;
    group = EC_KEY_get0_group(host); pub = EC_KEY_get0_public_key(host);
    body[0] = 0;
    n = EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED,
                           peer_oct, sizeof(peer_oct), NULL);
    if (n != 65) return -1;
    memcpy(body, peer_oct + 1, sizeof(body));
    peer = EC_POINT_new(group); if (!peer) return -1;
    peer_oct[0] = 4; memcpy(peer_oct + 1, a->client_pubkey2, 64);
    if (EC_POINT_oct2point(group, peer, peer_oct, sizeof(peer_oct), NULL) != 1) return -1;
    EC_KEY *peer_ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!peer_ec || EC_KEY_set_public_key(peer_ec, peer) != 1) return -1;
    peer_key = EVP_PKEY_new(); EVP_PKEY_assign_EC_KEY(peer_key, peer_ec);
    EVP_PKEY *host_key = EVP_PKEY_new(); EVP_PKEY_assign_EC_KEY(host_key, host);
    ctx = EVP_PKEY_CTX_new(host_key, NULL);
    if (!ctx || EVP_PKEY_derive_init(ctx) <= 0 || EVP_PKEY_derive_set_peer(ctx, peer_key) <= 0 ||
        EVP_PKEY_derive(ctx, shared, &shared_len) <= 0) return -1;
    SHA256(shared, shared_len, shared);
    memcpy(randoms, a->random_host, 32); memcpy(randoms + 32, a->random_client, 32);
    if (prf(shared, 32, "Master Secret", randoms, sizeof(randoms), a->master_secret, 48)) return -1;
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(host_key); EVP_PKEY_free(peer_key); EC_POINT_free(peer);
    return send_full(a, 0x25, body, sizeof(body));
}

static int send_finish(struct gip_auth *a, uint8_t cmd)
{
    uint8_t body[32];

    if (prf(a->master_secret, sizeof(a->master_secret), "Host Finished",
            a->transcript, sizeof(a->transcript), body, sizeof(body)))
        return -1;
    fprintf(stderr, "  --> AUTH host finish (v%d)\n", cmd >= 0x20 ? 2 : 1);
    return send_full(a, cmd, body, sizeof(body));
}

int gip_auth_process(struct gip_auth *a, const uint8_t *d, size_t len)
{
    if (len < 6) return -1;
    /* Current xone clients upgrade from the legacy hello to authentication
     * version 2 by putting different command IDs in the two headers. */
    if (len >= 10 && d[0] == 0 && d[3] != d[6]) {
        uint8_t body[36] = {0};
        RAND_bytes(a->random_host, sizeof(a->random_host));
        memcpy(body, a->random_host, 32);
        fprintf(stderr, "  <-- AUTH protocol upgrade; --> host hello (v2)\n");
        return send_full(a, 0x21, body, sizeof(body));
    }
    /* The delayed acknowledgement after the GIP zero-length terminator has
     * handshake command 0x01 again.  Its meaning is the last host message. */
    if (d[1] & 1) {
        switch (a->last_cmd) {
        case 0x01:
            fprintf(stderr, "  <-- AUTHENTICATE host hello ACK\n");
            return request(a, 0x02, 80);
        case 0x05:
            fprintf(stderr, "  <-- AUTH encrypted secret ACK\n");
            return send_finish(a, 0x07);
        case 0x07:
            fprintf(stderr, "  <-- AUTH host finish ACK; requesting client finish\n");
            return request(a, 0x08, 64);
        case 0x25:
            fprintf(stderr, "  <-- AUTH v2 public-key ACK; sending host finish\n");
            return send_finish(a, 0x26);
        case 0x26:
            return request(a, 0x27, 64);
        default:
            return 0;
        }
    }
    if (d[0] != 0 || len < 10) return 0;
    uint8_t cmd = d[3]; size_t body_len = rdbe16(d + 8);
    if (10 + body_len > len) body_len = len - 10;
    /* Match xone: the transcript starts at the inner data header and spans
     * the complete authenticated record, including its fixed trailer. */
    transcript_update(a, d + 6, len - 6);
    if (cmd == 0x02) {
        if (body_len < 32) return -1; memcpy(a->random_client, d + 10, 32);
        fprintf(stderr, "  <-- AUTH client hello; requesting certificate\n");
        return request(a, 0x03, 1024);
    }
    if (cmd == 0x22) {
        if (body_len < 32) return -1;
        memcpy(a->random_client, d + 10, 32);
        fprintf(stderr, "  <-- AUTH v2 client hello; requesting certificate\n");
        return request(a, 0x23, 768);
    }
    if (cmd == 0x23) {
        fprintf(stderr, "  <-- AUTH v2 certificate (%zu bytes); requesting public key\n", body_len);
        return request(a, 0x24, 128);
    }
    if (cmd == 0x24) {
        if (body_len < 64) return -1;
        memcpy(a->client_pubkey2, d + 10, 64);
        fprintf(stderr, "  <-- AUTH v2 client public key; exchanging ECDH key\n");
        return ecdh_exchange(a);
    }
    if (cmd == 0x27) {
        fprintf(stderr, "  <-- AUTH v2 client finish; authentication complete\n");
        uint8_t complete[2] = {1, 0};
        int ret = a->send(a->send_ctx, complete, sizeof(complete), false);
        if (!ret) a->authenticated = true;
        return ret;
    }
    if (cmd == 0x08) {
        uint8_t complete[2] = {1, 0};
        fprintf(stderr, "  <-- AUTH client finish; authentication complete\n");
        int ret = a->send(a->send_ctx, complete, sizeof(complete), false);
        if (!ret) a->authenticated = true;
        return ret;
    }
    if (cmd == 0x03) {
        static const uint8_t rsa_key_prefix[] = { 0x30, 0x82, 0x01, 0x0a };
        /* xone dispatches the certificate handler before appending the
         * received record to the transcript.  The handler sends Host Secret,
         * which must therefore precede the certificate in the transcript. */
        size_t cert_transcript_start = a->transcript_len - (len - 6);
        a->transcript_len = cert_transcript_start;
        SHA256(a->transcript_buf, a->transcript_len, a->transcript);
        if (body_len > sizeof(a->client_certificate)) return -1;
        memcpy(a->client_certificate, d + 10, body_len);
        a->client_certificate_len = body_len;
        fprintf(stderr, "  <-- AUTH client certificate (%zu bytes)\n", body_len);
        /* Microsoft's accessory certificate has an empty subject and is not
         * reliably usable as X.509.  Match xone: extract its embedded DER
         * SubjectPublicKeyInfo sequence directly. */
        for (size_t i = 0; i + sizeof(rsa_key_prefix) <= body_len; i++) {
            if (memcmp(d + 10 + i, rsa_key_prefix, sizeof(rsa_key_prefix)))
                continue;
            if (i + sizeof(a->client_pubkey) > body_len) return -1;
            memcpy(a->client_pubkey, d + 10 + i, sizeof(a->client_pubkey));
            int ret = rsa_secret(a);
            if (!ret)
                transcript_update(a, d + 6, len - 6);
            return ret;
        }
        fprintf(stderr, "  AUTH: embedded RSA key not found in certificate\n");
        return -1;
    }
    return 0;
}
