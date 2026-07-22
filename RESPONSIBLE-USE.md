# Responsible use

LxveOS is for authorized, lawful security research and education only. By building, flashing, or using it you
agree to use it only on networks, devices, and hardware you own or have explicit written permission to test.

## First-run gate
Every build shows an authorized-use acknowledgement on first boot (and on the serial/CLI banner) before any radio
feature is available. Feature modules stay disabled until it is acknowledged.

## Hard rules (non-negotiable)
- **No jammer, ever.** LxveOS does not build or transmit broadband RF-jamming / deauth-flood frames in any tier.
  Wi-Fi deauthentication ships only as detection (defensive). RF jamming violates FCC Part 15 / 47 U.S.C. §333
  and equivalents.
- nRF24 / CC1101 / SubGHz: receive and analyze; transmit only on devices you own, in an authorized and ideally
  RF-shielded context, respecting local regulations.
- NFC/RFID: provided where lawful; some schemes (e.g. MIFARE Crypto1) remain patent-encumbered, documented but
  not circumvented.
- Unauthorized use may violate the CFAA (US), the Computer Misuse Act (UK), and equivalents worldwide. You are
  solely responsible for compliance.

This is a good-faith notice, not legal advice. If in doubt about what is lawful for you, consult a qualified
attorney in your jurisdiction.
