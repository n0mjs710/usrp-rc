/* usrp-crx: the control terminal for usrp-rc.
 *
 *   usrp-crx              a session: the log scrolls above, the state and the entry
 *                         line sit below
 *   usrp-crx 123 01       one entry: prints what it caused, and exits 0 if a command
 *                         ran, 1 if none did, 2 if usrp-rc can't be reached
 *
 * Plain ANSI and libc only, for any terminal over any SSH link. The protocol is in
 * src/crx.h. */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/usrp-rc/crx.sock"
#define KEEP           200    /* log lines kept, to redraw after a resize */
#define LINE           256
#define ENTRY          40
#define HISTORY        50
#define RETRY_MS       2000
#define RECENT         25     /* as the daemon sends: what a reconnect may repeat */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   /* the daemon's clock: the ID time is on it */
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int connect_to(const char *path)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(sa.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static const char *why_not(int e)
{
    if (e == ENOENT || e == ECONNREFUSED)
        return "usrp-rc not running";
    if (e == EACCES || e == EPERM)
        return "permission denied: not in group usrp-rc?";
    return strerror(e);
}

static bool send_line(int fd, const char *text)
{
    char buf[ENTRY + 2];
    int n = snprintf(buf, sizeof(buf), "%s\n", text);
    return send(fd, buf, (size_t)n, MSG_NOSIGNAL) == n;
}

/* Lines from the daemon, split out of what arrives. */
typedef struct {
    char   buf[4096];
    size_t len;
} reader_t;

/* The next whole line, without its newline, or NULL. A line too long for the buffer
 * comes out in parts. */
static char *next_line(reader_t *r, char *out, size_t n)
{
    char *nl = memchr(r->buf, '\n', r->len);
    size_t take = nl ? (size_t)(nl - r->buf) : r->len == sizeof(r->buf) ? r->len : 0;
    if (!nl && !take)
        return NULL;
    size_t copy = take < n - 1 ? take : n - 1;
    memcpy(out, r->buf, copy);
    out[copy] = '\0';
    size_t used = take + (nl ? 1 : 0);
    memmove(r->buf, r->buf + used, r->len - used);
    r->len -= used;
    return out;
}

/* ── one entry, then exit ──────────────────────────────────────────────── */

static int one_shot(const char *path, const char *entry)
{
    int fd = connect_to(path);
    if (fd < 0) {
        fprintf(stderr, "usrp-crx: %s: %s\n", path, why_not(errno));
        return 2;
    }
    if (!send_line(fd, entry)) {
        fprintf(stderr, "usrp-crx: %s\n", strerror(errno));
        return 2;
    }

    reader_t r = {.len = 0};
    bool live = false;
    uint64_t deadline = now_ms() + 5000;
    for (;;) {
        char line[LINE];
        while (next_line(&r, line, sizeof(line))) {
            if (strcmp(line, "@live") == 0)
                live = true;
            else if (strncmp(line, "@done ", 6) == 0)
                return strcmp(line + 6, "ok") == 0 ? 0 : 1;
            else if (strncmp(line, "@bye ", 5) == 0) {
                fprintf(stderr, "usrp-crx: %s\n", line + 5);
                return 2;
            } else if (line[0] != '@' && live)
                puts(line);
        }
        uint64_t now = now_ms();
        if (now >= deadline)
            break;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, (int)(deadline - now)) < 0 && errno != EINTR)
            break;
        ssize_t n = recv(fd, r.buf + r.len, sizeof(r.buf) - r.len, MSG_DONTWAIT);
        if (n == 0)
            break;
        if (n > 0)
            r.len += (size_t)n;
    }
    fprintf(stderr, "usrp-crx: no answer from usrp-rc\n");
    return 2;
}

/* ── the session screen ─────────────────────────────────────────────────── */
/*
 *   rows 1 .. R-4   the log, a scroll region
 *   row  R-3        rule
 *   row  R-2        status
 *   row  R-1        rule
 *   row  R          crx> entry
 */

static struct termios g_saved;
static bool g_raw;
static volatile sig_atomic_t g_winch, g_quit;

static int  R = 24, C = 80;
static bool g_utf8;

static char g_log[KEEP][LINE];
static int  g_nlog, g_loghead;

static char g_entry[ENTRY + 1];
static int  g_elen;

static char g_hist[HISTORY][ENTRY + 1];
static int  g_nhist, g_hpos;
static char g_histfile[512];

static const char *g_path;
static int      g_sock = -1;
static reader_t g_rd;
static bool     g_backlog;          /* before @live: recent lines, maybe already shown */
static enum { CONNECTED, RETRYING, TAKEN } g_conn = RETRYING;
static char     g_why[128] = "connecting";
static uint64_t g_next_try;

static struct {
    bool have;
    char call[32], rep[32], link[32], tx[32], rx[32], id[32];
} g_st;

static void on_signal(int sig)
{
    if (sig == SIGWINCH)
        g_winch = 1;
    else
        g_quit = 1;
}

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

static void at(int row) { out("\033[%d;1H\033[2K", row); }

static int log_rows(void) { return R - 4 > 1 ? R - 4 : 1; }

/* At most `width` columns of text, control characters shown as '?'. UTF-8 sequences
 * count one column and are never cut. Returns the columns used. */
static int put_cols(const char *s, int width)
{
    int cols = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            if (cols == width)
                break;
            cols++;
        }
        fputc(*p < 0x20 || *p == 0x7F ? '?' : *p, stdout);
    }
    return cols;
}

static void rule(int row)
{
    at(row);
    for (int i = 0; i < C; i++)
        fputs(g_utf8 ? "─" : "-", stdout);
}

static void draw_prompt(void)
{
    at(R);
    fputs("crx> ", stdout);
    put_cols(g_entry, C - 6);
}

static const char *dot(void) { return g_utf8 ? "·" : "-"; }

static void draw_status(void)
{
    at(R - 2);
    char text[256];
    if (g_conn == TAKEN) {
        snprintf(text, sizeof(text), " %s %s press Enter to take control back", g_why, dot());
        put_cols(text, C);
        return;
    }
    if (g_conn == CONNECTED && !g_st.have) {
        put_cols(" connected", C);
        return;
    }
    if (g_conn == RETRYING) {
        snprintf(text, sizeof(text), " %s %s retrying every %d s", g_why, dot(), RETRY_MS / 1000);
        put_cols(text, C);
        return;
    }

    char id[32];
    if (strcmp(g_st.id, "-") == 0) {
        snprintf(id, sizeof(id), "no ID pending");
    } else {
        unsigned long long due = strtoull(g_st.id, NULL, 10);
        uint64_t now = now_ms();
        if (due == 0)
            snprintf(id, sizeof(id), "ID pending");
        else if (due <= now)
            snprintf(id, sizeof(id), "ID due");
        else {
            unsigned s = (unsigned)((due - now + 999) / 1000);
            snprintf(id, sizeof(id), "ID pending %u:%02u", s / 60, s % 60);
        }
    }

    bool alarm = strcmp(g_st.rep, "UP") != 0;
    const char *rep = strcmp(g_st.rep, "TIMEOUT") == 0 ? "TIMED OUT" : g_st.rep;
    char rx[48];
    snprintf(rx, sizeof(rx), "%s", strcmp(g_st.rx, "none") == 0 ? (g_utf8 ? "—" : "-")
                                   : strcmp(g_st.rx, "both") == 0 ? "local+link" : g_st.rx);
    char link[48] = "";
    if (strcmp(g_st.link, "none") != 0)
        snprintf(link, sizeof(link), "link %s   ", g_st.link);

    int used = 0;
    if (strcmp(g_st.call, "-") != 0) {
        snprintf(text, sizeof(text), " %s  ", g_st.call);
        used += put_cols(text, C - used);
    } else {
        used += put_cols(" ", C - used);
    }
    if (alarm)
        fputs("\033[7m", stdout);
    used += put_cols(rep, C - used);
    if (alarm)
        fputs("\033[0m", stdout);
    snprintf(text, sizeof(text), "   %sTX %s   RX %s   %s", link,
             strcmp(g_st.tx, "ON") == 0 ? "ON" : "off", rx, id);
    put_cols(text, C - used);
}

/* A log line into the scroll region: it enters at the bottom and the rest move up. */
static void show_line(const char *text)
{
    out("\033[%d;1H\n", log_rows());
    put_cols(text, C);
}

static void add_line(const char *text)
{
    snprintf(g_log[g_loghead], LINE, "%s", text);
    g_loghead = (g_loghead + 1) % KEEP;
    if (g_nlog < KEEP)
        g_nlog++;
    show_line(text);
}

static bool recently_shown(const char *text)
{
    for (int i = 0; i < g_nlog && i < RECENT; i++)
        if (strcmp(g_log[(g_loghead - 1 - i + KEEP) % KEEP], text) == 0)
            return true;
    return false;
}

static void get_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        R = ws.ws_row;
        C = ws.ws_col;
    }
    if (R < 6)
        R = 6;
    if (C < 20)
        C = 20;
}

static void draw_all(void)
{
    get_size();
    out("\033[r\033[2J\033[1;%dr", log_rows());
    int rows = log_rows(), n = g_nlog < rows ? g_nlog : rows;
    for (int i = 0; i < n; i++) {
        at(rows - n + 1 + i);
        put_cols(g_log[(g_loghead - n + i + KEEP) % KEEP], C);
    }
    rule(R - 3);
    draw_status();
    rule(R - 1);
    draw_prompt();
}

static void term_restore(void)
{
    if (!g_raw)
        return;
    g_raw = false;
    fputs("\033[r\033[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
}

static bool term_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0)
        return false;
    struct termios t = g_saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0)
        return false;
    g_raw = true;
    atexit(term_restore);
    fputs("\033[?1049h", stdout);   /* the alternate screen: the shell's comes back at exit */
    return true;
}

/* ── history ─────────────────────────────────────────────────────────────── */

static void hist_add(const char *e, bool save)
{
    if (g_nhist && strcmp(g_hist[g_nhist - 1], e) == 0)
        return;
    if (g_nhist == HISTORY) {
        memmove(g_hist[0], g_hist[1], sizeof(g_hist[0]) * (HISTORY - 1));
        g_nhist--;
    }
    snprintf(g_hist[g_nhist++], ENTRY + 1, "%s", e);
    if (save && g_histfile[0]) {
        FILE *f = fopen(g_histfile, "a");
        if (f) {
            fprintf(f, "%s\n", e);
            fclose(f);
        }
    }
}

/* The file keeps growing, but only the last HISTORY entries are read back; it is
 * rewritten short when it has grown four times over. */
static void hist_load(void)
{
    const char *home = getenv("HOME");
    if (!home)
        return;
    snprintf(g_histfile, sizeof(g_histfile), "%s/.usrp-crx_history", home);
    FILE *f = fopen(g_histfile, "r");
    if (!f)
        return;
    char line[128];
    int lines = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) {
            hist_add(line, false);
            lines++;
        }
    }
    fclose(f);
    if (lines > 4 * HISTORY && (f = fopen(g_histfile, "w"))) {
        for (int i = 0; i < g_nhist; i++)
            fprintf(f, "%s\n", g_hist[i]);
        fclose(f);
    }
}

/* ── the connection ──────────────────────────────────────────────────────── */

static void disconnect(int conn, const char *why)
{
    if (g_sock >= 0)
        close(g_sock);
    g_sock = -1;
    g_conn = conn;
    g_st.have = false;
    snprintf(g_why, sizeof(g_why), "%s", why);
    g_next_try = now_ms() + RETRY_MS;
}

static void try_connect(void)
{
    g_sock = connect_to(g_path);
    if (g_sock < 0) {
        char why[128];
        snprintf(why, sizeof(why), "disconnected: %s", why_not(errno));
        disconnect(RETRYING, why);
        return;
    }
    g_conn = CONNECTED;
    g_rd.len = 0;
    g_backlog = true;
}

static void parse_status(char *s)
{
    g_st.have = true;
    for (char *tok = strtok(s, " "); tok; tok = strtok(NULL, " ")) {
        char *v = strchr(tok, '=');
        if (!v)
            continue;
        *v++ = '\0';
        char *dst = strcmp(tok, "call") == 0 ? g_st.call : strcmp(tok, "rep") == 0 ? g_st.rep
                  : strcmp(tok, "link") == 0 ? g_st.link : strcmp(tok, "tx") == 0 ? g_st.tx
                  : strcmp(tok, "rx") == 0 ? g_st.rx : strcmp(tok, "id") == 0 ? g_st.id : NULL;
        if (dst)
            snprintf(dst, sizeof(g_st.call), "%s", v);   /* every field is this size */
    }
}

static void on_socket(void)
{
    ssize_t n = recv(g_sock, g_rd.buf + g_rd.len, sizeof(g_rd.buf) - g_rd.len, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        disconnect(RETRYING, "disconnected: connection lost");
        return;
    }
    if (n > 0)
        g_rd.len += (size_t)n;

    char line[LINE];
    while (g_sock >= 0 && next_line(&g_rd, line, sizeof(line))) {
        if (strcmp(line, "@live") == 0) {
            g_backlog = false;
        } else if (strncmp(line, "@status ", 8) == 0) {
            parse_status(line + 8);
        } else if (strncmp(line, "@bye ", 5) == 0) {
            bool taken = strncmp(line + 5, "taken over", 10) == 0;
            disconnect(taken ? TAKEN : RETRYING, line + 5);
        } else if (line[0] == '@') {
            continue;   /* @done, and whatever a later daemon adds */
        } else if (!(g_backlog && recently_shown(line))) {
            add_line(line);
        }
    }
}

/* ── keys ────────────────────────────────────────────────────────────────── */

static void enter(void)
{
    if (g_conn == TAKEN) {
        try_connect();
        return;
    }
    if (!g_elen)
        return;
    if (g_conn != CONNECTED) {
        fputc('\a', stdout);   /* kept on the line, to send once connected */
        return;
    }
    if (!send_line(g_sock, g_entry)) {
        disconnect(RETRYING, "disconnected: connection lost");
        return;
    }
    hist_add(g_entry, true);
    g_hpos = g_nhist;
    g_elen = 0;
    g_entry[0] = '\0';
}

static void recall(int step)
{
    int pos = g_hpos + step;
    if (pos < 0 || pos > g_nhist)
        return;
    g_hpos = pos;
    snprintf(g_entry, sizeof(g_entry), "%s", pos < g_nhist ? g_hist[pos] : "");
    g_elen = (int)strlen(g_entry);
}

static void on_keys(void)
{
    static int esc;   /* 0 none, 1 after ESC, 2 after ESC [ */
    unsigned char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0 || errno != EINTR)
            g_quit = 1;   /* the terminal is gone */
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        unsigned char k = buf[i];
        if (esc == 1) {
            esc = k == '[' || k == 'O' ? 2 : 0;
            continue;
        }
        if (esc == 2) {
            if (k >= 0x40 && k <= 0x7E) {
                esc = 0;
                if (k == 'A')
                    recall(-1);
                else if (k == 'B')
                    recall(+1);
            }
            continue;
        }
        if (k == 0x1B)
            esc = 1;
        else if (k == '\r' || k == '\n')
            enter();
        else if (k == 0x03 || k == 0x04)
            g_quit = 1;
        else if (k == 0x7F || k == 0x08) {
            if (g_elen)
                g_entry[--g_elen] = '\0';
        } else if (k == 0x15) {
            g_elen = 0;
            g_entry[0] = '\0';
        } else if (k == 0x0C) {
            g_winch = 1;
        } else if (strchr("0123456789*#ABCDabcd ", k) && k) {
            if (g_elen < ENTRY) {
                g_entry[g_elen++] = (char)(k >= 'a' && k <= 'd' ? k - 'a' + 'A' : k);
                g_entry[g_elen] = '\0';
            } else {
                fputc('\a', stdout);
            }
        } else {
            fputc('\a', stdout);   /* not a DTMF key */
        }
    }
}

static int session(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "usrp-crx: a session needs a terminal; for a script, give the keys "
                        "as arguments\n");
        return 2;
    }
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LC_CTYPE");
    if (!lang || !*lang) lang = getenv("LANG");
    g_utf8 = lang && (strstr(lang, "UTF-8") || strstr(lang, "utf8") || strstr(lang, "UTF8")
                      || strstr(lang, "utf-8"));

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    hist_load();
    g_hpos = g_nhist;
    if (!term_raw()) {
        fprintf(stderr, "usrp-crx: can't set up the terminal\n");
        return 2;
    }
    try_connect();
    draw_all();

    while (!g_quit) {
        if (g_winch) {
            g_winch = 0;
            draw_all();
        }
        if (g_conn == RETRYING && now_ms() >= g_next_try)
            try_connect();
        draw_status();
        draw_prompt();
        if (fflush(stdout) != 0)
            break;   /* the terminal is gone */

        struct pollfd p[2] = {{.fd = STDIN_FILENO, .events = POLLIN},
                              {.fd = g_sock, .events = POLLIN}};
        int np = g_sock >= 0 ? 2 : 1;
        if (poll(p, (nfds_t)np, 1000) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR))
            on_keys();
        if (np == 2 && g_sock >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
            on_socket();
    }
    term_restore();
    return 0;
}

int main(int argc, char **argv)
{
    g_path = DEFAULT_SOCKET;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            g_path = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: usrp-crx [-s socket]            a session\n"
                    "       usrp-crx [-s socket] KEYS ...   one entry, for example: usrp-crx 123 01\n"
                    "exit status of one entry: 0 a command ran, 1 none did, 2 no connection\n");
            return strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    if (i == argc)
        return session();

    char entry[ENTRY + 1] = "";
    for (; i < argc; i++) {
        if (strlen(entry) + strlen(argv[i]) + 1 > ENTRY) {
            fprintf(stderr, "usrp-crx: over %d keys\n", ENTRY);
            return 2;
        }
        strcat(entry, argv[i]);
    }
    return one_shot(g_path, entry);
}
