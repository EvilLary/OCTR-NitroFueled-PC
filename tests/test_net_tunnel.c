/* The RyuNET tunnel's bookkeeping (src/runtime_net_tunnel.c): when the game's packets to a
 * peer also go through the server, and the wrapped packet format nat_tunnel.go reads. */
#include "runtime_net_tunnel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t ip4(int a, int b, int c, int d) {
    uint8_t bytes[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    uint32_t ip;
    memcpy(&ip, bytes, 4);
    return ip;
}

static void candidates(void) {
    const uint32_t server = ip4(165, 1, 125, 122);
    CHECK(tun_candidate(ip4(179, 60, 85, 233), server));
    CHECK(tun_candidate(ip4(26, 241, 245, 203), server)); /* Radmin's range is a public one */
    CHECK(!tun_candidate(server, server));
    CHECK(!tun_candidate(ip4(192, 168, 18, 53), server));
    CHECK(!tun_candidate(ip4(10, 0, 0, 23), server));
    CHECK(!tun_candidate(ip4(172, 20, 1, 1), server));
    CHECK(tun_candidate(ip4(172, 32, 1, 1), server));
    CHECK(!tun_candidate(ip4(127, 0, 0, 1), server));
    CHECK(!tun_candidate(ip4(100, 64, 3, 4), server));
    CHECK(!tun_candidate(ip4(0, 255, 0, 255), server));
    CHECK(!tun_candidate(ip4(239, 1, 1, 1), server));
}

static void wrap_format(void) {
    uint8_t out[64], flags;
    const uint8_t payload[] = {0x0C, 0x02, 0x00, 0xAB};
    const uint32_t peer = ip4(201, 189, 26, 165);
    size_t n = tun_wrap(out, sizeof(out), 1, peer, 3074, payload, sizeof(payload));
    CHECK(n == TUN_HEADER_LEN + sizeof(payload));
    CHECK(memcmp(out, "RYUTN1", 6) == 0 && out[6] == 1);
    CHECK(out[7] == 201 && out[8] == 189 && out[9] == 26 && out[10] == 165);
    CHECK(out[11] == 0x0C && out[12] == 0x02); /* 3074, big-endian */
    CHECK(memcmp(out + TUN_HEADER_LEN, payload, sizeof(payload)) == 0);
    uint32_t ip;
    uint16_t port;
    CHECK(tun_unwrap(out, n, &flags, &ip, &port) && flags == 1 && ip == peer && port == 3074);
    CHECK(tun_wrap(out, 10, 0, peer, 1, payload, sizeof(payload)) == 0);
    const uint8_t nat_reply[] = {0x15, 0x02, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    CHECK(!tun_unwrap(nat_reply, sizeof(nat_reply), &flags, &ip, &port));
}

/* Two ports, A and B, as the two ends. */
static void both_directions_work(void) {
    tun_reset();
    const uint32_t b = ip4(187, 190, 184, 34);
    uint8_t flags;
    uint64_t t = 1000;
    /* Nothing proven yet: copies go through the tunnel, saying "your direct packets do not reach me". */
    CHECK(tun_on_send(b, 18270, t, &flags) == 1 && flags == 0);
    /* B's direct packets arrive: A now tells B so, but keeps tunnelling until B says the same. */
    tun_on_direct(b, 18270, t + 10);
    CHECK(tun_on_send(b, 18270, t + 20, &flags) == 1 && flags == 1);
    tun_on_tunnelled(b, 18270, 1, t + 30);
    CHECK(tun_on_send(b, 18270, t + 40, &flags) == 0 && flags == 1);
    /* Direct traffic from B stops: the tunnel takes over again. */
    CHECK(tun_on_send(b, 18270, t + 40 + TUN_DIRECT_FRESH_MS, &flags) == 1 && flags == 0);
}

static void one_way_path_keeps_the_tunnel(void) {
    tun_reset();
    const uint32_t b = ip4(187, 190, 184, 34);
    uint8_t flags;
    /* A gets B's direct packets, but B does not get A's (it says so through the tunnel). */
    tun_on_send(b, 18270, 1000, &flags);
    for (uint64_t t = 1100; t < 30000; t += 500) {
        tun_on_direct(b, 18270, t);
        tun_on_tunnelled(b, 18270, 0, t);
        CHECK(tun_on_send(b, 18270, t + 1, &flags) == 1 && flags == 1);
    }
}

static void peer_without_the_tunnel(void) {
    tun_reset();
    const uint32_t old = ip4(201, 1, 2, 3);
    uint8_t flags;
    CHECK(tun_on_send(old, 3074, 0 + 1, &flags) == 1);
    CHECK(tun_on_send(old, 3074, TUN_GIVE_UP_MS, &flags) == 1);
    /* It never answered through the tunnel: copies stop. */
    CHECK(tun_on_send(old, 3074, TUN_GIVE_UP_MS + 2, &flags) == 0);
}

static void keepalive(void) {
    tun_reset();
    CHECK(tun_keepalive_due(5));
    CHECK(!tun_keepalive_due(5 + TUN_KEEPALIVE_MS - 1));
    CHECK(tun_keepalive_due(5 + TUN_KEEPALIVE_MS));
}

static void many_peers(void) {
    tun_reset();
    uint8_t flags;
    for (int i = 0; i < 200; i++) CHECK(tun_on_send(ip4(80, 1, (uint8_t)(i >> 8), (uint8_t)i), 3074, 1000 + (uint64_t)i, &flags) == 1);
    CHECK(tun_active_peers(1300) == 64);
}

int main(void) {
    candidates();
    wrap_format();
    both_directions_work();
    one_way_path_keeps_the_tunnel();
    peer_without_the_tunnel();
    keepalive();
    many_peers();
    if (failures) {
        fprintf(stderr, "net tunnel: %d failure(s)\n", failures);
        return 1;
    }
    puts("net tunnel: ok");
    return 0;
}
