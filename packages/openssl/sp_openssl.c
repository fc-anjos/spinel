/* sp_openssl.c -- the C half of the `openssl` spin package, linked on demand
   when `require "openssl"` appears.

   Spinel does not implement TLS. This is glue over the system libssl: the
   handshake, the record-layer reads and writes, and the certificate check.
   Everything cryptographic, every protocol state machine, and the trust
   decision itself belong to OpenSSL, and the trust ANCHORS belong to the
   operating system -- SSL_CTX_set_default_verify_paths reads whatever the
   distribution's ca-certificates package installed, so a revoked CA stops
   being trusted on an OS update rather than on a Spinel release.

   The SSL * never reaches Ruby. Connections live in a table here and Ruby
   holds an int handle, the same shape sp_net gives a socket fd: no raw
   pointer is handed to a garbage-collected world, and a stale handle is a
   bounds check rather than a use-after-free.

   Layered over an fd the caller already owns (sp_net or a TCPSocket), so the
   plaintext socket surface stays exactly as it was; this unit only adds the
   record layer on top of a descriptor. Closing a connection here does not
   close the fd -- Ruby's IO owns that. */
#include "spinel/runtime.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <fcntl.h>

/* Version floor and the one API that moved. SSL_get1_peer_certificate is
   OpenSSL 3.0's name for what 1.1 called SSL_get_peer_certificate, and
   LibreSSL keeps the old one; the rest of what this file uses
   (TLS_client_method, SSL_set1_host, SSL_CTX_set_min_proto_version) arrived in
   OpenSSL 1.1.0 and LibreSSL 3.5. Below that the file will not compile, and a
   #error says so rather than leaving a page of diagnostics about missing
   declarations. */
#if defined(LIBRESSL_VERSION_NUMBER)
# if LIBRESSL_VERSION_NUMBER < 0x3050000fL
#  error "sp_openssl.c needs LibreSSL 3.5 or newer"
# endif
# define SP_SSL_PEER_CERT(ssl) SSL_get_peer_certificate(ssl)
#elif OPENSSL_VERSION_NUMBER < 0x10100000L
# error "sp_openssl.c needs OpenSSL 1.1.0 or newer"
#elif OPENSSL_VERSION_NUMBER < 0x30000000L
# define SP_SSL_PEER_CERT(ssl) SSL_get_peer_certificate(ssl)
#else
# define SP_SSL_PEER_CERT(ssl) SSL_get1_peer_certificate(ssl)
#endif

#define SP_SSL_MAX 256          /* concurrent TLS connections per process */
#define SP_SSL_BUF 65536

typedef struct {
  SSL     *ssl;
  SSL_CTX *ctx;
  int      fd;
  int      in_use;
  int      nonblock;      /* O_NONBLOCK armed by the first non-blocking read */
} sp_ssl_conn;

static sp_ssl_conn sp_ssl_tab[SP_SSL_MAX];
static char sp_ssl_errbuf[512];
static char *sp_ssl_rdbuf;

/* Record the most recent failure so the Ruby side can raise with a reason
   rather than a bare "connect failed". OpenSSL keeps its own per-thread queue;
   this flattens the top entry, or the caller's own message when there is
   none (a would-block, an EOF, a bad handle). */
static void sp_ssl_note(const char *what) {
  unsigned long e = ERR_get_error();
  if (e) {
    char b[256];
    ERR_error_string_n(e, b, sizeof b);
    snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s: %s", what, b);
    while (ERR_get_error()) { }          /* drain, so the next call starts clean */
  }
else {
    snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s", what);
  }
}

const char *sp_ssl_last_error(void) {
  return sp_str_dup_external(sp_ssl_errbuf[0] ? sp_ssl_errbuf : "");
}

static int sp_ssl_slot(void) {
  for (int i = 0; i < SP_SSL_MAX; i++) if (!sp_ssl_tab[i].in_use) return i;
  return -1;
}

static sp_ssl_conn *sp_ssl_at(sp_int h) {
  if (h < 0 || h >= SP_SSL_MAX) return NULL;
  sp_ssl_conn *c = &sp_ssl_tab[h];
  return c->in_use ? c : NULL;
}

/* Open a client connection over an already-connected fd.
   `hostname` drives both SNI and the certificate's name check; verify != 0
   asks OpenSSL to validate the chain against the OS trust store. Returns a
   handle, or -1 with the reason in sp_ssl_last_error. */
sp_int sp_ssl_connect(sp_int fd, const char *hostname, sp_int verify) {
  int i = sp_ssl_slot();
  if (i < 0) { sp_ssl_note("too many TLS connections"); return -1; }
  sp_ssl_errbuf[0] = 0;

  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) { sp_ssl_note("SSL_CTX_new"); return -1; }
  /* TLS 1.2 is the floor: 1.0 and 1.1 are withdrawn and a server offering
     only those is not one to fall back to silently. */
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  if (verify) {
    /* The trust anchors are the operating system's. Nothing is bundled here,
       so a CA the OS stops trusting stops being trusted with it. */
    if (!SSL_CTX_set_default_verify_paths(ctx)) {
      sp_ssl_note("no system trust store");
      SSL_CTX_free(ctx);
      return -1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  }
else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  SSL *ssl = SSL_new(ctx);
  if (!ssl) { sp_ssl_note("SSL_new"); SSL_CTX_free(ctx); return -1; }
  if (hostname && hostname[0]) {
    SSL_set_tlsext_host_name(ssl, hostname);     /* SNI */
    if (verify) {
      /* Without this the chain validates and the NAME does not, which is the
         classic way to have TLS and no security. */
      SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
      if (!SSL_set1_host(ssl, hostname)) {
        sp_ssl_note("SSL_set1_host");
        SSL_free(ssl); SSL_CTX_free(ctx);
        return -1;
      }
    }
  }
  if (!SSL_set_fd(ssl, (int)fd)) {
    sp_ssl_note("SSL_set_fd");
    SSL_free(ssl); SSL_CTX_free(ctx);
    return -1;
  }
  if (SSL_connect(ssl) != 1) {
    sp_ssl_note("TLS handshake failed");
    SSL_free(ssl); SSL_CTX_free(ctx);
    return -1;
  }
  sp_ssl_tab[i].ssl = ssl;
  sp_ssl_tab[i].ctx = ctx;
  sp_ssl_tab[i].fd  = (int)fd;
  sp_ssl_tab[i].nonblock = 0;
  sp_ssl_tab[i].in_use = 1;
  return i;
}

/* Up to `maxlen` decrypted bytes from one record-layer read. Returns "" at
   EOF or on error, with the reason recorded; the byte count rides the string
   header, so a payload with embedded NULs survives. */
const char *sp_ssl_read(sp_int h, sp_int maxlen) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) { sp_ssl_note("closed TLS connection"); return sp_str_dup_external(""); }
  if (!sp_ssl_rdbuf && !(sp_ssl_rdbuf = (char *)malloc(SP_SSL_BUF)))
    return sp_str_dup_external("");
  if (maxlen <= 0 || maxlen >= SP_SSL_BUF) maxlen = SP_SSL_BUF - 1;
  sp_ssl_errbuf[0] = 0;
  int n = SSL_read(c->ssl, sp_ssl_rdbuf, (int)maxlen);
  if (n <= 0) {
    int e = SSL_get_error(c->ssl, n);
    if (e == SSL_ERROR_ZERO_RETURN) sp_ssl_note("");        /* clean shutdown */
    else if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) sp_ssl_note("");
    else sp_ssl_note("SSL_read");
    return sp_str_dup_external("");
  }
  char *out = sp_str_alloc((size_t)n);
  memcpy(out, sp_ssl_rdbuf, (size_t)n);
  out[n] = 0;
  sp_str_set_len(out, (size_t)n);
  return out;
}

/* Why a non-blocking read needs a status and not just bytes: TLS is a record
   layer, so "nothing to read yet" and "I must WRITE before I can read" are
   different answers -- a renegotiating peer makes SSL_read ask for the socket
   to become writable. The caller has to wait on the right direction, so the
   reason rides in a separate slot rather than being flattened into "".
     0 ok   1 want-read   2 want-write   3 eof   4 error */
static SP_TLS int sp_ssl_want_state = 0;

sp_int sp_ssl_want(void) { return sp_ssl_want_state; }

/* Up to `maxlen` bytes without blocking. The fd must already be non-blocking;
   this does not set it, because the descriptor belongs to the caller's IO.
   Answers "" whenever it did not read, with the reason in sp_ssl_want. */
const char *sp_ssl_read_nb(sp_int h, sp_int maxlen) {
  sp_ssl_conn *c = sp_ssl_at(h);
  sp_ssl_want_state = 0;
  if (!c) { sp_ssl_want_state = 4; sp_ssl_note("closed TLS connection"); return sp_str_dup_external(""); }
  if (!sp_ssl_rdbuf && !(sp_ssl_rdbuf = (char *)malloc(SP_SSL_BUF))) {
    sp_ssl_want_state = 4;
    return sp_str_dup_external("");
  }
  if (maxlen <= 0 || maxlen >= SP_SSL_BUF) maxlen = SP_SSL_BUF - 1;
  sp_ssl_errbuf[0] = 0;
  /* CRuby's sysread_nonblock arms O_NONBLOCK on the descriptor itself rather
     than making the caller do it (rb_io_set_nonblock), so this does too --
     once, on the first non-blocking read, and never back off again, which is
     also what CRuby leaves behind. */
  if (!c->nonblock) {
    int fl = fcntl(c->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    c->nonblock = 1;
  }
  /* Bytes already decrypted answer immediately: the descriptor can be quiet
     while a whole record sits in the record layer, so asking the fd first
     would park a caller that already has data. */
  int n = SSL_read(c->ssl, sp_ssl_rdbuf, (int)maxlen);
  if (n > 0) {
    char *out = sp_str_alloc((size_t)n);
    memcpy(out, sp_ssl_rdbuf, (size_t)n);
    out[n] = 0;
    sp_str_set_len(out, (size_t)n);
    return out;
  }
  switch (SSL_get_error(c->ssl, n)) {
    case SSL_ERROR_WANT_READ:   sp_ssl_want_state = 1; break;
    case SSL_ERROR_WANT_WRITE:  sp_ssl_want_state = 2; break;
    case SSL_ERROR_ZERO_RETURN: sp_ssl_want_state = 3; break;
    default:                    sp_ssl_want_state = 4; sp_ssl_note("SSL_read"); break;
  }
  return sp_str_dup_external("");
}

/* Write `n` bytes; answers how many went, or -1. Binary: the length comes
   from the caller, not from strlen. */
sp_int sp_ssl_write(sp_int h, const char *data, sp_int n) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) { sp_ssl_note("closed TLS connection"); return -1; }
  if (n <= 0) return 0;
  sp_ssl_errbuf[0] = 0;
  int w = SSL_write(c->ssl, data, (int)n);
  if (w <= 0) { sp_ssl_note("SSL_write"); return -1; }
  return w;
}

/* One record-layer write that never blocks, the mirror of sp_ssl_read_nb: the
   reason for writing nothing rides in sp_ssl_want, because a TLS write can
   need the socket to become READABLE first -- the peer's side of a
   renegotiation -- and a caller waiting on the wrong direction waits forever.
   Answers the byte count, or 0 with the reason set. */
sp_int sp_ssl_write_nb(sp_int h, const char *data, sp_int n) {
  sp_ssl_conn *c = sp_ssl_at(h);
  sp_ssl_want_state = 0;
  if (!c) { sp_ssl_want_state = 4; sp_ssl_note("closed TLS connection"); return 0; }
  if (n <= 0) return 0;
  if (!c->nonblock) {
    int fl = fcntl(c->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    c->nonblock = 1;
  }
  sp_ssl_errbuf[0] = 0;
  int w = SSL_write(c->ssl, data, (int)n);
  if (w > 0) return w;
  switch (SSL_get_error(c->ssl, w)) {
    case SSL_ERROR_WANT_READ:   sp_ssl_want_state = 1; break;
    case SSL_ERROR_WANT_WRITE:  sp_ssl_want_state = 2; break;
    case SSL_ERROR_ZERO_RETURN: sp_ssl_want_state = 3; break;
    default:                    sp_ssl_want_state = 4; sp_ssl_note("SSL_write"); break;
  }
  return 0;
}

/* Bytes already decrypted and waiting in the record layer. An event loop that
   selects on the fd alone will miss these: a whole record can arrive in one
   read, leaving the descriptor quiet while the application still has data. */
sp_int sp_ssl_pending(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  return c ? (sp_int)SSL_pending(c->ssl) : 0;
}

/* The peer's certificate subject, for a caller that wants to report it.
   "" when there is none. */
const char *sp_ssl_peer_subject(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) return sp_str_dup_external("");
  X509 *cert = SP_SSL_PEER_CERT(c->ssl);
  if (!cert) return sp_str_dup_external("");
  char buf[512];
  X509_NAME_oneline(X509_get_subject_name(cert), buf, (int)sizeof buf);
  X509_free(cert);
  return sp_str_dup_external(buf);
}

const char *sp_ssl_version(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  return sp_str_dup_external(c ? SSL_get_version(c->ssl) : "");
}

const char *sp_ssl_cipher(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  const SSL_CIPHER *ci = c ? SSL_get_current_cipher(c->ssl) : NULL;
  return sp_str_dup_external(ci ? SSL_CIPHER_get_name(ci) : "");
}

/* Send close_notify and release the slot. The fd is NOT closed: Ruby's IO
   owns it and will close it itself, and closing it here would pull the
   descriptor out from under a still-live IO object. */
sp_int sp_ssl_close(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) return -1;
  SSL_shutdown(c->ssl);
  SSL_free(c->ssl);
  SSL_CTX_free(c->ctx);
  c->ssl = NULL; c->ctx = NULL; c->fd = -1; c->in_use = 0;
  return 0;
}
