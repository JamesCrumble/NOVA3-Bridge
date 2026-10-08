/*
 * The sockets layer, answered exactly the way the Vita reference answers it.
 *
 * ---------------------------------------------------------------------------
 * What was assumed, and why it was wrong
 *
 * The port spent several iterations on the theory that our EASP crashes because
 * DirtySock never got initialised - that the Vita's loader provides some
 * NetConnStartup / SocketInit / allocator hook we are missing, and porting that
 * one shim would let EASP come up the way it does there.
 *
 * Two findings killed that theory:
 *
 *   - DirtyMemAlloc is one instruction. `b _Znwj`. It is plain operator new,
 *     with no DirtySock allocator state behind it, so there is nothing to
 *     initialise and no hook to install.
 *
 *   - vita-ref/loader/reimpl/ has no net.c and no socket.c. The Vita
 *     reimplements io, mem, pthread, env, sys, ctype, controls and log, and
 *     nothing at all for networking.
 *
 * The Vita does not make EASP work. It makes EASP fail *early and cleanly*,
 * from default_dynlib.c:
 *
 *     { "socket",   &retminus1 }        line 386
 *     { "poll",     &ret0     }         line 323
 *     { "recv",     &ret0     }         { "recvfrom", &ret0 }
 *     { "send",     &ret0     }         { "sendto",   &ret0 }
 *     { "bind" / "connect" / "getsockopt" / "setsockopt" / "shutdown" -> real }
 *
 * socket() answering -1 is the whole trick. Every DirtySock path checks it, and
 * the engine's no-network branch is a supported state - it is what a phone in
 * airplane mode does. The real bind/connect/setsockopt entries are harmless
 * precisely because no descriptor ever exists to pass to them.
 *
 * ---------------------------------------------------------------------------
 * Why this port needs it and the Vita's authors barely had to think about it
 *
 * A Vita has no sockets unless the title asks for them. This loader runs on
 * Linux, under qemu, in a container with a working network stack, so our
 * socket() *succeeds*. EASP then gets a real descriptor, tries to reach EA
 * servers that were retired years ago, and dies somewhere down the failure path
 * that no Android build ever exercised with a live socket and a dead host.
 *
 * So the divergence was never a missing init. It was a missing *stub*: we gave
 * the game a working network it cannot use, where the reference gave it an
 * obviously absent one it knows how to handle.
 */
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

extern "C" {

/*
 * Announced once so a log reader can tell "the game never tried to go online"
 * apart from "the game went online and something ate the traffic".
 */
static void announce_once(void)
{
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("network: socket() refused - EA's servers for this game are gone, "
              "and the engine's offline path is the supported one");
    }
}

ABI_ATTR int bionic_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    announce_once();
    errno = EAFNOSUPPORT;
    return -1;
}

/*
 * The transfer calls answer 0 rather than -1, which is what the reference does.
 * Zero from recv() is an orderly shutdown and every caller has a path for it;
 * -1 with an errno invites a retry loop.
 */
ABI_ATTR ssize_t bionic_send(int fd, const void *buf, size_t len, int flags)
{
    (void)fd; (void)buf; (void)len; (void)flags;
    return 0;
}

ABI_ATTR ssize_t bionic_sendto(int fd, const void *buf, size_t len, int flags,
                               const struct sockaddr *dest, socklen_t dlen)
{
    (void)fd; (void)buf; (void)len; (void)flags; (void)dest; (void)dlen;
    return 0;
}

ABI_ATTR ssize_t bionic_recv(int fd, void *buf, size_t len, int flags)
{
    (void)fd; (void)buf; (void)len; (void)flags;
    return 0;
}

ABI_ATTR ssize_t bionic_recvfrom(int fd, void *buf, size_t len, int flags,
                                 struct sockaddr *src, socklen_t *slen)
{
    (void)fd; (void)buf; (void)len; (void)flags; (void)src; (void)slen;
    return 0;
}

/*
 * poll() answering 0 is "nothing is ready, and the wait expired". The engine
 * polls its (nonexistent) sockets from a worker thread; returning -1 there
 * would be an error it logs on every pass.
 */
ABI_ATTR int bionic_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    (void)fds; (void)nfds; (void)timeout;
    return 0;
}

} /* extern "C" */

DynLibFunction symtable_net[] = {
    THUNK_SPECIFIC("socket",   bionic_socket),
    THUNK_SPECIFIC("send",     bionic_send),
    THUNK_SPECIFIC("sendto",   bionic_sendto),
    THUNK_SPECIFIC("recv",     bionic_recv),
    THUNK_SPECIFIC("recvfrom", bionic_recvfrom),
    THUNK_SPECIFIC("poll",     bionic_poll),
    { NULL, 0 },
};
