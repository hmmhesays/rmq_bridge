# rmq_bridge

A C-based RabbitMQ message bridge that consumes from one RabbitMQ instance and
forwards message bodies to an exchange on another instance, preserving the
original routing key.

## Dependencies

- [rabbitmq-c](https://github.com/alanxz/rabbitmq-c) — the official C AMQP client
- [cJSON](https://github.com/DaveGamble/cJSON) — lightweight JSON parser

## Build (Dynamic — Debian/Ubuntu)

Install development packages:

```bash
sudo apt-get install librabbitmq-dev libcjson-dev
```

Build:

```bash
make
```

## Build (Dynamic — RHEL/CentOS/Fedora)

```bash
sudo dnf install librabbitmq-devel cjson-devel
make
```

## Build (Static — Alpine Linux)

Produces a fully static, dependency-free portable binary. No shared libraries
required on the target system. The build script fetches cJSON and rabbitmq-c
from source and compiles them as static libraries.

### Prerequisites

```bash
apk add build-base cmake linux-headers git
```

### Option A: Build script

```bash
chmod +x build_static.sh
./build_static.sh
```

This will:
1. Clone cJSON and rabbitmq-c into `_static_build/` (skips if already present)
2. Build both as static libraries
3. Compile and link `rmq_bridge` as a fully static binary
4. Strip the binary for size

### Option B: Makefile target

```bash
make static
```

On first run this invokes `build_static.sh` automatically. On subsequent runs
(if `_static_build/install` already exists) it just recompiles `rmq_bridge.c`
against the cached static libraries — useful for quick iteration on the C source
without rebuilding dependencies.

### Verifying the static binary

```bash
file rmq_bridge
# rmq_bridge: ELF 64-bit LSB executable, ... statically linked ...

ldd rmq_bridge
# Not a valid dynamic program
```

The resulting binary can be copied to any Linux x86_64 system and run without
installing any libraries.

### Cleaning up

```bash
make static-clean    # removes _static_build/ and the binary
```

## Installation

```bash
# Copy files to target location
mkdir -p /opt/rmq_bridge
cp rmq_bridge /opt/rmq_bridge/
cp config.json /opt/rmq_bridge/

# Install systemd service
cp rmq_bridge.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now rmq_bridge
```

## Usage

```
./rmq_bridge -c <config.json> [-l <logfile>] [-d <debuglog>] [-h]
```

### Command line options

| Flag       | Description                                          | Required |
|------------|------------------------------------------------------|----------|
| `-c <path>`| Configuration file                                   | Yes      |
| `-l <path>`| Log file (overrides `log_file` in config)            | No       |
| `-d <path>`| Debug payload log (overrides `debug_log_file` in config) | No   |
| `-h`       | Show help message                                    | No       |

Command line flags take precedence over config file values. This allows
quick debugging without editing the config:

```bash
# Production (paths from config)
./rmq_bridge -c /opt/rmq_bridge/config.json

# Quick debug session (override debug log from CLI)
./rmq_bridge -c /opt/rmq_bridge/config.json -d /tmp/debug.log

# Override both logs
./rmq_bridge -c /opt/rmq_bridge/config.json -l /tmp/rmq.log -d /tmp/debug.log
```

### As a systemd service

```bash
systemctl start rmq_bridge      # start
systemctl stop rmq_bridge       # graceful shutdown (SIGTERM)
systemctl reload rmq_bridge     # reopen log files (SIGHUP)
systemctl restart rmq_bridge    # full restart
systemctl status rmq_bridge     # check status
journalctl -u rmq_bridge -f     # follow journal output
```

## Configuration

See `config.json` for a full example.

### Connection settings (shared by source and destination)

| Field                | Description                                      | Default     |
|----------------------|--------------------------------------------------|-------------|
| `host`               | RabbitMQ hostname or IP                          | `localhost` |
| `port`               | AMQP port                                        | `5672`      |
| `vhost`              | Virtual host                                     | `/`         |
| `username`           | AMQP username                                    | `guest`     |
| `password`           | AMQP password                                    | `guest`     |
| `heartbeat`          | AMQP heartbeat interval in seconds (0=disabled)  | `60`        |
| `connection_timeout` | TCP connect timeout in seconds                   | `10`        |

### Source settings

| Field              | Description                                       | Default        |
|--------------------|---------------------------------------------------|----------------|
| `queue`            | Queue name to declare and consume from            | `bridge_queue` |
| `durable`          | Queue durability (`true`/`false`)                 | `true`         |
| `auto_delete`      | Queue auto-delete flag (`true`/`false`)           | `false`        |
| `prefetch_count`   | QoS prefetch count                                | `10`           |
| `bindings`         | Array of `{ exchange, binding_key }` pairs        | *(required)*   |

### Destination settings

| Field                   | Description                                    | Default |
|-------------------------|------------------------------------------------|---------|
| `exchange`              | Destination exchange name                      | `""`    |
| `exchange_durable`      | Exchange durable flag                          | `true`  |
| `exchange_auto_delete`  | Exchange auto-delete flag                      | `false` |
| `exchange_internal`     | Exchange internal flag                         | `false` |
| `persistent`            | Message delivery mode: persistent if true      | `true`  |

### Global settings

| Field             | Description                                          | Default  |
|-------------------|------------------------------------------------------|----------|
| `log_file`        | Path to log file (empty string = stderr)             | `""`     |
| `debug_log_file`  | Path to debug payload log (empty = disabled)         | `""`     |

## Debug Payload Log

When enabled (via config or `-d` flag), every consumed message is logged with
full AMQP properties and the complete message body. Each entry includes:

- Timestamp, exchange, routing key, delivery tag, redelivered flag, consumer tag
- All AMQP basic properties: content_type, content_encoding, delivery_mode,
  priority, correlation_id, reply_to, expiration, message_id, timestamp, type,
  user_id, app_id, cluster_id, and headers table entries
- Full message payload

This log is intended for debugging and should not be left enabled in production
on high-throughput systems as it will grow rapidly.

SIGHUP reopens both the main log and the debug log, so both can be rotated.

## Signals

| Signal  | Behavior                                            |
|---------|-----------------------------------------------------|
| SIGTERM | Finish current message, then graceful shutdown      |
| SIGINT  | Same as SIGTERM                                     |
| SIGHUP  | Close and reopen all log files (for logrotate)      |

## Log Rotation

Example `/etc/logrotate.d/rmq_bridge`:

```
/var/log/rmq_bridge.log /var/log/rmq_bridge_debug.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
    postrotate
        systemctl reload rmq_bridge 2>/dev/null || true
    endscript
}
```
