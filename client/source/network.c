#include "network.h"

#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_TIMEOUT_MS 12000
#define VOICE_TIMEOUT_MS 48000
#define DISCOVERY_PORT 18765

typedef struct {
    int fd;
    bool secure;
    bool ps_ready;
    mbedtls_ssl_context tls;
    mbedtls_ssl_config config;
} Transport;

static int wait_for_socket(int fd, short events, u64 deadline)
{
    u64 now = osGetTime();
    if (now >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }

    struct pollfd pending = {.fd = fd, .events = events};
    int ready = poll(&pending, 1, (int)(deadline - now));
    if (ready == 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    return ready < 0 ? -1 : 0;
}

static int connect_to_bridge(int fd, const BridgeEndpoint *endpoint, u64 deadline)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((u16)endpoint->port),
    };
    if (inet_pton(AF_INET, endpoint->address, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        return 0;
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        return -1;
    }
    // On hardware the TCP handshake can finish without POLLOUT waking the caller.
    while (osGetTime() < deadline) {
        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof(peer);
        if (getpeername(fd, (struct sockaddr *)&peer, &peer_length) == 0) return 0;

        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
            error && error != EINPROGRESS && error != EWOULDBLOCK) {
            errno = error;
            return -1;
        }
        svcSleepThread(20 * 1000 * 1000LL);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int psk_random(void *unused, unsigned char *buffer, size_t size)
{
    (void)unused;
    return R_SUCCEEDED(PS_GenerateRandomBytes(buffer, size)) ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_send(void *context, const unsigned char *buffer, size_t size)
{
    int fd = *(int *)context;
    ssize_t sent = send(fd, buffer, size, 0);
    if (sent >= 0) {
        return (int)sent;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_recv(void *context, unsigned char *buffer, size_t size)
{
    int fd = *(int *)context;
    ssize_t received = recv(fd, buffer, size, 0);
    if (received >= 0) {
        return (int)received;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int wait_tls(int fd, int result, u64 deadline)
{
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
        errno = EPROTO;
        return -1;
    }
    u64 now = osGetTime();
    if (now >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        svcSleepThread(10 * 1000 * 1000LL);
        return 0;
    }
    u64 check = now + 60 < deadline ? now + 60 : deadline;
    if (wait_for_socket(fd, POLLIN, check) == 0 || errno == ETIMEDOUT) return 0;
    return -1;
}

static int open_tls(Transport *transport, const BridgeEndpoint *endpoint, u64 deadline)
{
    static const int ciphers[] = {MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0};
    static const int direct_ciphers[] = {MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, 0};
    static const unsigned char direct_key[] = "3DS-CODEX-LAN-DIRECT-V1";
    const char *identity = endpoint->direct ? "lan-direct" : endpoint->identity;
    const unsigned char *key = endpoint->direct ? direct_key : endpoint->key;
    size_t key_length = endpoint->direct ? sizeof(direct_key) - 1 : endpoint->key_length;
    transport->secure = true;
    mbedtls_ssl_init(&transport->tls);
    mbedtls_ssl_config_init(&transport->config);
    if (R_FAILED(psInit())) {
        errno = EACCES;
        return -1;
    }
    transport->ps_ready = true;
    if (mbedtls_ssl_config_defaults(&transport->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        errno = EPROTO;
        return -1;
    }
    mbedtls_ssl_conf_min_version(&transport->config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&transport->config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_ciphersuites(&transport->config, endpoint->direct ? direct_ciphers : ciphers);
    mbedtls_ssl_conf_authmode(&transport->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&transport->config, psk_random, NULL);
    if (mbedtls_ssl_conf_psk(&transport->config, key, key_length,
                            (const unsigned char *)identity, strlen(identity)) != 0 ||
        mbedtls_ssl_setup(&transport->tls, &transport->config) != 0) {
        errno = EPROTO;
        return -1;
    }
    mbedtls_ssl_set_bio(&transport->tls, &transport->fd, tls_send, tls_recv, NULL);
    int result;
    while ((result = mbedtls_ssl_handshake(&transport->tls)) != 0) {
        if (wait_tls(transport->fd, result, deadline) < 0) {
            return -1;
        }
    }
    return 0;
}

static void close_transport(Transport *transport)
{
    if (transport->secure) {
        mbedtls_ssl_free(&transport->tls);
        mbedtls_ssl_config_free(&transport->config);
    }
    if (transport->ps_ready) {
        psExit();
    }
    close(transport->fd);
}

static int transfer(Transport *transport, bool writing, void *buffer, size_t size, u64 deadline)
{
    for (;;) {
        int result;
        if (transport->secure) {
            result = writing ? mbedtls_ssl_write(&transport->tls, buffer, size) :
                               mbedtls_ssl_read(&transport->tls, buffer, size);
            if (result >= 0) {
                return result;
            }
            if (!writing && result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                return 0;
            }
            if (wait_tls(transport->fd, result, deadline) < 0) {
                return -1;
            }
        } else {
            if (wait_for_socket(transport->fd, writing ? POLLOUT : POLLIN, deadline) < 0) {
                return -1;
            }
            result = writing ? (int)send(transport->fd, buffer, size, 0) :
                               (int)recv(transport->fd, buffer, size, 0);
            if (result >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                return result;
            }
        }
    }
}

static int send_all(Transport *transport, const char *data, size_t length, u64 deadline)
{
    size_t sent = 0;
    while (sent < length) {
        size_t chunk = length - sent < 4096 ? length - sent : 4096;
        int count = transfer(transport, true, (void *)(data + sent), chunk, deadline);
        if (count <= 0) {
            return -1;
        }
        sent += (size_t)count;
    }
    return 0;
}

static int send_request(Transport *transport, const BridgeRequest *request, u64 deadline)
{
    char header[640];
    char base[128];
    snprintf(base, sizeof(base), "Host: %s:%u\r\nUser-Agent: 3DS-Codex/0.4\r\nConnection: close\r\n",
             request->endpoint.address, request->endpoint.port);
    size_t size = strlen(request->message);
    int length;
    if (request->audio_length) {
        length = snprintf(header, sizeof(header),
            "POST %s HTTP/1.0\r\n%sContent-Type: application/octet-stream\r\n"
            "X-3DS-Session: %s\r\nContent-Length: %zu\r\n\r\n",
            request->path, base, request->session, request->audio_length);
    } else if (size) {
        length = snprintf(header, sizeof(header),
            "POST %s HTTP/1.0\r\n%sContent-Type: text/plain; charset=utf-8\r\n"
            "X-3DS-Session: %s\r\nX-3DS-Operation: %s\r\nContent-Length: %zu\r\n\r\n",
            request->path, base, request->session, request->operation, size);
    } else if (strcmp(request->path, "/pair") == 0) {
        length = snprintf(header, sizeof(header), "POST /pair HTTP/1.0\r\n%sContent-Length: 0\r\n\r\n", base);
    } else {
        length = snprintf(header, sizeof(header), "GET %s HTTP/1.0\r\n%s\r\n", request->path, base);
    }
    if (length < 0 || (size_t)length >= sizeof(header)) {
        errno = EINVAL;
        return -1;
    }
    if (send_all(transport, header, (size_t)length, deadline) < 0) {
        return -1;
    }
    if (request->audio_length) {
        return send_all(transport, request->audio, request->audio_length, deadline);
    }
    return send_all(transport, request->message, size, deadline);
}

static int receive_response(Transport *transport, char *response, size_t capacity,
                            u64 deadline, size_t *body_length)
{
    size_t length = 0;
    size_t expected = 0;
    while (length + 1 < capacity) {
        int count = transfer(transport, false, response + length, capacity - length - 1, deadline);
        if (count <= 0) {
            if (!count) {
                errno = EPROTO;
            }
            return -1;
        }
        length += (size_t)count;
        response[length] = '\0';
        if (!expected) {
            const char *body = strstr(response, "\r\n\r\n");
            if (body) {
                const char *header = strstr(response, "\r\n");
                while (header && header < body && strncasecmp(header + 2, "Content-Length:", 15) != 0) {
                    header = strstr(header + 2, "\r\n");
                }
                if (!header || header >= body) {
                    errno = EPROTO;
                    return -1;
                }
                char *end;
                unsigned long size = strtoul(header + 17, &end, 10);
                size_t offset = (size_t)(body + 4 - response);
                if (*end != '\r' || size >= capacity - offset) {
                    errno = EMSGSIZE;
                    return -1;
                }
                expected = offset + size;
                *body_length = size;
            }
        }
        if (expected && length >= expected) {
            response[expected] = '\0';
            return 0;
        }
    }
    errno = EMSGSIZE;
    return -1;
}

void fetch_bridge(void *context)
{
    BridgeRequest *request = context;
    request->error[0] = '\0';
    request->response_length = 0;
    request->rejected = false;
    Transport transport = {.fd = socket(AF_INET, SOCK_STREAM, 0)};
    int fd = transport.fd;
    if (fd < 0) {
        snprintf(request->error, sizeof(request->error), "网络出错（%d）。", errno);
        return;
    }

    unsigned timeout = request->timeout_ms ? request->timeout_ms :
                       request->audio_length ? VOICE_TIMEOUT_MS : REQUEST_TIMEOUT_MS;
    u64 deadline = osGetTime() + timeout;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || connect_to_bridge(fd, &request->endpoint, deadline) < 0) {
        snprintf(request->error, sizeof(request->error), "TCP 连接失败（%d）。", errno);
    } else if ((request->endpoint.key_length || request->endpoint.direct) && open_tls(&transport, &request->endpoint, deadline) < 0) {
        snprintf(request->error, sizeof(request->error), errno == EACCES ?
                 "无法访问掌机的加密随机数服务。" : "加密握手失败（%d）。", errno);
    } else if (send_request(&transport, request, deadline) < 0 ||
               receive_response(&transport, request->response, sizeof(request->response), deadline, &request->response_length) < 0) {
        snprintf(request->error, sizeof(request->error), "请求失败（%d）。", errno);
    } else {
        char *body = strstr(request->response, "\r\n\r\n");
        bool ok = strncmp(request->response, "HTTP/1.0 200 ", 13) == 0 ||
                  strncmp(request->response, "HTTP/1.1 200 ", 13) == 0;
        if (!body) {
            snprintf(request->error, sizeof(request->error), "桥接返回了无效内容。");
        } else if (!ok) {
            request->rejected = request->response[9] == '4';
            const char *message = body + 4;
            size_t size = strlen(message);
            if (size >= sizeof(request->error)) {
                size = sizeof(request->error) - 1;
                while (size && ((unsigned char)message[size] & 0xc0) == 0x80) {
                    size--;
                }
            }
            memcpy(request->error, message, size);
            request->error[size] = '\0';
        } else {
            memmove(request->response, body + 4, request->response_length + 1);
        }
    }
    close_transport(&transport);
}


static bool check_discovered(BridgeRequest *request)
{
    fetch_bridge(request);
    return !*request->error && !strcmp(request->response, "3ds-codex-bridge/1\n");
}

typedef struct {
    struct sockaddr_in address;
    bool direct;
} DiscoveryPeer;

static void send_probe(int fd, struct sockaddr_in destination, u32 address)
{
    const char query[] = "3DS-CODEX-DISCOVER-1";
    destination.sin_addr.s_addr = address;
    sendto(fd, query, sizeof(query) - 1, 0, (struct sockaddr *)&destination, sizeof(destination));
}

static unsigned receive_peers(int fd, DiscoveryPeer *peers, unsigned count, unsigned capacity,
                              unsigned timeout, bool has_credentials)
{
    const char marker[] = "3DS-CODEX-BRIDGE-1 ";
    u64 deadline = osGetTime() + timeout;
    while (count < capacity && wait_for_socket(fd, POLLIN, deadline) == 0) {
        char reply[64];
        struct sockaddr_in peer = {0};
        socklen_t size = sizeof(peer);
        ssize_t length = recvfrom(fd, reply, sizeof(reply) - 1, 0, (struct sockaddr *)&peer, &size);
        if (length <= (ssize_t)(sizeof(marker) - 1)) continue;
        reply[length] = '\0';
        if (peer.sin_port != htons(DISCOVERY_PORT) || strncmp(reply, marker, sizeof(marker) - 1)) continue;
        char *end;
        unsigned long port = strtoul(reply + sizeof(marker) - 1, &end, 10);
        bool direct = strcmp(end, " direct") == 0;
        if ((!direct && (*end || !has_credentials)) || !port || port > 65535) continue;
        peer.sin_port = htons((u16)port);
        bool duplicate = false;
        for (unsigned i = 0; i < count; i++) {
            if (peers[i].address.sin_addr.s_addr == peer.sin_addr.s_addr && peers[i].address.sin_port == peer.sin_port) {
                duplicate = true;
            }
        }
        if (!duplicate) peers[count++] = (DiscoveryPeer){peer, direct};
    }
    return count;
}

static unsigned scan_subnet(int fd, struct sockaddr_in destination, struct in_addr ip, struct in_addr mask,
                            DiscoveryPeer *peers, unsigned capacity, bool has_credentials)
{
    u32 local = ntohl(ip.s_addr);
    if (!local) local = ntohl(gethostid());
    bool private_ip = (local >> 24) == 10 || (local >> 20) == 0xac1 || (local >> 16) == 0xc0a8;
    if (!private_ip) return 0;

    u32 subnet_mask = ntohl(mask.s_addr) | 0xffffff00u;
    u32 first = (local & subnet_mask) + 1;
    u32 end = local | ~subnet_mask;
    unsigned count = 0;
    // Broadcast can be dropped even after sendto succeeds. Probe only the local subnet.
    for (u32 start = first; start < end && !count; start += 16) {
        for (u32 peer = start; peer < start + 16 && peer < end; peer++) {
            if (peer != local) send_probe(fd, destination, htonl(peer));
        }
        count = receive_peers(fd, peers, count, capacity, 95, has_credentials);
    }
    return count ? count : receive_peers(fd, peers, 0, capacity, 400, has_credentials);
}

void discover_bridge(void *context)
{
    BridgeRequest *request = context;
    BridgeEndpoint saved = request->endpoint;
    request->timeout_ms = 3600;
    char connection_error[sizeof(request->error)] = "";
    strcpy(request->path, "/health");
    request->message[0] = '\0';

    DiscoveryPeer peers[6];
    unsigned count = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT)};
        int enable = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
        fcntl(fd, F_SETFL, O_NONBLOCK);

        struct in_addr ip = {0}, mask = {0}, broadcast = {0};
        SOCU_GetIPInfo(&ip, &mask, &broadcast);
        if (broadcast.s_addr) send_probe(fd, destination, broadcast.s_addr);
        send_probe(fd, destination, INADDR_BROADCAST);
        send_probe(fd, destination, htonl(INADDR_LOOPBACK));
        if (*saved.address) send_probe(fd, destination, inet_addr(saved.address));
        count = receive_peers(fd, peers, count, sizeof(peers) / sizeof(peers[0]), 650, saved.key_length != 0);
        if (!count) {
            count = scan_subnet(fd, destination, ip, mask, peers, sizeof(peers) / sizeof(peers[0]),
                                saved.key_length != 0);
        }
        close(fd);
    }
    for (unsigned pass = 0; pass < 2; pass++) {
        bool direct = pass != 0;
        if (!direct && !saved.key_length) continue;
        for (unsigned i = 0; i < count; i++) {
            if (direct && !peers[i].direct) continue;
            request->endpoint = saved;
            request->endpoint.direct = direct;
            if (direct) request->endpoint.key_length = 0;
            if (!inet_ntop(AF_INET, &peers[i].address.sin_addr, request->endpoint.address, sizeof(saved.address))) continue;
            request->endpoint.port = ntohs(peers[i].address.sin_port);
            if (check_discovered(request)) {
                request->timeout_ms = 0;
                return;
            }
            snprintf(connection_error, sizeof(connection_error), "%s", request->error);
        }
        if (!direct && *saved.address) {
            request->endpoint = saved;
            if (check_discovered(request)) {
                request->timeout_ms = 0;
                return;
            }
            snprintf(connection_error, sizeof(connection_error), "%s", request->error);
        }
    }
    request->endpoint = saved;
    request->timeout_ms = 0;
    snprintf(request->error, sizeof(request->error), "%s",
             *connection_error ? connection_error : "未找到可连接的电脑，请检查同一局域网和桥接程序。");
}
