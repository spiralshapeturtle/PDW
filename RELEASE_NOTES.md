# PDW 3.6.7 - Release Notes

## New in 3.6.7

### Filter options

- **"Ignore in Groupcall" per-filter option (FIX [GroupcallScreenHide]):** the Ctrl+F filter editor has
  a new checkbox, *Ignore in Groupcall (FLEX group mode only)*. It is meant to cut screen clutter from
  routine subscriber codes inside a FLEX group call - think roadblock or station-technical addresses -
  so the genuine personnel-alarm subscribers stand out. When a capcode that matches such a filter
  appears as a subscriber in a group call: (1) its capcode line is left out of the on-screen group view
  (monitor and filter panes); (2) it no longer drags its group into the **filter window** on its own, so
  a group whose only matching member is an ignored code no longer floods the bottom (filter) pane - a
  group that also has a genuine matching member still appears there via that member; and (3) it does not
  trigger the filter beep. The group message header and text stay visible in the monitor pane, and the
  **full** group message - including the ignored capcode - is still written to the monitor log and sent
  to every output feed (e-mail, Telegram, Pushover, webhook, MQTT, database) exactly as before; only the
  *filter* log omits the ignored member, to match the filter window. The option is stored per filter in
  `filters.ini` (bit `0x80` of the existing filter flags field), is available only for non-reject capcode
  filters, and has no effect outside FLEX group calls or in the classic verbose view. Filters carrying
  the flag are marked `IGN-GRP` in the Ctrl+F overview (when the extra-info column is enabled).
  - **Windows toast follows the screen-hide (FIX [GroupcallToastHide]):** the Windows toast/tray
    notification is a notification path, not a log or feed, so it now honours the flag too. An ignored
    subscriber's capcode/label is no longer accumulated into the toast title for that group call - the
    toast previously still listed exactly the codes the user had hidden on screen. A group call that
    also has a genuine (non-ignored) matching member still raises a toast for that member. Disk logs and
    all output feeds remain unaffected and continue to carry the full group message.
  - **No effect on individual pages:** the flag only hides a capcode while it is a *subscriber inside* a
    FLEX group call. When the same capcode is paged individually (a normal alpha/numeric message with no
    group address), it is shown, filtered, logged and fed completely normally.
  - **Mutually exclusive with Monitor only:** the two request opposite behaviour (monitor-only shows on
    screen + suppresses feeds; ignore hides on screen + keeps feeds), so ticking either checkbox in the
    filter editor automatically clears the other. Both stay enabled (no greying), so the pair can never
    deadlock. A runtime safety net also lets monitor-only win should a hand-edited `filters.ini` ever
    carry both bits.

---

# PDW 3.6.6 - Release Notes

## New in 3.6.6

### Bug fixes

- **Reject log formatting inside group calls (FIX [LogRejected]):** when "Also log rejected messages"
  was enabled and a rejected capcode was part of a FLEX group call, the on-disk monitor log became
  garbled: a spurious blank separator line appeared before every rejected subscriber entry (caused by
  the local `bSeparator` always defaulting to `true` in the early-return path, missing the
  `bShown[MONITOR]` suppression the normal pane-loop applies). Additionally, with FlexGroupMode compact
  logging active, the rejected subscriber was written as a full standalone timestamp+capcode line instead
  of an indented capcode-only line under the shared group header, and `bLogged[MONITOR]` was never set,
  so a subsequent non-rejected subscriber would re-emit the group header after the mis-formatted
  rejected line. Both issues are now fixed: intra-group separators are suppressed for rejected
  subscribers exactly as for non-rejected ones, and FlexGroupMode logging writes rejected subscribers
  in the correct indented format under the group header.
- **Long log lines no longer dropped or glued together (FIX [LogLineSplit]):** every message-log write
  squeezed the formatted line through a fixed 1 KB buffer before handing it to the log manager. A line
  longer than that - easily reached by a long message plus label, and even faster when linefeed
  characters add per-line alignment padding - hit two failure modes: (1) the truncating formatter
  returns -1 and the write guard then silently **dropped the entire line**, so in FlexGroupMode the
  group header (which carries the message text) vanished while the short indented capcode+label lines
  still appeared - those labels then showed up under the *previous* message's text; (2) where a clamped
  write did go through, the trailing newline was cut off, so the **next** entry was glued onto the same
  line - capcodes and labels of one message appearing in the middle of another message's text. Message
  texts and capcode labels therefore appeared to mix and jump between entries. Fixed end-to-end: the
  log manager now splits long lines across consecutive ring-buffer slots under a single lock (so they
  are reassembled byte-exact on disk, with FIFO order already guaranteed since the 3.6.5 stable-sort
  fix), the monitor/filter/separate-file writers pass the full line without the 1 KB squeeze, and the
  remaining fixed-size writers (blocked/missed logs) keep a truncated line with a forced newline
  instead of dropping it. Message text and its label now always stay together on one record.

---

# PDW 3.6.5 - Release Notes

## New in 3.6.5

### Filter fixes

- **Reject filters can now match on capcode + text (FIX [RejectText]):** when the **Reject** action was
  enabled, the filter editor silently discarded the **Text** field on Apply/OK, so the typed text never
  reached the saved filter. This made it impossible to create a reject filter that only fires on a
  specific message (e.g. reject a capcode only when the message contains a given word). The text is now
  saved regardless of the reject flag - exactly like the capcode - so a capcode+text reject works and
  can, combined with "Also log rejected messages", keep just those messages in the log while letting the
  rest of the capcode through. The matcher already supported capcode+text; only the editor's save path
  was at fault. Capcode-only reject filters are unaffected. The filter list (Ctrl+F) now also shows the
  text criterion for a reject filter, e.g. `FLEX 1180000 | REJECT | "TEST"`, so the attached word filter
  is visible in the overview. The "Match EXACT message" option is likewise persisted for reject filters
  (previously editing it on a reject filter had no effect, as it sat behind the same reject gate as the
  text).
- **Reject filters grey out actions that do not apply (FIX [RejectActions]):** Send email, Send Telegram,
  Send Pushover and Monitor only are now disabled when **Reject** is checked. None of these ever ran for a
  rejected message - the reject path returns before any feed dispatch, and monitor-only is mutually
  exclusive with reject - so leaving them clickable was misleading. This mirrors how the sound, command
  and separate-file controls were already disabled for reject filters.
- **Label can be set on reject filters (FIX [RejectLabel]):** the filter Label field and its enable
  checkbox are no longer greyed out when **Reject** is checked. A reject filter is never shown on
  screen, so the label has no display effect, but when "Also log rejected messages" is on the label is
  written next to the message in the rejected log line - making that on-disk record readable (e.g.
  `... TESTOPROEP MOB / - MKB Oost-Brabant (heartbeat REJECT) -`). The label, its enable flag and colour
  now persist regardless of the reject flag, like the text and match-exact criteria.

### Logging

- **Scrambled log lines fixed (FIX [LogWriteOrder]):** the buffered log writer drained its ring buffer
  and then `qsort`-ed the batch by file path to group writes per file (fewer open/close calls). Because
  `qsort` is not a stable sort, lines that share the same file - i.e. every line of `monitor.log` - could
  be reordered relative to each other. On screen everything was correct (the screen does not go through
  the buffer), but on disk multi-line entries got shuffled, most visibly with FlexGroupMode group calls
  where a header line and its indented subscriber lines are separate writes that must stay in order. The
  batch is now stamped with its FIFO index and path-ties break on that index, so per-file order is
  preserved while still grouping by path. Affected all buffered log files; unbuffered logging was never
  affected.
- **Log rejected messages to disk (FIX [LogRejected]):** a new global checkbox **"Also log rejected
  messages"** in the Logfile dialog. By default a filter with the **Reject** action suppresses its
  messages everywhere, including the on-disk message log. With this option enabled, rejected messages
  are still written to the monitor log file - using the same columns and timestamp (including the ISO
  format, if selected) as a normal entry - so the log stays a complete record, while remaining hidden
  on screen, out of the filter log and out of every feed (SMTP, MQTT, webhook, etc.). The write goes
  through the central log manager like all other logging. The checkbox is only available when the
  message log itself is enabled, and the setting persists in PDW.ini (`LogRejectedMessages`). It is
  off by default, so existing behaviour is unchanged unless you turn it on. If the reject filter carries
  a label, the logged line includes that label just like a normal monitored entry (honouring the "Log
  labels" option). When FlexGroupMode compact logging is active, rejected subscribers appear as indented
  capcode lines under the shared group header, matching the format of non-rejected subscribers.

### Filter improvements

- **Whole-word matching with `=` (FIX [FilterWholeWord]):** prefix any filter term with `=` to match
  it only as a complete word, not as part of a longer word. For example `alpha&=cat` matches
  `alpha ... the cat` but no longer false-matches on `category` or `vacate`. A word boundary is any
  non-alphanumeric character or the start/end of the message. The `=` applies per term, so substring
  and whole-word terms can be mixed in one filter; matching stays case-insensitive. Filters without
  `=` are unaffected. This completes the filter-text operator set - `^` (starts with), `&` (AND),
  `|` (OR) and `=` (whole word) - all documented with examples in the manual (section 9.3).

## New in 3.6.4

### Command file fixes

- **Crash on failed command spawn fixed (FIX [CmdSpawnGuard]):** when an external command file failed
  to start (wrong path, missing file, permissions), PDW closed two uninitialised process handles,
  which could raise an invalid-handle exception. The handles are now closed only when the program
  actually started.
- **Command file runs from its own folder (FIX [CmdWorkDir]):** when a filter triggers an external
  command file, PDW now starts that program with its working directory set to the folder the program
  lives in, instead of leaving it on PDW's own working directory. Previously the spawned helper
  inherited whatever working directory PDW happened to have, so a program that keeps its config and
  log files next to itself (in a separate folder) could fail to find its config and would write its
  logs into the PDW folder instead. The working directory is taken from the full path of the
  configured command file; if no folder can be determined the previous behaviour is kept. This
  restores the pre-3.2-era behaviour where the command file always ran from its own directory.

## New in 3.6.3

### Filter improvements

- **OR operator in filter text (FIX [FilterOR]):** message-text filters now support `|` as a
  top-level OR operator in addition to the existing `&` (AND). For example `alpha&bravo|alpha&charlie`
  matches when the message contains `alpha` and `bravo`, **or** `alpha` and `charlie`. `&` binds tighter than `|`
  (standard precedence), empty terms are ignored (`|alpha` behaves like `alpha`), and a leading `^`
  on a term is ignored when `|` is present. Filters without `|` are completely unaffected. The
  highlight positions on a match are set exactly as for a normal single-term match. The *Match exact
  text* checkbox is now greyed out and cleared automatically while the filter text contains `&` or
  `|`, since whole-message matching is incompatible with those operators.
- **Filter text length raised to 256 (FIX [FilterTextLen]):** message-text filters now accept up to
  256 characters (was 120, matching the label field), giving room for longer `|`/`&` expressions.
  Existing `filters.ini` files load unchanged. Two fixed-size buffers that build strings from the
  filter text were hardened for the larger size so a long text can never overflow them: the filter-row
  display string (`BuildFilterString` now takes a destination size and appends within bounds) and the
  per-filter wave-file name lookup (`PlayWaveFile` now builds the file name with a bounded copy; an
  over-long name simply will not match a file on disk and falls back as before).

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
