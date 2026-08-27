/* sp_io.h -- File / IO handle surface.
 *
 * sp_File is a stdio FILE* plus its (GC-managed) path/mode strings,
 * shared between the generated translation unit and lib/sp_io.c, which
 * holds the allocation-free handle ops. The string-returning readers
 * (sp_File_gets / _read / _read_n / _path) stay inline in spinel_rt.h
 * because they allocate via the hot static sp_str_alloc; moving them
 * would split the per-TU string heap. */
#ifndef SP_IO_H
#define SP_IO_H

#include <stdio.h>
#include "sp_types.h"   /* sp_int, sp_bool */
#include "sp_array.h"   /* sp_IntArray (for #winsize) */

typedef struct {
  FILE *fp; const char *path; const char *mode; sp_int lineno;
  unsigned char bin_flag;      /* #binmode was called (#3131) */
  unsigned char no_autoclose;  /* #autoclose = false (#3131) */
  unsigned char is_sock;       /* a socket handle: writes bypass stdio (#2922) */
  unsigned char frozen;        /* Object#freeze; kept here, not in the GC header,
                                  because the standard streams are static storage
                                  with no header to flip (sp_io_stdout) */
} sp_File;

/* Object#frozen? / #freeze on a handle. A nil slot is nil's answer: frozen. */
sp_bool sp_io_frozen(sp_File *f);
sp_File *sp_io_freeze(sp_File *f);
/* Object#== on two handles: identity, except that two File::Stat handles
   compare as Comparable does for File::Stat -- by modification time. */
sp_bool sp_io_eq(sp_File *a, sp_File *b);

/* File.open(path, mode) -> GC-managed handle (block form is codegen-only). */
sp_File *sp_File_open(const char *path, const char *mode);
/* pipe(2) wrapper. 0 ok, -1 error. */
int sp_io_make_pipe(int fds[2]);
/* IO.pipe end: wrap a raw pipe fd in a GC-managed sp_File. */
sp_File *sp_io_fdopen(int fd, const char *mode);
sp_File *sp_io_fdopen_ex(int fd, const char *mode, int owns_fd);
sp_File *sp_File_open_perm(const char *path, const char *mode, sp_int perm);
/* Wrap a connected/listening socket fd. Reads stay on the buffered FILE* so
   #gets and friends work; writes bypass stdio straight to write(2), matching
   CRuby sockets' sync = true. `kind` labels the handle ("tcp", "tcpserver",
   ...) for #class rendering. (#2922) */
sp_File *sp_io_fdopen_sock(int fd, const char *kind);
void sp_sock_wait_readable(sp_File *f);
sp_int sp_File_write(sp_File *f, const char *s);
sp_int sp_File_write_bin(sp_File *f, const char *s);
sp_int sp_File_close(sp_File *f);
sp_bool sp_File_closed_p(sp_File *f);
const char *sp_File_inspect(sp_File *f);
const char *sp_io_kind_name(sp_File *f);
sp_File *sp_sock_accept(sp_File *f);
sp_File *sp_sock_accept_nb(sp_File *f, sp_bool exc);
const char *sp_sock_read_nb(sp_File *f, sp_int len, sp_bool exc, sp_bool is_recv);
sp_int sp_sock_write_nb(sp_File *f, const char *data, sp_bool exc);
sp_int sp_sock_write_nb_bin(sp_File *f, const char *data, sp_bool exc);
sp_int sp_sock_connect_nb(sp_File *f, const char *host, sp_int port, sp_bool exc);
/* Addrinfo-form connect_nonblock (already-resolved endpoint) */
sp_int sp_sock_connect_nb_sa(sp_File *f, const char *sa, sp_int salen,
                                 sp_bool exc);
sp_bool sp_io_is_a(sp_File *f, const char *cls);
sp_bool sp_io_instance_of(sp_File *f, const char *cls);
sp_File *sp_sock_udp_new(sp_int family);
const char *sp_sock_gethostname(void);
sp_PolyArray *sp_sock_getaddrinfo(const char *host, sp_int port);
sp_Addrinfo *sp_sock_address(sp_File *f, sp_int peer);
sp_File *sp_sock_new(sp_int domain, sp_int type, sp_int proto);
sp_File *sp_sock_pair_end(sp_int domain, sp_int type, sp_int proto, sp_int which);
sp_File *sp_sock_unix_server(const char *path);
sp_File *sp_sock_unix_connect(const char *path);
sp_int sp_sock_bind(sp_File *f, const char *host, sp_int port);
sp_int sp_sock_connect(sp_File *f, const char *host, sp_int port);
sp_int sp_sock_send(sp_File *f, const char *data, sp_int len, const char *host, sp_int port);
sp_int sp_sock_shutdown(sp_File *f, sp_int how);
sp_int sp_sock_const(const char *n);
const char *sp_sock_recv(sp_File *f, sp_int len);
const char *sp_sock_recvfrom(sp_File *f, sp_int len, const char **ip_out, sp_int *port_out);
sp_int sp_sock_setsockopt(sp_File *f, sp_int level, sp_int opt, sp_int value);
sp_SockOpt *sp_sock_getsockopt(sp_File *f, sp_int level, sp_int opt);
sp_int sp_sock_listen(sp_File *f, sp_int backlog);
void sp_File_puts(sp_File *f, const char *s);
void sp_File_print(sp_File *f, const char *s);
sp_int sp_File_flush(sp_File *f);
sp_bool sp_File_eof_p(sp_File *f);
/* IO instance methods riding the underlying fd (#3038). */
sp_int sp_File_readbyte(sp_File *f);
void sp_File_ungetbyte(sp_File *f, sp_int byte);
sp_bool sp_File_binmode_p(sp_File *f);
void sp_File_set_binmode(sp_File *f);
sp_File *sp_File_reopen_io(sp_File *f, sp_File *other);
sp_bool sp_File_close_on_exec_p(sp_File *f);
void sp_File_set_close_on_exec(sp_File *f, sp_bool on);
sp_int sp_File_fcntl(sp_File *f, sp_int cmd, sp_int arg);
sp_int sp_File_pwrite(sp_File *f, const char *s, sp_int off);
void sp_File_advise(sp_File *f, const char *kind, sp_int off, sp_int len);
void sp_File_close_half(sp_File *f, sp_bool reading);
sp_File *sp_File_reopen(sp_File *f, const char *path, const char *mode);
sp_int sp_File_seek(sp_File *f, sp_int off, sp_int whence); /* #seek -- whence: 0=SET 1=CUR 2=END */
sp_int sp_File_tell(sp_File *f);       /* #tell / #pos -- ftello, -1 on closed */
sp_int sp_File_rewind(sp_File *f);     /* #rewind */
sp_bool sp_File_tty_p(sp_File *f);     /* #tty? / #isatty -- isatty(fileno) */
sp_int sp_File_fileno(sp_File *f);     /* #fileno */
sp_IntArray *sp_File_winsize(sp_File *f); /* #winsize -> [rows, cols] (ioctl, or [0,0]) */

/* STDOUT / STDERR as shared IO handles wrapping the C stdout/stderr streams.
   The handle is a function-local static (stdout/stderr are not constant
   initializers) and is never closed. */
sp_File *sp_io_stdout(void);
sp_File *sp_io_stderr(void);
sp_File *sp_io_stdin(void);

/* File metadata predicates (libc/WinAPI only; defined in sp_io.c). */
sp_bool sp_file_directory(const char *path);
sp_bool sp_file_file(const char *path);
sp_bool sp_file_exist(const char *path);
sp_bool sp_file_symlink(const char *path);
sp_bool sp_file_owned(const char *path);
sp_bool sp_file_grpowned(const char *path);
sp_bool sp_file_setuid(const char *path);
sp_bool sp_file_setgid(const char *path);
sp_bool sp_file_sticky(const char *path);
sp_bool sp_file_socket(const char *path);
sp_bool sp_file_blockdev(const char *path);
sp_bool sp_file_chardev(const char *path);
sp_int sp_file_world_readable(const char *path);
sp_int sp_file_world_writable(const char *path);
sp_int sp_file_do_symlink(const char *oldp, const char *newp);
sp_int sp_file_do_link(const char *oldp, const char *newp);
sp_int sp_file_umask(sp_int mask, int have_arg);
sp_int sp_file_mkfifo(const char *path, sp_int mode);
sp_int sp_file_utime(double atime, double mtime, const char *path);
const char *sp_file_readlink(const char *path);  /* defined in sp_cold.c */
void sp_file_delete(const char *path);
void sp_file_rename(const char *from, const char *to);

#include <dirent.h>
/* Dir handle (Dir.open / Dir.each_child ...): ops live in lib/sp_cold.c. */
typedef struct { DIR *dp; const char *path; } sp_Dir;

/* ---- sp_io_pipe/sysopen relocated from spinel_rt.h (0 optcarrot uses). ---- */
sp_PolyArray *sp_io_pipe(void);
sp_int sp_io_sysopen(const char *path);

/* IO.select accepts anything that answers #to_io, which is how CRuby lets a
   wrapper -- a protocol object holding a socket -- be waited on. The runtime
   cannot dispatch a user method itself, so codegen emits the cls_id switch and
   main() installs it here, the same shape as sp_user_exc_parent_fn and the
   sp_json_*_fn hooks. NULL when the program defines no #to_io, which is when
   an element that is not an IO is the TypeError it always was. */
extern sp_File *(*sp_user_to_io_hook)(sp_RbVal);

#endif
