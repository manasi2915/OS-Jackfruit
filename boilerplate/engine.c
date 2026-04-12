/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE        (1024 * 1024)
#define CONTAINER_ID_LEN  32
#define CONTROL_PATH      "/tmp/mini_runtime.sock"
#define LOG_DIR           "logs"
#define CONTROL_MSG_LEN   256
#define CHILD_CMD_LEN     256
#define LOG_CHUNK_SIZE    4096
#define LOG_BUF_CAP       16
#define DEFAULT_SOFT      (40UL << 20)
#define DEFAULT_HARD      (64UL << 20)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char   id[CONTAINER_ID_LEN];
    pid_t  host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int    exit_code;
    int    exit_signal;
    int    stop_requested;   /* set before SIGTERM/SIGKILL from 'stop' */
    char   log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUF_CAP];
    size_t head, tail, count;
    int    shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char   container_id[CONTAINER_ID_LEN];
    char   rootfs[PATH_MAX];
    char   command[CHILD_CMD_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int    nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MSG_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_CMD_LEN];
    int  nice_value;
    int  pipe_write_fd;   /* write end of log pipe */
} child_config_t;

/* Per-producer thread argument */
typedef struct {
    int               pipe_read_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} producer_arg_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/* Globals needed by signal handlers                                    */
/* ------------------------------------------------------------------ */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *out)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s too large: %s\n", flag, value);
        return -1;
    }
    *out = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                 int argc, char *argv[], int start)
{
    int i;
    for (i = start; i < argc; i += 2) {
        char *end;
        long nv;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes))
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes))
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice value: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_str(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                       */
/* ------------------------------------------------------------------ */

static int bb_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    if ((rc = pthread_mutex_init(&b->mutex, NULL))) return rc;
    if ((rc = pthread_cond_init(&b->not_empty, NULL))) {
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    if ((rc = pthread_cond_init(&b->not_full, NULL))) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    return 0;
}

static void bb_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bb_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* Returns 0 on success, -1 if shutting down */
static int bb_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUF_CAP && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUF_CAP;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* Returns 0 on success, 1 if shutdown+empty (consumer should exit) */
static int bb_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0) {          /* shutting_down AND empty */
        pthread_mutex_unlock(&b->mutex);
        return 1;
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUF_CAP;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging consumer thread                                             */
/* ------------------------------------------------------------------ */

static void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;

    while (bb_pop(buf, &item) == 0) {
        /* open (or create) the log file for this container */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }
        ssize_t written = 0, total = (ssize_t)item.length;
        while (written < total) {
            ssize_t n = write(fd, item.data + written,
                              (size_t)(total - written));
            if (n <= 0) break;
            written += n;
        }
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread: reads from container pipe, pushes to buffer       */
/* ------------------------------------------------------------------ */

static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while ((n = read(pa->pipe_read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN - 1] = '\0';
        bb_push(pa->buf, &item);
    }
    close(pa->pipe_read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child function (runs inside new namespaces)              */
/* ------------------------------------------------------------------ */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the log pipe */
    dup2(cfg->pipe_write_fd, STDOUT_FILENO);
    dup2(cfg->pipe_write_fd, STDERR_FILENO);
    close(cfg->pipe_write_fd);

    /* Set hostname to container ID for UTS namespace */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");

    /* Mount /proc inside the container */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");  /* non-fatal */

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Apply nice value if requested */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the requested command */
    char *argv_exec[] = { cfg->command, NULL };
    char *envp[]      = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root", "TERM=xterm", NULL
    };
    execve(cfg->command, argv_exec, envp);
    perror("execve");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Supervisor: find container by id                                   */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                           const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Supervisor: launch a container                                      */
/* ------------------------------------------------------------------ */

static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             control_response_t *resp)
{
    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MSG_LEN,
                 "Container '%s' already exists", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Pipe: container stdout/stderr → supervisor producer */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MSG_LEN, "pipe: %s", strerror(errno));
        return -1;
    }

    /* Build child config */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CHILD_CMD_LEN - 1);
    cfg->nice_value    = req->nice_value;
    cfg->pipe_write_fd = pipefd[1];

    /* Allocate stack for clone */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MSG_LEN, "malloc stack failed");
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack_top, clone_flags, cfg);
    if (pid < 0) {
        free(stack); free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MSG_LEN, "clone: %s", strerror(errno));
        return -1;
    }

    /* Close write end in parent */
    close(pipefd[1]);

    /* Start producer thread for this container */
    producer_arg_t *pa = malloc(sizeof(*pa));
    pa->pipe_read_fd = pipefd[0];
    strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    pa->container_id[CONTAINER_ID_LEN - 1] = '\0';
    pa->buf = &ctx->log_buffer;
    pthread_t ptid;
    pthread_create(&ptid, NULL, producer_thread, pa);
    pthread_detach(ptid);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        struct monitor_request mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.pid = pid;
        mreq.soft_limit_bytes = req->soft_limit_bytes;
        mreq.hard_limit_bytes = req->hard_limit_bytes;
        strncpy(mreq.container_id, req->container_id, MONITOR_NAME_LEN - 1);
        if (ioctl(ctx->monitor_fd, MONITOR_REGISTER, &mreq) < 0)
            perror("ioctl MONITOR_REGISTER");
    }

    /* Store metadata */
    container_record_t *rec = calloc(1, sizeof(*rec));
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    free(stack);   /* safe after clone returns in parent */

    resp->status = 0;
    snprintf(resp->message, CONTROL_MSG_LEN,
             "Container '%s' started (pid %d)", req->container_id, pid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signal handlers                                                     */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;

    int saved_errno = errno;
    int wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->exit_code  = WEXITSTATUS(wstatus);
                    c->exit_signal = 0;
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    c->exit_signal = WTERMSIG(wstatus);
                    c->exit_code   = 128 + c->exit_signal;
                    if (c->exit_signal == SIGKILL && !c->stop_requested)
                        c->state = CONTAINER_KILLED;  /* hard limit kill */
                    else
                        c->state = CONTAINER_STOPPED;
                }
                /* Unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0) {
                    struct monitor_request mreq;
                    memset(&mreq, 0, sizeof(mreq));
                    mreq.pid = pid;
                    strncpy(mreq.container_id, c->id, MONITOR_NAME_LEN - 1);
                    ioctl(g_ctx->monitor_fd, MONITOR_UNREGISTER, &mreq);
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
    errno = saved_errno;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Supervisor: handle one control request                             */
/* ------------------------------------------------------------------ */

static void handle_request(supervisor_ctx_t *ctx,
                            int client_fd,
                            const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {

    case CMD_START:
    case CMD_RUN:
        launch_container(ctx, req, &resp);
        break;

    case CMD_PS: {
        char buf[4096];
        int  off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-12s %-10s %-10s\n",
                        "ID", "PID", "STATE", "SOFT(MB)", "HARD(MB)");
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && off < (int)sizeof(buf) - 80) {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-12s %-10lu %-10lu\n",
                            c->id, c->host_pid, state_str(c->state),
                            c->soft_limit_bytes >> 20,
                            c->hard_limit_bytes >> 20);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        strncpy(resp.message, buf, CONTROL_MSG_LEN - 1);
        break;
    }

    case CMD_LOGS: {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
        FILE *f = fopen(path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MSG_LEN,
                     "No log for '%s'", req->container_id);
        } else {
            resp.status = 0;
            size_t n = fread(resp.message, 1, CONTROL_MSG_LEN - 1, f);
            resp.message[n] = '\0';
            fclose(f);
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, CONTROL_MSG_LEN,
                     "Container '%s' not found", req->container_id);
            break;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);
        /* Give it 3s, then force */
        int waited = 0;
        while (waited < 30) {
            usleep(100000);
            pthread_mutex_lock(&ctx->metadata_lock);
            container_state_t st = c->state;
            pthread_mutex_unlock(&ctx->metadata_lock);
            if (st == CONTAINER_STOPPED || st == CONTAINER_EXITED ||
                st == CONTAINER_KILLED)
                break;
            waited++;
        }
        /* If still running, SIGKILL */
        pthread_mutex_lock(&ctx->metadata_lock);
        if (c->state == CONTAINER_RUNNING) {
            kill(pid, SIGKILL);
            c->state = CONTAINER_STOPPED;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, CONTROL_MSG_LEN,
                 "Container '%s' stopped", req->container_id);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MSG_LEN, "Unknown command");
        break;
    }

    /* Send response back to CLI client */
    write(client_fd, &resp, sizeof(resp));
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    (void)rootfs;

    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Init synchronization */
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bb_init(&ctx.log_buffer);

    /* Open kernel monitor device (optional — ok if not loaded) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: /dev/container_monitor not available\n");

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen"); return 1;
    }
    chmod(CONTROL_PATH, 0666);

    /* Signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* Start consumer (logging) thread */
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);

    fprintf(stderr, "[supervisor] Ready on %s\n", CONTROL_PATH);

    /* Use select so signals can interrupt accept() */
    while (!ctx.should_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int rc = select(ctx.server_fd + 1, &fds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (rc == 0) continue;   /* timeout — check should_stop */

        int cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        control_request_t req;
        ssize_t n = read(cfd, &req, sizeof(req));
        if (n == (ssize_t)sizeof(req))
            handle_request(&ctx, cfd, &req);

        close(cfd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGKILL);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    while (waitpid(-1, NULL, WNOHANG) > 0);
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0);

    /* Shutdown logging pipeline */
    bb_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free metadata */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Cleanup */
    bb_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Clean exit.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client: send control request to supervisor                     */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    control_response_t resp;
    if (read(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        perror("read response");
        close(fd);
        return 1;
    }

    close(fd);

    if (req->kind == CMD_PS || req->kind == CMD_LOGS)
        printf("%s\n", resp.message);
    else
        printf("%s\n", resp.message);

    return resp.status == 0 ? 0 : 1;
}

/* CLI: run blocks until container exits */
static int send_run_request(const control_request_t *req)
{
    /* First send the start request */
    if (send_control_request(req) != 0)
        return 1;

    /* Then poll until container exits */
    control_request_t ps_req;
    memset(&ps_req, 0, sizeof(ps_req));
    ps_req.kind = CMD_PS;

    fprintf(stderr, "Waiting for container '%s' to finish...\n",
            req->container_id);
    while (1) {
        sleep(1);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) break;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd); break;
        }
        write(fd, &ps_req, sizeof(ps_req));
        control_response_t resp;
        if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp)) {
            if (strstr(resp.message, req->container_id) &&
                (strstr(resp.message, "exited") ||
                 strstr(resp.message, "stopped") ||
                 strstr(resp.message, "killed"))) {
                close(fd);
                printf("Container '%s' finished.\n", req->container_id);
                return 0;
            }
        }
        close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI entry points                                                    */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <cmd> [...]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,        argv[3], PATH_MAX - 1);
    strncpy(req.command,       argv[4], CHILD_CMD_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT;
    req.hard_limit_bytes = DEFAULT_HARD;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <cmd> [...]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,        argv[3], PATH_MAX - 1);
    strncpy(req.command,       argv[4], CHILD_CMD_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT;
    req.hard_limit_bytes = DEFAULT_HARD;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_run_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
