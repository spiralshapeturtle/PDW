# PDW 3.6.2 - Release Notes

## New in 3.6.2

### Stability fixes

- **Run-flag made atomic (FIX [AtomicRunning]):** the worker run flag in both the Telegram and
  Pushover sinks was a `volatile BOOL`, which provides neither atomicity nor cross-thread memory
  ordering. It is written by the GUI thread (Init/Shutdown) and read lock-free by the decoder thread
  and the worker, so it is now a `std::atomic<bool>`. Behaviour is unchanged; this removes an
  undefined-behaviour data race that could, in theory, let a worker miss a shutdown request and delay
  a clean stop.
- **Telegram long-message split no longer corrupts text (FIX [TgSplitBoundary]):** with Telegram
  **Split** on, a message over 4096 characters was cut at an exact byte offset. That could sever a
  UTF-8 character (making the request invalid UTF-8, so Telegram silently dropped the whole message)
  or split an HTML tag/entity. The split now backs off to the nearest safe boundary, so every chunk is
  valid UTF-8 with intact markup.
- **Shutdown teardown race closed (FIX [TgCsTeardown] / [PoCsTeardown]):** `TelegramNotify`/
  `PushoverNotify` test the lock-free run flag and *then* take the critical section, while
  `*Destroy` (called at app exit) used to `DeleteCriticalSection`. A decoder thread that passed the
  flag test just before shutdown could enter an already-deleted section (crash). `*Destroy` no longer
  deletes the section - it is process exit, so the OS reclaims it - removing the window entirely.
- **MOBITEX clock recovery fix (FIX [ModemResync]):** the inter-crossing sample counter (`atb_len`)
  was reset after both transition branches, so it was always 1 when a crossing fired and the bit-clock
  resync guard never accumulated. The reset now happens inside each branch (mirroring `Audio_To_Bits`),
  and the low->high transition also participates in clock recovery.

### Performance

- **Batched recording writes (FIX [RecBatch]):** the raw recorder issued two `fwrite()` calls per
  sample (~38k syscalls/sec at 19200 baud). Samples are now batched (up to 256 at a time) and flushed
  once per batch. The on-disk layout is byte-for-byte identical.
- **Log writes grouped by file (FIX [LogWriteSort]):** the LogManager drain buffer is sorted by path
  before writing, so interleaved channels (Telegram/MQTT/Telegram...) no longer cause redundant
  fopen/fclose pairs.
- **RS232 four-level flag hoisted (FIX [Rs232FourLevel]):** `Profile.fourlevel` is read once per read
  instead of on every bit in the inner loop.

### Dialog fixes

- **Telegram dialog converted to DIALOGEX** with corrected label layout; minor control-style tidy-ups
  in the Telegram/Pushover/log settings dialogs.

## New in 3.6.1

### Telegram & Pushover refinements (FIX [TgBodyTemplate] / [PoHtmlNewline] / [TgTestPreview])

- **Title + Body templates** for both sinks, with a `{message}` placeholder and `\n` for line breaks.
  New default layout (title-less, Body `<b>{message}</b>\n{label}`): bold page text with each capcode
  label on its own line. Pushover defaults to **HTML formatting on** so the bold renders.
- **Test button previews the real templates**: it renders a sample page (with three sample labels)
  through the current Title/Body fields - Telegram via `parse_mode=HTML`, Pushover honouring the HTML
  checkbox - so you see the exact formatting before saving.
- **Pushover HTML line breaks fixed:** Pushover treats a bare newline as whitespace in HTML mode, so
  PDW now converts `\n` to `<br>` automatically when HTML formatting is on (FIX [PoHtmlNewline]).
- **Group calls** are delivered as one message with each subscriber label on its own line; the label
  list is buffered up to 32 KB (matching the MQTT/webhook feeds) so a 122-capcode test alert fits, and
  with Telegram **Split** on it is delivered in full across several messages.
- **Send-in modes** now mirror SMTP exactly (All / Filtered / Filtered+Monitor / Selected filters
  only); the per-capcode Ctrl-F checkbox is consulted only in *Selected filters only* mode.

### Telegram & Pushover stability fixes

- **Test button data race fixed (FIX [TgBuildRace]):** the Telegram message builder used a shared
  static buffer reached from both the worker thread and the GUI **Test** button. Clicking Test while a
  live page was being sent could corrupt either message body. The builder now uses a per-call buffer.
- **Status-message id collision removed (FIX [StatusMsgId]):** the Telegram (`WM_USER+52`) and Pushover
  (`WM_USER+53`) dialog status messages collided with MySQL/SQLite. They never misdelivered in practice
  (each feed posts only to its own dialog), but the duplicate ids are now unique (`+54` / `+55`).
- **Event-handle guard (FIX [TgEventGuard] / [PoEventGuard]):** the queue wake-up no longer signals a
  handle that shutdown may have just closed.
- **Shutdown/enqueue race closed (FIX [TgEventRace] / [PoEventRace]):** the event-handle guard still
  left a small window where a decoder-thread enqueue could call `SetEvent` after the GUI-thread
  shutdown had closed the handle (and could tear the ring-buffer indices). The wake-up now signals the
  event while holding the lock, and shutdown clears/closes the event and resets the queue under the
  same lock, so the two can no longer interleave.

## New in 3.5.9

### Telegram output (new sink, FIX [Telegram])

PDW can now push decoded messages to Telegram via the Bot API, as a regular output sink alongside
SMTP, webhook, MQTT, MySQL and SQLite. Configure it under **Telegram...** in the menu:

- Enter the bot token (from @BotFather) and one or more numeric chat_id's (';'-separated). 1:1 chats
  are positive, groups negative, supergroups start with `-100`. Because a bot cannot message a user
  first, send `/start` (or add the bot to your group) once, then press **Discover...** to fetch the
  chat_id automatically.
- Separate, configurable **Title** and **Body** templates (like the SMTP subject/body split). The
  title (default `<b>{label}</b>`) is a bold first line; the body (default `{message}`) follows after a
  blank line. Both accept the placeholders `{message} {label} {capcode} {time} {date} {mode} {type}
  {bitrate}`, and `\n` forces a line break. Leave the title empty for a body-only message. Recommended
  for group calls: Title empty + Body `<b>{message}</b>\n{label}` (bold page text, then each capcode
  label on its own line). See the manual's template cookbook for more examples.
- HTML formatting (`parse_mode=HTML`) with automatic fall-back to plain text if Telegram rejects the
  markup. Messages over 4096 characters are split (or truncated, configurable). Rate-limit (HTTP 429)
  responses are honoured with back-off, and supergroup migrations update the stored chat_id
  automatically.
- Options: silent delivery, disable link preview, optional `message_thread_id` for supergroup topics.
  The **Test** button renders a sample page through the current Title/Body fields with `parse_mode=HTML`,
  so you preview the real formatting (bold, line breaks, stacked labels) before saving. The default
  layout is now title-less with Body `<b>{message}</b>\n{label}`. The bot token is never written to the log.
- **Send-in mode** (mirrors the SMTP modes 1:1): *All messages*, *Filtered messages only*,
  *Filtered + monitor-only messages*, or *Selected filters only*. The per-capcode **Send Telegram**
  checkbox in the filter window (Ctrl-F) is consulted **only** in *Selected filters only* mode - so you
  can forward just a handful of capcodes. In the other modes the checkbox is ignored and every matched
  (or every) message is sent. The sink runs on its own worker thread with a ring buffer.
- **Group calls** (FLEX): a group page is delivered as **one** message listing all matching subscriber
  capcodes/labels (one label per line), instead of one Telegram per member capcode. The label list is
  collected up to 32 KB (same as the MQTT/webhook feeds) so a 122-capcode test alert fits; turn
  **Split** ON to deliver it in full across several messages (OFF truncates at 4096). Pushover follows
  the same model but is bound by its 1024-char cap.

The new per-filter flags are stored as extra bits in the existing `filters.ini` filter field, so an
older PDW build keeps reading the file without corruption (it simply ignores the unknown bits). The
global Telegram configuration lives in a `[Telegram]` section in the main INI.

### Pushover output (new sink, FIX [Pushover])

PDW can also push decoded messages to [Pushover](https://pushover.net) via its Messages API, set up
under **Pushover...** in the menu. Same send-in model as Telegram: *All / Filtered / Filtered +
monitor-only / Selected filters only*. The per-capcode **Send Pushover** checkbox (Ctrl-F) is only
used in *Selected filters only* mode. FLEX group calls are likewise delivered as one notification
listing all matching subscriber capcodes.

- Application API token + user-key or group-key (both stored locally, never logged)
- Separate **Title** and **Body** templates (default `{label}` / `{message}`) with the same
  `{message} {label} {capcode} {time} {date} {mode} {type} {bitrate}` placeholders as Telegram, so the
  title and body can be swapped freely (empty body falls back to the raw page text)
- Priority -2..1, optional sound, target device, and optional HTML formatting (`html=1`)
- Message capped at 1024 chars / title at 250 (Pushover limits); HTTP 429 rate-limit back-off
- **Test** button renders a sample page through the current Title/Body fields (and HTML checkbox) so
  you preview the real formatting, just like Telegram
- Emergency priority 2 (acknowledgement + receipt polling) is intentionally not offered yet

Configuration lives in a `[Pushover]` section in the main INI; the per-filter flag is a second bit in
the same `filters.ini` field, equally safe for older builds.



### Stability: hardening for 24/7 unattended operation (audit fixes)

A focused audit of the long-running worker threads closed several remaining edge cases:

- Audio capture no longer leaks the sound device handle or its buffers when the device is briefly
  busy (for example grabbed by another application) and a capture start fails - previously every
  failed start leaked handles and memory, which over weeks of auto-retry could exhaust resources
  (FIX [WaveInStartLeak], FIX [WaveOutLeak]).
- The mail sender no longer blocks shutdown or a settings change for minutes when a secure (TLS)
  mail server accepts the connection but then goes silent; the TLS wait is now capped at 30 seconds,
  matching the plain-text path (FIX [SmtpTlsTimeout]).
- The MQTT and webhook status indicators can no longer crash if their setup dialog is opened before
  the feed has finished initialising (FIX [StatusWndCsGuard]).
- If the central log writer thread cannot start, logging now falls back to direct writes instead of
  silently buffering and dropping the oldest lines (FIX [LogWorkerStart]).
- Minor internal consistency fixes to the mail queue and log filename handling under heavy
  multi-threaded load (FIX [SmtpRingVolatile], FIX [BuildPathSnapshot]).

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
