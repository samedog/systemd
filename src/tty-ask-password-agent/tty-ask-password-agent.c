/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "alloc-util.h"
#include "ask-password-api.h"
#include "conf-parser.h"
#include "def.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "io-util.h"
#include "mkdir.h"
#include "path-util.h"
#include "process-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "util.h"
#include "utmp-wtmp.h"

static enum {
        ACTION_LIST,
        ACTION_QUERY,
        ACTION_WATCH,
        ACTION_WALL
} arg_action = ACTION_QUERY;

static bool arg_plymouth = false;
static bool arg_console = false;

static int ask_password_plymouth(
                const char *message,
                usec_t until,
                AskPasswordFlags flags,
                const char *flag_file,
                char ***ret) {

        _cleanup_close_ int fd = -1, notify = -1;
        union sockaddr_union sa = PLYMOUTH_SOCKET;
        _cleanup_free_ char *packet = NULL;
        ssize_t k;
        int r, n;
        struct pollfd pollfd[2] = {};
        char buffer[LINE_MAX];
        size_t p = 0;
        enum {
                POLL_SOCKET,
                POLL_INOTIFY
        };

        assert(ret);

        if (flag_file) {
                notify = inotify_init1(IN_CLOEXEC|IN_NONBLOCK);
                if (notify < 0)
                        return -errno;

                r = inotify_add_watch(notify, flag_file, IN_ATTRIB); /* for the link count */
                if (r < 0)
                        return -errno;
        }

        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0)
                return -errno;

        r = connect(fd, &sa.sa, offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sa.un.sun_path+1));
        if (r < 0)
                return -errno;

        if (flags & ASK_PASSWORD_ACCEPT_CACHED) {
                packet = strdup("c");
                n = 1;
        } else if (asprintf(&packet, "*\002%c%s%n", (int) (strlen(message) + 1), message, &n) < 0)
                packet = NULL;
        if (!packet)
                return -ENOMEM;

        r = loop_write(fd, packet, n + 1, true);
        if (r < 0)
                return r;

        pollfd[POLL_SOCKET].fd = fd;
        pollfd[POLL_SOCKET].events = POLLIN;
        pollfd[POLL_INOTIFY].fd = notify;
        pollfd[POLL_INOTIFY].events = POLLIN;

        for (;;) {
                int sleep_for = -1, j;

                if (until > 0) {
                        usec_t y;

                        y = now(CLOCK_MONOTONIC);

                        if (y > until) {
                                r = -ETIME;
                                goto finish;
                        }

                        sleep_for = (int) ((until - y) / USEC_PER_MSEC);
                }

                if (flag_file && access(flag_file, F_OK) < 0) {
                        r = -errno;
                        goto finish;
                }

                j = poll(pollfd, notify >= 0 ? 2 : 1, sleep_for);
                if (j < 0) {
                        if (errno == EINTR)
                                continue;

                        r = -errno;
                        goto finish;
                } else if (j == 0) {
                        r = -ETIME;
                        goto finish;
                }

                if (notify >= 0 && pollfd[POLL_INOTIFY].revents != 0)
                        flush_fd(notify);

                if (pollfd[POLL_SOCKET].revents == 0)
                        continue;

                k = read(fd, buffer + p, sizeof(buffer) - p);
                if (k < 0) {
                        if (errno == EINTR || errno == EAGAIN)
                                continue;

                        r = -errno;
                        goto finish;
                } else if (k == 0) {
                        r = -EIO;
                        goto finish;
                }

                p += k;

                if (p < 1)
                        continue;

                if (buffer[0] == 5) {

                        if (flags & ASK_PASSWORD_ACCEPT_CACHED) {
                                /* Hmm, first try with cached
                                 * passwords failed, so let's retry
                                 * with a normal password request */
                                packet = mfree(packet);

                                if (asprintf(&packet, "*\002%c%s%n", (int) (strlen(message) + 1), message, &n) < 0) {
                                        r = -ENOMEM;
                                        goto finish;
                                }

                                r = loop_write(fd, packet, n+1, true);
                                if (r < 0)
                                        goto finish;

                                flags &= ~ASK_PASSWORD_ACCEPT_CACHED;
                                p = 0;
                                continue;
                        }

                        /* No password, because UI not shown */
                        r = -ENOENT;
                        goto finish;

                } else if (buffer[0] == 2 || buffer[0] == 9) {
                        uint32_t size;
                        char **l;

                        /* One or more answers */
                        if (p < 5)
                                continue;

                        memcpy(&size, buffer+1, sizeof(size));
                        size = le32toh(size);
                        if (size + 5 > sizeof(buffer)) {
                                r = -EIO;
                                goto finish;
                        }

                        if (p-5 < size)
                                continue;

                        l = strv_parse_nulstr(buffer + 5, size);
                        if (!l) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        *ret = l;
                        break;

                } else {
                        /* Unknown packet */
                        r = -EIO;
                        goto finish;
                }
        }

        r = 0;

finish:
        memory_erase(buffer, sizeof(buffer));
        return r;
}

static int parse_password(const char *filename, char **wall) {
        _cleanup_free_ char *socket_name = NULL, *message = NULL, *packet = NULL;
        bool accept_cached = false, echo = false;
        size_t packet_length = 0;
        uint64_t not_after = 0;
        unsigned pid = 0;

        const ConfigTableItem items[] = {
                { "Ask", "Socket",       config_parse_string,   0, &socket_name   },
                { "Ask", "NotAfter",     config_parse_uint64,   0, &not_after     },
                { "Ask", "Message",      config_parse_string,   0, &message       },
                { "Ask", "PID",          config_parse_unsigned, 0, &pid           },
                { "Ask", "AcceptCached", config_parse_bool,     0, &accept_cached },
                { "Ask", "Echo",         config_parse_bool,     0, &echo          },
                {}
        };

        int r;

        assert(filename);

        r = config_parse(NULL, filename, NULL,
                         NULL,
                         config_item_table_lookup, items,
                         true, false, true, NULL);
        if (r < 0)
                return r;

        if (!socket_name) {
                log_error("Invalid password file %s", filename);
                return -EBADMSG;
        }

        if (not_after > 0 && now(CLOCK_MONOTONIC) > not_after)
                return 0;

        if (pid > 0 && !pid_is_alive(pid))
                return 0;

        if (arg_action == ACTION_LIST)
                printf("'%s' (PID %u)\n", message, pid);

        else if (arg_action == ACTION_WALL) {
                char *_wall;

                if (asprintf(&_wall,
                             "%s%sPassword entry required for \'%s\' (PID %u).\r\n"
                             "Please enter password with the systemd-tty-ask-password-agent tool!",
                             strempty(*wall),
                             *wall ? "\r\n\r\n" : "",
                             message,
                             pid) < 0)
                        return log_oom();

                free(*wall);
                *wall = _wall;

        } else {
                union sockaddr_union sa = {};
                _cleanup_close_ int socket_fd = -1;

                assert(arg_action == ACTION_QUERY ||
                       arg_action == ACTION_WATCH);

                if (access(socket_name, W_OK) < 0) {
                        if (arg_action == ACTION_QUERY)
                                log_info("Not querying '%s' (PID %u), lacking privileges.", message, pid);

                        return 0;
                }

                if (arg_plymouth) {
                        _cleanup_strv_free_erase_ char **passwords = NULL;

                        r = ask_password_plymouth(message, not_after, accept_cached ? ASK_PASSWORD_ACCEPT_CACHED : 0, filename, &passwords);
                        if (r >= 0) {
                                char **p;

                                packet_length = 1;
                                STRV_FOREACH(p, passwords)
                                        packet_length += strlen(*p) + 1;

                                packet = new(char, packet_length);
                                if (!packet)
                                        r = -ENOMEM;
                                else {
                                        char *d = packet + 1;

                                        STRV_FOREACH(p, passwords)
                                                d = stpcpy(d, *p) + 1;

                                        packet[0] = '+';
                                }
                        }

                } else {
                        _cleanup_string_free_erase_ char *password = NULL;
                        int tty_fd = -1;

                        if (arg_console) {
                                tty_fd = acquire_terminal("/dev/console", false, false, false, USEC_INFINITY);
                                if (tty_fd < 0)
                                        return log_error_errno(tty_fd, "Failed to acquire /dev/console: %m");

                                r = reset_terminal_fd(tty_fd, true);
                                if (r < 0)
                                        log_warning_errno(r, "Failed to reset terminal, ignoring: %m");
                        }

                        r = ask_password_tty(message, NULL, not_after, echo ? ASK_PASSWORD_ECHO : 0, filename, &password);

                        if (arg_console) {
                                tty_fd = safe_close(tty_fd);
                                release_terminal();
                        }

                        if (r >= 0) {
                                packet_length = 1 + strlen(password) + 1;
                                packet = new(char, packet_length);
                                if (!packet)
                                        r = -ENOMEM;
                                else {
                                        packet[0] = '+';
                                        strcpy(packet + 1, password);
                                }
                        }
                }

                if (IN_SET(r, -ETIME, -ENOENT)) {
                        /* If the query went away, that's OK */
                        r = 0;
                        goto finish;
                }
                if (r < 0) {
                        log_error_errno(r, "Failed to query password: %m");
                        goto finish;
                }

                socket_fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0);
                if (socket_fd < 0) {
                        r = log_error_errno(errno, "socket(): %m");
                        goto finish;
                }

                sa.un.sun_family = AF_UNIX;
                strncpy(sa.un.sun_path, socket_name, sizeof(sa.un.sun_path));

                r = sendto(socket_fd, packet, packet_length, MSG_NOSIGNAL, &sa.sa, offsetof(struct sockaddr_un, sun_path) + strlen(socket_name));
                memory_erase(packet, packet_length);
                if (r < 0)
                        return log_error_errno(errno, "Failed to send: %m");
        }

        return 0;

finish:
        memory_erase(packet, packet_length);
        return r;
}

static int wall_tty_block(void) {
        _cleanup_free_ char *p = NULL;
        dev_t devnr;
        int fd, r;

        r = get_ctty_devnr(0, &devnr);
        if (r == -ENXIO) /* We have no controlling tty */
                return -ENOTTY;
        if (r < 0)
                return log_error_errno(r, "Failed to get controlling TTY: %m");

        if (asprintf(&p, "/run/systemd/ask-password-block/%u:%u", major(devnr), minor(devnr)) < 0)
                return log_oom();

        mkdir_parents_label(p, 0700);
        mkfifo(p, 0600);

        fd = open(p, O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
        if (fd < 0)
                return log_debug_errno(errno, "Failed to open %s: %m", p);

        return fd;
}

static bool wall_tty_match(const char *path, void *userdata) {
        _cleanup_free_ char *p = NULL;
        _cleanup_close_ int fd = -1;
        struct stat st;

        if (!path_is_absolute(path))
                path = strjoina("/dev/", path);

        if (lstat(path, &st) < 0) {
                log_debug_errno(errno, "Failed to stat %s: %m", path);
                return true;
        }

        if (!S_ISCHR(st.st_mode)) {
                log_debug("%s is not a character device.", path);
                return true;
        }

        /* We use named pipes to ensure that wall messages suggesting
         * password entry are not printed over password prompts
         * already shown. We use the fact here that opening a pipe in
         * non-blocking mode for write-only will succeed only if
         * there's some writer behind it. Using pipes has the
         * advantage that the block will automatically go away if the
         * process dies. */

        if (asprintf(&p, "/run/systemd/ask-password-block/%u:%u", major(st.st_rdev), minor(st.st_rdev)) < 0) {
                log_oom();
                return true;
        }

        fd = open(p, O_WRONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
        if (fd < 0) {
                log_debug_errno(errno, "Failed top open the wall pipe: %m");
                return 1;
        }

        /* What, we managed to open the pipe? Then this tty is filtered. */
        return 0;
}

static int show_passwords(void) {
        _cleanup_closedir_ DIR *d;
        struct dirent *de;
        int r = 0;

        d = opendir("/run/systemd/ask-password");
        if (!d) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open /run/systemd/ask-password: %m");
        }

        FOREACH_DIRENT_ALL(de, d, return log_error_errno(errno, "Failed to read directory: %m")) {
                _cleanup_free_ char *p = NULL, *wall = NULL;
                int q;

                /* We only support /dev on tmpfs, hence we can rely on
                 * d_type to be reliable */

                if (de->d_type != DT_REG)
                        continue;

                if (hidden_file(de->d_name))
                        continue;

                if (!startswith(de->d_name, "ask."))
                        continue;

                p = strappend("/run/systemd/ask-password/", de->d_name);
                if (!p)
                        return log_oom();

                q = parse_password(p, &wall);
                if (q < 0 && r == 0)
                        r = q;

                if (wall)
                        (void) utmp_wall(wall, NULL, NULL, wall_tty_match, NULL);
        }

        return r;
}

static int watch_passwords(void) {
        enum {
                FD_INOTIFY,
                FD_SIGNAL,
                _FD_MAX
        };

        _cleanup_close_ int notify = -1, signal_fd = -1, tty_block_fd = -1;
        struct pollfd pollfd[_FD_MAX] = {};
        sigset_t mask;
        int r;

        tty_block_fd = wall_tty_block();

        (void) mkdir_p_label("/run/systemd/ask-password", 0755);

        notify = inotify_init1(IN_CLOEXEC);
        if (notify < 0)
                return log_error_errno(errno, "Failed to allocate directory watch: %m");

        if (inotify_add_watch(notify, "/run/systemd/ask-password", IN_CLOSE_WRITE|IN_MOVED_TO) < 0)
                return log_error_errno(errno, "Failed to add /run/systemd/ask-password to directory watch: %m");

        assert_se(sigemptyset(&mask) >= 0);
        assert_se(sigset_add_many(&mask, SIGINT, SIGTERM, -1) >= 0);
        assert_se(sigprocmask(SIG_SETMASK, &mask, NULL) >= 0);

        signal_fd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
        if (signal_fd < 0)
                return log_error_errno(errno, "Failed to allocate signal file descriptor: %m");

        pollfd[FD_INOTIFY].fd = notify;
        pollfd[FD_INOTIFY].events = POLLIN;
        pollfd[FD_SIGNAL].fd = signal_fd;
        pollfd[FD_SIGNAL].events = POLLIN;

        for (;;) {
                r = show_passwords();
                if (r < 0)
                        log_error_errno(r, "Failed to show password: %m");

                if (poll(pollfd, _FD_MAX, -1) < 0) {
                        if (errno == EINTR)
                                continue;

                        return -errno;
                }

                if (pollfd[FD_INOTIFY].revents != 0)
                        (void) flush_fd(notify);

                if (pollfd[FD_SIGNAL].revents != 0)
                        break;
        }

        return 0;
}

static void help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Process system password requests.\n\n"
               "  -h --help     Show this help\n"
               "     --version  Show package version\n"
               "     --list     Show pending password requests\n"
               "     --query    Process pending password requests\n"
               "     --watch    Continuously process password requests\n"
               "     --wall     Continuously forward password requests to wall\n"
               "     --plymouth Ask question with Plymouth instead of on TTY\n"
               "     --console  Ask question on /dev/console instead of current TTY\n",
               program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_LIST = 0x100,
                ARG_QUERY,
                ARG_WATCH,
                ARG_WALL,
                ARG_PLYMOUTH,
                ARG_CONSOLE,
                ARG_VERSION
        };

        static const struct option options[] = {
                { "help",     no_argument, NULL, 'h'          },
                { "version",  no_argument, NULL, ARG_VERSION  },
                { "list",     no_argument, NULL, ARG_LIST     },
                { "query",    no_argument, NULL, ARG_QUERY    },
                { "watch",    no_argument, NULL, ARG_WATCH    },
                { "wall",     no_argument, NULL, ARG_WALL     },
                { "plymouth", no_argument, NULL, ARG_PLYMOUTH },
                { "console",  no_argument, NULL, ARG_CONSOLE  },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_LIST:
                        arg_action = ACTION_LIST;
                        break;

                case ARG_QUERY:
                        arg_action = ACTION_QUERY;
                        break;

                case ARG_WATCH:
                        arg_action = ACTION_WATCH;
                        break;

                case ARG_WALL:
                        arg_action = ACTION_WALL;
                        break;

                case ARG_PLYMOUTH:
                        arg_plymouth = true;
                        break;

                case ARG_CONSOLE:
                        arg_console = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (optind != argc) {
                log_error("%s takes no arguments.", program_invocation_short_name);
                return -EINVAL;
        }

        return 1;
}

int main(int argc, char *argv[]) {
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (arg_console) {
                (void) setsid();
                (void) release_terminal();
        }

        if (IN_SET(arg_action, ACTION_WATCH, ACTION_WALL))
                r = watch_passwords();
        else
                r = show_passwords();

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
