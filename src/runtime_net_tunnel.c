#include "runtime_net_tunnel.h"

#include <stdatomic.h>
#include <string.h>

/* Per peer address the game sends to:
 *   last_direct    when a packet last arrived from it directly
 *   peer_ok        its latest tunnelled packet said our direct packets reach it
 * A copy goes through the tunnel unless both directions work (our direct packets arrive and
 * its direct packets arrive), so a one-way path still gets the other way tunnelled. Each side
 * keeps sending directly too, which is how the other side notices the direct path. */
typedef struct {
    uint32_t ip;
    uint16_t port;
    int used;
    int peer_ok;
    int answered;          /* it sent something through the tunnel: it has the tunnel too */
    uint64_t first_send;
    uint64_t last_use;
    uint64_t last_direct;
    uint64_t last_tunnel_send;
} TunPeer;

#define TUN_MAX_PEERS 64

static TunPeer peers[TUN_MAX_PEERS];
static uint64_t keepalive_at;
static atomic_flag lock = ATOMIC_FLAG_INIT;

static void take(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void give(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

void tun_reset(void) {
    take();
    memset(peers, 0, sizeof(peers));
    keepalive_at = 0;
    give();
}

int tun_candidate(uint32_t ip, uint32_t server_ip) {
    const uint8_t *b = (const uint8_t *)&ip;
    if (ip == server_ip || ip == 0 || ip == 0xFFFFFFFFu) return 0;
    if (b[0] == 10 || b[0] == 127 || b[0] == 0 || b[0] >= 224) return 0;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) return 0;
    if (b[0] == 192 && b[1] == 168) return 0;
    if (b[0] == 169 && b[1] == 254) return 0;
    if (b[0] == 100 && (b[1] & 0xC0) == 64) return 0;
    if (b[0] == 0 && b[1] == 255) return 0; /* the game's empty address slot */
    return 1;
}

/* The peer's entry, made (reusing the idlest one) when missing. Holds the lock. */
static TunPeer *peer_for(uint32_t ip, uint16_t port, uint64_t now, int create) {
    TunPeer *free_slot = NULL, *idlest = NULL;
    for (int i = 0; i < TUN_MAX_PEERS; i++) {
        TunPeer *p = &peers[i];
        if (p->used && p->ip == ip && p->port == port) return p;
        if (!p->used || now - p->last_use > TUN_PEER_IDLE_MS) {
            if (!free_slot) free_slot = p;
        } else if (!idlest || p->last_use < idlest->last_use) {
            idlest = p;
        }
    }
    if (!create) return NULL;
    TunPeer *p = free_slot ? free_slot : idlest;
    memset(p, 0, sizeof(*p));
    p->ip = ip;
    p->port = port;
    p->used = 1;
    p->first_send = now;
    p->last_use = now;
    return p;
}

static int direct_fresh(const TunPeer *p, uint64_t now) {
    return p->last_direct && now - p->last_direct < TUN_DIRECT_FRESH_MS;
}

int tun_on_send(uint32_t ip, uint16_t port, uint64_t now_ms, uint8_t *flags) {
    take();
    TunPeer *p = peer_for(ip, port, now_ms, 1);
    p->last_use = now_ms;
    const int fresh = direct_fresh(p, now_ms);
    *flags = fresh ? 1 : 0;
    int tunnel = !(fresh && p->peer_ok);
    if (tunnel && !p->answered && now_ms - p->first_send > TUN_GIVE_UP_MS) tunnel = 0;
    if (tunnel) p->last_tunnel_send = now_ms;
    give();
    return tunnel;
}

void tun_on_direct(uint32_t ip, uint16_t port, uint64_t now_ms) {
    take();
    TunPeer *p = peer_for(ip, port, now_ms, 0);
    if (p) {
        p->last_direct = now_ms;
        p->last_use = now_ms;
    }
    give();
}

void tun_on_tunnelled(uint32_t ip, uint16_t port, uint8_t flags, uint64_t now_ms) {
    take();
    TunPeer *p = peer_for(ip, port, now_ms, 1);
    p->peer_ok = flags & 1;
    p->answered = 1;
    p->last_use = now_ms;
    give();
}

int tun_keepalive_due(uint64_t now_ms) {
    take();
    const int due = keepalive_at == 0 || now_ms - keepalive_at >= TUN_KEEPALIVE_MS;
    if (due) keepalive_at = now_ms;
    give();
    return due;
}

int tun_active_peers(uint64_t now_ms) {
    int n = 0;
    take();
    for (int i = 0; i < TUN_MAX_PEERS; i++) {
        if (peers[i].used && peers[i].last_tunnel_send && now_ms - peers[i].last_tunnel_send < TUN_DIRECT_FRESH_MS) n++;
    }
    give();
    return n;
}

size_t tun_wrap(uint8_t *out, size_t cap, uint8_t flags, uint32_t ip, uint16_t port,
                const void *payload, size_t len) {
    if (cap < TUN_HEADER_LEN + len) return 0;
    memcpy(out, "RYUTN1", 6);
    out[6] = flags;
    memcpy(out + 7, &ip, 4);
    out[11] = (uint8_t)(port >> 8);
    out[12] = (uint8_t)port;
    memcpy(out + TUN_HEADER_LEN, payload, len);
    return TUN_HEADER_LEN + len;
}

int tun_unwrap(const uint8_t *in, size_t len, uint8_t *flags, uint32_t *ip, uint16_t *port) {
    if (len < TUN_HEADER_LEN || memcmp(in, "RYUTN1", 6) != 0) return 0;
    *flags = in[6];
    memcpy(ip, in + 7, 4);
    *port = (uint16_t)(in[11] << 8 | in[12]);
    return 1;
}
