// Host-side unit test for lxveos_pcap (the little-endian PCAP header encoder). Pure libc, no ESP-IDF. Built +
// run by tests/host_c/run.sh. Aborts (non-zero exit) on the first failed assertion. The expected byte layouts
// were cross-checked with Python's struct.pack('<...') before committing.
#include "lxveos_pcap.h"

#include <assert.h>
#include <stdio.h>

static void test_global_header(void)
{
    uint8_t buf[LXVEOS_PCAP_GLOBAL_HDR_LEN];
    assert(lxveos_pcap_global_header(buf, sizeof(buf), 65535, LXVEOS_PCAP_DLT_IEEE802_11)
           == LXVEOS_PCAP_GLOBAL_HDR_LEN);
    // magic a1b2c3d4 in a little-endian file -> d4 c3 b2 a1
    assert(buf[0] == 0xd4 && buf[1] == 0xc3 && buf[2] == 0xb2 && buf[3] == 0xa1);
    // version_major 2, version_minor 4
    assert(buf[4] == 0x02 && buf[5] == 0x00 && buf[6] == 0x04 && buf[7] == 0x00);
    // thiszone + sigfigs are both zero (bytes 8..15)
    for (int i = 8; i < 16; i++) {
        assert(buf[i] == 0x00);
    }
    // snaplen 65535 = 0x0000ffff -> ff ff 00 00
    assert(buf[16] == 0xff && buf[17] == 0xff && buf[18] == 0x00 && buf[19] == 0x00);
    // network 105 (DLT_IEEE802_11) -> 69 00 00 00
    assert(buf[20] == 0x69 && buf[21] == 0x00 && buf[22] == 0x00 && buf[23] == 0x00);
    // a too-small buffer or NULL writes nothing and returns 0
    assert(lxveos_pcap_global_header(buf, LXVEOS_PCAP_GLOBAL_HDR_LEN - 1, 65535, 105) == 0);
    assert(lxveos_pcap_global_header(NULL, 24, 65535, 105) == 0);
}

static void test_record_header(void)
{
    uint8_t buf[LXVEOS_PCAP_RECORD_HDR_LEN];
    assert(lxveos_pcap_record_header(buf, sizeof(buf), 0x11223344u, 0x55667788u, 100, 100)
           == LXVEOS_PCAP_RECORD_HDR_LEN);
    // ts_sec 0x11223344 -> 44 33 22 11 ; ts_usec 0x55667788 -> 88 77 66 55
    assert(buf[0] == 0x44 && buf[1] == 0x33 && buf[2] == 0x22 && buf[3] == 0x11);
    assert(buf[4] == 0x88 && buf[5] == 0x77 && buf[6] == 0x66 && buf[7] == 0x55);
    // incl_len and orig_len 100 = 0x64 -> 64 00 00 00
    assert(buf[8] == 0x64 && buf[9] == 0x00 && buf[10] == 0x00 && buf[11] == 0x00);
    assert(buf[12] == 0x64 && buf[13] == 0x00 && buf[14] == 0x00 && buf[15] == 0x00);
    assert(lxveos_pcap_record_header(buf, LXVEOS_PCAP_RECORD_HDR_LEN - 1, 0, 0, 0, 0) == 0);
    assert(lxveos_pcap_record_header(NULL, 16, 0, 0, 0, 0) == 0);
}

int main(void)
{
    test_global_header();
    test_record_header();
    printf("test_pcap: all tests passed\n");
    return 0;
}
