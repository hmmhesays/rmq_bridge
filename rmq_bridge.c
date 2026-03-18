/*
 * rmq_bridge.c
 *
 * A RabbitMQ consumer that:
 *   - Reads configuration from a JSON file
 *   - Connects to a source RabbitMQ instance
 *   - Declares a queue and binds it to multiple topic exchanges
 *   - Consumes messages and republishes the body to a destination
 *     RabbitMQ instance, preserving the original routing key
 *
 * Signals:
 *   SIGTERM / SIGINT  – graceful shutdown after current message
 *   SIGHUP            – reopen log file (for log rotation)
 *
 * Build:
 *   gcc -o rmq_bridge rmq_bridge.c -lrabbitmq -lcjson -lpthread
 *
 * Usage:
 *   ./rmq_bridge /path/to/config.json
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#include <cjson/cJSON.h>

/* ------------------------------------------------------------------ */
/*  Globals for signal handling                                        */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_shutdown   = 0;
static volatile sig_atomic_t g_reopen_log = 0;

static void handle_term(int sig) { (void)sig; g_shutdown = 1; }
static void handle_hup(int sig)  { (void)sig; g_reopen_log = 1; }

/* ------------------------------------------------------------------ */
/*  Logging                                                            */
/* ------------------------------------------------------------------ */
static FILE *g_logfp = NULL;
static char  g_log_path[1024] = {0};

static void log_open(void)
{
    if (g_log_path[0] == '\0') {
        g_logfp = stderr;
        return;
    }
    g_logfp = fopen(g_log_path, "a");
    if (!g_logfp) {
        fprintf(stderr, "WARN: cannot open log '%s': %s, falling back to stderr\n",
                g_log_path, strerror(errno));
        g_logfp = stderr;
    }
}

static void log_reopen(void)
{
    if (g_logfp && g_logfp != stderr)
        fclose(g_logfp);
    log_open();
}

static void log_msg(const char *level, const char *fmt, ...)
{
    if (!g_logfp) return;

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(g_logfp, "[%s] [%s] ", ts, level);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logfp, fmt, ap);
    va_end(ap);

    fprintf(g_logfp, "\n");
    fflush(g_logfp);
}

#define LOG_INFO(...)  log_msg("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) log_msg("ERROR", __VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Configuration structures                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    char exchange[256];
    char binding_key[256];
} binding_t;

typedef struct {
    char host[256];
    int  port;
    char vhost[256];
    char username[256];
    char password[256];
    int  heartbeat;
    int  connection_timeout;
} conn_cfg_t;

typedef struct {
    conn_cfg_t  conn;
    char        queue[256];
    int         prefetch_count;
    int         queue_durable;
    int         queue_auto_delete;
    binding_t  *bindings;
    int         binding_count;
} source_cfg_t;

typedef struct {
    conn_cfg_t conn;
    char       exchange[256];
    int        persistent;
    int        exchange_durable;
    int        exchange_auto_delete;
    int        exchange_internal;
} dest_cfg_t;

typedef struct {
    char         log_file[1024];
    source_cfg_t source;
    dest_cfg_t   dest;
} config_t;

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                       */
/* ------------------------------------------------------------------ */
static const char *json_str(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        return item->valuestring;
    return def;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item))
        return item->valueint;
    return def;
}

static int json_bool(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item))
        return cJSON_IsTrue(item) ? 1 : 0;
    return def;
}

/* ------------------------------------------------------------------ */
/*  Parse configuration                                                */
/* ------------------------------------------------------------------ */
static void parse_conn(const cJSON *obj, conn_cfg_t *c)
{
    snprintf(c->host,     sizeof(c->host),     "%s", json_str(obj, "host",     "localhost"));
    c->port = json_int(obj, "port", 5672);
    snprintf(c->vhost,    sizeof(c->vhost),    "%s", json_str(obj, "vhost",    "/"));
    snprintf(c->username, sizeof(c->username), "%s", json_str(obj, "username", "guest"));
    snprintf(c->password, sizeof(c->password), "%s", json_str(obj, "password", "guest"));
    c->heartbeat          = json_int(obj, "heartbeat", 60);
    c->connection_timeout = json_int(obj, "connection_timeout", 10);
}

static int parse_config(const char *path, config_t *cfg)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ERROR: cannot open config '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, (size_t)len, fp);
    buf[len] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fprintf(stderr, "ERROR: JSON parse error near: %s\n", cJSON_GetErrorPtr());
        return -1;
    }

    /* log file */
    snprintf(cfg->log_file, sizeof(cfg->log_file), "%s",
             json_str(root, "log_file", ""));

    /* source */
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (!src) { fprintf(stderr, "ERROR: missing 'source' in config\n"); cJSON_Delete(root); return -1; }

    parse_conn(src, &cfg->source.conn);
    snprintf(cfg->source.queue, sizeof(cfg->source.queue), "%s",
             json_str(src, "queue", "bridge_queue"));
    cfg->source.prefetch_count    = json_int(src, "prefetch_count", 10);
    cfg->source.queue_durable     = json_bool(src, "durable", 1);
    cfg->source.queue_auto_delete = json_bool(src, "auto_delete", 0);

    /* bindings */
    const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(src, "bindings");
    if (!cJSON_IsArray(bindings)) {
        fprintf(stderr, "ERROR: 'source.bindings' must be an array\n");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(bindings);
    cfg->source.bindings = calloc((size_t)count, sizeof(binding_t));
    cfg->source.binding_count = count;

    for (int i = 0; i < count; i++) {
        const cJSON *b = cJSON_GetArrayItem(bindings, i);
        snprintf(cfg->source.bindings[i].exchange,    sizeof(cfg->source.bindings[i].exchange),
                 "%s", json_str(b, "exchange", ""));
        snprintf(cfg->source.bindings[i].binding_key, sizeof(cfg->source.bindings[i].binding_key),
                 "%s", json_str(b, "binding_key", "#"));
    }

    /* destination */
    const cJSON *dst = cJSON_GetObjectItemCaseSensitive(root, "destination");
    if (!dst) { fprintf(stderr, "ERROR: missing 'destination' in config\n"); cJSON_Delete(root); return -1; }

    parse_conn(dst, &cfg->dest.conn);
    snprintf(cfg->dest.exchange, sizeof(cfg->dest.exchange), "%s",
             json_str(dst, "exchange", ""));
    cfg->dest.persistent           = json_bool(dst, "persistent", 1);
    cfg->dest.exchange_durable     = json_bool(dst, "exchange_durable", 1);
    cfg->dest.exchange_auto_delete = json_bool(dst, "exchange_auto_delete", 0);
    cfg->dest.exchange_internal    = json_bool(dst, "exchange_internal", 0);

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AMQP helpers                                                       */
/* ------------------------------------------------------------------ */

/* Check the result of an amqp_rpc_reply.  Returns 0 on success, -1 on error. */
static int check_amqp_reply(amqp_connection_state_t conn, const char *context)
{
    amqp_rpc_reply_t reply = amqp_get_rpc_reply(conn);

    switch (reply.reply_type) {
    case AMQP_RESPONSE_NORMAL:
        return 0;

    case AMQP_RESPONSE_NONE:
        LOG_ERROR("%s: missing RPC reply type", context);
        break;

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
        LOG_ERROR("%s: %s", context, amqp_error_string2(reply.library_error));
        break;

    case AMQP_RESPONSE_SERVER_EXCEPTION:
        if (reply.reply.id == AMQP_CONNECTION_CLOSE_METHOD) {
            amqp_connection_close_t *m = (amqp_connection_close_t *)reply.reply.decoded;
            LOG_ERROR("%s: server connection error %d: %.*s", context,
                      m->reply_code, (int)m->reply_text.len, (char *)m->reply_text.bytes);
        } else if (reply.reply.id == AMQP_CHANNEL_CLOSE_METHOD) {
            amqp_channel_close_t *m = (amqp_channel_close_t *)reply.reply.decoded;
            LOG_ERROR("%s: server channel error %d: %.*s", context,
                      m->reply_code, (int)m->reply_text.len, (char *)m->reply_text.bytes);
        } else {
            LOG_ERROR("%s: unknown server error, method id 0x%08X", context, reply.reply.id);
        }
        break;
    }
    return -1;
}

/* Open a connection + channel.  Returns NULL on failure. */
static amqp_connection_state_t amqp_connect(const conn_cfg_t *cfg, const char *label)
{
    amqp_connection_state_t conn = amqp_new_connection();
    amqp_socket_t *sock = amqp_tcp_socket_new(conn);
    if (!sock) {
        LOG_ERROR("[%s] cannot create TCP socket", label);
        amqp_destroy_connection(conn);
        return NULL;
    }

    /* Connect with timeout */
    struct timeval connect_tv;
    connect_tv.tv_sec  = cfg->connection_timeout;
    connect_tv.tv_usec = 0;

    int rc = amqp_socket_open_noblock(sock, cfg->host, cfg->port, &connect_tv);
    if (rc != AMQP_STATUS_OK) {
        LOG_ERROR("[%s] cannot connect to %s:%d (timeout=%ds): %s", label,
                  cfg->host, cfg->port, cfg->connection_timeout,
                  amqp_error_string2(rc));
        amqp_destroy_connection(conn);
        return NULL;
    }

    amqp_rpc_reply_t login_reply = amqp_login(conn, cfg->vhost, 0, 131072,
                                              cfg->heartbeat,
                                              AMQP_SASL_METHOD_PLAIN,
                                              cfg->username, cfg->password);
    if (login_reply.reply_type != AMQP_RESPONSE_NORMAL) {
        switch (login_reply.reply_type) {
        case AMQP_RESPONSE_NONE:
            LOG_ERROR("[%s] login: missing RPC reply type", label);
            break;
        case AMQP_RESPONSE_LIBRARY_EXCEPTION:
            LOG_ERROR("[%s] login: %s", label,
                      amqp_error_string2(login_reply.library_error));
            break;
        case AMQP_RESPONSE_SERVER_EXCEPTION:
            if (login_reply.reply.id == AMQP_CONNECTION_CLOSE_METHOD) {
                amqp_connection_close_t *m =
                    (amqp_connection_close_t *)login_reply.reply.decoded;
                LOG_ERROR("[%s] login: server error %d: %.*s", label,
                          m->reply_code, (int)m->reply_text.len,
                          (char *)m->reply_text.bytes);
            } else {
                LOG_ERROR("[%s] login: server error, method id 0x%08X", label,
                          login_reply.reply.id);
            }
            break;
        default:
            LOG_ERROR("[%s] login: unknown error", label);
            break;
        }
        amqp_destroy_connection(conn);
        return NULL;
    }

    amqp_channel_open(conn, 1);
    if (check_amqp_reply(conn, label) != 0) {
        amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(conn);
        return NULL;
    }

    LOG_INFO("[%s] connected to %s:%d vhost=%s heartbeat=%ds timeout=%ds",
             label, cfg->host, cfg->port, cfg->vhost,
             cfg->heartbeat, cfg->connection_timeout);
    return conn;
}

static void amqp_disconnect(amqp_connection_state_t conn, const char *label)
{
    if (!conn) return;
    amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
    amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(conn);
    LOG_INFO("[%s] disconnected", label);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* ---- Parse config -------------------------------------------- */
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (parse_config(argv[1], &cfg) != 0)
        return EXIT_FAILURE;

    /* ---- Open log ------------------------------------------------ */
    snprintf(g_log_path, sizeof(g_log_path), "%s", cfg.log_file);
    log_open();
    LOG_INFO("rmq_bridge starting, config=%s", argv[1]);

    /* ---- Install signal handlers --------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    sa.sa_handler = handle_hup;
    sigaction(SIGHUP, &sa, NULL);

    /* ---- Connect source ------------------------------------------ */
    amqp_connection_state_t src_conn = amqp_connect(&cfg.source.conn, "source");
    if (!src_conn) {
        LOG_ERROR("failed to connect to source, exiting");
        free(cfg.source.bindings);
        return EXIT_FAILURE;
    }

    /* Declare queue */
    amqp_bytes_t queue_name = amqp_cstring_bytes(cfg.source.queue);
    amqp_queue_declare(src_conn, 1, queue_name,
                       /* passive */ 0,
                       /* durable */ cfg.source.queue_durable,
                       /* exclusive */ 0,
                       /* auto_delete */ cfg.source.queue_auto_delete,
                       amqp_empty_table);
    if (check_amqp_reply(src_conn, "queue_declare") != 0) {
        amqp_disconnect(src_conn, "source");
        free(cfg.source.bindings);
        return EXIT_FAILURE;
    }
    LOG_INFO("declared queue '%s' (durable=%d, auto_delete=%d)",
             cfg.source.queue, cfg.source.queue_durable, cfg.source.queue_auto_delete);

    /* Bind queue to each exchange */
    for (int i = 0; i < cfg.source.binding_count; i++) {
        binding_t *b = &cfg.source.bindings[i];
        amqp_queue_bind(src_conn, 1,
                        queue_name,
                        amqp_cstring_bytes(b->exchange),
                        amqp_cstring_bytes(b->binding_key),
                        amqp_empty_table);
        if (check_amqp_reply(src_conn, "queue_bind") != 0) {
            LOG_ERROR("failed to bind queue to exchange='%s' key='%s'",
                      b->exchange, b->binding_key);
            amqp_disconnect(src_conn, "source");
            free(cfg.source.bindings);
            return EXIT_FAILURE;
        }
        LOG_INFO("bound queue '%s' -> exchange='%s' key='%s'",
                 cfg.source.queue, b->exchange, b->binding_key);
    }

    /* Set QoS */
    amqp_basic_qos(src_conn, 1, 0, (uint16_t)cfg.source.prefetch_count, 0);
    if (check_amqp_reply(src_conn, "basic_qos") != 0) {
        amqp_disconnect(src_conn, "source");
        free(cfg.source.bindings);
        return EXIT_FAILURE;
    }
    LOG_INFO("QoS prefetch_count=%d", cfg.source.prefetch_count);

    /* Start consuming */
    amqp_basic_consume(src_conn, 1, queue_name,
                       amqp_empty_bytes,  /* consumer tag */
                       /* no_local */ 0,
                       /* no_ack */  0,
                       /* exclusive */ 0,
                       amqp_empty_table);
    if (check_amqp_reply(src_conn, "basic_consume") != 0) {
        amqp_disconnect(src_conn, "source");
        free(cfg.source.bindings);
        return EXIT_FAILURE;
    }

    /* ---- Connect destination ------------------------------------- */
    amqp_connection_state_t dst_conn = amqp_connect(&cfg.dest.conn, "dest");
    if (!dst_conn) {
        LOG_ERROR("failed to connect to destination, exiting");
        amqp_disconnect(src_conn, "source");
        free(cfg.source.bindings);
        return EXIT_FAILURE;
    }

    /* Declare destination exchange (topic) */
    amqp_exchange_declare(dst_conn, 1,
                          amqp_cstring_bytes(cfg.dest.exchange),
                          amqp_cstring_bytes("topic"),
                          /* passive */ 0,
                          /* durable */ cfg.dest.exchange_durable,
                          /* auto_delete */ cfg.dest.exchange_auto_delete,
                          /* internal */ cfg.dest.exchange_internal,
                          amqp_empty_table);
    if (check_amqp_reply(dst_conn, "exchange_declare") != 0) {
        LOG_ERROR("failed to declare destination exchange '%s'", cfg.dest.exchange);
        amqp_disconnect(dst_conn, "dest");
        amqp_disconnect(src_conn, "source");
        free(cfg.source.bindings);
        return EXIT_FAILURE;
    }
    LOG_INFO("declared destination exchange '%s' (topic, durable=%d, auto_delete=%d, internal=%d)",
             cfg.dest.exchange, cfg.dest.exchange_durable,
             cfg.dest.exchange_auto_delete, cfg.dest.exchange_internal);

    /* ---- Consume loop -------------------------------------------- */
    LOG_INFO("entering consume loop");

    int exit_code = EXIT_SUCCESS;

    while (!g_shutdown) {
        /* Check if we need to reopen the log file (SIGHUP) */
        if (g_reopen_log) {
            g_reopen_log = 0;
            LOG_INFO("reopening log file (SIGHUP)");
            log_reopen();
            LOG_INFO("log file reopened");
        }

        /* Wait for a message with a timeout so we can check signals */
        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(src_conn);

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        amqp_rpc_reply_t reply = amqp_consume_message(src_conn, &envelope, &tv, 0);

        if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION) {
            if (reply.library_error == AMQP_STATUS_TIMEOUT) {
                /* No message within timeout — loop back and check signals */
                continue;
            }
            /* Real connection error */
            LOG_ERROR("source connection lost: %s", amqp_error_string2(reply.library_error));
            exit_code = EXIT_FAILURE;
            break;
        }

        if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
            LOG_ERROR("unexpected reply type %d during consume", reply.reply_type);
            exit_code = EXIT_FAILURE;
            break;
        }

        /* We have a message — extract routing key and body */
        char routing_key[512] = {0};
        size_t rk_len = envelope.routing_key.len < sizeof(routing_key) - 1
                            ? envelope.routing_key.len
                            : sizeof(routing_key) - 1;
        memcpy(routing_key, envelope.routing_key.bytes, rk_len);
        routing_key[rk_len] = '\0';

        LOG_INFO("received message: exchange='%.*s' routing_key='%s' size=%lu",
                 (int)envelope.exchange.len, (char *)envelope.exchange.bytes,
                 routing_key, (unsigned long)envelope.message.body.len);

        /* Publish body to destination */
        amqp_basic_properties_t props;
        memset(&props, 0, sizeof(props));
        props._flags = AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.delivery_mode = cfg.dest.persistent ? 2 : 1; /* 2=persistent, 1=transient */

        int pub_rc = amqp_basic_publish(dst_conn, 1,
                                        amqp_cstring_bytes(cfg.dest.exchange),
                                        amqp_cstring_bytes(routing_key),
                                        /* mandatory */ 0,
                                        /* immediate */ 0,
                                        &props,
                                        envelope.message.body);

        if (pub_rc != AMQP_STATUS_OK) {
            LOG_ERROR("publish to destination failed: %s", amqp_error_string2(pub_rc));
            amqp_destroy_envelope(&envelope);
            exit_code = EXIT_FAILURE;
            break;
        }

        /* Check destination connection is still healthy */
        amqp_rpc_reply_t dst_reply = amqp_get_rpc_reply(dst_conn);
        if (dst_reply.reply_type != AMQP_RESPONSE_NORMAL) {
            LOG_ERROR("destination connection error after publish");
            amqp_destroy_envelope(&envelope);
            exit_code = EXIT_FAILURE;
            break;
        }

        LOG_INFO("forwarded message routing_key='%s' to dest exchange='%s'",
                 routing_key, cfg.dest.exchange);

        /* ACK the message on source */
        amqp_basic_ack(src_conn, 1, envelope.delivery_tag, /* multiple */ 0);

        amqp_destroy_envelope(&envelope);
    }

    /* ---- Cleanup ------------------------------------------------- */
    if (g_shutdown)
        LOG_INFO("shutdown signal received, cleaning up");

    amqp_disconnect(dst_conn, "dest");
    amqp_disconnect(src_conn, "source");
    free(cfg.source.bindings);

    LOG_INFO("rmq_bridge exiting (code=%d)", exit_code);

    if (g_logfp && g_logfp != stderr)
        fclose(g_logfp);

    return exit_code;
}
