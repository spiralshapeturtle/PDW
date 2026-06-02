# PDW 3.5.6 — Release Notes

## New Features

### MySQL Output Feed
PDW can now write decoded messages directly to a MySQL database. Zero external DLL
dependencies — the MySQL wire protocol (v10) is implemented natively using only Win32 and
CryptAPI (advapi32.dll, always present on Windows). Authentication uses
`mysql_native_password`. Configure via **Options → MySQL…**.

- Three schema modes:
  - **Classic** — compatible with existing `meld2mysql.exe` tooling (capcode / melding / label)
  - **Extended** — all eight PDW text fields stored as strings
  - **Optimized** — type-correct columns (timestamps, integers, label colour) — *recommended for
    new deployments*; see `PDW-SCHEMA.md` for full column reference.
- Worker thread with a 64-slot ring buffer and exponential-backoff reconnect (1 s → 2 s → 4 s →
  30 s cap).
- Connection test button in the settings dialog.
- Group-call subscriber rows are accumulated and flushed as a single row when the group capcode
  arrives, matching MQTT/Webhook behaviour.

### RX Quality Alert
PDW can now send an e-mail when telnet RX quality drops below a configurable threshold for a
sustained period. Configure via **Options → RX Quality Alert…**.

- **Threshold** (default 25 %): quality level that starts the timer.
- **Recovery** (default 35 %): quality level that cancels a pending alert.
- **Minimum duration** (default 15 min): consecutive minutes below threshold required before
  the mail is sent.
- **Cooldown** (default 120 min): silence window after each alert to prevent flooding.
- Uses the existing SMTP profile — no extra mail account needed.
- Independent recipient list, separate from normal message recipients.

---

## Improvements

### SMTP Hardening

**Crash fix — heap corruption on rapid Test-button clicks**
Each `MailInit()` call performed a stop + start of the mail worker. If the worker was inside a
blocking TLS read or connect, the 3-second join timed out and the old thread was abandoned.
The next `MailInit()` then started a *second* worker sharing the same SSL context, socket, and
mail queue, causing heap corruption (0xc0000374 in ntdll). PDW now uses a single long-lived
worker that is never abandoned; `MailInit()` only updates configuration, and shutdown blocks
until the worker fully exits.

Additional hardening in the same area:

| Fix | Detail |
|---|---|
| Queue buffer | Increased to `MAX_STR_LEN + 256` (was 1024) — long FLEX messages and the Subject/Body split separator are no longer silently truncated in the ring slot. |
| Recipient pipeline | Unified to 512 bytes end-to-end (queue override slot → RCPT TO → To: header). Alert-recipient lists are no longer clipped. |
| RFC 5321 line folding | Outbound body text is now folded at ≤ 998 octets per line, satisfying strict server limits. Folding prefers whitespace around column 900; tokens longer than 990 bytes are hard-broken. |
| Queue overflow | Producer checks for a full ring before writing and drops the oldest slot, preventing silent data loss when the server is slow. |
| Worker mutation | The worker no longer reads or writes `Profile.bMailSplitConfig`. Split mode is detected by separator presence in the queued string. |
| Header isolation | From/MAIL FROM are formatted from a local copy — the worker no longer writes back into `Profile.szMailFrom`. |
| Charset guard | The charset-index is clamped before the lookup array, eliminating a potential wild `strcpy` on the alert / split path. |

### Telnet RX Quality Parity
The telnet `<RXQ:NN>` value is now computed on the same basis as p2kflexDecoder:

- **POCSAG sync and idle words** are credited to the quality track, keeping the score stable
  between messages on a busy channel (previously only message words grew the denominator, making
  each error weigh far more than in p2kflexDecoder).
- **POCSAG address and capcode penalties** for uncorrectable words are applied correctly.
- **FLEX EMA update** is gated to valid-BIW frames — prevents extra jitter on frames with
  corrupted block-information words.
- **FLEX cycle-info acceptance** threshold raised to `cer < 3` (was `cer < 2`), matching
  p2kflexDecoder and the BCH(31,21) two-error-correction spec. Reduces false 99/999 cycle
  sentinels on both audio and RS232 input paths.

### Worker Stability (MQTT / Webhook / Telnet)

**Shutdown/reconfigure race fix**
All three workers (MQTT, Webhook, Telnet) previously joined with a 5-second timeout that could
expire while the worker was still active. `CloseHandle` and `DeleteCriticalSection` then ran
against a live thread, causing crashes on exit and the possibility of a second worker being
launched on the same shared state during a reconfigure. All joins are now `INFINITE`; the
workers themselves are bounded:

- MQTT — Paho connect/waitForCompletion timeouts (5 s each) plus a 200 ms event wait.
- Webhook — WinHTTP request timeouts set to 10 s each (DNS, connect, send, receive). The
  library default left DNS resolution unbounded.
- Telnet — listen-socket close unblocks the 1-second `select()`, so the worker returns within
  ~1 s.

**MQTT connection test button**
A new **Test connection** button in **Options → MQTT…** verifies broker reachability with the
settings currently typed in the dialog, without affecting the running worker. Friendly error
messages are shown for common CONNACK refusal codes.

### FLEX Decode Improvements

**BIW buffer fix (`FIX [FlexTimeMutate]`)**
FLEX date and time BIW words were being shifted in-place inside the shared `frame[]` array. The
shifted (and now meaningless) values were later read back by `showblock()` for checksum
verification, producing spurious 100-bit RXQ penalties. This manifested as a characteristic
quality dip visible on the telnet feed immediately after a PDW restart. Both the time (case 2)
and date (case 1) BIW paths now work on a local copy.

**Decode guard (`FIX [DecodeGuard]`)**
The main decode tick is wrapped in a Win32 SEH `__try/__except` block. A malformed or
truncated frame that raises an access violation is logged to the debug channel and skipped
instead of propagating out of `DispatchMessage` and silently killing the process.

### Daily-Rotating Log Files
All PDW log files now use date-stamped filenames (`YYMMDD_<type>.log`). Files over 5 MB are
rotated to `<name>.1` (one backup generation retained). Affected logs:

| Log | Old name | New name pattern |
|---|---|---|
| MQTT | `pdw_mqtt.log` | `YYMMDD_mqtt.log` |
| Webhook | `pdw_webhook.log` | `YYMMDD_webhook.log` |
| Telnet events | `pdw_telnet_server.log` | `YYMMDD_telnet_server.log` |
| Telnet wire | `pdw_flexdecoder.log` | `YYMMDD_telnet_traffic.log` |
| Missed group-calls | `missed-groupcalls.log` | `YYMMDD_missed_groupcalls.log` |

`<WD>` (watchdog heartbeat) lines are suppressed from the telnet wire log — they are sent over
the wire but not written to disk.

---

## Bug Fixes

- **Missed-groupcall session summary** — the summary line is now suppressed when both miss
  counters are zero, preventing a meaningless `0/0` entry from being appended on every clean
  PDW shutdown.
- **Filter dialog capcode field (High-DPI)** — when switching between POCSAG (narrow) and
  other types (wide), the old bounding rectangle was not invalidated, leaving a white artefact.
  The resize path now calls `InvalidateRect` + `UpdateWindow` on the old rect, and uses the
  DPI-scaled `Scale()` helper for widths instead of hard-coded pixel values.
- **Telnet graceful-shutdown `<RS232:0>` suppression** — `TelnetServerBeginShutdown()` is
  called at the start of `WM_DESTROY` so the telnet feed does not emit a `<RS232:0>` on clean
  exit (which would put a remote slave into exponential-backoff reconnect). The TCP close from
  `TelnetServerDestroy()` already signals session end.
- **Telnet wire log `<WD>` not written to disk** — `<WD>` heartbeat lines were being appended
  to the wire log on every watchdog interval; they are now filtered at the write path.
