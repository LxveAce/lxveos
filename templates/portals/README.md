# Captive-portal templates (generic)

Reusable, generic captive-portal login pages for authorized network / phishing-awareness testing.
They are deliberately unbranded (no logos, names, or styling of any real company), so they impersonate
no one. Each posts a `username` and `password` field to `/login`; the LxveOS `evil_portal` engine (and the
Cyber Controller template feature) serve the selected one and record submissions.

To add a brand-specific page for a specific authorized engagement, drop your own HTML in this folder using
the same `username`/`password` form fields, and the engine loads any template. Authoring real-brand impersonation
pages is left to the operator running the authorized test.

| file | pretext |
|------|---------|
| `netlogin.html`      | Generic "network sign-in required" |
| `router_update.html` | Generic router administration sign-in |
| `wifi_guest.html`    | Guest Wi-Fi access (email + access code) |
| `session_reauth.html`| Generic "session expired, re-authenticate" |
