# PDW — Technical Fixes & Architecture Decisions

This document tracks non-obvious implementation decisions, bug fixes, and architectural choices made in this fork. It is aimed at contributors and integrators, not end users.

For the feature overview and user-facing changelog see [README.md](README.md).

---

## Fix index

| Tag | File(s) | Summary |
|-----|---------|---------|
| `[MysqlUtf8]` | `utils/mysql.cpp` | Sanitize message/label to valid UTF-8 before INSERT |
| `[MySQLFeed]` | `utils/mysql.cpp/.h` | Native MySQL wire-protocol feed, no external DLLs |
| `[RxQualAlert]` | `RxQualMonitor.cpp/.h` | E-mail alert when RXQ drops below threshold |
| `[SmtpThreadRace]` | `utils/smtp.cpp` | Heap corruption from concurrent SMTP workers |
| `[SmtpHelo]` | `utils/smtp.cpp` | RFC 5321 EHLO argument: user input → socket IP literal |
| `[SmtpTLS]` | `utils/smtp.cpp` | Port 465 = implicit TLS; 587/25 = STARTTLS + second EHLO |
| `[MailSplit]` | `utils/smtp.cpp` `PDW.cpp` | Independent Subject/Body field bitmasks |
| `[SmtpLog]` | `utils/smtp.cpp` | SMTP error logging with disk backup |
| `[TelnetReject]` | `Misc.cpp` | `TelnetServerNotifyMessage` moved before early-return paths |
| `[TsStartupGating]` | `utils/telnet_server.cpp` | RS232 startup state: no false `<RS232:1>` on first byte |
| `[DpiScale]` | `PDW.cpp` `Gfx.cpp` | High-DPI manifest + runtime `Scale()` helper |
| `[RS232Flap]` | `utils/rs232.cpp` | Debounce for short disconnects on unstable COM links |
| `[TrayBalloon]` | `PDW.cpp` | Replace legacy tray balloon with `IUserNotification` |

---

## Detailed notes

### `[MysqlUtf8]`
**File:** `utils/mysql.cpp`

MySQL `utf8mb4` columns reject bytes that are not valid UTF-8. FLEX/POCSAG messages occasionally contain invalid sequences (legacy encoding artefacts, partial multi-byte characters). Before every INSERT, the message and label fields are walked byte-by-byte:
- Valid UTF-8 sequences (1–4 bytes, correct continuation bytes) are copied as-is.
- The 0xBB marker byte (used internally as a colour tag) is stripped.
- Any byte that cannot be part of a valid UTF-8 sequence is replaced with `?`.

Without this, a single bad message can throw an error that breaks the entire connection.

---

### `[SmtpThreadRace]`
**File:** `utils/smtp.cpp`

Symptom: heap corruption exception (`0xc0000374`) when clicking the SMTP Test button quickly, or when two messages arrived within the previous worker's lifetime.

Root cause: `MailInit()` was creating a new worker thread without waiting for the previous one to finish. The new thread allocated a fresh `MailContext` on the same heap while the old thread was still running and potentially freeing or writing to the same region.

Fix: PDW now maintains a single long-lived SMTP worker thread that processes a job queue. `MailInit()` signals the existing thread to drain and only replaces it if it has genuinely exited. Rapid Test-button clicks are serialised through the queue.

---

### `[SmtpHelo]`
**File:** `utils/smtp.cpp`

RFC 5321 §4.1.1.1 requires that the EHLO argument for a connection from a numeric IP address be an address literal in brackets: `EHLO [a.b.c.d]`.

Previous behaviour: the user-configured "HELO domain" field was sent verbatim. Strict servers (e.g. Telenet cmsmtp) rejected bare IPs without brackets.

Fix priority:
1. Use the user-configured domain if non-empty.
2. Retrieve the local socket address after `connect()` and format it as `[a.b.c.d]`.
3. Fall back to `[127.0.0.1]`.

---

### `[SmtpTLS]`
**File:** `utils/smtp.cpp`

Two distinct TLS modes:

- **Port 465 (implicit TLS):** The TLS handshake happens immediately after `connect()`, before any SMTP dialogue. PDW calls `SSL_connect()` first, then sends `EHLO`.
- **Port 587 / 25 (STARTTLS):** Plain SMTP starts, server advertises `STARTTLS` in the EHLO response, PDW sends `STARTTLS`, upgrades the socket to TLS, and then sends a **second EHLO** over the encrypted channel. The second EHLO is mandatory — many servers return a different capability list over TLS (e.g. AUTH methods only appear after STARTTLS).

Previously the second EHLO was absent, causing AUTH failures on strict servers.

---

### `[MailSplit]`
**Files:** `utils/smtp.cpp`, `PDW.cpp`

Previously a single `nMailOptions` bitmask controlled which fields appeared in the e-mail (they all went into the body). Some users wanted a short Subject with just the capcode and a full Body with all fields.

Fix: when `bMailSplitConfig == 1`, two independent bitmasks apply:
- `nMailSubjectOptions` — fields written to the Subject line.
- `nMailBodyOptions` — fields written to the message body.

The settings are backward-compatible: existing configurations with `bMailSplitConfig == 0` behave identically to before.

---

### `[TelnetReject]`
**File:** `Misc.cpp` — `ShowMessage()`

`ShowMessage()` has several early-return paths: duplicate suppression, reject filter, and the "blocked" count. `TelnetServerNotifyMessage()` was called after these guards, so rejected and duplicate messages were silently dropped from the Telnet stream.

For operators using the Telnet feed as a monitoring bus this was wrong — they need to see *all* messages, not the post-filter view. Fix: `TelnetServerNotifyMessage()` is now called at the very top of `ShowMessage()`, before any filtering.

---

### `[TsStartupGating]`
**File:** `utils/telnet_server.cpp`

The Telnet wire-format includes `<RS232:0>` (data lost) and `<RS232:1>` (data recovered). The RS232 state machine has three states: `-1` (startup / unknown), `0` (lost), `1` (present).

Problem: on PDW startup, the first serial byte arrived and emitted `<RS232:1>` even though there had been no prior loss. This looked like a spurious recovery event to p2kflexMonitor.

Fix (mirrors p2kflexDecoder behaviour):
- State `-1` → `1` on first byte: set state silently, do **not** emit `<RS232:1>`.
- State `0` → `1`: emit `<RS232:1>` (genuine recovery).
- A COM-port reconfigure (`Enable(0)` → `Enable(1)`) leaves a pending `lost (0)` state so the next byte does emit the recovery event.

The same logic applies to the slicer (`TelnetServerSlicerActivity`). `<AUDIO:1>` was already handled correctly via the warmup-discard mechanism.

---

### `[DpiScale]`
**Files:** `PDW.cpp`, `Gfx.cpp`

Added `dpiAware` to the application manifest (`<dpiAware>true</dpiAware>`, System DPI Aware level). At runtime, `g_dpi` is set from `GetDpiForWindow()` / `GetDeviceCaps(LOGPIXELSX)` once the main window is created. A `Scale(n)` helper multiplies any pixel value by `g_dpi / 96`.

The toolbar height is read back from the actual created control rather than hardcoded, and `hboxfont` is recreated after `g_dpi` is known. Without the font recreation fix, the toolbar text was sized for 96 DPI regardless of actual scaling.

---

### `[RS232Flap]`
**File:** `utils/rs232.cpp`

On electrically noisy COM links (e.g. Moxa NPort over TCP) the RxThread could see very short gaps that were not genuine disconnects. Each gap toggled the RS232-present state and generated `<RS232:0>` / `<RS232:1>` event pairs in the Telnet stream.

Fix: a debounce timer ignores transitions shorter than N milliseconds. Only sustained absence of data (beyond the RS232 watchdog threshold of ~10 s) is treated as a genuine disconnect.

---

### `[TrayBalloon]`
**File:** `PDW.cpp`

`Shell_NotifyIcon` with `NIF_INFO` / `NIIF_INFO` (the "balloon tip" API) was silently ignored by Windows 10 and Windows 11 — Microsoft removed balloon display from the taskbar notification area.

Replacement: `IUserNotification` COM interface, which maps to the Action Center toast system. This API is available from Windows Vista onwards and is the correct path for tray-context notifications on modern Windows.

---

## Architecture: output feed threading model

All five output feeds (SMTP, Webhook, MQTT, Telnet, MySQL) follow the same threading pattern:

1. **Decoder thread** calls the feed's `Notify*()` function.
2. The function serialises the message into a job struct and places it in a **ring buffer** (64 slots, power-of-two mask).
3. A `SetEvent()` wakes the **worker thread**.
4. The worker thread drains the ring buffer, performing the slow I/O (TCP connect, TLS, HTTP POST, MySQL INSERT) without ever blocking the decoder.

On application exit, the main thread signals each worker to stop (`SetEvent` on a stop-event), then `WaitForSingleObject(worker, 5000)` with a 5-second timeout to allow in-flight jobs to drain. Workers that do not exit within the timeout are forcibly terminated.

This ensures:
- No decoded message is lost due to a slow network operation.
- The audio input ring buffer never stalls.
- Clean shutdown without dangling threads or heap corruption.

---

## MySQL: native wire-protocol implementation

Rather than shipping `libmysqlclient.dll` (which requires a MySQL installation), PDW implements the minimum subset of MySQL wire-protocol version 10 needed for INSERT operations:

1. **Handshake:** read server greeting, parse auth plugin name and nonce.
2. **Auth reply:** send `HandshakeResponse41` with `mysql_native_password` (SHA1(password) XOR SHA1(nonce + SHA1(SHA1(password)))), computed via `CryptCreateHash(CALG_SHA1)` from Windows `advapi32.dll`.
3. **Query:** send `COM_QUERY` with UTF-8 encoded SQL.
4. **Result:** read and discard OK/ERR/column packets.

Only `mysql_native_password` is supported (not `caching_sha2_password`). Most MySQL 5.7 / 8.0 and MariaDB servers can be configured to use it per-account.

The database is auto-created if it does not exist: PDW connects to the `mysql` system database first and issues `CREATE DATABASE IF NOT EXISTS`.
