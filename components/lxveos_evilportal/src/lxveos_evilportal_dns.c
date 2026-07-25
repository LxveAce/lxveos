// See lxveos_evilportal_dns.h. Pure captive-portal DNS reply builder, host-tested off-target
// (tests/host_c/test_evilportal_dns.c). Behaviour-preserving extraction of the packet-build that was inline in
// lxveos_evilportal.c's dns_task — no socket, no TX authored.
#include "lxveos_evilportal_dns.h"

size_t lxveos_evilportal_dns_reply(uint8_t *buf, size_t len, size_t cap, const uint8_t ip[4])
{
    if (buf == NULL || ip == NULL || len < 12) {
        return 0;  // too short to hold a DNS header
    }
    buf[2] = 0x81;  // QR=1, opcode 0, RD copied
    buf[3] = 0x80;  // RA=1, RCODE=0
    buf[6] = 0x00;
    buf[7] = 0x01;  // ANCOUNT = 1
    buf[8] = buf[9] = buf[10] = buf[11] = 0x00;  // NSCOUNT / ARCOUNT = 0
    size_t qend = 12;
    while (qend < len && buf[qend] != 0) {
        qend += (size_t)buf[qend] + 1;  // walk the QNAME labels (reads buf[qend] only while qend < len)
    }
    qend += 5;  // null label (1) + QTYPE (2) + QCLASS (2)
    if (qend > len || qend + 16 > cap) {
        return 0;  // malformed (question runs past the packet) or no room for the answer
    }
    size_t p = qend;
    buf[p++] = 0xC0;  buf[p++] = 0x0C;  // NAME: compression pointer to the question at offset 12
    buf[p++] = 0x00;  buf[p++] = 0x01;  // TYPE  A
    buf[p++] = 0x00;  buf[p++] = 0x01;  // CLASS IN
    buf[p++] = 0x00;  buf[p++] = 0x00;  buf[p++] = 0x00;  buf[p++] = 0x3c;  // TTL 60s
    buf[p++] = 0x00;  buf[p++] = 0x04;  // RDLENGTH 4
    buf[p++] = ip[0];  buf[p++] = ip[1];  buf[p++] = ip[2];  buf[p++] = ip[3];  // RDATA (the AP gateway)
    return p;
}
