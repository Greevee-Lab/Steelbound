/* sb_tls.c - W334 LOC-SEC-CLIENT: SChannel-TLS-Client mit SPKI-Pin (siehe sb_tls.h). */
#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <security.h>
#define SCHANNEL_USE_BLACKLISTS /* SCH_CREDENTIALS/TLS_PARAMETERS (TLS 1.3) */
#include <subauth.h>
#include <schannel.h>
#include <sspi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sb_tls.h"
#include "sb_sha256.h"

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

#define TLS_IN_CAP (16384 + 2048)
#define TLS_OUT_CAP 65536
#define TLS_PLAIN_CAP 16384

enum { ST_INIT = 0, ST_HS = 1, ST_OPEN = 2, ST_FAIL = -1, ST_CLOSED = -2 };

struct sbtls {
    SOCKET s;
    CredHandle cred;
    CtxtHandle ctx;
    int have_cred, have_ctx, state, pin_failed;
    char sni[256];
    uint8_t pins[2][32];
    int npins;
    SecPkgContext_StreamSizes sizes;
    char err[160], ver[16];
    int inlen, outlen, outoff, plen, poff;
    uint8_t in[TLS_IN_CAP];
    uint8_t out[TLS_OUT_CAP];
    uint8_t plain[TLS_PLAIN_CAP];
};

static int tls_fail(sbtls *t, const char *msg, long code)
{
    if (t->state != ST_FAIL) {
        if (code) _snprintf_s(t->err, sizeof t->err, _TRUNCATE, "%s (0x%08lx)", msg, (unsigned long)code);
        else _snprintf_s(t->err, sizeof t->err, _TRUNCATE, "%s", msg);
    }
    t->state = ST_FAIL;
    return SBTLS_FAIL;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sbtls_parse_pin(const char *hex, uint8_t out[32])
{
    int i;
    if (!hex || strlen(hex) != 64) return 0;
    for (i = 0; i < 32; i++) {
        int a = hexv(hex[2 * i]), b = hexv(hex[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    for (i = 0; i < 32; i++) {
        if (out[i]) return 1;
    }
    return 0; /* alles 0 = leer */
}

static int pin_empty(const uint8_t p[32])
{
    int i;
    uint8_t a = 0;
    for (i = 0; i < 32; i++) a |= p[i];
    return a == 0;
}

static int ct_eq32(const uint8_t *a, const uint8_t *b)
{
    volatile uint8_t d = 0;
    int i;
    for (i = 0; i < 32; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* Anmeldedaten: SCH_CREDENTIALS (ab Windows 10 1809, TLS 1.3 moeglich), sonst SCHANNEL_CRED mit TLS 1.2. */
static int tls_acquire(sbtls *t)
{
    SECURITY_STATUS ss;
    TimeStamp ts;
    SCH_CREDENTIALS sc;
    TLS_PARAMETERS tp;
    memset(&sc, 0, sizeof sc);
    memset(&tp, 0, sizeof tp);
    tp.grbitDisabledProtocols = (DWORD) ~(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT);
    sc.dwVersion = SCH_CREDENTIALS_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO |
                 SCH_CRED_NO_SERVERNAME_CHECK;
    sc.cTlsParameters = 1;
    sc.pTlsParameters = &tp;
    ss = AcquireCredentialsHandleA(NULL, (LPSTR)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &t->cred, &ts);
    if (ss != SEC_E_OK) {
        SCHANNEL_CRED oc;
        memset(&oc, 0, sizeof oc);
        oc.dwVersion = SCHANNEL_CRED_VERSION;
        oc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        oc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO |
                     SCH_CRED_NO_SERVERNAME_CHECK;
        ss = AcquireCredentialsHandleA(NULL, (LPSTR)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &oc, NULL, NULL, &t->cred, &ts);
        if (ss != SEC_E_OK) return tls_fail(t, "TLS: keine SChannel-Anmeldedaten", (long)ss);
    }
    t->have_cred = 1;
    return SBTLS_OK;
}

int sbtls_client_new(sbtls **out, SOCKET s, const char *sni, const uint8_t (*spki)[32], int nspki)
{
    sbtls *t;
    int i;
    *out = NULL;
    if (s == INVALID_SOCKET || !sni || !spki || nspki <= 0) return SBTLS_FAIL;
    t = (sbtls *)calloc(1, sizeof *t);
    if (!t) return SBTLS_FAIL;
    t->s = s;
    strncpy_s(t->sni, sizeof t->sni, sni, _TRUNCATE);
    for (i = 0; i < nspki && t->npins < 2; i++) {
        if (!pin_empty(spki[i])) memcpy(t->pins[t->npins++], spki[i], 32);
    }
    *out = t;
    if (t->npins == 0) return tls_fail(t, "TLS: kein Pin gesetzt", 0); /* nie ohne Pin */
    return tls_acquire(t);
}

/* Ausgabepuffer senden. OK = leer, WANT_IO = Rest offen, FAIL. */
static int tls_send_pending(sbtls *t)
{
    while (t->outoff < t->outlen) {
        int r = send(t->s, (const char *)t->out + t->outoff, t->outlen - t->outoff, 0);
        if (r > 0) {
            t->outoff += r;
            continue;
        }
        if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return SBTLS_WANT_IO;
        return tls_fail(t, "TLS: Senden fehlgeschlagen", WSAGetLastError());
    }
    t->outoff = t->outlen = 0;
    return SBTLS_OK;
}

static int tls_queue(sbtls *t, const void *p, int n)
{
    if (n <= 0) return SBTLS_OK;
    if (t->outoff > 0 && t->outoff == t->outlen) t->outoff = t->outlen = 0;
    if (t->outoff > 0) {
        memmove(t->out, t->out + t->outoff, (size_t)(t->outlen - t->outoff));
        t->outlen -= t->outoff;
        t->outoff = 0;
    }
    if (t->outlen + n > TLS_OUT_CAP) return tls_fail(t, "TLS: Sendepuffer voll", 0);
    memcpy(t->out + t->outlen, p, (size_t)n);
    t->outlen += n;
    return SBTLS_OK;
}

/* Empfangen in t->in. 1 = neue Daten, 0 = nichts (WOULDBLOCK), -1 = Gegenseite zu, -2 = Fehler. */
static int tls_recv(sbtls *t)
{
    int r;
    if (t->inlen >= TLS_IN_CAP) return -2;
    r = recv(t->s, (char *)t->in + t->inlen, TLS_IN_CAP - t->inlen, 0);
    if (r > 0) {
        t->inlen += r;
        return 1;
    }
    if (r == 0) return -1;
    if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
    return -2;
}

/* Pin (SPKI-SHA-256) und ausgehandelte Suite pruefen - vor jedem Anwendungsbyte. */
static int tls_verify(sbtls *t)
{
    PCCERT_CONTEXT cert = NULL;
    SecPkgContext_CipherInfo ci;
    SecPkgContext_ConnectionInfo cn;
    SECURITY_STATUS ss;
    BYTE *der = NULL;
    DWORD dl = 0;
    uint8_t sha[32];
    int i, ok = 0;
    ss = QueryContextAttributesA(&t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)&cert);
    if (ss != SEC_E_OK || !cert) {
        t->pin_failed = 1;
        return tls_fail(t, "TLS: kein Serverzertifikat", (long)ss);
    }
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &cert->pCertInfo->SubjectPublicKeyInfo,
                             CRYPT_ENCODE_ALLOC_FLAG, NULL, &der, &dl) ||
        !der) {
        CertFreeCertificateContext(cert);
        t->pin_failed = 1;
        return tls_fail(t, "TLS: SPKI nicht lesbar", (long)GetLastError());
    }
    sb_sha256(der, dl, sha);
    LocalFree(der);
    CertFreeCertificateContext(cert);
    for (i = 0; i < t->npins; i++) ok |= ct_eq32(sha, t->pins[i]);
    if (!ok) {
        t->pin_failed = 1;
        return tls_fail(t, "TLS: Server-Pruefung fehlgeschlagen (Pin passt nicht)", 0);
    }
    /* Suite: TLS 1.3 (immer AEAD + Vorwaertssicherheit) oder TLS 1.2 nur ECDHE + AEAD (F1) */
    memset(&ci, 0, sizeof ci);
    memset(&cn, 0, sizeof cn);
    ss = QueryContextAttributesW(&t->ctx, SECPKG_ATTR_CIPHER_INFO, &ci);
    if (ss != SEC_E_OK) return tls_fail(t, "TLS: Suite nicht lesbar", (long)ss);
    ss = QueryContextAttributesA(&t->ctx, SECPKG_ATTR_CONNECTION_INFO, &cn);
    if (ss != SEC_E_OK) return tls_fail(t, "TLS: Version nicht lesbar", (long)ss);
    if (cn.dwProtocol & SP_PROT_TLS1_3_CLIENT) {
        strcpy_s(t->ver, sizeof t->ver, "TLS 1.3");
    } else if (cn.dwProtocol & SP_PROT_TLS1_2_CLIENT) {
        if (wcsncmp(ci.szCipherSuite, L"TLS_ECDHE_", 10) != 0 ||
            (wcsstr(ci.szCipherSuite, L"_GCM_") == NULL && wcsstr(ci.szCipherSuite, L"CHACHA20") == NULL))
            return tls_fail(t, "TLS: Suite ohne ECDHE/AEAD abgelehnt", 0);
        strcpy_s(t->ver, sizeof t->ver, "TLS 1.2");
    } else {
        return tls_fail(t, "TLS: Version unter 1.2 abgelehnt", 0);
    }
    ss = QueryContextAttributesA(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes);
    if (ss != SEC_E_OK) return tls_fail(t, "TLS: Stream-Groessen", (long)ss);
    t->state = ST_OPEN;
    return SBTLS_OK;
}

/* Ein Schritt von InitializeSecurityContext mit den Eingangsdaten in t->in (oder ohne beim ersten Aufruf). */
static SECURITY_STATUS tls_isc(sbtls *t, int first)
{
    SecBuffer ib[2], ob[1];
    SecBufferDesc id, od;
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                  ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR | ISC_REQ_MANUAL_CRED_VALIDATION, got = 0;
    TimeStamp ts;
    SECURITY_STATUS ss;
    ib[0].BufferType = SECBUFFER_TOKEN;
    ib[0].pvBuffer = t->in;
    ib[0].cbBuffer = (unsigned long)t->inlen;
    ib[1].BufferType = SECBUFFER_EMPTY;
    ib[1].pvBuffer = NULL;
    ib[1].cbBuffer = 0;
    id.ulVersion = SECBUFFER_VERSION;
    id.cBuffers = 2;
    id.pBuffers = ib;
    ob[0].BufferType = SECBUFFER_TOKEN;
    ob[0].pvBuffer = NULL;
    ob[0].cbBuffer = 0;
    od.ulVersion = SECBUFFER_VERSION;
    od.cBuffers = 1;
    od.pBuffers = ob;
    ss = InitializeSecurityContextA(&t->cred, first ? NULL : &t->ctx, first ? t->sni : NULL, flags, 0, 0,
                                    first ? NULL : &id, 0, first ? &t->ctx : NULL, &od, &got, &ts);
    if (first && (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED)) t->have_ctx = 1;
    if (ob[0].pvBuffer) {
        if (ob[0].cbBuffer > 0 && (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED || ss == SEC_I_RENEGOTIATE) &&
            tls_queue(t, ob[0].pvBuffer, (int)ob[0].cbBuffer) != SBTLS_OK)
            ss = SEC_E_INTERNAL_ERROR;
        FreeContextBuffer(ob[0].pvBuffer);
    }
    if (ss == SEC_E_INCOMPLETE_MESSAGE) return ss; /* t->in unveraendert lassen */
    if (!first) {
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer > 0 && (int)ib[1].cbBuffer <= t->inlen) {
            memmove(t->in, t->in + t->inlen - ib[1].cbBuffer, ib[1].cbBuffer);
            t->inlen = (int)ib[1].cbBuffer;
        } else {
            t->inlen = 0;
        }
    }
    return ss;
}

int sbtls_handshake(sbtls *t)
{
    int r;
    if (!t) return SBTLS_FAIL;
    if (t->state == ST_FAIL) return SBTLS_FAIL;
    if (t->state == ST_OPEN) return tls_send_pending(t) == SBTLS_FAIL ? SBTLS_FAIL : SBTLS_OK;
    if (t->state == ST_CLOSED) return SBTLS_CLOSED;
    if (t->state == ST_INIT) {
        SECURITY_STATUS ss = tls_isc(t, 1);
        if (ss != SEC_I_CONTINUE_NEEDED) return tls_fail(t, "TLS: Handshake-Start", (long)ss);
        t->state = ST_HS;
    }
    for (;;) {
        SECURITY_STATUS ss;
        r = tls_send_pending(t);
        if (r == SBTLS_FAIL) return r;
        if (t->inlen == 0) {
            int g = tls_recv(t);
            if (g == 0) return SBTLS_WANT_IO;
            if (g == -1) return tls_fail(t, "TLS: Server hat waehrend des Handshakes getrennt", 0);
            if (g < 0) return tls_fail(t, "TLS: Empfang im Handshake", WSAGetLastError());
        }
        ss = tls_isc(t, 0);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            int g = tls_recv(t);
            if (g == 0) return SBTLS_WANT_IO;
            if (g == -1) return tls_fail(t, "TLS: Server hat waehrend des Handshakes getrennt", 0);
            if (g < 0) return tls_fail(t, "TLS: Handshake-Nachricht zu gross oder Empfangsfehler", WSAGetLastError());
            continue;
        }
        if (ss == SEC_I_CONTINUE_NEEDED) continue;
        if (ss == SEC_E_OK) {
            r = tls_verify(t); /* Pin + Suite: vorher kein Anwendungsbyte */
            if (r != SBTLS_OK) return r;
            r = tls_send_pending(t);
            return r == SBTLS_FAIL ? r : SBTLS_OK;
        }
        return tls_fail(t, "TLS: Handshake fehlgeschlagen (kein TLS-Server oder abgelehnt)", (long)ss);
    }
}

int sbtls_write(sbtls *t, const void *p, int n)
{
    const uint8_t *src = (const uint8_t *)p;
    if (!t || t->state != ST_OPEN || n < 0) return SBTLS_FAIL; /* vor bestandener Pruefung nie Daten */
    while (n > 0) {
        int chunk = n > (int)t->sizes.cbMaximumMessage ? (int)t->sizes.cbMaximumMessage : n;
        int total = (int)(t->sizes.cbHeader + t->sizes.cbTrailer) + chunk;
        uint8_t *rec;
        SecBuffer b[4];
        SecBufferDesc d;
        SECURITY_STATUS ss;
        rec = (uint8_t *)malloc((size_t)total);
        if (!rec) return tls_fail(t, "TLS: kein Speicher", 0);
        memcpy(rec + t->sizes.cbHeader, src, (size_t)chunk);
        b[0].BufferType = SECBUFFER_STREAM_HEADER;
        b[0].pvBuffer = rec;
        b[0].cbBuffer = t->sizes.cbHeader;
        b[1].BufferType = SECBUFFER_DATA;
        b[1].pvBuffer = rec + t->sizes.cbHeader;
        b[1].cbBuffer = (unsigned long)chunk;
        b[2].BufferType = SECBUFFER_STREAM_TRAILER;
        b[2].pvBuffer = rec + t->sizes.cbHeader + chunk;
        b[2].cbBuffer = t->sizes.cbTrailer;
        b[3].BufferType = SECBUFFER_EMPTY;
        b[3].pvBuffer = NULL;
        b[3].cbBuffer = 0;
        d.ulVersion = SECBUFFER_VERSION;
        d.cBuffers = 4;
        d.pBuffers = b;
        ss = EncryptMessage(&t->ctx, 0, &d, 0);
        if (ss != SEC_E_OK) {
            SecureZeroMemory(rec, (size_t)total);
            free(rec);
            return tls_fail(t, "TLS: Verschluesseln", (long)ss);
        }
        total = (int)(b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
        if (tls_queue(t, rec, total) != SBTLS_OK) {
            SecureZeroMemory(rec, (size_t)(t->sizes.cbHeader + t->sizes.cbTrailer) + chunk);
            free(rec);
            return SBTLS_FAIL;
        }
        SecureZeroMemory(rec, (size_t)(t->sizes.cbHeader + t->sizes.cbTrailer) + chunk);
        free(rec);
        src += chunk;
        n -= chunk;
    }
    return tls_send_pending(t) == SBTLS_FAIL ? SBTLS_FAIL : SBTLS_OK;
}

int sbtls_flush(sbtls *t)
{
    if (!t || t->state == ST_FAIL) return SBTLS_FAIL;
    return tls_send_pending(t);
}

int sbtls_read(sbtls *t, void *p, int cap)
{
    if (!t || cap <= 0) return SBTLS_FAIL;
    if (t->state == ST_FAIL) return SBTLS_FAIL;
    if (t->state != ST_OPEN && t->state != ST_CLOSED) return SBTLS_FAIL;
    for (;;) {
        SecBuffer b[4];
        SecBufferDesc d;
        SECURITY_STATUS ss;
        int i, g;
        if (t->plen > t->poff) {
            int n = t->plen - t->poff;
            if (n > cap) n = cap;
            memcpy(p, t->plain + t->poff, (size_t)n);
            t->poff += n;
            if (t->poff == t->plen) {
                SecureZeroMemory(t->plain, (size_t)t->plen);
                t->plen = t->poff = 0;
            }
            return n;
        }
        if (t->state == ST_CLOSED) return SBTLS_CLOSED;
        if (t->inlen > 0) {
            for (i = 0; i < 4; i++) {
                b[i].BufferType = SECBUFFER_EMPTY;
                b[i].pvBuffer = NULL;
                b[i].cbBuffer = 0;
            }
            b[0].BufferType = SECBUFFER_DATA;
            b[0].pvBuffer = t->in;
            b[0].cbBuffer = (unsigned long)t->inlen;
            d.ulVersion = SECBUFFER_VERSION;
            d.cBuffers = 4;
            d.pBuffers = b;
            ss = DecryptMessage(&t->ctx, &d, 0, NULL);
            if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE || ss == SEC_I_CONTEXT_EXPIRED) {
                SecBuffer *data = NULL, *extra = NULL;
                for (i = 0; i < 4; i++) {
                    if (b[i].BufferType == SECBUFFER_DATA && !data) data = &b[i];
                    if (b[i].BufferType == SECBUFFER_EXTRA && !extra) extra = &b[i];
                }
                if (data && data->cbBuffer > 0) {
                    if (data->cbBuffer > TLS_PLAIN_CAP) return tls_fail(t, "TLS: Datensatz zu gross", 0);
                    memcpy(t->plain, data->pvBuffer, data->cbBuffer);
                    t->plen = (int)data->cbBuffer;
                    t->poff = 0;
                }
                if (extra && extra->cbBuffer > 0 && (int)extra->cbBuffer <= t->inlen) {
                    memmove(t->in, t->in + t->inlen - extra->cbBuffer, extra->cbBuffer);
                    t->inlen = (int)extra->cbBuffer;
                } else {
                    t->inlen = 0;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) {
                    t->state = ST_CLOSED; /* close_notify */
                    continue;
                }
                if (ss == SEC_I_RENEGOTIATE) {
                    /* TLS 1.3: Nachricht nach dem Handshake (NewSessionTicket/KeyUpdate) - an SChannel geben.
                     * Echte Neuverhandlung unter TLS 1.2 lehnt der Server ab (OP_NO_RENEGOTIATION). */
                    SECURITY_STATUS s2 = tls_isc(t, 0);
                    if (s2 != SEC_E_OK && s2 != SEC_I_CONTINUE_NEEDED && s2 != SEC_E_INCOMPLETE_MESSAGE)
                        return tls_fail(t, "TLS: Nachricht nach dem Handshake", (long)s2);
                    if (tls_send_pending(t) == SBTLS_FAIL) return SBTLS_FAIL;
                }
                continue;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) return tls_fail(t, "TLS: Entschluesseln", (long)ss);
        }
        g = tls_recv(t);
        if (g == 0) return SBTLS_WANT_IO;
        if (g == -1) {
            t->state = ST_CLOSED;
            return SBTLS_CLOSED;
        }
        if (g < 0) return tls_fail(t, "TLS: Empfang", WSAGetLastError());
    }
}

const char *sbtls_error(const sbtls *t)
{
    return t ? t->err : "TLS: kein Objekt";
}

const char *sbtls_version(const sbtls *t)
{
    return t && t->ver[0] ? t->ver : "-";
}

int sbtls_pin_failed(const sbtls *t)
{
    return t ? t->pin_failed : 0;
}

void sbtls_free(sbtls *t)
{
    if (!t) return;
    if (t->have_ctx) DeleteSecurityContext(&t->ctx);
    if (t->have_cred) FreeCredentialsHandle(&t->cred);
    SecureZeroMemory(t, sizeof *t);
    free(t);
}
