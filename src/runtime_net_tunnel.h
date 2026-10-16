#ifndef RUNTIME_NET_TUNNEL_H
#define RUNTIME_NET_TUNNEL_H

/* RyuNET tunnel: the game's traffic to a peer it cannot reach directly (strict or carrier
 * NAT on either side) goes through the server's UDP port, wrapped, and comes out at the peer
 * as if sent straight to it. The server side is nat_tunnel.go in ryunet-demonware-ctr, which
 * describes the packets. Pure bookkeeping here; runtime_net.c sends and receives. Addresses
 * are an IPv4 address in network byte order and a port in host byte order. */

#include <stddef.h>
#include <stdint.h>

#define TUN_HEADER_LEN 13
#define TUN_KEEPALIVE "RYUKA1"
#define TUN_KEEPALIVE_LEN 6
#define TUN_KEEPALIVE_MS 15000u
/* A peer counts as reachable directly while its packets keep arriving this often. */
#define TUN_DIRECT_FRESH_MS 3000u
/* A peer that never answers through the tunnel (a port without it) stops getting copies. */
#define TUN_GIVE_UP_MS 20000u
#define TUN_PEER_IDLE_MS 60000u

void tun_reset(void);
/* Whether the game's packets to this address may need the tunnel: a public address that is
 * not the server itself. LAN, loopback and carrier-grade (100.64/10) ones go direct only. */
int tun_candidate(uint32_t ip, uint32_t server_ip);
/* The game sends to a peer: 1 when a wrapped copy should also go through the server, with
 * *flags set to what this side tells the peer (bit 0: its direct packets reach us). */
int tun_on_send(uint32_t ip, uint16_t port, uint64_t now_ms, uint8_t *flags);
void tun_on_direct(uint32_t ip, uint16_t port, uint64_t now_ms);
void tun_on_tunnelled(uint32_t ip, uint16_t port, uint8_t flags, uint64_t now_ms);
/* Keepalive due (keeps this socket's path to the server open): 1 once per interval. */
int tun_keepalive_due(uint64_t now_ms);

size_t tun_wrap(uint8_t *out, size_t cap, uint8_t flags, uint32_t ip, uint16_t port,
                const void *payload, size_t len);
/* A packet from the server: 1 for a tunnelled one (its payload starts at TUN_HEADER_LEN),
 * 0 for anything else. */
int tun_unwrap(const uint8_t *in, size_t len, uint8_t *flags, uint32_t *ip, uint16_t *port);

/* For the log: how many peers currently get tunnelled copies. */
int tun_active_peers(uint64_t now_ms);

#endif
