#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include "stats.h"
#include "corvus.h"
#include "socket.h"
#include "logging.h"
#include "slot.h"

#define HOST_LEN 255

struct bytes {
    char key[DSN_LEN + 1];
    long long recv;
    long long send;
    long long completed;
};

static int statsd_fd = -1;
static struct sockaddr_in dest;
static pthread_t stats_thread;
static int metric_interval = 10;
static struct dict bytes_map;
static char hostname[HOST_LEN + 1];

struct stats global_stats;

static void stats_send(char *metric, double value)
{
    if (statsd_fd == -1) {
        statsd_fd = socket_create_udp_client();
    }

    int n;
    const char *fmt = "corvus.%s.%s-%d.%s:%f|g";
    n = snprintf(NULL, 0, fmt, config.cluster, hostname, config.bind, metric, value);
    char buf[n + 1];
    snprintf(buf, sizeof(buf), fmt, config.cluster, hostname, config.bind, metric, value);
    if (sendto(statsd_fd, buf, n, 0, (struct sockaddr*)&dest, sizeof(dest)) == -1) {
        LOG(WARN, "fail to send metrics data: %s", strerror(errno));
    }
}

void stats_global_add(struct stats *stats)
{
    global_stats.basic.completed_commands += stats->basic.completed_commands;
    global_stats.basic.remote_latency += stats->basic.remote_latency;
    global_stats.basic.total_latency += stats->basic.total_latency;
    global_stats.basic.recv_bytes += stats->basic.recv_bytes;
    global_stats.basic.send_bytes += stats->basic.send_bytes;
}

void stats_get_memory(struct memory_stats *stats)
{
    struct context *contexts = get_contexts();

    for (int i = 0; i < config.thread; i++) {
        stats->buffers        += contexts[i].mstats.buffers;
        stats->conns          += contexts[i].mstats.conns;
        stats->cmds           += contexts[i].mstats.cmds;
        stats->conn_info      += contexts[i].mstats.conn_info;
        stats->buf_times      += contexts[i].mstats.buf_times;
        stats->free_buffers   += contexts[i].mstats.free_buffers;
        stats->free_cmds      += contexts[i].mstats.free_cmds;
        stats->free_conns     += contexts[i].mstats.free_conns;
        stats->free_conn_info += contexts[i].mstats.free_conn_info;
        stats->free_buf_times += contexts[i].mstats.free_buf_times;
    }
}

void stats_get_simple(struct stats *stats)
{
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    getrusage(RUSAGE_SELF, &ru);

    struct context *contexts = get_contexts();

    stats->pid = getpid();
    stats->threads = config.thread;

    stats->used_cpu_sys = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1000000.0;
    stats->used_cpu_user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1000000.0;

    int i;
    for (i = 0; i < config.thread; i++) {
        stats->basic.completed_commands += ATOMIC_IGET(contexts[i].stats.completed_commands, 0);
        stats->basic.remote_latency     += ATOMIC_IGET(contexts[i].stats.remote_latency, 0);
        stats->basic.total_latency      += ATOMIC_IGET(contexts[i].stats.total_latency, 0);
        stats->basic.recv_bytes         += ATOMIC_IGET(contexts[i].stats.recv_bytes, 0);
        stats->basic.send_bytes         += ATOMIC_IGET(contexts[i].stats.send_bytes, 0);
        stats->basic.connected_clients  += ATOMIC_GET(contexts[i].stats.connected_clients);
    }
    stats_global_add(stats);
}

void stats_node_info_agg(struct bytes *bytes)
{
    struct bytes *b = NULL;
    struct connection *server;
    struct context *contexts = get_contexts();
    int j, n, m = 0;

    for (int i = 0; i < config.thread; i++) {
        TAILQ_FOREACH(server, &contexts[i].servers, next) {
            n = strlen(server->info->addr.ip);
            if (n <= 0) continue;

            char ip[n + 8];
            for (j = 0; j < n; j++) {
                ip[j] = server->info->addr.ip[j];
                if (ip[j] == '.') ip[j] = '-';
            }
            sprintf(ip + j, "-%d", server->info->addr.port);

            b = dict_get(&bytes_map, ip);
            if (b == NULL) {
                b = &bytes[m++];
                strncpy(b->key, ip, sizeof(b->key));
                b->send = 0;
                b->recv = 0;
                b->completed = 0;
                dict_set(&bytes_map, b->key, (void*)b);
            }
            b->send += ATOMIC_GET(server->info->send_bytes);
            b->recv += ATOMIC_GET(server->info->recv_bytes);
            b->completed += ATOMIC_GET(server->info->completed_commands);
        }
    }
}

void stats_send_simple()
{
    struct stats stats;
    memset(&stats, 0, sizeof(stats));
    stats_get_simple(&stats);
    stats_send("connected_clients", stats.basic.connected_clients);
    stats_send("completed_commands", stats.basic.completed_commands);
    stats_send("used_cpu_sys", stats.used_cpu_sys);
    stats_send("used_cpu_user", stats.used_cpu_user);
    stats_send("latency", stats.basic.total_latency / 1000000.0);
}

void stats_send_node_info()
{
    struct bytes *value;

    /* redis-node.127-0-0-1:8000.bytes.{send,recv} */
    int len = HOST_LEN + 64;
    char name[len];

    struct bytes bytes[REDIS_CLUSTER_SLOTS];
    stats_node_info_agg(bytes);

    struct dict_iter iter = DICT_ITER_INITIALIZER;
    DICT_FOREACH(&bytes_map, &iter) {
        value = (struct bytes*)iter.value;
        snprintf(name, len, "redis-node.%s.bytes.send", iter.key);
        stats_send(name, value->send);
        snprintf(name, len, "redis-node.%s.bytes.recv", iter.key);
        stats_send(name, value->recv);
        snprintf(name, len, "redis-node.%s.commands.completed", iter.key);
        stats_send(name, value->completed);
        value->send = 0;
        value->recv = 0;
        value->completed = 0;
    }
    dict_clear(&bytes_map);
}

void stats_get(struct stats *stats)
{
    stats_get_simple(stats);

    stats->basic.completed_commands = global_stats.basic.completed_commands;
    stats->basic.remote_latency = global_stats.basic.remote_latency;
    stats->basic.total_latency = global_stats.basic.total_latency;
    stats->basic.recv_bytes = global_stats.basic.recv_bytes;
    stats->basic.send_bytes = global_stats.basic.send_bytes;

    memset(stats->remote_nodes, 0, sizeof(stats->remote_nodes));
    slot_get_addr_list(stats->remote_nodes);

    struct context *contexts = get_contexts();

    memset(stats->last_command_latency, 0, sizeof(stats->last_command_latency));
    for (int i = 0; i < stats->threads; i++) {
        if (i >= ADDR_MAX) break;
        stats->last_command_latency[i] = ATOMIC_GET(contexts[i].last_command_latency);
    }
}

void *stats_daemon(void *data)
{
    while (1) {
        sleep(metric_interval);
        stats_send_simple();
        stats_send_node_info();
        LOG(DEBUG, "sending metrics");
    }
    return NULL;
}

int stats_init(int interval)
{
    size_t stacksize;
    pthread_attr_t attr;
    int len;
    dict_init(&bytes_map);

    memset(&global_stats, 0, sizeof(global_stats));

    gethostname(hostname, HOST_LEN + 1);
    len = strlen(hostname);
    if (len > HOST_LEN) {
        hostname[HOST_LEN] = '\0';
        len--;
    }

    for (int i = 0; i < len; i++) {
        if (hostname[i] == '.') hostname[i] = '-';
    }

    metric_interval = interval;

    /* Make the thread killable at any time can work reliably. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    if (pthread_create(&stats_thread, &attr, stats_daemon, NULL) != 0) {
        LOG(ERROR, "can't initialize stats thread");
        return CORVUS_ERR;
    }
    LOG(INFO, "starting stats thread");

    return CORVUS_OK;
}

void stats_kill()
{
    int err;

    dict_free(&bytes_map);

    if (pthread_cancel(stats_thread) == 0) {
        if ((err = pthread_join(stats_thread, NULL)) != 0) {
            LOG(WARN, "fail to kill stats thread: %s", strerror(err));
        }
    }
}

int stats_resolve_addr(char *addr)
{
    struct address a;

    memset(&dest, 0, sizeof(struct sockaddr_in));
    if (socket_parse_ip(addr, &a) == CORVUS_ERR) {
        LOG(ERROR, "stats_resolve_addr: fail to parse addr %s", addr);
        return CORVUS_ERR;
    }
    return socket_get_sockaddr(a.ip, a.port, &dest, SOCK_DGRAM);
}
