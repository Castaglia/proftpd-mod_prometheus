/*
 * ProFTPD - mod_prometheus
 * Copyright (c) 2021 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * -----DO NOT EDIT BELOW THIS LINE-----
 * $Archive: mod_prometheus.a $
 * $Libraries: -lmicrohttpd -lsqlite3$
 */

#include "mod_prometheus.h"
#include "prometheus/db.h"
#include "prometheus/registry.h"
#include "prometheus/metric.h"
#include "prometheus/metric/db.h"
#include "prometheus/http.h"

/* Defaults */
#define PROMETHEUS_DEFAULT_EXPORTER_PORT	9273

extern xaset_t *server_list;

int prometheus_logfd = -1;
module prometheus_module;
pool *prometheus_pool = NULL;

static int prometheus_engine = FALSE;
static unsigned long prometheus_opts = 0UL;
static const char *prometheus_tables_dir = NULL;

static struct prom_dbh *prometheus_dbh = NULL;
static struct prom_registry *prometheus_registry = NULL;
static struct prom_http *prometheus_exporter_http = NULL;
static pid_t prometheus_exporter_pid = 0;

/* Number of seconds to wait for the exporter process to stop before
 * we terminate it with extreme prejudice.
 *
 * Currently this has a granularity of seconds; needs to be in millsecs
 * (e.g. for 500 ms timeout).
 */
static time_t prometheus_exporter_timeout = 1;

/* Used for tracking download, upload byte totals. */
static off_t prometheus_retr_bytes = 0, prometheus_stor_bytes = 0;

static const char *trace_channel = "prometheus";

static int prom_mkdir(const char *dir, uid_t uid, gid_t gid, mode_t mode) {
  mode_t prev_mask;
  struct stat st;
  int res = -1;

  pr_fs_clear_cache2(dir);
  res = pr_fsio_stat(dir, &st);

  if (res == -1 &&
      errno != ENOENT) {
    return -1;
  }

  /* The directory already exists. */
  if (res == 0) {
    return 0;
  }

  /* The given mode is absolute, not subject to any Umask setting. */
  prev_mask = umask(0);

  if (pr_fsio_mkdir(dir, mode) < 0) {
    int xerrno = errno;

    (void) umask(prev_mask);
    errno = xerrno;
    return -1;
  }

  umask(prev_mask);

  if (pr_fsio_chown(dir, uid, gid) < 0) {
    return -1;
  }

  return 0;
}

static int prom_mkpath(pool *p, const char *path, uid_t uid, gid_t gid,
    mode_t mode) {
  char *currpath = NULL, *tmppath = NULL;
  struct stat st;

  pr_fs_clear_cache2(path);
  if (pr_fsio_stat(path, &st) == 0) {
    /* Path already exists, nothing to be done. */
    errno = EEXIST;
    return -1;
  }

  tmppath = pstrdup(p, path);

  currpath = "/";
  while (tmppath && *tmppath) {
    char *currdir = strsep(&tmppath, "/");
    currpath = pdircat(p, currpath, currdir, NULL);

    if (prom_mkdir(currpath, uid, gid, mode) < 0) {
      return -1;
    }

    pr_signals_handle();
  }

  return 0;
}

static int prom_openlog(void) {
  int res = 0;
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "PrometheusLog", FALSE);
  if (c != NULL) {
    const char *path;

    path = c->argv[0];

    if (strncasecmp(path, "none", 5) != 0) {
      int xerrno;

      pr_signals_block();
      PRIVS_ROOT
      res = pr_log_openfile(path, &prometheus_logfd, 0600);
      xerrno = errno;
      PRIVS_RELINQUISH
      pr_signals_unblock();

      if (res < 0) {
        if (res == -1) {
          pr_log_pri(PR_LOG_NOTICE, MOD_PROMETHEUS_VERSION
            ": notice: unable to open PrometheusLog '%s': %s", path,
            strerror(xerrno));

        } else if (res == PR_LOG_WRITABLE_DIR) {
          pr_log_pri(PR_LOG_WARNING, MOD_PROMETHEUS_VERSION
            ": notice: unable to open PrometheusLog '%s': parent directory is "
            "world-writable", path);

        } else if (res == PR_LOG_SYMLINK) {
          pr_log_pri(PR_LOG_WARNING, MOD_PROMETHEUS_VERSION
            ": notice: unable to open PrometheusLog '%s': cannot log to "
            "a symlink", path);
        }
      }
    }
  }

  return res;
}

/* We don't want to do the full daemonize() as provided in main.c; we
 * already forked.
 */
static void prom_daemonize(const char *daemon_dir) {
#ifndef HAVE_SETSID
  int tty_fd;
#endif

#ifdef HAVE_SETSID
  /* setsid() is the preferred way to disassociate from the
   * controlling terminal
   */
  setsid();
#else
  /* Open /dev/tty to access our controlling tty (if any) */
  tty_fd = open("/dev/tty", O_RDWR);
  if (tty_fd != -1) {
    if (ioctl(tty_fd, TIOCNOTTY, NULL) == -1) {
      perror("ioctl");
      exit(1);
    }

    close(tty_fd);
  }
#endif /* HAVE_SETSID */

  /* Close the three big boys. */
  close(fileno(stdin));
  close(fileno(stdout));
  close(fileno(stderr));

  /* Portable way to prevent re-acquiring a tty in the future */

#if defined(HAVE_SETPGID)
  setpgid(0, getpid());

#else
# if defined(SETPGRP_VOID)
  setpgrp();

# else
  setpgrp(0, getpid());
# endif /* SETPGRP_VOID */
#endif /* HAVE_SETPGID */

  pr_fsio_chdir(daemon_dir, 0);
}

static pid_t prom_exporter_start(pool *p, const pr_netaddr_t *exporter_addr) {
  pid_t exporter_pid;
  struct prom_dbh *dbh;
  char *exporter_chroot = NULL;

  exporter_pid = fork();
  switch (exporter_pid) {
    case -1:
      pr_log_pri(PR_LOG_ALERT,
        MOD_PROMETHEUS_VERSION ": unable to fork: %s", strerror(errno));
      return 0;

    case 0:
      /* We're the child. */
      break;

    default:
      /* We're the parent. */
      return exporter_pid;
  }

  /* Reset the cached PID, so that it is correctly reflected in the logs. */
  session.pid = getpid();

  pr_trace_msg(trace_channel, 3, "forked exporter PID %lu",
    (unsigned long) session.pid);

  prom_daemonize(prometheus_tables_dir);

  /* Install our own signal handlers (mostly to ignore signals) */
  (void) signal(SIGALRM, SIG_IGN);
  (void) signal(SIGHUP, SIG_IGN);
  (void) signal(SIGUSR1, SIG_IGN);
  (void) signal(SIGUSR2, SIG_IGN);

  /* Remove our event listeners. */
  pr_event_unregister(&prometheus_module, NULL, NULL);

  /* Close any database handle inherited from our parent, and open a new
   * one, per SQLite3 recommendation.
   */
  (void) prom_db_close(prometheus_pool, prometheus_dbh);
  prometheus_dbh = NULL;
  dbh = prom_metric_db_open(prometheus_pool, prometheus_tables_dir);
  if (dbh == NULL) {
    pr_trace_msg(trace_channel, 3, "exporter error opening '%s' database: %s",
      prometheus_tables_dir, strerror(errno));
  }

  if (prom_registry_set_dbh(prometheus_registry, dbh) < 0) {
    pr_trace_msg(trace_channel, 3, "exporter error setting registry dbh: %s",
      strerror(errno));
  }

  PRIVS_ROOT
  if (getuid() == PR_ROOT_UID) {
    int res;

    /* Chroot to the PrometheusTables/empty/ directory before dropping
     * root privs.
     */
    exporter_chroot = pdircat(prometheus_pool, prometheus_tables_dir, "empty",
      NULL);
    res = chroot(exporter_chroot);
    if (res < 0) {
      int xerrno = errno;

      PRIVS_RELINQUISH
 
      (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
        "unable to chroot to PrometheusTables/empty/ directory '%s': %s",
        exporter_chroot, strerror(xerrno));
      exit(0);
    }

    if (chdir("/") < 0) {
      int xerrno = errno;

      PRIVS_RELINQUISH

      (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
        "unable to chdir to root directory within chroot: %s",
        strerror(xerrno));
      exit(0);
    }
  }

  pr_proctitle_set("(listening for Prometheus requests)");

  /* Make the exporter process have the identity of the configured daemon
   * User/Group.
   */
  session.uid = geteuid();
  session.gid = getegid();
  PRIVS_REVOKE

  prometheus_exporter_http = prom_http_start(p, exporter_addr,
    prometheus_registry);
  if (prometheus_exporter_http == NULL) {
    return 0;
  }

  if (exporter_chroot != NULL) {
    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "exporter process running with UID %s, GID %s, restricted to '%s'",
      pr_uid2str(prometheus_pool, getuid()),
      pr_gid2str(prometheus_pool, getgid()), exporter_chroot);

  } else {
    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "exporter process running with UID %s, GID %s, located in '%s'",
      pr_uid2str(prometheus_pool, getuid()),
      pr_gid2str(prometheus_pool, getgid()), getcwd(NULL, 0));
  }

  /* This function will exit once the exporter finishes. */
  prom_http_run_loop(p, prometheus_exporter_http);

  pr_trace_msg(trace_channel, 3, "exporter PID %lu exiting",
    (unsigned long) session.pid);
  exit(0);
}

static void prom_exporter_stop(pid_t exporter_pid) {
  int res, status;
  time_t start_time = time(NULL);

  if (exporter_pid == 0) {
    /* Nothing to do. */
    return;
  }

  pr_trace_msg(trace_channel, 3, "stopping exporter PID %lu",
    (unsigned long) exporter_pid);

  /* Litmus test: is the exporter process still around?  If not, there's
   * nothing for us to do.
   */
  res = kill(exporter_pid, 0);
  if (res < 0 &&
      errno == ESRCH) {
    return;
  }

  if (prom_http_stop(prometheus_pool, prometheus_exporter_http) < 0) {
    pr_trace_msg(trace_channel, 3, "error stopping exporter http listener: %s",
      strerror(errno));
  }

  res = kill(exporter_pid, SIGTERM);
  if (res < 0) {
    int xerrno = errno;

    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "error sending SIGTERM (signal %d) to exporter process ID %lu: %s",
      SIGTERM, (unsigned long) exporter_pid, strerror(xerrno));
  }

  /* Poll every 500 millsecs. */
  pr_timer_usleep(500 * 1000);

  res = waitpid(exporter_pid, &status, WNOHANG);
  while (res <= 0) {
    if (res < 0) {
      if (errno == EINTR) {
        pr_signals_handle();
        continue;
      }

      if (errno == ECHILD) {
        /* XXX Maybe we shouldn't be using waitpid(2) here, since the
         * main SIGCHLD handler may handle the termination of the exporter
         * process?
         */

        return;
      }

      if (errno != EINTR) {
        (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
          "error waiting for exporter process ID %lu: %s",
          (unsigned long) exporter_pid, strerror(errno));
        status = -1;
        break;
      }
    }

    /* Check the time elapsed since we started. */
    if ((time(NULL) - start_time) > prometheus_exporter_timeout) {
      (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
        "exporter process ID %lu took longer than timeout (%lu secs) to "
        "stop, sending SIGKILL (signal %d)", (unsigned long) exporter_pid,
        prometheus_exporter_timeout, SIGKILL);
      res = kill(exporter_pid, SIGKILL);
      if (res < 0) {
        (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
         "error sending SIGKILL (signal %d) to exporter process ID %lu: %s",
         SIGKILL, (unsigned long) exporter_pid, strerror(errno));
      }

      break;
    }

    /* Poll every 500 millsecs. */
    pr_timer_usleep(500 * 1000);
  }

  if (WIFEXITED(status)) {
    int exit_status;

    exit_status = WEXITSTATUS(status);
    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "exporter process ID %lu terminated normally, with exit status %d",
      (unsigned long) exporter_pid, exit_status);
  }

  if (WIFSIGNALED(status)) {
    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "exporter process ID %lu died from signal %d",
      (unsigned long) exporter_pid, WTERMSIG(status));

    if (WCOREDUMP(status)) {
      (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
        "exporter process ID %lu created a coredump",
        (unsigned long) exporter_pid);
    }
  }

  exporter_pid = 0;
  prometheus_exporter_http = NULL;
}

static pr_table_t *prom_get_labels(pool *p) {
  pr_table_t *labels;

  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add(labels, "protocol", pr_session_get_protocol(0), 0);

  return labels;
}

static void prom_event_incr(const char *metric_name, int32_t incr, ...) {
  pool *tmp_pool;
  int res;
  va_list ap;
  char *key;
  const struct prom_metric *metric;
  pr_table_t *labels;

  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric == NULL) {
    pr_trace_msg(trace_channel, 17, "unknown metric name '%s' requested",
      metric_name);
    return;
  }

  if (session.pool != NULL) {
    tmp_pool = make_sub_pool(session.pool);

  } else {
    tmp_pool = make_sub_pool(prometheus_pool);
  }

  labels = prom_get_labels(tmp_pool);
  va_start(ap, incr);
  key = va_arg(ap, char *);
  while (key != NULL) {
    char *val;

    pr_signals_handle();

    val = va_arg(ap, char *);
    (void) pr_table_add_dup(labels, key, val, 0);

    key = va_arg(ap, char *);
  }
  va_end(ap);

  if (incr >= 0) {
    res = prom_metric_incr(tmp_pool, metric, incr, labels);

  } else {
    res = prom_metric_decr(tmp_pool, metric, -incr, labels);
  }

  if (res < 0) {
    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "error %s %s: %s",
      incr < 0 ? "decrementing" : "incrementing", metric_name,
      strerror(errno));
  }

  destroy_pool(tmp_pool);
}

/* Configuration handlers
 */

/* usage: PrometheusEngine on|off */
MODRET set_prometheusengine(cmd_rec *cmd) {
  int engine = -1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: PrometheusExporter address[:port] */
MODRET set_prometheusexporter(cmd_rec *cmd) {
  char *addr, *ptr;
  size_t addrlen;
  config_rec *c;
  pr_netaddr_t *exporter_addr;
  int exporter_port = PROMETHEUS_DEFAULT_EXPORTER_PORT;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  c = add_config_param(cmd->argv[0], 1, NULL);

  /* Separate the port out from the address, if present. */
  ptr = strrchr(cmd->argv[1], ':');
  if (ptr != NULL) {
    char *ptr2;

    /* We need to handle the following possibilities:
     *
     *  ipv4-addr
     *  ipv4-addr:port
     *  [ipv6-addr]
     *  [ipv6-addr]:port
     *
     * Thus we check to see if the last ':' occurs before, or after,
     * a ']' for an IPv6 address.
     */

    ptr2 = strrchr(cmd->argv[1], ']');
    if (ptr2 != NULL) {
      if (ptr2 > ptr) {
        /* The found ':' is part of an IPv6 address, not a port delimiter. */
        ptr = NULL;
      }
    }

    if (ptr != NULL) {
      *ptr = '\0';

      exporter_port = atoi(ptr + 1);
      if (exporter_port < 1 ||
          exporter_port > 65535) {
        CONF_ERROR(cmd, "port must be between 1-65535");
      }
    }
  }

  addr = cmd->argv[1];
  addrlen = strlen(addr);

  /* Make sure we can handle an IPv6 address here, e.g.:
   *
   *   [::1]:162
   */
  if (addrlen > 0 &&
      (addr[0] == '[' && addr[addrlen-1] == ']')) {
    addr = pstrndup(cmd->pool, addr + 1, addrlen - 2);
  }

  /* Watch for wildcard addresses. */
  if (strcmp(addr, "0.0.0.0") == 0) {
    exporter_addr = pr_netaddr_alloc(c->pool);
    pr_netaddr_set_family(exporter_addr, AF_INET);
    pr_netaddr_set_sockaddr_any(exporter_addr);

#if defined(PR_USE_IPV6)
  } else if (strcmp(addr, "::") == 0) {
    exporter_addr = pr_netaddr_alloc(c->pool);
    pr_netaddr_set_family(exporter_addr, AF_INET6);
    pr_netaddr_set_sockaddr_any(exporter_addr);
#endif /* PR_USE_IPV6 */

  } else {
    exporter_addr = (pr_netaddr_t *) pr_netaddr_get_addr(c->pool, addr, NULL);
    if (exporter_addr == NULL) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "unable to resolve \"", addr, "\"", NULL));
    }
  }

  pr_netaddr_set_port2(exporter_addr, exporter_port);
  c->argv[0] = exporter_addr;
 
  return PR_HANDLED(cmd);
}

/* usage: PrometheusLog path|"none" */
MODRET set_prometheuslog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: PrometheusOptions opt1 ... optN */
MODRET set_prometheusoptions(cmd_rec *cmd) {
  config_rec *c = NULL;
  register unsigned int i;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown PrometheusOption '",
      cmd->argv[i], "'", NULL));
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;
 
  return PR_HANDLED(cmd);
}

/* usage: PrometheusTables path */
MODRET set_prometheustables(cmd_rec *cmd) {
  int res;
  struct stat st;
  char *path;
 
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT);

  path = cmd->argv[1]; 
  if (*path != '/') {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "must be a full path: '", path, "'",
      NULL));
  }

  res = stat(path, &st);
  if (res < 0) {
    char *exporter_chroot;

    if (errno != ENOENT) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to stat '", path, "': ",
        strerror(errno), NULL));
    }

    pr_log_debug(DEBUG0, MOD_PROMETHEUS_VERSION
      ": PrometheusTables directory '%s' does not exist, creating it", path);

    /* Create the directory. */
    res = prom_mkpath(cmd->tmp_pool, path, geteuid(), getegid(), 0755);
    if (res < 0) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create directory '",
        path, "': ", strerror(errno), NULL));
    }

    /* Also create the empty/ directory underneath, for the chroot. */
    exporter_chroot = pdircat(cmd->tmp_pool, path, "empty", NULL);

    res = prom_mkpath(cmd->tmp_pool, exporter_chroot, geteuid(), getegid(),
      0111);
    if (res < 0) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create directory '",
        exporter_chroot, "': ", strerror(errno), NULL));
    }

    pr_log_debug(DEBUG2, MOD_PROMETHEUS_VERSION
      ": created PrometheusTables directory '%s'", path);

  } else {
    char *exporter_chroot;

    if (!S_ISDIR(st.st_mode)) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use '", path,
        ": Not a directory", NULL));
    }

    /* See if the chroot directory empty/ already exists as well.  And enforce
     * the permissions on that directory.
     */
    exporter_chroot = pdircat(cmd->tmp_pool, path, "empty", NULL);

    res = stat(exporter_chroot, &st);
    if (res < 0) {
      if (errno != ENOENT) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to stat '",
          exporter_chroot, "': ", strerror(errno), NULL));
      }

      res = prom_mkpath(cmd->tmp_pool, exporter_chroot, geteuid(), getegid(),
        0111);
      if (res < 0) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to create directory '",
          exporter_chroot, "': ", strerror(errno), NULL));
      }

    } else {
      mode_t dir_mode, expected_mode;

      dir_mode = st.st_mode;
      dir_mode &= ~S_IFMT;
      expected_mode = (S_IXUSR|S_IXGRP|S_IXOTH);

      if (dir_mode != expected_mode) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "directory '", exporter_chroot,
          "' has incorrect permissions (not 0111 as required)", NULL));
      }
    }
  }

  (void) add_config_param_str(cmd->argv[0], 1, path);
  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET prom_pre_list(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  metric_name = "directory_list";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;

    labels = prom_get_labels(cmd->tmp_pool);
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_log_list(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  metric_name = "directory_list";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;

    labels = prom_get_labels(cmd->tmp_pool);
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_err_list(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;
  pr_table_t *labels;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  labels = prom_get_labels(cmd->tmp_pool);

  metric_name = "directory_list";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  metric_name = "directory_list_error";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    /* TODO: Add a reason label for the error? */
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_pre_user(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  metric_name = "login";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;

    labels = prom_get_labels(cmd->tmp_pool);
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_log_pass(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  metric_name = "login";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;

    labels = prom_get_labels(cmd->tmp_pool);
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_err_login(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;
  pr_table_t *labels;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  labels = prom_get_labels(cmd->tmp_pool);

  metric_name = "login";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  metric_name = "login_error";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    /* TODO: Add a reason label for the error? */
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_pre_retr(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  metric_name = "file_download";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;

    labels = prom_get_labels(cmd->tmp_pool);
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_log_retr(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;
  pr_table_t *labels;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  labels = prom_get_labels(cmd->tmp_pool);

  metric_name = "file_download";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  /* We also need to increment the KB download count.  We know the number
   * of bytes downloaded as an off_t here, but we only store the number of KB
   * in the mod_prometheus db tables.
   * 
   * We could just increment by xfer_bytes / 1024, but that would mean that
   * several small files of say 999 bytes could be downloaded, and the KB
   * count would not be incremented.
   *
   * To deal with this situation, we use the prometheus_retr_bytes static
   * variable as a "holding bucket" of bytes, from which we get the KB to add
   * to the db tables.
   */

  metric_name = "file_download_bytes";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    uint32_t retr_kb;
    off_t rem_bytes;

    prometheus_retr_bytes += session.xfer.total_bytes;
    retr_kb = (prometheus_retr_bytes / 1024);
    rem_bytes = (prometheus_retr_bytes % 1024);
    prometheus_retr_bytes = rem_bytes;

    /* TODO: Update with the histogram function, once implemented. */
    prom_metric_incr(cmd->tmp_pool, metric, retr_kb, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_err_retr(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;
  pr_table_t *labels;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  labels = prom_get_labels(cmd->tmp_pool);

  metric_name = "file_download";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  metric_name = "file_download_error";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    /* TODO: Add a reason label for the error? */
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_pre_stor(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  metric_name = "file_upload";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;

    labels = prom_get_labels(cmd->tmp_pool);
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_log_stor(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;
  pr_table_t *labels;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  labels = prom_get_labels(cmd->tmp_pool);

  metric_name = "file_upload";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  /* We also need to increment the KB upload count.  We know the number
   * of bytes downloaded as an off_t here, but we only store the number of KB
   * in the mod_prometheus db tables.
   * 
   * We could just increment by xfer_bytes / 1024, but that would mean that
   * several small files of say 999 bytes could be uploaded, and the KB
   * count would not be incremented.
   *
   * To deal with this situation, we use the prometheus_stor_bytes static
   * variable as a "holding bucket" of bytes, from which we get the KB to add
   * to the db tables.
   */

  metric_name = "file_upload_bytes";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    uint32_t stor_kb;
    off_t rem_bytes;

    prometheus_stor_bytes += session.xfer.total_bytes;
    stor_kb = (prometheus_stor_bytes / 1024);
    rem_bytes = (prometheus_stor_bytes % 1024);
    prometheus_stor_bytes = rem_bytes;

    /* TODO: Update with the histogram function, once implemented. */
    prom_metric_incr(cmd->tmp_pool, metric, stor_kb, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_err_stor(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;
  pr_table_t *labels;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  labels = prom_get_labels(cmd->tmp_pool);

  metric_name = "file_upload";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    prom_metric_decr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  metric_name = "file_upload_error";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    /* TODO: Add a reason label for the error? */
    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

MODRET prom_log_auth(cmd_rec *cmd) {
  const char *metric_name;
  const struct prom_metric *metric;

  if (prometheus_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* Note: we are not currently properly incrementing
   * session{protocol="ftps"} for FTPS connections accepted using the
   * UseImplicitSSL TLSOption.
   *
   * The issue is that for those connections, the protocol will be set to
   * "ftps" in mod_tls' sess_init callback.  But here in mod_prometheus, we
   * are not guaranteed to being called AFTER mod_tls, due to module load
   * ordering.  Thus we do not have a good way of determining when to
   * increment those counts for implicit FTPS connections.
   */

  metric_name = "tls_protocol";
  metric = prom_registry_get_metric(prometheus_registry, metric_name);
  if (metric != NULL) {
    pr_table_t *labels;
    const char *tls_version;

    labels = prom_get_labels(cmd->tmp_pool);

    tls_version = pr_table_get(session.notes, "TLS_PROTOCOL", NULL);
    if (tls_version == NULL) {
      /* Try the environment. */
      tls_version = pr_env_get(cmd->tmp_pool, "TLS_PROTOCOL");
    }

    if (tls_version != NULL) {
      (void) pr_table_add_dup(labels, "version", tls_version, 0);
    }

    prom_metric_incr(cmd->tmp_pool, metric, 1, labels);

  } else {
    pr_trace_msg(trace_channel, 19, "%s: unknown '%s' metric requested",
      (char *) cmd->argv[0], metric_name);
  }

  return PR_DECLINED(cmd);
}

/* Event listeners
 */

static void prom_auth_code_ev(const void *event_data, void *user_data) {
  int auth_code;

  if (prometheus_engine == FALSE) {
    return;
  }

  auth_code = *((int *) event_data);

  switch (auth_code) {
    case PR_AUTH_RFC2228_OK:
      prom_event_incr("login", 1, "method", "certificate", NULL);
      break;

    case PR_AUTH_OK:
      prom_event_incr("login", 1, "method", "password", NULL);
      break;

    case PR_AUTH_NOPWD:
      prom_event_incr("login_error", 1, "reason", "unknown user", NULL);
      break;

    case PR_AUTH_BADPWD:
      prom_event_incr("login_error", 1, "reason", "bad password", NULL);
      break;

    default:
      prom_event_incr("login_error", 1, NULL);
      break;
  }
}

static void prom_exit_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  switch (session.disconnect_reason) {
    case PR_SESS_DISCONNECT_BANNED:
    case PR_SESS_DISCONNECT_CONFIG_ACL:
    case PR_SESS_DISCONNECT_MODULE_ACL:
    case PR_SESS_DISCONNECT_SESSION_INIT_FAILED: {
      const void *reason;

      reason = pr_table_get(session.notes, "core.disconnect-details", NULL);
      if (reason != NULL) {
        prom_event_incr("connection_refused", 1, "reason", reason, NULL);

      } else {
        prom_event_incr("connection_refused", 1, NULL);
      }
      break;
    }

    case PR_SESS_DISCONNECT_SEGFAULT:
      prom_event_incr("segfault", 1, NULL);
      break;

    default: {
      prom_event_incr("session", -1, NULL);
      break;
    }
  }

  prom_http_free();

  if (prometheus_logfd >= 0) {
    (void) close(prometheus_logfd);
    prometheus_logfd = -1;
  }
}

#if defined(PR_SHARED_MODULE)
static void prom_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp((const char *) event_data, "mod_prometheus.c") != 0) {
    return;
  }

  /* Unregister ourselves from all events. */
  pr_event_unregister(&prometheus_module, NULL, NULL);

  (void) prom_db_close(prometheus_pool, prometheus_dbh);
  promtheus_dbh = NULL;
  prometheus_exporter_http = NULL;

  (void) prom_registry_free(prometheus_registry);
  prometheus_registry = NULL;
  prometheus_tables_dir = NULL;

  destroy_pool(prometheus_pool);
  prometheus_pool = NULL;

  (void) close(prometheus_logfd);
  prometheus_logfd = -1;
}
#endif /* PR_SHARED_MODULE */

static void create_session_metrics(pool *p, struct prom_dbh *dbh) {
  int res;
  struct prom_metric *metric;

  /* Session metrics:
   *
   *  directory_list
   *  directory_list_error
   *  file_download
   *  file_download_error
   *  file_upload
   *  file_upload_error
   *  login
   *  login_error
   *  timeout
   *  handshake_error
   *  tls_protocol
   *  sftp_protocol
   */

  metric = prom_metric_create(prometheus_pool, "directory_list", dbh);
  prom_metric_add_counter(metric, "total", "Number of directory listings");
  prom_metric_add_gauge(metric, "count", "Current count of directory listings");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "directory_list_error", dbh);
  prom_metric_add_counter(metric, "total",
    "Number of failed directory listings");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "file_download", dbh);
  prom_metric_add_counter(metric, "total", "Number of file downloads");
  prom_metric_add_gauge(metric, "count", "Current count of file downloads");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "file_download_error", dbh);
  prom_metric_add_counter(metric, "total", "Number of failed file downloads");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "file_upload", dbh);
  prom_metric_add_counter(metric, "total", "Number of file uploads");
  prom_metric_add_gauge(metric, "count", "Current count of file uploads");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "file_upload_error", dbh);
  prom_metric_add_counter(metric, "total", "Number of failed file uploads");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "login", dbh);
  prom_metric_add_counter(metric, "total", "Number of logins");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "login_error", dbh);
  prom_metric_add_counter(metric, "total", "Number of failed logins");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "timeout", dbh);
  prom_metric_add_counter(metric, "total", "Number of timeouts");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "handshake_error", dbh);
  prom_metric_add_counter(metric, "total",
    "Number of failed SFTP/TLS handshakes");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "sftp_protocol", dbh);
  prom_metric_add_counter(metric, NULL,
    "Number of SFTP sessions by protocol version");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "tls_protocol", dbh);
  prom_metric_add_counter(metric, NULL,
    "Number of TLS sessions by protocol version");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }
}

static void create_server_metrics(pool *p, struct prom_dbh *dbh) {
  int res;
  struct prom_metric *metric;

  /* Server metrics:
   *
   *  connection_refused
   *  log_message
   *  segfault
   *  session
   */

  metric = prom_metric_create(prometheus_pool, "connection_refused", dbh);
  prom_metric_add_counter(metric, "total", "Number of refused connections");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "log_message", dbh);
  prom_metric_add_counter(metric, "total", "Number of log_messages");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "segfault", dbh);
  prom_metric_add_counter(metric, "total", "Number of segfaults");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }

  metric = prom_metric_create(prometheus_pool, "session", dbh);
  prom_metric_add_counter(metric, "total", "Number of sessions");
  prom_metric_add_gauge(metric, "count", "Current count of sessions");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));
  }
}

static void create_metrics(struct prom_dbh *dbh) {
  pool *tmp_pool;
  int res;
  struct prom_metric *metric;

  tmp_pool = make_sub_pool(prometheus_pool);
  pr_pool_tag(tmp_pool, "Prometheus metrics creation pool");

  metric = prom_metric_create(prometheus_pool, "build_info", dbh);
  prom_metric_add_counter(metric, NULL, "ProFTPD build information");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));

  } else {
    pr_table_t *labels;

    labels = pr_table_nalloc(tmp_pool, 0, 2);
    (void) pr_table_add_dup(labels, "proftpd_version", pr_version_get_str(), 0);
    (void) pr_table_add_dup(labels, "mod_prometheus_version",
      MOD_PROMETHEUS_VERSION, 0);

    res = prom_metric_incr(tmp_pool, metric, 1, labels);
    if (res <  0) {
      pr_trace_msg(trace_channel, 3, "error incrementing metric '%s': %s",
        prom_metric_get_name(metric), strerror(errno));
    }
  }

  metric = prom_metric_create(prometheus_pool, "startup_time_seconds", dbh);
  prom_metric_add_counter(metric, NULL,
    "ProFTPD startup time, in unixtime seconds");
  res = prom_registry_add_metric(prometheus_registry, metric);
  if (res < 0) {
    pr_trace_msg(trace_channel, 1, "error registering metric '%s': %s",
      prom_metric_get_name(metric), strerror(errno));

  } else {
    time_t now;

    now = time(NULL);
    res = prom_metric_incr(tmp_pool, metric, now, NULL);
    if (res <  0) {
      pr_trace_msg(trace_channel, 3, "error incrementing metric '%s': %s",
        prom_metric_get_name(metric), strerror(errno));
    }
  }

  create_server_metrics(tmp_pool, dbh);
  create_session_metrics(tmp_pool, dbh);

  res = prom_registry_sort_metrics(prometheus_registry);
  if (res < 0) {
    pr_trace_msg(trace_channel, 3, "error sorting registry metrics: %s",
      strerror(errno));
  }

  destroy_pool(tmp_pool);
}

static void prom_postparse_ev(const void *event_data, void *user_data) {
  config_rec *c;
  pr_netaddr_t *exporter_addr;

  c = find_config(main_server->conf, CONF_PARAM, "PrometheusEngine", FALSE);
  if (c != NULL) {
    prometheus_engine = *((int *) c->argv[0]);
  }

  if (prometheus_engine == FALSE) {
    return;
  }

  prom_openlog();

  c = find_config(main_server->conf, CONF_PARAM, "PrometheusOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    prometheus_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "PrometheusOptions", FALSE);
  }

  c = find_config(main_server->conf, CONF_PARAM, "PrometheusTables", FALSE);
  if (c == NULL) {
    /* No PrometheusTables configured, mod_prometheus cannot run. */
    (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
      "no PrometheusTables configured, disabling module");

    prometheus_engine = FALSE;
    return;
  }

  prometheus_tables_dir = c->argv[0];
  prometheus_dbh = prom_metric_init(prometheus_pool, prometheus_tables_dir);
  if (prometheus_dbh == NULL) {
    pr_log_pri(PR_LOG_WARNING, MOD_PROMETHEUS_VERSION
      ": unable to initialize metrics, failing to start up: %s",
      strerror(errno));
    pr_session_disconnect(&prometheus_module, PR_SESS_DISCONNECT_BAD_CONFIG,
      "Failed metrics initialization");
  }

  prometheus_registry = prom_registry_init(prometheus_pool, "proftpd");

  /* Create our known metrics, and register them. */
  create_metrics(prometheus_dbh);

  c = find_config(main_server->conf, CONF_PARAM, "PrometheusExporter", FALSE);
  if (c == NULL) {
    prometheus_engine = FALSE;
    pr_log_debug(DEBUG0, MOD_PROMETHEUS_VERSION
      ": missing required PrometheusExporter directive, disabling module");

    prom_metric_free(prometheus_pool, prometheus_dbh);
    prometheus_dbh = NULL;

    prom_registry_free(prometheus_registry);
    prometheus_registry = NULL;

    return;
  }

  if (prom_http_init(prometheus_pool) < 0) {
    prom_metric_free(prometheus_pool, prometheus_dbh);
    prometheus_dbh = NULL;

    prom_registry_free(prometheus_registry);
    prometheus_registry = NULL;

    pr_log_pri(PR_LOG_ERR, MOD_PROMETHEUS_VERSION
      ": unable to initialize HTTP API, failing to start up: %s",
      strerror(errno));
    pr_session_disconnect(&prometheus_module, PR_SESS_DISCONNECT_BAD_CONFIG,
      "Failed HTTP initialization");
  }

  exporter_addr = c->argv[0];

  prometheus_exporter_pid = prom_exporter_start(prometheus_pool, exporter_addr);
  if (prometheus_exporter_pid == 0) {
    prometheus_engine = FALSE;
    pr_log_debug(DEBUG0, MOD_PROMETHEUS_VERSION
      ": failed to start exporter process, disabling module");

    prom_metric_free(prometheus_pool, prometheus_dbh);
    prometheus_dbh = NULL;

    prom_registry_free(prometheus_registry);
    prometheus_registry = NULL;
  }
}

static void prom_restart_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  pr_trace_msg(trace_channel, 17,
    "restart event received, resetting counters");

  prom_exporter_stop(prometheus_exporter_pid);

  (void) prom_db_close(prometheus_pool, prometheus_dbh);
  prometheus_dbh = NULL;
  prometheus_exporter_http = NULL;

  (void) prom_registry_free(prometheus_registry);
  prometheus_registry = NULL;
  prometheus_tables_dir = NULL;

  /* Close the PrometheusLog file descriptor; it will be reopened in the
   * postparse event listener.
   */
  (void) close(prometheus_logfd);
  prometheus_logfd = -1;
}

static void prom_shutdown_ev(const void *event_data, void *user_data) {
  prom_exporter_stop(prometheus_exporter_pid);

  (void) prom_db_close(prometheus_pool, prometheus_dbh);
  prometheus_dbh = NULL;

  destroy_pool(prometheus_pool);
  prometheus_pool = NULL;

  (void) close(prometheus_logfd);
  prometheus_logfd = -1;
}

static void prom_startup_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  if (ServerType == SERVER_INETD) {
    pr_log_debug(DEBUG0, MOD_PROMETHEUS_VERSION
      ": cannot support Prometheus for ServerType inetd, disabling module");
    prometheus_engine = FALSE;
    return;
  }
}

static void prom_timeout_idle_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("timeout", 1, "reason", "TimeoutIdle", NULL);
}

static void prom_timeout_login_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("timeout", 1, "reason", "TimeoutLogin", NULL);
}

static void prom_timeout_noxfer_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("timeout", 1, "reason", "TimeoutNoTransfer", NULL);
}

static void prom_timeout_stalled_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("timeout", 1, "reason", "TimeoutStalled", NULL);
}

/* mod_tls-generated events */
static void prom_tls_ctrl_handshake_err_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("handshake_error", 1, "connection", "ctrl", NULL);
}

static void prom_tls_data_handshake_err_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }
  
  prom_event_incr("handshake_error", 1, "connection", "data", NULL);
}

/* mod_sftp-generated events */
static void prom_ssh2_kex_err_ev(const void *event_data, void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("handshake_error", 1, NULL);
}

static void prom_ssh2_auth_hostbased_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login", 1, "method", "hostbased", NULL);
}

static void prom_ssh2_auth_hostbased_err_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login_error", 1, "method", "hostbased", NULL);
}

static void prom_ssh2_auth_kbdint_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login", 1, "method", "keyboard-interactive", NULL);
}

static void prom_ssh2_auth_kbdint_err_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login_error", 1, "method", "keyboard-interactive", NULL);
}

static void prom_ssh2_auth_passwd_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login", 1, "method", "password", NULL);
}

static void prom_ssh2_auth_passwd_err_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login_error", 1, "method", "password", NULL);
}

static void prom_ssh2_auth_publickey_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login", 1, "method", "publickey", NULL);
}

static void prom_ssh2_auth_publickey_err_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("login_error", 1, "method", "publickey", NULL);
}

static void prom_ssh2_sftp_proto_version_ev(const void *event_data,
    void *user_data) {
  unsigned long protocol_version;

  if (prometheus_engine == FALSE) {
    return;
  }

  if (event_data == NULL) {
    /* Missing required data. */
    return;
  }

  protocol_version = *((unsigned long *) event_data);

  switch (protocol_version) {
    case 3:
      prom_event_incr("sftp_protocol", 1, "version", "3", NULL);
      break;

    case 4:
      prom_event_incr("sftp_protocol", 1, "version", "4", NULL);
      break;

    case 5:
      prom_event_incr("sftp_protocol", 1, "version", "5", NULL);
      break;

    case 6:
      prom_event_incr("sftp_protocol", 1, "version", "6", NULL);
      break;

    default:
      (void) pr_log_writefile(prometheus_logfd, MOD_PROMETHEUS_VERSION,
        "unknown SFTP protocol version %lu, ignoring", protocol_version);
  }
}

static void prom_ssh2_sftp_sess_opened_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("session", 1, NULL);
}

static void prom_ssh2_sftp_sess_closed_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("session", -1, NULL);
}

static void prom_ssh2_scp_sess_opened_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("session", 1, NULL);
}

static void prom_ssh2_scp_sess_closed_ev(const void *event_data,
    void *user_data) {
  if (prometheus_engine == FALSE) {
    return;
  }

  prom_event_incr("session", -1, NULL);
}

/* Initialization routines
 */

static int prom_init(void) {
  prometheus_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(prometheus_pool, MOD_PROMETHEUS_VERSION);

#if defined(PR_SHARED_MODULE)
  pr_event_register(&prometheus_module, "core.module-unload",
    prom_mod_unload_ev, NULL);
#endif /* PR_SHARED_MODULE */
  pr_event_register(&prometheus_module, "core.postparse", prom_postparse_ev,
    NULL);
  pr_event_register(&prometheus_module, "core.restart", prom_restart_ev, NULL);
  pr_event_register(&prometheus_module, "core.shutdown", prom_shutdown_ev,
    NULL);
  pr_event_register(&prometheus_module, "core.startup", prom_startup_ev, NULL);

  /* Normally we should register the 'core.exit' event listener in the
   * sess_init callback.  However, we use this listener to listen for
   * refused connections, e.g. connections refused by other modules'
   * sess_init callbacks.  And depending on the module load order, another
   * module might refuse the connection before mod_snmp's sess_init callback
   * is invoked, which would prevent mod_prometheus from registering its
   * 'core.exit' event listener.
   *
   * Thus to work around this timing issue, we register our 'core.exit' event
   * listener here, in the daemon process.  It should not hurt anything.
   */
  pr_event_register(&prometheus_module, "core.exit", prom_exit_ev, NULL);

  return 0;
}

static int prom_sess_init(void) {
  pool *tmp_pool;
  struct prom_dbh *dbh;
  const struct prom_metric *metric;
  pr_table_t *labels;

  pr_event_register(&prometheus_module, "core.timeout-idle",
    prom_timeout_idle_ev, NULL);
  pr_event_register(&prometheus_module, "core.timeout-login",
    prom_timeout_login_ev, NULL);
  pr_event_register(&prometheus_module, "core.timeout-no-transfer",
    prom_timeout_noxfer_ev, NULL);
  pr_event_register(&prometheus_module, "core.timeout-stalled",
    prom_timeout_stalled_ev, NULL);

  pr_event_register(&prometheus_module, "mod_auth.authentication-code",
    prom_auth_code_ev, NULL);

  if (pr_module_exists("mod_tls.c") == TRUE) {
    /* mod_tls events */
    pr_event_register(&prometheus_module, "mod_tls.ctrl-handshake-failed",
      prom_tls_ctrl_handshake_err_ev, NULL);
    pr_event_register(&prometheus_module, "mod_tls.data-handshake-failed",
      prom_tls_data_handshake_err_ev, NULL);
  }

  if (pr_module_exists("mod_sftp.c") == TRUE) {
    /* mod_sftp events */

    pr_event_register(&prometheus_module, "mod_sftp.ssh2.kex.failed",
      prom_ssh2_kex_err_ev, NULL);

    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-hostbased",
      prom_ssh2_auth_hostbased_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-hostbased.failed",
      prom_ssh2_auth_hostbased_err_ev, NULL);

    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-kbdint",
      prom_ssh2_auth_kbdint_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-kbdint.failed",
      prom_ssh2_auth_kbdint_err_ev, NULL);

    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-password",
      prom_ssh2_auth_passwd_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-password.failed",
      prom_ssh2_auth_passwd_err_ev, NULL);

    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-publickey",
      prom_ssh2_auth_publickey_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.ssh2.auth-publickey.failed",
      prom_ssh2_auth_publickey_err_ev, NULL);

    pr_event_register(&prometheus_module, "mod_sftp.sftp.session-opened",
      prom_ssh2_sftp_sess_opened_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.sftp.session-closed",
      prom_ssh2_sftp_sess_closed_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.sftp.protocol-version",
      prom_ssh2_sftp_proto_version_ev, NULL);

    pr_event_register(&prometheus_module, "mod_sftp.scp.session-opened",
      prom_ssh2_scp_sess_opened_ev, NULL);
    pr_event_register(&prometheus_module, "mod_sftp.scp.session-closed",
      prom_ssh2_scp_sess_closed_ev, NULL);
  }

  /* Close any database handle inherited from our parent, and open a new
   * one, per SQLite3 recommendation.
   */
  (void) prom_db_close(prometheus_pool, prometheus_dbh);
  prometheus_dbh = NULL;
  dbh = prom_metric_db_init(session.pool, prometheus_tables_dir,
    PROM_DB_OPEN_FL_VACUUM);
  if (prom_registry_set_dbh(prometheus_registry, dbh) < 0) {
    pr_trace_msg(trace_channel, 3, "error setting registry dbh: %s",
      strerror(errno));
  }

  tmp_pool = make_sub_pool(session.pool);
  labels = pr_table_nalloc(tmp_pool, 0, 2);
  (void) pr_table_add(labels, "protocol", pr_session_get_protocol(0), 0);

  metric = prom_registry_get_metric(prometheus_registry, "session");
  prom_metric_incr(tmp_pool, metric, 1, labels);
  destroy_pool(tmp_pool);

  return 0;
}

/* Module API tables
 */

static conftable prometheus_conftab[] = {
  { "PrometheusEngine",		set_prometheusengine,		NULL },
  { "PrometheusExporter",	set_prometheusexporter,		NULL },
  { "PrometheusLog",		set_prometheuslog,		NULL },
  { "PrometheusOptions",	set_prometheusoptions,		NULL },
  { "PrometheusTables",		set_prometheustables,		NULL },
  { NULL }
};

static cmdtable prometheus_cmdtab[] = {
  { PRE_CMD,		C_LIST,	G_NONE,	prom_pre_list,	FALSE,	FALSE },
  { LOG_CMD,		C_LIST,	G_NONE,	prom_log_list,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_LIST,	G_NONE,	prom_err_list,	FALSE,	FALSE },

  { PRE_CMD,		C_MLSD,	G_NONE,	prom_pre_list,	FALSE,	FALSE },
  { LOG_CMD,		C_MLSD,	G_NONE,	prom_log_list,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_MLSD,	G_NONE,	prom_err_list,	FALSE,	FALSE },

  { PRE_CMD,		C_NLST,	G_NONE,	prom_pre_list,	FALSE,	FALSE },
  { LOG_CMD,		C_NLST,	G_NONE,	prom_log_list,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_NLST,	G_NONE,	prom_err_list,	FALSE,	FALSE },

  { PRE_CMD,		C_USER, G_NONE, prom_pre_user,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_USER, G_NONE, prom_err_login,	FALSE,	FALSE },
  { LOG_CMD,		C_PASS,	G_NONE,	prom_log_pass,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_PASS,	G_NONE,	prom_err_login,	FALSE,	FALSE },

  { PRE_CMD,		C_RETR,	G_NONE,	prom_pre_retr,	FALSE,	FALSE },
  { LOG_CMD,		C_RETR,	G_NONE,	prom_log_retr,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_RETR,	G_NONE,	prom_err_retr,	FALSE,	FALSE },

  { PRE_CMD,		C_STOR,	G_NONE,	prom_pre_stor,	FALSE,	FALSE },
  { LOG_CMD,		C_STOR,	G_NONE,	prom_log_stor,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_STOR,	G_NONE,	prom_err_stor,	FALSE,	FALSE },

  /* For mod_tls */
  { LOG_CMD,		C_AUTH,	G_NONE,	prom_log_auth,	FALSE,	FALSE },

  { 0, NULL }
};

module prometheus_module = {
  /* Always NULL */
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "prometheus",

  /* Module configuration handler table */
  prometheus_conftab,

  /* Module command handler table */
  prometheus_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization */
  prom_init,

  /* Session initialization */
  prom_sess_init,

  /* Module version */
  MOD_PROMETHEUS_VERSION
};
