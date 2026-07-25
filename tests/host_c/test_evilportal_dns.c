// Host-side unit test for lxveos_evilportal_dns_reply (captive-portal DNS reply builder). Pure libc, no lwip.
// Built + run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion.
#include "lxveos_evilportal_dns.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t ip[4] = {192, 168, 4, 1};

    // A well-formed DNS A query for "a.com": 12-byte header (QDCOUNT=1) + QNAME(1 a 3 c o m 0) + QTYPE A + QCLASS IN.
    uint8_t q[512] = {
        0xAB, 0xCD,                          // ID
        0x01, 0x00,                          // flags (RD)
        0x00, 0x01,                          // QDCOUNT = 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // AN / NS / AR = 0
        0x01, 'a', 0x03, 'c', 'o', 'm', 0x00,  // QNAME
        0x00, 0x01,                          // QTYPE  A
        0x00, 0x01,                          // QCLASS IN
    };

    size_t rlen = lxveos_evilportal_dns_reply(q, 23, sizeof(q), ip);
    assert(rlen == 39);                                 // 23-byte question + a 16-byte A-record answer
    assert(q[2] == 0x81 && q[3] == 0x80);               // QR=1, RA=1, RCODE=0
    assert(q[6] == 0x00 && q[7] == 0x01);               // ANCOUNT = 1
    assert(q[8] == 0 && q[9] == 0 && q[10] == 0 && q[11] == 0);  // NS / AR = 0
    // The question is preserved (QNAME + QTYPE + QCLASS untouched).
    assert(q[12] == 0x01 && q[13] == 'a' && q[18] == 0x00 && q[19] == 0x00 && q[20] == 0x01);
    // The A-record answer appended right after the question (offset 23).
    const uint8_t want_ans[16] = {
        0xC0, 0x0C,              // NAME: compression pointer to the question
        0x00, 0x01,              // TYPE  A
        0x00, 0x01,              // CLASS IN
        0x00, 0x00, 0x00, 0x3c,  // TTL 60
        0x00, 0x04,              // RDLENGTH 4
        192, 168, 4, 1,          // RDATA
    };
    assert(memcmp(q + 23, want_ans, 16) == 0);

    // The `ip` argument is what lands in RDATA (not hard-coded) — a distinct IP flows through.
    uint8_t q2[64] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        0x01, 'a', 0x03, 'c', 'o', 'm', 0x00, 0x00, 0x01, 0x00, 0x01,
    };
    const uint8_t ip2[4] = {10, 0, 0, 5};
    assert(lxveos_evilportal_dns_reply(q2, 23, sizeof(q2), ip2) == 39);
    assert(q2[35] == 10 && q2[36] == 0 && q2[37] == 0 && q2[38] == 5);  // RDATA = ip2

    // Too short to hold a DNS header (< 12) -> 0.
    uint8_t tiny[8] = {0};
    assert(lxveos_evilportal_dns_reply(tiny, 8, sizeof(tiny), ip) == 0);

    // Malformed: a QNAME label length walks past the packet (never null-terminates) -> 0, no over-read.
    uint8_t bad[14] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x05, 'x'};  // buf[12]=5 but only 1 byte follows
    assert(lxveos_evilportal_dns_reply(bad, sizeof(bad), sizeof(bad), ip) == 0);

    // A valid question but no room for the 16-byte answer -> 0 (never overruns the buffer).
    uint8_t q3[64] = {
        0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        0x01, 'a', 0x03, 'c', 'o', 'm', 0x00, 0x00, 0x01, 0x00, 0x01,
    };
    assert(lxveos_evilportal_dns_reply(q3, 23, 30, ip) == 0);  // qend 23 + 16 = 39 > cap 30

    // NULL args are safe.
    assert(lxveos_evilportal_dns_reply(NULL, 23, 512, ip) == 0);
    assert(lxveos_evilportal_dns_reply(q, 23, sizeof(q), NULL) == 0);

    printf("test_evilportal_dns: all tests passed\n");
    return 0;
}
