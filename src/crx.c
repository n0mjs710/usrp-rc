#include "crx.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Two: an operator's session and a spare, for a one-shot command or a second
 * session. A third connection closes the oldest, so a session lost on a bad link
 * can't lock anyone out. */
#define CRX_CLIENTS 2
#define CRX_RECENT  25    /* log lines a new connection gets, to see what just happened */
#define CRX_LINE    200   /* a log line as sent, cut to fit */
#define CRX_ENTRY   128   /* the longest entry taken */
#define CRX_GROUP   "usrp-rc"

typedef struct {
    int      fd;          /* -1 = free */
    bool     dead;        /* a send failed: closed once the current work is done */
    unsigned order;       /* connection order: the lowest is the oldest */
    char     who[40];
    char     label[48];   /* "crx (who)", as its log lines start */
    char     in[CRX_ENTRY];
    size_t   inlen;
    bool     overlong;
    command_session_t *cmd;
} client_t;

struct crx {
    const config_t *cfg;
    char        path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int         epfd;
    int         lfd;
    crx_run_fn  run;
    void       *arg;
    client_t    client[CRX_CLIENTS];
    unsigned    order;

    char   recent[CRX_RECENT][CRX_LINE];
    int    nrecent;
    int    head;
    char   partial[CRX_LINE];   /* the log line being built, without its time */
    size_t plen;

    char   status[256];   /* as last sent; empty = send the next one */
};

static void send_text(client_t *c, const char *text)
{
    if (c->fd < 0 || c->dead)
        return;
    size_t len = strlen(text);
    ssize_t n = send(c->fd, text, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n != (ssize_t)len)
        c->dead = true;
}

static void broadcast(crx_t *x, const char *text)
{
    for (int i = 0; i < CRX_CLIENTS; i++)
        send_text(&x->client[i], text);
}

/* Log sink: whole lines, stamped with the time, to the recent list and each client. */
static void on_log(void *arg, const char *text)
{
    crx_t *x = arg;
    for (const char *p = text; *p; p++) {
        if (*p != '\n') {
            if (x->plen < sizeof(x->partial) - 12)
                x->partial[x->plen++] = *p;
            continue;
        }
        x->partial[x->plen] = '\0';
        x->plen = 0;

        char stamp[16] = "";
        time_t t = time(NULL);
        struct tm tm;
        if (localtime_r(&t, &tm))
            strftime(stamp, sizeof(stamp), "%H:%M:%S ", &tm);
        char *line = x->recent[x->head];
        snprintf(line, CRX_LINE, "%.9s%.*s\n", stamp, CRX_LINE - 12, x->partial);
        x->head = (x->head + 1) % CRX_RECENT;
        if (x->nrecent < CRX_RECENT)
            x->nrecent++;
        broadcast(x, line);
    }
}

static void close_client(crx_t *x, client_t *c, const char *how)
{
    if (c->fd < 0)
        return;
    epoll_ctl(x->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;   /* before the log line, which would go to it */
    command_session_destroy(c->cmd);
    c->cmd = NULL;
    LOGI("crx: %s %s\n", c->who, how);
}

static void sweep(crx_t *x)
{
    for (int i = 0; i < CRX_CLIENTS; i++)
        if (x->client[i].fd >= 0 && x->client[i].dead)
            close_client(x, &x->client[i], "DISCONNECTED");
}

static void peer_name(int fd, char *out, size_t n)
{
    struct ucred cr;
    socklen_t len = sizeof(cr);
    snprintf(out, n, "?");
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0)
        return;
    struct passwd pw, *res = NULL;
    char buf[1024];
    if (getpwuid_r(cr.uid, &pw, buf, sizeof(buf), &res) == 0 && res)
        snprintf(out, n, "%s", pw.pw_name);
    else
        snprintf(out, n, "uid %u", (unsigned)cr.uid);
}

static void on_accept(crx_t *x)
{
    for (;;) {
        int fd = accept4(x->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;

        char who[40];
        peer_name(fd, who, sizeof(who));

        client_t *c = NULL;
        for (int i = 0; i < CRX_CLIENTS && !c; i++)
            if (x->client[i].fd < 0)
                c = &x->client[i];
        if (!c) {
            c = &x->client[0];
            for (int i = 1; i < CRX_CLIENTS; i++)
                if (x->client[i].order < c->order)
                    c = &x->client[i];
            char bye[80];
            snprintf(bye, sizeof(bye), "@bye taken over by %s\n", who);
            send_text(c, bye);
            close_client(x, c, "TAKEN OVER");
        }

        snprintf(c->who, sizeof(c->who), "%s", who);
        snprintf(c->label, sizeof(c->label), "crx (%s)", who);
        if (command_session_create_keys(&c->cmd, x->cfg, c->label) != 0) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->dead = false;
        c->order = ++x->order;
        c->inlen = 0;
        c->overlong = false;

        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
        epoll_ctl(x->epfd, EPOLL_CTL_ADD, fd, &ev);

        for (int i = 0; i < x->nrecent; i++)
            send_text(c, x->recent[(x->head - x->nrecent + i + CRX_RECENT) % CRX_RECENT]);
        send_text(c, "@live\n");
        x->status[0] = '\0';   /* the next status goes out, to everyone */
        LOGI("crx: %s CONNECTED\n", who);
    }
}

/* One entry, as one transmission: each key in turn, then the unkey. */
static void entry(crx_t *x, client_t *c, const char *line)
{
    char keys[CRX_ENTRY];
    size_t n = 0;
    for (const char *p = line; *p; p++) {
        char k = *p;
        if (k == ' ' || k == '\t')
            continue;
        if (k >= 'a' && k <= 'd')
            k = (char)(k - 'a' + 'A');
        if (!strchr("0123456789*#ABCD", k)) {
            LOGI("%s: '%s': ERROR '%c' is not a DTMF key\n", c->label, line,
                 isprint((unsigned char)k) ? k : '?');
            send_text(c, "@done fail\n");
            return;
        }
        keys[n++] = k;
    }

    bool ran = false;
    command_t cmd;
    for (size_t i = 0; i < n && c->fd >= 0; i++)
        if (command_session_key(c->cmd, keys[i], &cmd))
            ran |= x->run(x->arg, &cmd, c->label);
    if (command_session_end_of_tx(c->cmd, &cmd))
        ran |= x->run(x->arg, &cmd, c->label);
    send_text(c, ran ? "@done ok\n" : "@done fail\n");
}

static void on_client(crx_t *x, client_t *c)
{
    char buf[256];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0) {
            c->dead = true;
            return;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                c->dead = true;
            return;
        }
        for (ssize_t i = 0; i < n && !c->dead; i++) {
            char ch = buf[i];
            if (ch == '\r')
                continue;
            if (ch != '\n') {
                if (c->inlen < sizeof(c->in) - 1)
                    c->in[c->inlen++] = ch;
                else
                    c->overlong = true;
                continue;
            }
            c->in[c->inlen] = '\0';
            if (c->overlong) {
                LOGI("%s: entry over %d keys, DISCARDED\n", c->label, CRX_ENTRY - 1);
                send_text(c, "@done fail\n");
            } else {
                entry(x, c, c->in);
            }
            c->inlen = 0;
            c->overlong = false;
        }
    }
}

bool crx_handle(crx_t *x, int fd)
{
    if (fd == x->lfd) {
        on_accept(x);
    } else {
        client_t *c = NULL;
        for (int i = 0; i < CRX_CLIENTS && !c; i++)
            if (x->client[i].fd == fd)
                c = &x->client[i];
        if (!c)
            return false;
        on_client(x, c);
    }
    sweep(x);
    return true;
}

bool crx_connected(const crx_t *x)
{
    for (int i = 0; i < CRX_CLIENTS; i++)
        if (x->client[i].fd >= 0)
            return true;
    return false;
}

void crx_status(crx_t *x, const char *status)
{
    sweep(x);
    if (strcmp(status, x->status) == 0)
        return;
    snprintf(x->status, sizeof(x->status), "%s", status);
    char line[sizeof(x->status) + 16];
    snprintf(line, sizeof(line), "@status %s\n", x->status);
    broadcast(x, line);
    sweep(x);
}

int crx_create(crx_t **out, const config_t *cfg, int epfd, crx_run_fn run, void *arg)
{
    *out = NULL;
    const char *path = cfg->crx.socket;
    if (!path[0])
        return 0;

    crx_t *x = calloc(1, sizeof(*x));
    if (!x)
        return -1;
    x->cfg = cfg;
    x->epfd = epfd;
    x->run = run;
    x->arg = arg;
    for (int i = 0; i < CRX_CLIENTS; i++)
        x->client[i].fd = -1;

    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(sa.sun_path)) {
        LOGW("crx: socket path too long, NO control terminal: %s\n", path);
        free(x);
        return 0;
    }
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    snprintf(x->path, sizeof(x->path), "%s", path);

    x->lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    /* A socket left by a daemon that didn't stop cleanly. */
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
        unlink(path);
    if (x->lfd < 0 || bind(x->lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0
        || listen(x->lfd, 4) != 0) {
        LOGW("crx: can't listen on %s (%s), NO control terminal\n", path, strerror(errno));
        if (x->lfd >= 0)
            close(x->lfd);
        free(x);
        return 0;
    }

    /* The group's members may connect. Without the group, only the owner (root) can. */
    struct group gr, *res = NULL;
    char buf[1024];
    bool group = getgrnam_r(CRX_GROUP, &gr, buf, sizeof(buf), &res) == 0 && res
              && chown(path, (uid_t)-1, gr.gr_gid) == 0;
    chmod(path, group ? 0660 : 0600);

    struct epoll_event ev = {.events = EPOLLIN, .data.fd = x->lfd};
    epoll_ctl(epfd, EPOLL_CTL_ADD, x->lfd, &ev);
    log_set_sink(on_log, x);
    LOGI("crx: listening on %s%s\n", path, group ? "" : ", owner only: no group " CRX_GROUP);
    *out = x;
    return 0;
}

void crx_destroy(crx_t *x)
{
    if (!x)
        return;
    broadcast(x, "@bye usrp-rc stopping\n");
    log_set_sink(NULL, NULL);
    for (int i = 0; i < CRX_CLIENTS; i++)
        close_client(x, &x->client[i], "DISCONNECTED");
    close(x->lfd);
    unlink(x->path);
    free(x);
}
