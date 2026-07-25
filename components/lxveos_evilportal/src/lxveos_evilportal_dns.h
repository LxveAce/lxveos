#pragma once
// Pure captive-portal DNS reply builder, split out of lxveos_evilportal.c's dns_task so the exact DNS packet
// layout host-tests off-target (tests/host_c/test_evilportal_dns.c) without lwip / FreeRTOS. No socket, no TX:
// this only rewrites an in-memory DNS packet; the dns_task recvfrom/sendto loop wraps it.
#include <stddef.h>
#include <stdint.h>

// Turn a DNS query in `buf` (holding `len` request bytes, buffer capacity `cap`) into a captive-portal reply
// IN PLACE: flip the header to a response and append a single A-record answer pointing the query at `ip[4]`
// (the AP gateway) so a client's captive-portal detection resolves to us. Returns the new packet length, or 0
// if the request is too short (< 12) / malformed / there is no room for the 16-byte answer (the caller drops
// it). Pure — dependency-free, no socket.
size_t lxveos_evilportal_dns_reply(uint8_t *buf, size_t len, size_t cap, const uint8_t ip[4]);
