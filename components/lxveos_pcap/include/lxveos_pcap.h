#pragma once
// lxveos_pcap — dependency-free (libc-only) encoder for the classic little-endian PCAP capture format: the
// 24-byte global file header and the 16-byte per-record header. Kept a standalone component so the exact byte
// layout host-tests off-target (tests/host_c/test_pcap.c). The pcap_log op (writing captured frames to an SD
// card) stays implemented:false until the storage path is wired; this is only the pure header encoder.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXVEOS_PCAP_GLOBAL_HDR_LEN  24u
#define LXVEOS_PCAP_RECORD_HDR_LEN  16u
#define LXVEOS_PCAP_DLT_IEEE802_11  105u  // LINKTYPE_IEEE802_11 (the Wireshark link type for raw 802.11)

// Write the 24-byte PCAP global header into out[cap] (little-endian: magic a1b2c3d4, version 2.4, a zero
// timezone/sigfigs pair, then `snaplen` and the DLT `network` link type). Returns LXVEOS_PCAP_GLOBAL_HDR_LEN,
// or 0 if out is NULL or cap is too small.
size_t lxveos_pcap_global_header(uint8_t *out, size_t cap, uint32_t snaplen, uint32_t network);

// Write the 16-byte PCAP per-record header into out[cap] (little-endian: ts_sec, ts_usec, captured length,
// original length). Returns LXVEOS_PCAP_RECORD_HDR_LEN, or 0 if out is NULL or cap is too small.
size_t lxveos_pcap_record_header(uint8_t *out, size_t cap, uint32_t ts_sec, uint32_t ts_usec,
                                 uint32_t incl_len, uint32_t orig_len);

#ifdef __cplusplus
}
#endif
