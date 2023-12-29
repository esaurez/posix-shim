// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// NOTE: libc requires this for RTLD_NEXT.
#define _GNU_SOURCE

#include "epoll.h"
#include "error.h"
#include "qman.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#define INTERPOSE_CALL(type, fn_libc, fn_demi, ...)                                                                    \
    {                                                                                                                  \
        type ret = -1;                                                                                                 \
        static bool reentrant = false;                                                                                 \
                                                                                                                       \
        if ((!initialized) || (reentrant))                                                                             \
            return (fn_libc(__VA_ARGS__));                                                                             \
                                                                                                                       \
        init();                                                                                                        \
                                                                                                                       \
        int last_errno = errno;                                                                                        \
                                                                                                                       \
        reentrant = true;                                                                                              \
        ret = fn_demi(__VA_ARGS__);                                                                                    \
        reentrant = false;                                                                                             \
                                                                                                                       \
        if ((ret) == -1 && (errno == EBADF))                                                                           \
        {                                                                                                              \
            errno = last_errno;                                                                                        \
            return fn_libc(__VA_ARGS__);                                                                               \
        }                                                                                                              \
                                                                                                                       \
        return ret;                                                                                                    \
    }

// Control-path hooks.
extern int __demi_init(void);
extern int __demi_socket(int domain, int type, int protocol);
extern int __demi_shutdown(int sockfd, int how);
extern int __demi_close(int sockfd);
extern int __demi_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
extern int __demi_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
extern int __demi_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern int __demi_listen(int sockfd, int backlog);
extern int __demi_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern int __demi_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern int __demi_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
extern int __demi_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern int __demi_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

// Data-path hooks.
extern ssize_t __demi_read(int sockfd, void *buf, size_t count);
extern ssize_t __demi_recv(int sockfd, void *buf, size_t len, int flags);
extern ssize_t __demi_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr,
                               socklen_t *addrlen);
extern ssize_t __demi_recvmsg(int sockfd, struct msghdr *msg, int flags);
extern ssize_t __demi_readv(int sockfd, const struct iovec *iov, int iovcnt);
extern ssize_t __demi_write(int sockfd, const void *buf, size_t count);
extern ssize_t __demi_send(int sockfd, const void *buf, size_t len, int flags);
extern ssize_t __demi_sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr,
                             socklen_t addrlen);
extern ssize_t __demi_sendmsg(int sockfd, const struct msghdr *msg, int flags);
extern ssize_t __demi_writev(int sockfd, const struct iovec *iov, int iovcnt);
extern ssize_t __demi_pread(int sockfd, void *buf, size_t count, off_t offset);
extern ssize_t __demi_pwrite(int sockfd, const void *buf, size_t count, off_t offset);

// Epoll hooks
extern int __demi_epoll_create(int size);
extern int __demi_epoll_create1(int flags);
extern int __demi_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
extern int __demi_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

// System calls that we interpose.
static int (*libc_socket)(int, int, int) = NULL;
static int (*libc_close)(int) = NULL;
static int (*libc_shutdown)(int, int) = NULL;
static int (*libc_bind)(int, const struct sockaddr *, socklen_t) = NULL;
static int (*libc_connect)(int, const struct sockaddr *, socklen_t) = NULL;
static int (*libc_listen)(int, int) = NULL;
static int (*libc_accept4)(int, struct sockaddr *, socklen_t *, int) = NULL;
static int (*libc_accept)(int, struct sockaddr *, socklen_t *) = NULL;
static int (*libc_getsockopt)(int, int, int, void *, socklen_t *) = NULL;
static int (*libc_setsockopt)(int, int, int, const void *, socklen_t) = NULL;
static int (*libc_getsockname)(int, struct sockaddr *, socklen_t *) = NULL;
static int (*libc_getpeername)(int, struct sockaddr *, socklen_t *) = NULL;
static ssize_t (*libc_read)(int, void *, size_t) = NULL;
static ssize_t (*libc_recv)(int, void *, size_t, int) = NULL;
static ssize_t (*libc_recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *) = NULL;
static ssize_t (*libc_recvmsg)(int, struct msghdr *, int) = NULL;
static ssize_t (*libc_readv)(int, const struct iovec *, int) = NULL;
static ssize_t (*libc_pread)(int, void *, size_t, off_t) = NULL;
static ssize_t (*libc_write)(int, const void *, size_t) = NULL;
static ssize_t (*libc_send)(int, const void *, size_t, int) = NULL;
static ssize_t (*libc_sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t) = NULL;
static ssize_t (*libc_sendmsg)(int, const struct msghdr *, int) = NULL;
static ssize_t (*libc_writev)(int, const struct iovec *, int) = NULL;
static ssize_t (*libc_pwrite)(int, const void *, size_t, off_t) = NULL;
static int (*libc_epoll_create)(int) = NULL;
static int (*libc_epoll_create1)(int) = NULL;
static int (*libc_epoll_ctl)(int, int, int, struct epoll_event *) = NULL;
static int (*libc_epoll_wait)(int, struct epoll_event *, int, int) = NULL;

static bool initialized = false;

static void __attribute__((constructor)) libc_init() 
{
    assert((libc_socket = dlsym(RTLD_NEXT, "socket")) != NULL);
    assert((libc_shutdown = dlsym(RTLD_NEXT, "shutdown")) != NULL);
    assert((libc_bind = dlsym(RTLD_NEXT, "bind")) != NULL);
    assert((libc_connect = dlsym(RTLD_NEXT, "connect")) != NULL);
    assert((libc_listen = dlsym(RTLD_NEXT, "listen")) != NULL);
    assert((libc_accept4 = dlsym(RTLD_NEXT, "accept4")) != NULL);
    assert((libc_accept = dlsym(RTLD_NEXT, "accept")) != NULL);
    assert((libc_getsockopt = dlsym(RTLD_NEXT, "getsockopt")) != NULL);
    assert((libc_setsockopt = dlsym(RTLD_NEXT, "setsockopt")) != NULL);
    assert((libc_getsockname = dlsym(RTLD_NEXT, "getsockname")) != NULL);
    assert((libc_getpeername = dlsym(RTLD_NEXT, "getpeername")) != NULL);
    assert((libc_read = dlsym(RTLD_NEXT, "read")) != NULL);
    assert((libc_recv = dlsym(RTLD_NEXT, "recv")) != NULL);
    assert((libc_recvfrom = dlsym(RTLD_NEXT, "recvfrom")) != NULL);
    assert((libc_recvmsg = dlsym(RTLD_NEXT, "recvmsg")) != NULL);
    assert((libc_readv = dlsym(RTLD_NEXT, "readv")) != NULL);
    assert((libc_pread = dlsym(RTLD_NEXT, "pread")) != NULL);
    assert((libc_write = dlsym(RTLD_NEXT, "write")) != NULL);
    assert((libc_send = dlsym(RTLD_NEXT, "send")) != NULL);
    assert((libc_sendto = dlsym(RTLD_NEXT, "sendto")) != NULL);
    assert((libc_sendmsg = dlsym(RTLD_NEXT, "sendmsg")) != NULL);
    assert((libc_writev = dlsym(RTLD_NEXT, "writev")) != NULL);
    assert((libc_pwrite = dlsym(RTLD_NEXT, "pwrite")) != NULL);
    assert((libc_close = dlsym(RTLD_NEXT, "close")) != NULL);
    assert((libc_epoll_create = dlsym(RTLD_NEXT, "epoll_create")) != NULL);
    assert((libc_epoll_create1 = dlsym(RTLD_NEXT, "epoll_create1")) != NULL);
    assert((libc_epoll_ctl = dlsym(RTLD_NEXT, "epoll_ctl")) != NULL);
    assert((libc_epoll_wait = dlsym(RTLD_NEXT, "epoll_wait")) != NULL);
}

static void init(void)
{
    if (!initialized)
    {

        if (__demi_init() != 0)
            abort();

        initialized = true;
    }
}

int close(int sockfd)
{
    INTERPOSE_CALL(int, libc_close, __demi_close, sockfd);
}

int shutdown(int sockfd, int how)
{
    INTERPOSE_CALL(int, libc_shutdown, __demi_shutdown, sockfd, how);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    INTERPOSE_CALL(int, libc_bind, __demi_bind, sockfd, addr, addrlen);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    INTERPOSE_CALL(int, libc_connect, __demi_connect, sockfd, addr, addrlen);
}

int listen(int sockfd, int backlog)
{
    INTERPOSE_CALL(int, libc_listen, __demi_listen, sockfd, backlog);
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    INTERPOSE_CALL(int, libc_accept4, __demi_accept4, sockfd, addr, addrlen, flags);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    INTERPOSE_CALL(int, libc_accept, __demi_accept, sockfd, addr, addrlen);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    INTERPOSE_CALL(int, libc_getsockopt, __demi_getsockopt, sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    INTERPOSE_CALL(int, libc_setsockopt, __demi_setsockopt, sockfd, level, optname, optval, optlen);
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    INTERPOSE_CALL(int, libc_getsockname, __demi_getsockname, sockfd, addr, addrlen);
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    INTERPOSE_CALL(int, libc_getpeername, __demi_getpeername, sockfd, addr, addrlen);
}

ssize_t read(int sockfd, void *buf, size_t count)
{
    INTERPOSE_CALL(ssize_t, libc_read, __demi_read, sockfd, buf, count);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    INTERPOSE_CALL(ssize_t, libc_recv, __demi_recv, sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    INTERPOSE_CALL(ssize_t, libc_recvfrom, __demi_recvfrom, sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    INTERPOSE_CALL(ssize_t, libc_recvmsg, __demi_recvmsg, sockfd, msg, flags);
}

ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt)
{
    INTERPOSE_CALL(ssize_t, libc_readv, __demi_readv, sockfd, iov, iovcnt);
}

ssize_t write(int sockfd, const void *buf, size_t count)
{
    INTERPOSE_CALL(ssize_t, libc_write, __demi_write, sockfd, buf, count);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    INTERPOSE_CALL(ssize_t, libc_send, __demi_send, sockfd, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
    INTERPOSE_CALL(ssize_t, libc_sendto, __demi_sendto, sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    INTERPOSE_CALL(ssize_t, libc_sendmsg, __demi_sendmsg, sockfd, msg, flags);
}

ssize_t writev(int sockfd, const struct iovec *iov, int iovcnt)
{
    INTERPOSE_CALL(ssize_t, libc_writev, __demi_writev, sockfd, iov, iovcnt);
}

ssize_t pread(int sockfd, void *buf, size_t count, off_t offset)
{
    INTERPOSE_CALL(ssize_t, libc_pread, __demi_pread, sockfd, buf, count, offset);
}

ssize_t pwrite(int sockfd, const void *buf, size_t count, off_t offset)
{
    INTERPOSE_CALL(ssize_t, libc_pwrite, __demi_pwrite, sockfd, buf, count, offset);
}

int epoll_create1(int flags)
{
    assert(flags == 0);
    return (epoll_create(EPOLL_MAX_FDS));
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    INTERPOSE_CALL(int, libc_epoll_ctl, __demi_epoll_ctl, epfd, op, fd, event);
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    INTERPOSE_CALL(int, libc_epoll_wait, __demi_epoll_wait, epfd, events, maxevents, timeout);
}

int socket(int domain, int type, int protocol)
{
    int ret = -1;
    static bool reentrant = false;

    init();

    if ((!initialized) || (reentrant))
        return (libc_socket(domain, type, protocol));

    int last_errno = errno;

    reentrant = true;
    ret = __demi_socket(domain, type, protocol);
    reentrant = false;

    if ((ret) == -1 && (errno == EBADF))
    {
        errno = last_errno;
        return (libc_socket(domain, type, protocol));
    }

    return ret;
}

int epoll_create(int size)
{
    int ret = -1;
    int linux_epfd = -1;
    int demikernel_epfd = -1;
    init();

    // Check if size argument is valid.
    if (size < 0)
    {
        errno = EINVAL;
        return -1;
    }

    // First, create epoll on kernel side.
    if ((ret = libc_epoll_create(size)) == -1)
    {
        ERROR("epoll_create() failed - %s", strerror(errno));
        return (ret);
    }

    linux_epfd = ret;

    int last_errno = errno;
    if ((ret = __demi_epoll_create(size)) == -1 && errno == EBADF)
    {
        errno = last_errno;
        return linux_epfd;
    }

    demikernel_epfd = ret;

    queue_man_register_linux_epfd(linux_epfd, demikernel_epfd);

    return linux_epfd;
}
