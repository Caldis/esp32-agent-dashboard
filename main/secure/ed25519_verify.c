/*
 * ed25519_verify.c — verify-only ed25519 (RFC 8032 Ed25519).
 *
 * Threat addressed: A4 — see docs/THREAT_MODEL.md. This module is
 * the cryptographic core of the OTA signature check.
 *
 * ## Why
 *
 * IDF v6.0.1's mbedtls does NOT expose ed25519 (no `mbedtls/ed25519.h`
 * in upstream and not in Espressif's fork). Rather than wire a third-
 * party managed-component just for verify, we bundle a tiny portable
 * verify-only implementation here. It is derived from TweetNaCl
 * (Bernstein/Janssen/Lange/Schwabe, public-domain) — specifically
 * the `crypto_sign_ed25519_open_detached` flow — with the SHA-512
 * call routed to mbedtls (so we use the S3's HW SHA peripheral via
 * CONFIG_MBEDTLS_HARDWARE_SHA when available).
 *
 * Properties:
 *   - VERIFY ONLY. No signing here. Signing is host-side
 *     (tools/sign/sign_firmware.py).
 *   - NOT constant-time. That's OK for signature *verification*
 *     (public-key operation, no secret data flows through).
 *   - Public domain (TweetNaCl) + ISC-licensed glue (this file).
 *
 * Interface:
 *   int ed25519_verify_detached(const uint8_t sig[64],
 *                               const uint8_t *msg, size_t msg_len,
 *                               const uint8_t pubkey[32]);
 *
 * Returns 1 on success (signature is valid for msg under pubkey),
 * 0 on failure.
 *
 * The caller in ota_verify.c passes the SHA-512 digest of the
 * (magic || version || size || firmware) bytes as `msg`. That is
 * compatible with RFC 8032 — ed25519 internally hashes `R || A || M`
 * with SHA-512, so the message M can be any byte string; we just
 * pick a 64-byte hash to avoid streaming-message buffer constraints.
 * The host signer in tools/sign/sign_firmware.py signs the same
 * 64-byte digest, so signer and verifier agree on M.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/sha512.h"

/* ── 64-bit field arithmetic over GF(2^255 - 19) ──────────────────── */

typedef int64_t fe[16];        /* 16 limbs of 16 bits, stored in int64 */

static const fe fe_zero = {0};
static const fe fe_one  = {1};

/* d = -121665/121666 mod p, RFC 8032 §5.1.1 */
static const fe d = {
    0x78a3, 0x1359, 0x4dca, 0x75eb,
    0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7,
    0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
/* 2*d */
static const fe d2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6,
    0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e,
    0xfce7, 0x56df, 0xd9dc, 0x2406,
};
/* 2^((p-1)/4) = sqrt(-1) mod p */
static const fe I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee,
    0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d,
    0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

/* L: order of base point (little-endian), RFC 8032 §5.1.5 */
static const uint8_t L_order[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
    0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,
};

static void set25519(fe r, const fe a)
{
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(fe o)
{
    int64_t c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(fe p, fe q, int b)
{
    int64_t t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const fe n)
{
    fe t, m;
    set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = t[i] & 0xff;
        o[2 * i + 1] = t[i] >> 8;
    }
}

static int neq25519(const fe a, const fe b)
{
    uint8_t c[32], d_[32];
    pack25519(c, a);
    pack25519(d_, b);
    int r = 0;
    for (int i = 0; i < 32; i++) r |= c[i] ^ d_[i];
    return (1 & ((r - 1) >> 8)) - 1;   /* 0 if equal, -1 if not */
}

static uint8_t par25519(const fe a)
{
    uint8_t d_[32];
    pack25519(d_, a);
    return d_[0] & 1;
}

static void unpack25519(fe o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) {
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static void A(fe o, const fe a, const fe b)    /* add */
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(fe o, const fe a, const fe b)    /* sub */
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(fe o, const fe a, const fe b)    /* mul */
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(fe o, const fe a)                /* square */
{
    M(o, a, a);
}

static void inv25519(fe o, const fe i)
{
    fe c;
    set25519(c, i);
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    set25519(o, c);
}

static void pow2523(fe o, const fe i)
{
    fe c;
    set25519(c, i);
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    set25519(o, c);
}

/* ── Group operations on the Edwards curve ────────────────────────── */

/* TweetNaCl's `add` written out cleanly. Edwards-curve point
 * addition in extended coordinates (X:Y:Z:T). */
static void edwards_add(fe p[4], const fe q[4])
{
    fe a, b, c, d_, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(b, q[1], q[0]);
    M(a, a, b);
    A(b, p[0], p[1]);
    A(c, q[0], q[1]);
    M(b, b, c);
    M(c, p[3], q[3]);
    M(c, c, d2);
    M(d_, p[2], q[2]);
    A(d_, d_, d_);
    Z(e, b, a);
    Z(f, d_, c);
    A(g, d_, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(fe p[4], fe q[4], uint8_t b)
{
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(uint8_t *r, fe p[4])
{
    fe tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void scalarmult(fe p[4], fe q[4], const uint8_t *s)
{
    set25519(p[0], fe_zero);
    set25519(p[1], fe_one);
    set25519(p[2], fe_one);
    set25519(p[3], fe_zero);
    for (int i = 255; i >= 0; i--) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        edwards_add(q, p);
        edwards_add(p, p);
        cswap(p, q, b);
    }
}

/* Standard base point B from RFC 8032 §5.1, projective (X,Y,Z,T). */
static const fe base_x = {
    0xd51a, 0x8f25, 0x2d60, 0xc956,
    0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4,
    0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
static const fe base_y = {
    0x6658, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666,
};

static void scalarbase(fe p[4], const uint8_t *s)
{
    fe q[4];
    set25519(q[0], base_x);
    set25519(q[1], base_y);
    set25519(q[2], fe_one);
    M(q[3], base_x, base_y);
    scalarmult(p, q, s);
}

/* Unpack a point — return 0 on success, -1 on bad encoding. */
static int unpackneg(fe r[4], const uint8_t p[32])
{
    fe t, chk, num, den, den2, den4, den6;
    set25519(r[2], fe_one);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, d);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], fe_zero, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

/* ── Scalar reduction mod L ───────────────────────────────────────── */

static void reduce(uint8_t r[64])
{
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)(uint64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;

    /* modL: standard reduction lifted from TweetNaCl. */
    int64_t carry;
    for (int i = 63; i >= 32; i--) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * L_order[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L_order[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * L_order[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

/* ── Verify ───────────────────────────────────────────────────────── */

int ed25519_verify_detached(const uint8_t sig[64],
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t pk[32])
{
    if (sig[63] & 224) return 0;   /* S must be < 2^253 */

    fe q[4];
    if (unpackneg(q, pk) != 0) return 0;

    /* k = SHA-512(R || A || M) reduced mod L */
    uint8_t k[64];
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    if (mbedtls_sha512_starts(&ctx, 0) != 0) { mbedtls_sha512_free(&ctx); return 0; }
    if (mbedtls_sha512_update(&ctx, sig, 32) != 0) { mbedtls_sha512_free(&ctx); return 0; }
    if (mbedtls_sha512_update(&ctx, pk, 32) != 0)  { mbedtls_sha512_free(&ctx); return 0; }
    if (mbedtls_sha512_update(&ctx, msg, msg_len) != 0) { mbedtls_sha512_free(&ctx); return 0; }
    if (mbedtls_sha512_finish(&ctx, k) != 0) { mbedtls_sha512_free(&ctx); return 0; }
    mbedtls_sha512_free(&ctx);
    reduce(k);

    /* p = [k]A */
    fe p[4];
    scalarmult(p, q, k);

    /* q' = [s]B */
    fe qp[4];
    scalarbase(qp, sig + 32);

    /* [s]B = R + [k]A  iff  pack(p + qp) == R */
    edwards_add(p, qp);
    uint8_t check[32];
    pack(check, p);

    /* compare with R (sig[0..32]) */
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= check[i] ^ sig[i];
    return diff == 0 ? 1 : 0;
}
