# PDW 3.5.9 — Release Notes

## New in 3.5.9

### Database: per-capcode match state for group calls (FIX [GroupMatchPerCapcode])

For FLEX group calls the MySQL and SQLite feeds store one group row that lists all member capcodes in
the `subscribers` JSON. Previously the match state (filtered / monitor-only / no match) was a single
value taken from the first member, so a group made up mostly of monitor-only codes with one or two
filtered codes lost that distinction - even though the PDW window shows only the filtered member in
the filter pane and keeps the rest monitor-only.

Each member in the `subscribers` JSON now carries its own `match_type` field, so a viewer can render
every capcode in exactly the pane PDW shows it in. The group row's own `match_type` column is set to
the strongest match across all members purely so the group still surfaces in `WHERE match_type >= 1`
queries; it no longer dictates how individual members are displayed.

### Stability: log manager shutdown no longer risks a crash on exit (FIX [LogJoinRace])

When log settings were changed at runtime, or when PDW closed, the central log manager waited only
a bounded time for its background writer thread to finish before freeing its internal buffers. If
the log folder was on a slow or disconnected network/drive, a write could still be in progress when
the buffers were released, which could corrupt memory and crash the application. PDW now always
waits for the writer to finish completely before freeing those buffers, matching the other
background workers (MQTT, webhook, telnet).

### Stability: cleaner shutdown of the MQTT and webhook senders (FIX [FlushLockRace])

The final flush of queued messages when an output feed stops now reads its job queue with the same
locking used during normal operation, removing a narrow race with a message arriving at the exact
moment of shutdown.

## New in 3.5.8

### COM port held exclusively - cannot be hijacked while PDW runs (FIX [ComPortExclusive])

Previously a second program (another PDW instance, a terminal, or anything else) could open the
same COM port PDW was already using. On virtual COM ports - for example a Moxa NPort redirector
that tunnels the serial link over TCP - this silently split the incoming byte stream across both
readers, so neither received a coherent bitstream and decoding stopped in both, with the window
still responding normally.

PDW now opens the COM port for exclusive read/write access. While PDW is running and connected,
any other program that tries to open the same port fails - the running instance keeps sole,
uninterrupted ownership of the link. If PDW itself cannot open a port because another program
already holds it, it reports **"Unable to open the selected COM port - it may already be in use
by another program."** This is enforced by Windows at the driver level, so it protects against
every other program, not just a second PDW.

### Filter window font follows the main window (FIX [FilterFont])

The filter window (Ctrl+F) list now uses the same font and size as the main window
(configurable via Options -> Font) and scales with the display DPI. Previously the list was
hardcoded to 11pt MS Sans Serif and did not grow with the configured font size.



### Central Log Manager with Write Buffering

All log output — decoded messages, system events, and feed activity — now flows through a single
central log manager. The most visible user-facing change is a new **Write buffering** option in
the Logfile dialog.

**What changed:**

- Every log file (monitor, filter, separate filter files, debug, telnet, MQTT, webhook, MySQL,
  SMTP, blocked messages, missed group calls) is written through one consistent path
- All log files are now **date-stamped** with a `YYMMDD_` prefix and rotate automatically at
  midnight — including `mysql.log` which previously grew without bound
- Timestamp format in system/process logs is now uniform: `YYYY-MM-DD HH:MM:SS.mmm`
- **ISO date format option** in the Logfile dialog: enables `YYYY-MM-DD HH:MM:SS` timestamps
  inside monitor and filter log lines (the filename format on disk is unchanged). When enabled,
  the Time and Date column checkboxes are automatically grayed out.

**Write buffering (NVMe protection) — for busy networks:**

PDW runs 24/7 on PCs and laptops with SSDs. On networks with high message throughput, frequent
small writes cause unnecessary write amplification on the drive. The new buffer option coalesces
writes into timed batches instead of writing once per message.

Enable via **File → Open/Close Logfile → "Reduce disk writes (buffer)"**.

| Setting | Effect |
|---------|--------|
| Flush interval | How often the buffer is written to disk (default 500 ms) |
| Buffer slots | Maximum entries held before an early flush (default 512) |

- Recommended for **busy POCSAG/FLEX networks** where blocked-message logs can fill quickly
- Default 500 ms / 512 slots handles peaks of ~250 messages/second without dropping entries
- For minimum writes on very active networks: 2000 ms / 1024 slots
- Maximum potential log loss on hard crash equals the flush interval

### Bug fixes

**Large group calls no longer corrupt MQTT and webhook output**

Very large FLEX group calls (e.g. a regional proefalarm hitting ~80 capcodes in one message)
exceeded the internal MQTT and webhook buffers. The space-separated address list was truncated
after ~64 capcodes, and the subscribers JSON array was cut off mid-object — after which a closing
`]` was appended blindly, producing invalid JSON that Node-RED / Home Assistant could not parse.

- MQTT and webhook subscriber buffers raised from 2 KB to 32 KB (now matching the MySQL feed),
  covering ~170 capcodes with long labels
- Address-list buffers raised from 512 B to 2 KB
- The MySQL feed was already correctly sized and was not affected

**MQTT log no longer reports recovered reconnects as errors**

When a broker, NAT, or firewall silently drops an idle TCP connection, the next MQTT publish hits a
stale socket and fails (`rc=-1`). PDW already retried automatically and the message was delivered on
the second attempt, but the transient failure was logged as a scary `ERROR` line. This routine
reconnect is now logged as a quiet `RECONNECT` line instead; a real `ERROR` is only logged when both
attempts fail (broker genuinely unreachable). No behaviour changed — only the log severity. No
messages were ever lost; the retry already recovered them.

---

## New in 3.5.7

### SQLite Output Feed
PDW can now write decoded messages to a local SQLite database file. No server, no installer,
no external libraries required. Configure via **Options → SQLite…**.

- Single file — easy to back up, copy, or open with any SQLite browser
- Same column layout as the MySQL Optimized schema; column names are identical
- `capcode` stored as text to preserve leading zeros in long POCSAG pager addresses
- **LowWrite mode**: reduces disk writes on SSDs by batching commits every ~15 s.
  Trade-off: up to 15 seconds of messages may be lost on a hard crash or power failure.
- **Auto-maintenance** (off by default): automatically delete rows older than N days
  and/or keep the file under a maximum size. Runs once per hour; never deletes without
  explicit configuration.
- Connection test button; optional activity log (`YYMMDD_pdw_sqlite.log`)

---

## New in 3.5.6

### MySQL Output Feed
PDW can write decoded messages to a MySQL or MariaDB database. No external DLLs or MySQL
client libraries required. Configure via **Options → MySQL…**.

- Three schema modes:
  - **Classic** — minimal: capcode, message, label
  - **Extended** — all fields stored as text columns
  - **Optimized** — typed columns with indexes; recommended for new installations.
    See `README.md` for the full column reference and example queries.
- Automatic reconnect on connection loss
- Connection test button
- FLEX group calls stored with the full list of paged addresses
- Optional activity log (`pdw_mysql.log`)

### RX Quality Alert
PDW sends an e-mail when the RX quality indicator stays below a threshold for a
sustained period. Configure via **Options → RX Quality Alert…**.

| Setting | Default | Description |
|---------|---------|-------------|
| Threshold | 80 % | Quality below this level starts the timer |
| Recovery | 90 % | Quality above this level cancels a pending alert |
| Minimum duration | 15 min | How long below threshold before sending |
| Cooldown | 120 min | Minimum time between repeated alerts |

Uses the existing SMTP settings — no separate mail account needed. Supports an independent
recipient list, separate from normal filter-based alerts.

---

## Improvements in 3.5.6

### SMTP reliability
- Crash on rapid Test-button clicks fixed
- Long messages and split Subject/Body content no longer truncated
- Multiple recipients handled correctly throughout

### Telnet RX Quality
- RX quality score corrected for POCSAG channels — sync and idle words now count
  toward the quality track, giving a stable reading between messages
- FLEX cycle-info error threshold aligned with the BCH(31,21) specification

### Output feed stability
- MQTT, Webhook, and Telnet workers now shut down cleanly on application exit
  and on settings changes
- New **Test connection** button in **Options → MQTT…**
- Raw feed mode added to MQTT, Webhook, and Telnet

### Log files
All log files use date-stamped filenames (`YYMMDD_<type>.log`):

| Feed | Log file |
|------|----------|
| SMTP | `YYMMDD_mail.log` |
| MQTT | `YYMMDD_mqtt.log` |
| Webhook | `YYMMDD_webhook.log` |
| Telnet events | `YYMMDD_telnet_server.log` |
| Telnet wire | `YYMMDD_telnet_traffic.log` |
| SQLite | `YYMMDD_pdw_sqlite.log` |
| MySQL | `pdw_mysql.log` |

### Bug fixes
- Filter dialog capcode field no longer leaves a visual artefact when switching
  message type on a HiDPI display
- Telnet feed no longer sends a spurious `<RS232:0>` on clean PDW exit
- `<WD>` heartbeat lines suppressed from the telnet wire log (sent over the wire
  but not written to disk)
- Missed group-call summary no longer written when both counters are zero
