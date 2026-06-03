# PDW 3.5.8 — Release Notes

## New in 3.5.8

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

PDW runs 24/7 on hardware ranging from NAS boxes to laptops. On networks with high message
throughput, frequent small writes cause unnecessary write amplification on SSDs. The new buffer
option coalesces writes into timed batches instead of writing once per message.

Enable via **File → Open/Close Logfile → "Reduce disk writes (buffer)"**.

| Setting | Effect |
|---------|--------|
| Flush interval | How often the buffer is written to disk (default 500 ms) |
| Buffer slots | Maximum entries held before an early flush (default 512) |

- Recommended for **busy POCSAG/FLEX networks** where blocked-message logs can fill quickly
- Default 500 ms / 512 slots handles peaks of ~250 messages/second without dropping entries
- For minimum writes on very active networks: 2000 ms / 1024 slots
- Maximum potential log loss on hard crash equals the flush interval

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
