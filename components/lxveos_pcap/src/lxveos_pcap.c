// lxveos_pcap — see lxveos_pcap.h. Dependency-free PCAP header encoder, host-tested off-target
// (tests/host_c/test_pcap.c). Explicit little-endian byte writes so the output is identical on any host or
// target endianness. libc-only, no allocation.
#include "lxveos_pcap.h"

static void wr_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t lxveos_pcap_global_header(uint8_t *out, size_t cap, uint32_t snaplen, uint32_t network)
{
    if (out == NULL || cap < LXVEOS_PCAP_GLOBAL_HDR_LEN) {
        return 0;
    }
    wr_u32le(out + 0, 0xa1b2c3d4u);  // magic number (little-endian capture file)
    wr_u16le(out + 4, 2);            // version_major
    wr_u16le(out + 6, 4);            // version_minor
    wr_u32le(out + 8, 0);            // thiszone (GMT-to-local correction; always 0 in practice)
    wr_u32le(out + 12, 0);           // sigfigs (timestamp accuracy; always 0)
    wr_u32le(out + 16, snaplen);     // snaplen (max captured bytes per packet)
    wr_u32le(out + 20, network);     // network (data-link type, e.g. DLT_IEEE802_11)
    return LXVEOS_PCAP_GLOBAL_HDR_LEN;
}

size_t lxveos_pcap_record_header(uint8_t *out, size_t cap, uint32_t ts_sec, uint32_t ts_usec,
                                 uint32_t incl_len, uint32_t orig_len)
{
    if (out == NULL || cap < LXVEOS_PCAP_RECORD_HDR_LEN) {
        return 0;
    }
    wr_u32le(out + 0, ts_sec);
    wr_u32le(out + 4, ts_usec);
    wr_u32le(out + 8, incl_len);
    wr_u32le(out + 12, orig_len);
    return LXVEOS_PCAP_RECORD_HDR_LEN;
}
