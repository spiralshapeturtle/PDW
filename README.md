# PDW — Paging Decoder for Windows

> **Legal notice:** Receiving and decoding paging transmissions may be restricted or prohibited by law in your country or region. It is your sole responsibility to verify that your use of this software complies with all applicable local, national, and international laws and regulations. This software is provided "as is", without warranty of any kind, express or implied. The authors and contributors accept no liability whatsoever for any damages, legal consequences, or other losses arising from the use or misuse of this software. By using this software you accept full responsibility for ensuring its lawful use.

---

**Version 4.0.4** | Windows 7-11 | Win32 + x64 | Visual Studio 2017+

PDW is a software paging decoder that turns a sound card or serial port into a full FLEX/ReFLEX/POCSAG receiver. It decodes, filters, and distributes paging messages to a wide range of output channels — from simple on-screen display and e-mail alerts to MQTT brokers, webhooks, Telnet clients, and MySQL databases.

This fork builds on the classic PDW 3.2 codebase and adds **five years of production-hardened improvements**: modern SMTP, MQTT/webhook integration, a built-in Telnet server, MySQL and SQLite output feeds, RX quality monitoring, High-DPI support, a central log manager with write buffering, and many reliability fixes.

---

## Why use this version?

If you already know PDW, here is what you get over the original 3.2 release:

| What | Why you want it |
|------|-----------------|
| **MQTT output** | Push every decoded page to a broker; Node-RED, Home Assistant, or any subscriber picks it up |
| **Webhook output** | HTTP/HTTPS POST to any endpoint — Zapier, n8n, custom APIs |
| **MySQL output** | Persist all decoded messages in a relational database; three schema variants available |
| **SQLite output** | Same as MySQL but a single local file — no server, no install, works on any machine |
| **System Alerts** | Get an e-mail when signal quality drops for too long, or when the serial (COM) input goes dead |
| **SMTP hardening** | STARTTLS (port 587) + implicit TLS (port 465) + RFC-compliant EHLO + reliable worker thread |
| **Telnet server** (port 8024) | Streams decoded messages in a structured wire-format to any Telnet client — custom internal feature, not intended for general use |
| **FLEX fragment reassembly** | Multi-frame FLEX messages are reassembled into a single, correct string |
| **Windows 11 toast notifications** | Modern native notifications instead of the obsolete tray balloon API |
| **High-DPI support** | Crisp display on 4K/HiDPI monitors |
| **x64 build** | 64-bit binary for modern systems |
| **Central log manager** | All log output through one path; uniform timestamps; write buffering reduces SSD write amplification on busy POCSAG/FLEX networks — configure flush interval and buffer size in the Logfile dialog |
| **ISO timestamps in logs** | Optional `YYYY-MM-DD HH:MM:SS` format inside monitor/filter log lines (sortable); all log files now date-rotate daily |
| **Log rejected messages** | Optional global Logfile setting that keeps reject-filtered messages in the on-disk message log while they stay suppressed on screen and in every feed |

---

## Features

### Protocols decoded

- **FLEX** 1600 / 3200 / 6400 bps — Alpha, Numeric, Tone, Short-Instruction, Frame-Info, group calls
- **ReFLEX** — same decoder, extended protocol
- **POCSAG** 512 / 1200 / 2400 bps — Alpha, Numeric, Tone

Fragment reassembly is implemented for FLEX: multi-frame alpha messages are accumulated and displayed as a single complete string.

---

### Filter system

Each filter entry matches on **capcode**, **label**, or **message text** and can independently:

- Assign a custom label and colour
- Play a WAV alert sound
- Send an SMTP e-mail
- Trigger an external command / script
- Write to a separate log file (up to 3 per filter)
- Mark as monitor-only or reject

A **reject** filter can be narrowed by combining a capcode with a Text value, so it only rejects messages from that capcode that also contain the text. A reject filter normally suppresses its messages everywhere, including the on-disk log. The global **"Also log rejected messages"** option in the Logfile dialog optionally keeps them in the monitor log file while they stay hidden on screen and out of every feed.

Message-text matching supports substring search, `&` (AND, all parts present in order), `|` (OR, any term matches — e.g. `alpha&bravo|alpha&charlie`), a leading `^` (anchor to the start of the message, e.g. `^ALARM` matches only messages that begin with `ALARM`), and `=` before a word for whole-word matching (e.g. `=cat` matches `cat` but not `category`). `&` binds tighter than `|`. The *Match exact text* option is disabled automatically while the filter text uses `&` or `|`.

Filter labels and filter text each support up to 256 characters; COM ports ≥ 10 are supported. Search-while-typing is available in the filter list. The filter list font follows the main window font setting.

---

### SMTP e-mail alerts

Sends a formatted e-mail for any matched filter. The SMTP client is fully self-contained (no external library):

- **Port 465** — implicit TLS (SSL from the first byte)
- **Port 587 / 25** — STARTTLS with mandatory second EHLO over TLS
- RFC 5321-compliant EHLO with IP-literal fallback (`[a.b.c.d]`)
- LOGIN / PLAIN authentication with Base64
- **Split Subject/Body mode** — choose independently which fields (capcode, time, date, mode, type, bitrate, message, label) go into the Subject line and which go into the Body. Added to customize alerts to mobile push services like pushover.net
- Error logging to disk (`YYMMDD_mail.log`)
- Reliable single worker thread

---

### MQTT output

Publishes every decoded page to an MQTT broker. Static-linked Paho library — no external DLLs required.

**Published fields** (all optional via bitmask):

| Field | Description |
|-------|-------------|
| `message` | Decoded text |
| `address` | Capcode(s) |
| `label` | Matched filter label(s) |
| `time` | HH:mm:ss |
| `date` | DD-MM-YY |
| `timestamp` | Unix epoch (seconds) |
| `mode` | FLEX / REFLEX / POCSAG / … |
| `type` | ALPHA / NUMERIC / TONE |
| `bitrate` | 1600 / 3200 / 6400 (FLEX) |
| `subscribers` | JSON array of `{address, label}` for group calls |

**Two JSON formats:**

*PDW-native* (with nested `data.new_state.attributes`):
```json
{
  "payload": "Fire alarm activated",
  "data": { "new_state": { "state": "ALPHA", "attributes": {
    "address": "1234567", "label": "Brandweer", "mode": "FLEX",
    "bitrate": "1600", "timestamp": 1748880000
  }}}
}
```

*Flat / Node-RED format*:
```json
{
  "message": "Fire alarm activated",
  "address": "1234567",
  "label": "Brandweer",
  "mode": "FLEX",
  "type": "ALPHA",
  "bitrate": "1600",
  "timestamp": 1748880000
}
```

**Send-in filter:** All messages / Filtered only / Filtered + Monitor / Raw feed (unprocessed)

---

### Webhook HTTP(S) notifications

HTTP POST to any endpoint using WinHTTP. No external libraries.

- JSON payload — same two formats as MQTT (PDW-native or flat)
- TLS 1.0–1.3; optional trust for self-signed certificates
- 3-attempt exponential backoff (1 s → 2 s → 4 s)
- TCP keep-alive; connection reused across requests
- Optional self-signed certificate bypass
- Capcode padding to 9 digits (optional)
- Send-in filter: All / Filtered / Filtered+Monitor / Raw feed

---

### Telegram output

Push decoded pages to Telegram chats, groups, and supergroups via the Bot API (WinHTTP, no external
libraries). Configure via **Telegram** in the menu.

- Bot token from @BotFather; one or more numeric chat_id's (';'-separated)
- **Discover** helper lists every distinct chat (id, type, name) from the bot's recent `getUpdates`
  after you message the bot - so a 1-on-1 chat is found even when the bot also sits in a busy group
- Separate **Title** and **Body** templates (default `<b>{label}</b>` / `{message}`) with placeholders
  `{message}/{label}/{capcode}/{time}/{date}/{mode}/{type}/{bitrate}`; `\n` forces a line break. E.g.
  leave Title empty and set Body `<b>{message}</b>\n{label}` for a bold page text with each capcode
  label on its own line underneath (see the manual's template cookbook for more examples)
- HTML formatting with automatic plain-text fall-back; 4096-char split or truncate
- HTTP 429 rate-limit back-off; automatic supergroup `migrate_to_chat_id` handling
- Silent delivery, link-preview toggle, optional supergroup topic (`message_thread_id`; applied only
  to group/supergroup destinations so a direct chat in the list never fails with HTTP 400), Test button
- Send-in modes mirror SMTP: All / Filtered / Filtered+Monitor / **Selected filters only**
- **Per-capcode control**: each filter (Ctrl-F) has a *Send Telegram* checkbox, used only in
  "Selected filters only" mode (forward just a few capcodes); ignored in the other modes
- **Per-filter silent override**: the filter editor has a *Telegram silent* checkbox that overrides
  the global silent setting for a specific capcode — useful for alarm capcodes that must still
  buzz even when global silent is on, or noise capcodes that should never alert
- **Per-filter routing override**: the filter editor exposes a *TG topic* field (numeric forum
  thread/topic id; blank = default thread) and a *TG chat* field (one or more chat-id overrides such
  as `-1001234567890`, `@mychannel`, or several separated by `;`; blank = global chat). Route a
  specific capcode to its own topic or chat(s) without changing the global target. A topic id must
  exist in whatever chat it is sent to.
- **FLEX group calls** sent as one message listing all matching subscriber capcodes. Bot token never logged.

---

### Pushover output

Push decoded pages to [Pushover](https://pushover.net) via its Messages API (WinHTTP, no external
libraries). Configure via **Pushover** in the menu.

- Application token + user-key or group-key (stored locally, never logged)
- Separate **Title** and **Body** templates (default `{label}` / `{message}`) with the same
  `{message}/{label}/{capcode}/...` placeholders as Telegram - swap them to reshape the notification
- Priority -2..1, optional sound, target device, optional HTML formatting
- Message/title length caps and HTTP 429 rate-limit back-off; Test button
- Send-in modes mirror SMTP: All / Filtered / Filtered+Monitor / **Selected filters only**
- **Per-capcode control**: each filter (Ctrl-F) has a *Send Pushover* checkbox, used only in
  "Selected filters only" mode; FLEX group calls sent as one notification listing all subscribers
- **Per-filter priority and sound overrides**: the filter editor exposes a *PO priority* dropdown
  (-2 Lowest / -1 Low / 0 Normal / 1 High; "Global" uses the Pushover config setting) and a
  *PO sound* text field (leave blank to use the global sound). Set high priority on alarm capcodes
  or mute routine pagers with priority -2, without changing the global setting
- Emergency priority 2 (receipt polling) intentionally not offered yet

---

### MySQL output

Persists all decoded messages to a MySQL or MariaDB database. No external DLLs or MySQL client libraries required.

**Three schema variants** — choose in settings:

#### Classic
```sql
CREATE TABLE `messages` (
    `id`        INT(11)    NOT NULL AUTO_INCREMENT,
    `timestamp` TIMESTAMP  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `capcode`   VARCHAR(10) NOT NULL DEFAULT '',
    `melding`   TEXT       NOT NULL,
    `label`     TEXT       NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

#### Extended
```sql
CREATE TABLE `messages` (
    `id`        INT(11)    NOT NULL AUTO_INCREMENT,
    `timestamp` TIMESTAMP  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `address`   VARCHAR(20) NOT NULL DEFAULT '',
    `msg_time`  VARCHAR(10) NOT NULL DEFAULT '',
    `msg_date`  VARCHAR(12) NOT NULL DEFAULT '',
    `mode`      VARCHAR(15) NOT NULL DEFAULT '',
    `msg_type`  VARCHAR(20) NOT NULL DEFAULT '',
    `bitrate`   VARCHAR(10) NOT NULL DEFAULT '',
    `message`   TEXT       NOT NULL,
    `label`     TEXT       NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

#### Optimized *(recommended for new installations)*
```sql
CREATE TABLE `messages` (
    `id`          BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
    `received`   DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `address`     CHAR(9)           NOT NULL DEFAULT '',
    `mode`        VARCHAR(15)       NOT NULL DEFAULT '',
    `msg_type`    VARCHAR(10)       NOT NULL DEFAULT '',
    `bitrate`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `message`     TEXT              NOT NULL,
    `label`       VARCHAR(256)      NOT NULL DEFAULT '',
    `subscribers` TEXT              NOT NULL DEFAULT '',
    `match_type`  TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    `label_color` VARCHAR(7)        NOT NULL DEFAULT '',
    PRIMARY KEY (`id`),
    INDEX `idx_address`   (`address`),
    INDEX `idx_received` (`received`),
    INDEX `idx_match`     (`match_type`),
    INDEX `idx_label`     (`label`(64)),
    FULLTEXT `ft_message` (`message`, `label`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
```

**Connection features:**

- Auto-creates database if it does not exist
- Exponential-backoff reconnect (1 s → 2 s → 4 s → … → 30 s)
- TCP keep-alive (60 s interval)
- Non-blocking background worker — decoding is never delayed by database writes
- Group-call subscribers stored as a JSON array in `subscribers`
- Optional activity log (`pdw_mysql.log`)
- Select which fields to write

**Column reference (Optimized schema):**

| Column | Type | Notes |
|--------|------|-------|
| `id` | BIGINT | Monotonic auto-increment. Use for live polling: `WHERE id > :since` |
| `received` | DATETIME | Message time in PDW machine's local timezone (no UTC offset stored) |
| `address` | CHAR(9) | Pager address stored as a zero-padded string. Leading zeros are preserved for long POCSAG addresses. FLEX group capcodes are `2029568`–`2029583` |
| `mode` | VARCHAR | Protocol + rate, e.g. `FLEX-1600`, `POCSAG-1200`. Protocol = part before `-` |
| `msg_type` | VARCHAR | `ALPHA` / `NUMERIC` / `TONE` / `GROUP` / `TRANSP` |
| `bitrate` | SMALLINT | 512 / 1200 / 2400 (POCSAG) or 1600 / 3200 / 6400 (FLEX) |
| `message` | TEXT | Up to ~5120 bytes. `>>` (byte `0xBB`) marks a line break — render as newline |
| `label` | VARCHAR | Filter label assigned to this capcode; empty when no rule matched |
| `subscribers` | TEXT | JSON array of group members (see below); empty for non-group messages |
| `match_type` | TINYINT | `0` = no match · `1` = filtered · `2` = monitor-only. For a group call: the strongest match across all members (so the group surfaces in `match_type >= 1` queries); per-member display state lives in the `subscribers` JSON |
| `label_color` | VARCHAR(7) | `#RRGGBB` of the label; empty when none |

`received`, `address`, and `match_type` are always written. `mode`, `msg_type`, `bitrate`, `message`, and `label` are written only when enabled in the PDW field-bitmask setting. `subscribers` and `label_color` are written only when non-empty.

**Group calls (`subscribers`):**

FLEX group calls store the individual paged addresses as a JSON array. Each member carries its own
`match_type` (`0`/`1`/`2`) so a viewer can render each capcode in its correct pane just like the PDW
window - only the filtered member shows as filtered, the rest stay monitor-only:

```json
[
  {"address": "1234567", "label": "Ambulance 1", "match_type": 1, "color": "#1565c0"},
  {"address": "1234568", "label": "Ambulance 2", "match_type": 2}
]
```

`address` inside `subscribers` is a string (same as the main `address` column). `color` is optional — older rows may not have it; fall back to a neutral colour chip in your UI. Detect a group call with: `address BETWEEN '2029568' AND '2029583'` or `subscribers <> ''`.

**Common queries:**

```sql
-- Latest 100, newest first
SELECT * FROM messages ORDER BY id DESC LIMIT 100;

-- Live polling (only newer than the last seen id)
SELECT * FROM messages WHERE id > :since ORDER BY id DESC LIMIT 50;

-- One address (address is CHAR(9) -- pass as string, e.g. '1234567' or '012345678')
SELECT * FROM messages WHERE address = :cc ORDER BY id DESC;
-- Also as a group member (indexless LIKE):
--   WHERE address = :cc OR subscribers LIKE CONCAT('%"address":"', :cc, '"%')

-- Only matched/filtered messages
SELECT * FROM messages WHERE match_type >= 1;

-- Full-text search across message and label
SELECT * FROM messages
WHERE MATCH(message, label) AGAINST (:q IN BOOLEAN MODE)
ORDER BY id DESC LIMIT 50;
```

**PHP (PDO) example:**

```php
$pdo = new PDO('mysql:host=HOST;port=3306;dbname=DB;charset=utf8mb4',
               'USER', 'PASS', [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

$rows = $pdo->query('SELECT * FROM messages ORDER BY id DESC LIMIT 100')
            ->fetchAll(PDO::FETCH_ASSOC);

foreach ($rows as $r) {
    $subs    = $r['subscribers'] !== '' ? (json_decode($r['subscribers'], true) ?: []) : [];
    $isGroup = $subs !== [];
    // render $r['message'] (replace 0xBB byte with newline),
    // $r['label'] / $r['label_color'], and for groups each $subs[i] address/label/color
}
```

---

### SQLite output

Persists all decoded messages to a local SQLite database file. No server, no installer, no external DLLs — everything is compiled into PDW. The database is a single file you can copy, backup, or open with any SQLite tool.

`address` is stored as text to preserve leading zeros in long POCSAG pager addresses.

**Schema:**

```sql
CREATE TABLE IF NOT EXISTS "messages" (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    received    TEXT    NOT NULL DEFAULT '',   -- 'YYYY-MM-DD HH:MM:SS'
    address     TEXT    NOT NULL DEFAULT '',   -- leading zeros preserved
    mode        TEXT    NOT NULL DEFAULT '',
    msg_type    TEXT    NOT NULL DEFAULT '',
    bitrate     INTEGER NOT NULL DEFAULT 0,
    message     TEXT    NOT NULL DEFAULT '',
    label       TEXT    NOT NULL DEFAULT '',
    subscribers TEXT    NOT NULL DEFAULT '',
    match_type  INTEGER NOT NULL DEFAULT 0,
    label_color TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_messages_address   ON messages(address);
CREATE INDEX IF NOT EXISTS idx_messages_received ON messages(received);
CREATE INDEX IF NOT EXISTS idx_messages_match     ON messages(match_type);
CREATE INDEX IF NOT EXISTS idx_messages_label     ON messages(label);
```

**Default file path:** `<PDW exe directory>\pdw.db` (configurable).

**Best-practice defaults applied automatically:**

| PRAGMA | Default | LowWrite mode |
|--------|---------|---------------|
| `journal_mode` | `WAL` | `WAL` |
| `synchronous` | `NORMAL` | `OFF` |
| `auto_vacuum` | `INCREMENTAL` | `INCREMENTAL` |
| `wal_autocheckpoint` | 1000 pages | 10000 pages |
| Commit cadence | every message | ~15 s batched |

**LowWrite mode** (`Options → SQLite → Reduce NVMe writes`): commits are batched every ~15 seconds instead of per message. Significantly reduces write amplification on SSDs. Trade-off: up to 15 seconds of messages may be lost on a hard crash or power failure.

**Automatic maintenance** (runs once per hour in the worker thread, off by default):

| Option | Description |
|--------|-------------|
| `PurgeDays` | Delete rows older than N days |
| `MaxSizeMB` | Delete oldest rows until the file is under this size |

Both options are disabled by default — PDW never deletes data without explicit configuration.

**Other features:**
- Connection test button in the settings dialog
- Optional activity log (`YYMMDD_pdw_sqlite.log`)
- Select which fields to write

---

### Telnet server *(custom internal feature)*

PDW includes a built-in **Telnet server on port 8024** that streams every decoded message in a structured wire-format. This is a custom internal feature; it is not intended for general use or third-party client compatibility.

TCP keepalive is enabled per connection so a half-open client (router/NAT rebind, crash, network blip) is detected and its slot freed within about a minute instead of lingering as a phantom. Multiple simultaneous connections from the same address are supported - each TCP connection is its own session.

**Wire-format messages:**

| Prefix | Meaning |
|--------|---------|
| `CC/FFF -ALPHA- capcode message` | FLEX alpha (CC=cycle, FFF=frame) |
| `-ALPHA- capcode-N message` | POCSAG alpha (N=function) |
| `<TX_START>` / `<TX_STOP>` | Transmission boundaries |
| `<RXQ:NN>` | RX quality percentage (0–100) |
| `<WD>` | Watchdog heartbeat (every 20 s by default) |
| `<RS232:0>` / `<RS232:1>` | Serial data lost / recovered |
| `<AUDIO:0>` / `<AUDIO:1>` | Audio signal lost / recovered |
| `<BUFFER_START>` / `<BUFFER_STOP>` | Reconnection replay window |

**Configuration options:**

- Bind address (default `0.0.0.0`)
- Max simultaneous clients (default 25)
- Watchdog interval
- Reconnect backlog window (default 60 s)
- Event log (`YYMMDD_telnet_server.log`) and wire-format log (`YYMMDD_telnet_traffic.log`)

---

### System Alerts (RX quality + COM link)

**Options → System Alerts** holds two independent e-mail alerts that share one recipient list.

The **RX quality alert** tracks the on-screen RX Quality bar over time; when signal quality stays below a configurable threshold for too long, PDW sends an e-mail.

**Settings (Options → System Alerts):**

| Setting | Default | Description |
|---------|---------|-------------|
| Threshold | 80 % | Quality below this triggers the timer |
| Recovery level | 90 % | Quality above this cancels the timer |
| Minimum duration | 15 min | How long below threshold before sending |
| Cooldown | 120 min | Silence period between repeated alerts |

The alert uses the same SMTP worker as the filter-based mail, so no extra configuration is needed.

The alert reads the **active health source** selected on the toolbar Health panel (see below), so
switching the panel between the classic RX-Q score and the stricter penalty score also switches
what the alert reacts to. Threshold/recovery/duration/cooldown semantics are unchanged.

The second alert in the same dialog, the **COM link-lost alert**, is for serial (COM) input and watches
the physical link instead of decode quality. When the serial feed dies completely (adapter unplugged, or
a Moxa NPort whose network tunnel drops) the classic RX-Q score just freezes and the quality alert never
fires - the COM link-lost alert catches exactly that. It e-mails when serial input is enabled but no data
has arrived for a configurable number of minutes (default 3), recovers automatically, and reuses the same
recipient list and SMTP host. Off by default; enable it with the **Enable COM link-lost alert** checkbox
(stored in `pdw.ini` under `[ComLinkAlert]`).

---

### Toolbar Health panel

A compact status strip on the right side of the toolbar band. While shown it **replaces** the classic
needle meter and RX-Q percentage box (a clean switchover, nothing is displayed twice); hide it via the
right-click menu and the classic corner returns exactly as before. It shows:

- **Overall-status accent bar** on the panel's left edge — the single light to glance at. A
  full-height colour strip showing the worst of everything (RX health, COM input link and every
  enabled feed): green only when the whole chain is healthy, orange/red the moment anything degrades
  or fails. Being part of the panel edge, a red bar reads as "the strip has a problem" and is never
  mistaken for the RX score itself being red.
- **Health score** (0-100%) in green/orange/red, prefixed with a small `RX` caption so it reads
  clearly as the RX-health score. Right-click the panel to choose the source:
  *RX needle (classic)* — the same lenient score as the RX-Q corner box (default) — or
  *Penalty system* — a stricter bucket-penalty score with "instant drop, slow recovery" behaviour
  (the same algorithm as the internal telnet wire score, but it works regardless of the telnet server).
- **Trend sparkline** of the score over a configurable window (1/5/15/60 minutes, or 4/8 hours to
  review the past night), with a soft colour-tinted area fill under the curve and an optional dotted
  marker line at the mail-alert threshold so the headroom to the alarm level is visible at a glance.
  On the long windows one pixel spans several minutes, so besides the average trend line the
  sparkline draws a bold colour-coded band down to the *worst* value in each pixel, keeping brief RX
  dips visible as a downward spike; the tooltip names the lowest value and when it occurred.
- **COM dot** (only when serial input is enabled): green = open + receiving, orange ring = open but
  stalled, red = not open.
- **Feed dots** for every *enabled* output feed (`SM` SMTP, `WH` webhook, `TG` Telegram, `PO`
  Pushover, `MQ` MQTT, `MY` MySQL, `SQ` SQLite, `TS` telnet server): green = no known problem
  (including "enabled, nothing delivered yet"), orange = retrying, red = last delivery/connection
  failed. For colour-blind readability the retrying state also differs in shape: it draws as a
  hollow *ring*, while healthy and failed are both solid discs (green and red). Disabled feeds are
  hidden, so the strip stays compact however many feeds you run.
- **Hover tooltips** on every entry: the score names its active source, the sparkline its window
  and alert level, and each feed dot shows its full status including the *last problem* and when the
  state changed (e.g. `Telegram: FAILED since 14:02 - API rejected message (HTTP 401)`). Because the
  panel sits against the right window edge the tooltips open to the left of the cursor and are
  clamped to the monitor, so even the rightmost dots' tips stay fully on-screen.

Feed-dot semantics: every enabled feed starts green ("configured, no known problem"). The
connection-oriented feeds (MQTT, MySQL) additionally track their connection live (red while the
broker/server is unreachable); the per-message feeds go orange/red on their first failing delivery.
The dot shows the last *outcome*, latched: a send in progress never blinks a broken feed green, and
a retry never downgrades red back to orange - only a real success clears the dot. Every status
*change* (feeds and COM link) is also written to a daily `{date}_health.log`, so a dot that was red
overnight can be diagnosed the next morning.

The panel adapts to the available width (labels drop first, then the sparkline); on windows too
narrow for it the classic corner is shown instead. **Left-click** an entry to jump straight to its
settings: a feed dot opens that feed's configuration dialog, the COM dot opens Interface setup, and
the score opens the System Alerts dialog. **Right-click** it to switch source, set the trend window,
or hide/show the panel. A third right-click option, **Show RX needle alongside**, keeps the panel but
brings the classic RX signal-strength needle back into its old far-right corner slot: the panel
shrinks just enough to make room, the RX-health percentage stays put and the needle sits to its right
(the old RX-Q percentage box and warning square stay hidden - the panel's score replaces them).
Settings persist in `pdw.ini` under `[HealthPanel]` (`Visible`, `Source`, `SparkMinutes`,
`ShowNeedle`).

---

### Windows push notifications

Press **Ctrl+T** to send a test Windows toast notification (Windows 10/11). PDW uses the native `IUserNotification` API instead of the legacy tray balloon, so notifications appear correctly in the Action Center on Windows 10 and Windows 11.

The system tray icon provides minimize-to-tray, click-to-restore, and optional per-message notifications for filtered messages.

---

### High-DPI support

PDW declares `System DPI Aware` in its manifest. Fonts, toolbar, and layout are recalculated from the actual DPI at startup — no blurring or clipping on 125 %, 150 %, or 200 % scaled displays. The toolbar buttons use a modern high-resolution icon set: each icon ships as a 72x72 32-bit image with a real alpha channel and is smoothly downscaled to the exact button size for the current DPI, so the icons stay crisp and anti-aliased at every scale factor instead of being stretched up from tiny 18x18 bitmaps.

---

### Multi-instance title bar

When running two PDW windows simultaneously (e.g. one on audio, one on serial), the title bar shows the active **[MODE]** — FLEX or POCSAG — so you can immediately see which window is which.

---

## Building

### Visual Studio 2017 or later

```
pdw_vs2017.sln
```

Open the solution, select **Release / Win32** or **Release / x64**, and build. OpenSSL 3.5.6 is included in `openssl-3.5.6/lib` (x86) and `lib64` (x64).

### CMake

```
cmake -A Win32 -B build
cmake --build build --config Release
```

> Note: the CMake build path does not include MQTT (Paho) — use the Visual Studio solution for a full build.

### Dependencies

| Library | Version | How included |
|---------|---------|-------------|
| OpenSSL | 3.5.6 | Pre-built static libs in repo |
| Paho MQTT C | latest | Static lib in repo |
| Windows SDK | 10.0+ | System |

No runtime installer required — PDW ships as a single `.exe`.

---

## Quick start

1. Download the latest `PDW.exe` from [Releases](../../releases).
2. Connect a radio to your sound card line-in and tune to a FLEX or POCSAG frequency.
3. Launch PDW and select your sound card input under **Settings → Input**.
4. Add filters under **Filters** to match capcodes you care about.
5. Optionally configure output feeds under **Options**:
   - **Options → SMTP Settings** — e-mail on filter match
   - **Options → Webhook** — HTTP POST on every message
   - **Options → MQTT** — publish to a broker
   - **Options → MySQL** — persist to MySQL/MariaDB database
   - **Options → SQLite** — persist to a local database file (no server needed)
   - **Options → Telnet Server** — stream to Telnet clients
   - **Options → RX Quality Alert** — e-mail on signal loss

---

## Running PDW

### C++ Redistributable

PDW requires the **Microsoft Visual C++ Redistributable for Visual Studio 2017 or later**. On a machine without Visual Studio installed, download and run `vc_redist.x86.exe` (Win32 build) or `vc_redist.x64.exe` (x64 build) from Microsoft before launching PDW.

### Executable size

`PDW.exe` is a single binary of under 7 MB because OpenSSL, Paho MQTT, SQLite and the MySQL client are **statically compiled in** rather than shipped as separate `*.DLL` files. This keeps deployment dependency-free — upgrading is just copying one file.

---

## Changelog highlights

### v4.0.4 (July 2026)
Combined stability and hardening release: a full source-code audit (memory safety, buffer handling, and rare corner cases across the decoders, output feeds, input paths, and the screen/GUI code), the new high-resolution toolbar, a title-bar/corner display-glitch fix, a window-position safety fix, and one small new option (a menu-bar toggle). No decoder-output or configuration-format changes; existing `pdw.ini` and `filters.ini` files work unchanged. See `RELEASE_NOTES.md` for the full list; highlights:
- **Penalty-system RX score no longer over-strict on POCSAG (FIX [RxqSyncThreshold])** — the Health panel's "Penalty system" score (and the shared telnet wire-format RXQ) could drop faster than intended on POCSAG even when every message decoded and displayed correctly. A borderline sync/idle match - accepted for decoding because PDW is deliberately more noise-tolerant there than the reference scoring algorithm - was still scored as a full error; it no longer is. Decoding and on-screen behaviour are unchanged; only the "Penalty system" score is affected (the classic RX-Q needle never looked at this). FLEX was audited alongside this and already matched.
- **Unreadable POCSAG capcode shown in red (FIX [PocsagCapcodeColor])** — the `???????` placeholder for an uncorrectable capcode now uses the same red already used for corrupted message text, making it stand out at a glance. Display-only; the underlying value, filter matching, logging and output feeds are unchanged.
- **Default message font is now Consolas 12pt (FIX [DefaultFontConsolas])** — a fresh install (no `pdw.ini` font settings yet) now starts with Consolas at 12pt instead of Courier New; the shipped release `pdw.ini` was updated to match. Anyone with an existing `pdw.ini` keeps their current font unchanged - this only changes the built-in default.
- **New toolbar Health panel (FIX [HealthPanel], [HealthSource], [FeedStatus], [FeedStatusConn], [HealthPanelCorner], [HealthSparkColor])** — a compact status strip on the right of the toolbar band: colour-coded health score with a selectable source (classic RX-Q score or the stricter "Penalty system" score), a status-coloured trend sparkline (1/5/15/60 min; a healthy period reads as a solid green line), a COM-port link dot, and one status dot per enabled output feed (green = no known problem, orange = retrying, red = failing; connection-oriented feeds track their connection live). The existing RX-quality mail alert follows the selected health source. While shown, the panel replaces the classic needle + RX-Q corner (clean switchover, nothing displayed twice); hide it via the right-click menu to get the classic corner back. Settings persist under `[HealthPanel]` in `pdw.ini`. (A per-entry hover-tooltip addition was tried and pulled the same day - FIX [HealthPanelTipsDisabled] - after it introduced the only new, unverified risk alongside a reported crash.)
- **Health panel follow-up fixes (FIX [HealthRxqStale], [Rs232LinkRealData], [HealthPanelFitsFail])** — the Penalty-system score now reads 0% after 2 minutes of dead air instead of freezing at its last healthy value, so the low-quality mail alert also catches a receiver that dies after a healthy period (the telnet wire score keeps exact p2kflex parity and is untouched); the COM dot no longer shows green on a serial link that reopens fine but delivers no data (e.g. a Moxa NPort with its TCP tunnel down) - it is now judged by actually received bytes; and a failed off-screen back-buffer (GDI-handle pressure) no longer leaves the toolbar corner empty - the classic needle/RX-Q corner returns on the next repaint.
- **Health log no longer logs routine "idle -> OK" (FIX [HealthLogNoIdleOk])** — a feed's first successful contact after startup (or after MQTT's periodic idle-reconnect) is not a failure; that specific transition is no longer written to `{date}_health.log`. Every transition that involves an actual problem (going into RETRYING/FAILED, and the recovery back to OK) still logs exactly as before, so the file stays a record of what actually went wrong.
- **Health trend: 4/8-hour windows + worst-dip band (FIX [HealthSparkLong])** — the trend sparkline can now look back 4 or 8 hours (buffer grown from 1 to 8 hours) so you can review the past night. Because one pixel then spans several minutes, the sparkline draws the average as its line but adds a bold colour-coded band down to the worst value in each pixel, so a brief overnight RX collapse shows as a red spike stabbing downward instead of being smoothed away; the tooltip reports the lowest value and when it happened ("lowest 40% at 03:15").
- **Health panel dot size and colour tuned (FIX [HealthDotSize], [HealthStatusTriad])** — the feed/COM status dots settled at 8px (7px was too small, 10px too big); the layout and the retry-ring/error-bar shapes scale with the new size automatically. The status colours were re-tuned into one coherent triad for the light-gray toolbar band: a darker, more saturated green (the earlier light green read too faint on the "100%" score text), a strong deep red so faults read instantly, and a clearly distinct orange. The message-text colour palette is unchanged.
- **Smooth, round status dots (FIX [HealthDotAA])** — the small status dots were drawn with the plain GDI circle primitive (no antialiasing), so at ~8px the outline came out angular ("cog"-like). Each dot is now rendered at 4x and smoothly downscaled - the same supersampling technique the signal meter and toolbar icons already use - so the dots read as clean, round discs at any DPI.
- **Clearer failed dot + size/alignment polish (FIX [HealthDotNoBar], [HealthDotSize], [HealthDotVAlign])** — dropped the white "minus" bar from the red "failed" dot: a red disc with a horizontal white bar reads as the universal "no-entry / disabled / off" glyph, so an errored feed looked switched-off rather than faulty. Failed and healthy are now plain solid discs (red / green), retrying stays a hollow ring, and the hover tooltip still spells the state out. The dots are also one pixel larger and nudged down slightly so they sit optically centred against their tag text.
- **Health panel usability (FIX [HealthRollupBar], [HealthScoreLabel], [HealthClickConfig], [HealthSparkFill])** — a full-height overall-status accent bar on the panel's left edge summarises RX/COM/feeds at a glance (kept off the score so a red bar never reads as "RX is red"); an `RX` caption labels the score; left-clicking an entry opens its settings dialog; and the sparkline gained a soft colour-tinted area fill under the curve.
- **Stray RX-Q "100%" under the combined-layout needle (FIX [HealthComboRxqLeak], [HealthFitSizeInvalidate])** — with "Show RX needle alongside" enabled (the Health panel plus the classic needle), the old green RX-Q "100%" box could briefly appear under the needle after maximizing and then vanish. The panel's "does it fit" state was cached from the previous window size, so on a resize the classic corner pieces drew one frame using the stale value before the panel refreshed it. The fit is now re-evaluated up front on every title-bar repaint and invalidated on any window-size change, so the classic percentage is never drawn while the panel is shown.
- **Show/hide the menu bar (FIX [MenuBarToggle])** — new **Display > Show Menu Bar** checkbox (shortcut Ctrl+Shift+M) hides the whole menu bar while keeping the toolbar visible; the panes reflow to use the freed space immediately. With the menu bar hidden it can still be brought back via the shortcut or via right-click anywhere in the main window (toolbar included). The state is remembered in `pdw.ini` (`ShowMenuBar`, default on).
- **New high-resolution toolbar + themed dialogs (FIX [ToolbarHiResIcons])** — the toolbar now uses 72x72 32-bit icons with a real alpha channel, box-filtered down to the current DPI; the manifest opts in to Common Controls v6, so every dialog also gets the modern themed look instead of the classic Windows 2000 style.
- **Toolbar icons squared up to one grid (FIX [ToolbarIconGrid])** — cosmetic follow-up so every button carries the same optical weight: all 13 glyphs are drawn to one 54 px content box centred on the canvas (the folder and copy-pane icons enlarged, the statistics bars widened and raised, the pause bars thickened so they no longer look small, and the help "?" given a larger ring with clear whitespace and a 1 px optical bump for the round shape), corner radii unified, and the button-to-button spacing set to a single fixed 8 px grid gutter. No button, tooltip or behaviour changed.
- **Window never opens off-screen (FIX [WindowPosMinimized])** — closing PDW while minimized could save an off-screen window position (-32000,-32000) and start up invisible next time (only the tray icon reachable). The saved position is now sanity-checked against the connected monitors, with a fallback to the default on a corrupt or zero size.
- **Title-bar/corner display glitch fixed (FIX [ToolbarResync], [ToolbarResyncProactive], [TitleBarSelfHeal], [DisplayBitmapReload])** — a full-width black band under the toolbar plus a broken divider line by the signal meter, seen after the machine ran a while, often over a Remote Desktop session. A theme/display change (which an RDP connect/disconnect triggers) silently resizes the toolbar, but PDW only recomputed the toolbar height on a window resize, so the header/divider/meter sat several pixels below the toolbar's new bottom until a manual resize. PDW now detects the change and re-aligns the layout automatically (within ~150 ms via direct system-event handling, with a once-per-second self-heal as a fallback); the meter/warning-icon bitmaps are also recreated after a display-driver change.
- **Signal-meter and divider alignment (FIX [SigindBandAlign], [SigindDividerClip], [RxqSquareBandClamp], [RxqSquareDividerClip])** — the signal-strength meter is now vertically centered in the actual toolbar band instead of a fixed offset, and the divider line under the toolbar no longer breaks off beneath the meter or the RX-quality indicator square once the toolbar settles to its themed steady-state height.
- **Redesigned signal meter (FIX [SigindFlatGauge], [SigindGaugeGray], [SigindGaugeBigger], [SigindWiggleVisibility])** — the signal meter is now a clean flat gauge (rounded frame + half-circle scale + red needle) drawn from a high-resolution source, scaled crisply to any DPI, in the same slate-gray tone and stroke weight as the redesigned toolbar icons, and sized to fill more of its box. The needle is composited over a cached back-buffer so the moving needle can never nibble the scale and there is no flicker. The at-rest needle wiggle (noise present, nothing decoding) is now as visible as on the classic meter — the needle was lengthened to fill the enlarged scale and its low end restored to the original hand-tuned spacing, so it visibly snaps out of rest again. Signal-strength behaviour is unchanged; visual refresh only.
- **Window close no longer runs unrelated re-sync code (FIX [WmCloseFallthrough])** — internal correctness fix: closing the main window briefly ran the toolbar re-sync handler's code via a shared `switch` fallthrough; window close now returns directly instead of falling through.
- **Copy with a full scrollback (FIX [VscrollClamp])** — with a full message buffer and the view scrolled up, the scroll position went negative with every arriving line; using Copy Upper/Lower/Selection then read far outside the buffer (a likely crash on x64). The position is now clamped at the source and defensively in the copy loop.
- **Double-click word selection (FIX [DblClickBounds])** — double-clicking on a very wide window, in the empty band below the last text line, or on a word starting at column 0 could read outside the pane buffer (garbage selection at best, crash at worst). Click coordinates and the word-boundary scans are now bounded; the Google Maps lookup gets the same guards.
- **Filter delete no longer re-enters itself (FIX [FilterDelReentry])** — pressing Delete/F8 again while a multi-filter delete was still running re-entered the delete loop with a stale index and could corrupt the filter list (crash). A second request is now ignored until the first pass finishes.
- **Mouse wheel fixes (FIX [WheelDelta], [WheelCoordSign])** — precision touchpads and fast wheel scrolls always scrolled DOWN (only an exact delta of +120 counted as "up"); the wheel is now direction-tested by sign. The wheel was also dead on a monitor left of/above the primary (negative screen coordinates were misread); fixed.
- **ACARS Colors dialog (FIX [AcarsColorsInit])** — opening ACARS Colors as the first color dialog since startup and clicking OK silently saved the main Time/Date, Message and Bit-errors colors as black; the DBI and Labels fields also shared one color slot, overwriting each other. All values are now seeded on open and DBI has its own slot.
- **Logfile dialog keeps the filename (FIX [LogDlgNameWipe])** — with a fixed (non-date) logfile name the filename field was always blank on open, and toggling any checkbox in the dialog erased a just-typed name ("You haven't entered a file name!" on OK).
- **POCSAG function-number list (FIX [FnuComboReset])** — browsing filters with Next/Previous duplicated the All/1/2/3/4 function-number list on every jump onto a POCSAG filter; picking a duplicated entry saved an invalid function number that never matched.
- **Config-file validation (FIX [ScreenColClamp], [FilterTypeClamp], [ReadFiltersBounds])** — hand-edited or corrupt `pdw.ini`/`filters.ini` values (screen column order, default filter type, truncated filter lines, overlong last-hit fields) could index or write out of bounds; everything is now clamped/bounded at load.
- **Filtered-pane scrollbar accuracy (FIX [PaneFilterScrollbarSync])** — the Filtered pane's vertical scrollbar is now refreshed on every appended line, so it always reflects whether older messages have scrolled off the top.
- **Group-call logging (FIX [GroupcallLogFilename])** — later members of a FLEX group call no longer risk being logged through a stale filename (which could route monitor-log lines into the filter file). Screen and feeds were unaffected.
- **POCSAG corrupt capcodes (FIX [PocsagCapcodeGuard])** — a RIC with an uncorrectable bit error is again shown as `???????` instead of a plausible-but-wrong capcode.
- **FLEX long-address capcode (FIX [FlexLongAddrOverflow])** — with FLEX Group Mode off, a long-address message whose capcode fell outside the 9-digit range could overflow a 32-bit intermediate and show a wrong capcode; it is now computed in 64-bit and shown as `?????????` when out of range. Default (Group Mode on) skips long addresses, so this never affected a default setup.
- **FLEX RX-quality reading (FIX [FlexBiterrorDoubleCount])** — FLEX bit errors were counted twice into the on-screen RX-Quality meter (POCSAG once), making FLEX read slightly low; now counted once. The telnet RX-quality feed was unaffected. Plus explicit decoder bounds hardening (FIX [PocsagNumBound], [FlexNumWordClamp], [EccPosGuard]).
- **SMTP monitor-window freeze (FIX [SmtpAddRespDeadlock])** — disabling SMTP or exiting with the SMTP monitor/test window open could deadlock the whole app; the status update is now non-blocking.
- **SQLite size cap (FIX [SqliteSizeCap])** — the optional maximum-size limit no longer risks emptying the whole table on a database without incremental auto-vacuum.
- **COM port lock-out (FIX [ComPortReopenLeak])** — a force-stopped serial reader mid-reconnect no longer leaks an exclusive COM handle that locked PDW out of its own port until restart.
- **Telnet server wire-log latency (FIX [TsWireLogLockFree])** — the optional wire-log disk write used to run while the telnet server's internal state lock was held (a synchronous file write by default), briefly stalling client accept/send and RS232/AUDIO state updates for the duration of that write. The line is now formatted under the lock but written to disk right after the lock is released.
- **Output-feed & logging concurrency audit** — a deep scrub of the feed ring buffers, worker lifecycles and shutdown/reconfigure paths. The telnet wire-log no longer drops lines when several are emitted in one locked operation (FIX [WireLogMultiLine]); the webhook feed no longer writes the (token-bearing) URL to its log and keeps its JSON valid on very large group calls (FIX [WebhookUrlSecret], [WebhookSubRollback]); the mail worker start is failure-safe and the SMTP test-dialog response handle is cleared on close (FIX [SmtpThreadStart], [SmtpRespWnd]); plus sqlite maintenance aborts promptly on shutdown (FIX [SqliteMaintCancel]), mqtt/webhook event teardown+signalling are serialized under the feed lock (FIX [MqttEventRace], [WebhookEventRace]), mysql/smtp snapshot the socket before `shutdown()` (FIX [MysqlShutdownFd], [SmtpShutdownFd]), the mqtt/smtp run-flags use `std::atomic` (FIX [MqttAtomicRun], [SmtpAtomicRun]), and a debug-only credential trace was removed (FIX [SmtpCredTrace]). Day-to-day behaviour is unchanged.
- **Pre-release feed scrub (second pass)** — exit/reconfigure can no longer freeze on a dead endpoint (bounded shutdown flush with `LOST` logging — FIX [FlushBounded]); exiting while a mail was in flight can no longer crash (FIX [SmtpShutdownNullCfg]); SQLite maintenance is batched/interruptible and a failed COMMIT can no longer silently wedge the feed (FIX [SqliteMaintInterrupt], [SqliteCommitCheck]); the MySQL shutdown flush now actually delivers and a stop interrupts an in-progress connect (FIX [MysqlStopDrain], [MysqlConnAbort]); the lock-free SMTP queue survives its full-ring corner and captures the recipient per queued mail (FIX [SmtpRingPublish], [SmtpToSnapshot]); the telnet server replays a completely full backlog correctly, stages its event log off the server lock and detaches the listen socket under the lock (FIX [TelnetReplayCount], [TsEventLogStaged], [TelnetListenSockLock]); Telegram applies supergroup migrations on the GUI thread and retries a rate-limited plain-text fallback (FIX [TgProfileGuiSync], [TgFallback429]); applying log settings no longer discards buffered lines (FIX [LogReconfigureFlush], [LogEventRace]); and every dropped message is now visible via feed status + log (FIX [QueueDropVisible], [ShutdownLost], [MysqlDropLog], [SqliteDrainLog]).
- **Low-severity hardening sweep** — GDI/handle leaks fixed (font-dialog HFONT per font change, printer hDevMode/hDevNames per print job, cancelled print jobs now abort cleanly — FIX [FontDlgLeak], [PrinterJobGuards]); clipboard-owned memory is no longer freed (FIX [ClipOwnedFree]); selection/scroll edge cases (negative drag coordinates, >65535-line scrollback thumb drag, click-without-drag copy artifact — FIX [PaneSelCoordSign], [ThumbTrack32], [CopySelNoDrag]); scrollback resize is now OOM-safe (new buffers allocated before old ones are freed — FIX [ScrollDlgOomSafe]); stale dialog state (filter-window color restyle, filter-edit init guard, two uninitialized locals — FIX [FilterColorsStale], [FilterEditInitGuard], [SortFilterFocusInit], [ResetHitExtInit]); line-wrap and highlight corner cases on very wide windows and matches at message start (FIX [LastcharWrapEscape], [HighlightPosZero]); right-to-left (Hebrew) mode now reverses colors together with the text (FIX [ReverseColorSync]); signal-meter self-heal after failed bitmap reloads (FIX [GaugeSrcdcFail], [SigindReloadRetry], [ExclamLoadGuard]); title-bar text assembly and file-extension appends are bounded (FIX [WindowTextBound], [EditFileExtBound], [PathBufferOffByOne]).
- Plus hardening for MOBITEX/ACARS decoding, MySQL/Telnet/MQTT/Telegram/Pushover feeds, sound-card restart, printing (pagination), clipboard copy, several dialogs, and language-table loading.

### v4.0.3 (July 2026)
MQTT reliability fix plus a small display option. No decoder-output or configuration-format changes; existing `pdw.ini` and `filters.ini` files work unchanged.
- **Optional fragment marker (FIX [FragMarkerOptional])** — the `*` that PDW draws after the capcode of a reassembled multi-frame FLEX message is now **off by default** and controlled by a new checkbox in **Screen Options** ("Mark reassembled fragmented messages with '*' after the capcode"). The fragment-reassembly logic itself is unchanged; only the on-screen marker is now opt-in. Enable it if you want to see at a glance which messages were rebuilt from fragments.
- **Pane layout polish (FIX [PaneBottomPad], FIX [PaneScrollbarAlign])** — both message panes now size their text area to whole lines (no more half-clipped bottom row at any scroll position) and keep a small margin between the newest row and the pane edge below it. The padding lives inside the pane window, so each pane's vertical scrollbar stays flush with the pane footer (the Filtered Messages header, resp. the window bottom edge). The RX-Q / percentage box in the Monitored Messages title bar is now pixel-aligned with the rest of the bar at any display scaling (FIX [RxqTitleAlign]), the separator line under the title bar stays continuous beneath it (FIX [RxqBottomLine]), the toolbar's RX-quality warning square is drawn consistently in both states (FIX [RxqSquareRect]), and hovering the toolbar no longer flickers the title bars (FIX [NotifyRedraw])
- **Main window could start invisible (FIX [WindowPosMinimized])** — exiting PDW while minimized to the taskbar saved the Windows minimized-window position (-32000,-32000) to `pdw.ini`, so the next start created the main window far off-screen: only the tray icon was reachable while decoding kept running. The position is no longer recorded while minimized/trayed, and at startup a saved position that does not touch any monitor now falls back to the primary work area
- **High-resolution toolbar icons (FIX [ToolbarHiResIcons])** — the 13 toolbar buttons have a new, modern icon set. The old assets were 18x18 pixel, 16-color bitmaps that were stretched up on scaled displays, which made them look blocky and dated; the new icons are 72x72 32-bit images with a real alpha channel that are smoothly downscaled (box filter) to the exact DPI-scaled button size, so they render crisp and anti-aliased at 100 %, 125 %, 150 % and 200 % scaling. PDW now also declares the Common Controls v6 dependency in its manifest, which enables alpha-blended toolbar icons and gives all dialogs the modern themed Windows look instead of the classic style
- **Warm MQTT connection (FIX [MqttWarmConn], FIX [MqttKeepAlive])** — the 3-minute idle disconnect is removed and keepalive is now actually driven (the Paho synchronous client only emits keepalive pings when the app calls `MQTTClient_yield()`, which PDW as a pure publisher never did, so the broker kept reaping the idle session as "exceeded timeout"). The worker now yields ~every 10 s so pings go out and the session survives multi-minute quiet periods (keepalive 45 s), matching how every other MQTT client behaves
- **Proactive idle reconnect (FIX [MqttIdleReconnect])** — if the broker drops the connection while PDW is idle (broker restart/update), the worker now quietly re-establishes it within at most 60 s instead of leaving it cold until the next message arrives. A healthy connection is never touched, and a broker that stays down is retried only once per minute (no connect storm, one log line instead of one per attempt)
- **Hardened MQTT reconnect (FIX [MqttReconnHarden])** — connect timeout 5 s to 10 s and 2 to 4 publish attempts with exponential back-off (1/2/4 s), mirroring the webhook feed's retry profile, so a genuine broker restart is ridden out rather than dropped. Shutdown/reconfigure is unaffected (retries are skipped once a stop is in progress)
- **Retry on unconfirmed QoS delivery (FIX [MqttWaitRetry])** — with QoS 1/2, a publish that is accepted locally but whose broker acknowledgement never arrives (a half-open connection after an idle gap) was previously dropped; it is now retried on a fresh connection instead of being lost. This was the exact failure seen in the logs once QoS was raised above 0

### v4.0.2 (July 2026)
Stability release — hardening fixes from a full release-readiness audit (memory safety, bounds checking). No decoder-output or configuration changes; existing `pdw.ini` and `filters.ini` files work unchanged.
- **Bounded filename-extension append** — the Logfile, Filterfile and Statistics dialogs appended their default extension (`.log`, `.flt`, `.st`) with an unchecked `strcat`; a maximum-length filename filled the buffer completely and the append wrote past it (a GUI-triggerable stack overrun). Same for the per-filter separate-filterfile fields (`.txt`). The extension is now only appended when it still fits
- **Group-call sort bounded** — sorting a FLEX group-call member list could read one integer past the group array when a group was at its absolute maximum capacity (999 members); now explicitly bounded
- **MOBITEX hardening** — a hand-edited MOBITEX filter capcode shorter than 2 characters made the matcher index before the string (now falls back to the default address+mode match), and the sweep-info decoder took its channel count from a raw decoded byte, letting a corrupted frame overrun the 15-entry channel table (now capped and bounded)

### v4.0.1 (June 2026)
- **Readable, unique filter label colours** — the **Default** filter label colour was pure blue (`0,0,255`), which reads poorly on the black message background; it is now a readable azure (`64,128,255`). With **Better contrast** enabled, three blue slots (Default, Light Blue, Blue) previously remapped to the *same* colour — they are now distinct, so every label colour in the enhanced palette is unique. Existing installations keep their saved Default colour; use **Reset all colours to Default** (or start from a fresh `pdw.ini`) to pick up the new blue
- **Custom filter label colours via `pdw.ini`** — all 17 filter label colours can now be overridden in `pdw.ini` without any menu setting. Slot 0 keeps the existing `Color.FilterLabel=R,G,B` key; slots 1-16 use `Color.FilterLabel1=` .. `Color.FilterLabel16=`. It is per-key autodetect: a missing, empty or invalid key keeps the built-in default, so an existing config behaves exactly as before. Filters still reference a colour by its slot number (the `filters.ini` format is unchanged), so this is fully backward compatible

### v4.0.0 (June 2026)
First final release of the modernised fork. It consolidates five years of work on the classic PDW 3.2 codebase — six output feeds (SMTP, MQTT, webhook, Telnet, MySQL, SQLite) plus Telegram and Pushover, a central log manager, High-DPI support, RX quality monitoring and extensive 24/7-reliability hardening — into one stable, dependency-free binary. The changes carried over from the 3.7.x development line are:
- **Telnet server: half-open client sessions detected and cleaned up** — an ungraceful client drop (router/NAT rebind, crash, or a blip that swallowed the TCP close) used to leave a "connected" slot that PDW kept sending to, while a reconnect was assigned a *second* slot — two simultaneous sessions, both shown active, with only one processed downstream. PDW now enables TCP keepalive on every accepted connection, so a dead peer is detected within ~45 s and its slot freed for reuse. Multiple genuine connections from one address (several clients behind a NAT, test setups) stay fully supported — each TCP connection is its own session
- **TCP keepalive on the SMTP connection** — a safety net so a mail server that vanished without closing the link cannot leave a half-open connection lingering, on top of the existing 30 s receive timeout
- **Telegram: direct (1-on-1) chats no longer fail with HTTP 400** — a configured forum topic / thread id is only valid for a group/supergroup, but was previously applied to *every* destination, so a direct chat with the bot was rejected and logged as `failed`. The topic is now applied only to group/supergroup/channel destinations (negative chat ids); direct chats (positive ids) send normally. Covers both the global chat list and per-filter chat overrides
- **Telegram: Discover finds every chat, including your direct chat** — Discover used to return only the *last* chat in the bot's recent updates, so an active group always won and a 1-on-1 chat was never offered. It now lists *all* distinct chats (id, type, name), de-duplicated, and appends each new one in a single click; the reply buffer was enlarged so a busy bot's updates are no longer truncated
- **Per-filter Telegram routing** — the Ctrl+F filter editor gained *TG topic* (route a capcode's messages to a specific forum topic / thread id) and *TG chat* (override the destination chat for this capcode — one or more `-100…` ids, `@channel` names, or several separated by `;`). Both are stored in `filters.ini` (new CSV fields 14-15) and apply inside FLEX group calls; old files load with the global routing as before
- **Pushover: HTML line breaks no longer add a stray space/blank line** — with HTML formatting on, PDW used to translate every `\n` into a `<br>`, which stacked a second break on top of Pushover's own newline-to-break handling. Newlines are now sent verbatim in both plain and HTML mode: a single `\n` renders as one clean line break everywhere (use `\n\n` for a blank separator line)
- **Filter dialog layout fixes** — the "Number of hits" field is back to its original width (multi-digit counts and last-hit date/time are fully visible again), and the "PO priority" / "PO sound" labels no longer run underneath the input next to them

### v3.6.9 (June 2026)
- **Late / fragmented FLEX group messages keep their subscriber list** — a group call is announced by Short Instructions that say which frame the message will arrive in; PDW then lists the collected member capcodes under that message. A long message can be split into fragments sent in later frames, and a busy frame can also delay the message, so it may arrive a frame or more after the announced one. Previously PDW required an exact frame match, so a late message was shown bare (no members), its collected member list was never cleared, and that stale list later leaked onto the next group call reusing the same group slot (showing up minutes later under the wrong group). PDW now accepts the message within the same grace window it already uses for missed-call detection, shows the members under the correct message right away, clears the slot, and no longer mis-counts the call as "missed". Applies to all 16 group codes and to every output feed and database; the telnet wire feed was already correct

### v3.6.8 (June 2026)
- **Telegram/Pushover state shown in the Ctrl+F overview** — with *Show extra info* on, each filter row now displays `TG`/`tg` and `PO`/`po` flags (uppercase = on) alongside the existing `CMD`/`LAB`/`SEP`/`IGN-GRP` markers, so you can see at a glance which capcodes are wired to each notification service across a large filter set
- **Per-filter Telegram silent and Pushover priority/sound overrides** — the Ctrl+F filter editor now has three new per-filter notification controls: a *Telegram silent* checkbox (overrides the global silent setting for this capcode only), a *PO priority* dropdown (-2 Lowest / -1 Low / 0 Normal / 1 High / Global), and a *PO sound* text field (blank = use global). These let you set high priority on alarm capcodes or mute routine pagers without touching the global sink configuration. Stored in `filters.ini` (bit `0x100` of the flags field for silent; appended fields 12-13 for priority/sound). Disabled for reject rules; supported in multi-edit (tri-state / "Don't change" where selections differ). **The overrides also apply inside FLEX group calls** — a monitor capcode that only ever appears as a group-call subscriber still drives the notification: most-urgent priority wins, first non-empty sound wins, and a group is silenced only when every matched subscriber asked for silent. The Telegram/Pushover **Test** buttons now also include the configured priority/sound/silent

### v3.6.7 (June 2026)
- **"Ignore in Groupcall" filter option** — a new per-filter checkbox in the Ctrl+F editor hides a routine subscriber capcode (roadblock, station-technical, etc.) from the on-screen FLEX group view so the genuine personnel-alarm subscribers stand out. An ignored code no longer shows its line, drags its group into the filter window, or beeps; but the **full** group message - including the ignored capcode - is still written to the monitor log and sent to every output feed. It has no effect on individual (non-group) pages to that capcode, and is mutually exclusive with *Monitor only* (ticking one clears the other). Applies to non-reject capcode filters inside FLEX group calls; flagged filters are marked `IGN-GRP` in the filter overview

### v3.6.6 (June 2026)
- **Reject log formatting inside group calls fixed** — when "Also log rejected messages" was on and a rejected capcode appeared inside a FLEX group call, the monitor log got garbled: a spurious blank line appeared before every rejected subscriber entry, and with FlexGroupMode compact logging the rejected subscriber was written as a standalone timestamp line instead of the correct indented capcode line under the group header. Both issues are fixed; rejected group-call subscribers now integrate cleanly into the log in the same format as non-rejected ones.
- **Long log lines no longer dropped or glued together** — log lines longer than an internal 1 KB buffer were either silently dropped (making capcode/label lines appear under the wrong message text) or written without their trailing newline (gluing the next entry onto the same line, mixing texts and labels of different messages). The log manager now splits long lines losslessly across its ring buffer and all message-log writers pass the full line, so a message and its label always stay together

### v3.6.5 (June 2026)
- **Whole-word matching with `=`** — prefix a filter term with `=` to match it only as a complete word, not inside a longer word, e.g. `alpha&=cat` matches the word `cat` but no longer false-matches on `category`. A word boundary is any non-alphanumeric character or the start/end of the message; the `=` applies per term so substring and whole-word terms can be mixed. This completes the filter-text operator set: `^` (starts with), `&` (AND), `|` (OR) and `=` (whole word) — all documented with examples in the manual (section 9.3)

### v3.6.4 (June 2026)
- **Command file runs from its own folder** — an external command file triggered by a filter now starts with its working directory set to the folder it lives in, so a helper that keeps its config and log files next to itself works again even when it sits in a separate folder; previously it inherited PDW's working directory and could fail to find its config or write its logs into the PDW folder

### v3.6.3 (June 2026)
- **OR operator in filter text** — message-text filters now support `|` (OR) alongside `&` (AND), e.g. `alpha&bravo|alpha&charlie`; `&` binds tighter than `|`. The *Match exact text* option is greyed out automatically while the text uses `&` or `|`
- **Filter text length raised to 256** — message-text patterns now accept up to 256 characters (was 120), matching the label field; existing `filters.ini` files load unchanged
- **Hardening** — the two fixed-size buffers that build strings from the longer filter text (the filter-row display string and the per-filter wave-file name) are now bounds-checked so a long text cannot overflow them

### v3.6.2 (June 2026)
- **Telegram long-message split fixed** — splitting a message over 4096 characters no longer cuts through a UTF-8 character (which made Telegram silently drop the whole message) or an HTML tag; the split now backs off to the nearest safe boundary
- **Stability** — the Telegram/Pushover worker run-flag is now `std::atomic` (removes a data race on shutdown)
- **Performance** — recording writes are batched (fewer disk syscalls), the log manager groups writes by file, and the RS232 four-level flag is hoisted out of the per-bit loop
- **MOBITEX clock recovery fix** — the resync sample counter is now reset per transition so the bit-clock guard works as intended

### v3.6.1 (June 2026)
- **Telegram & Pushover output sinks** with separate Title/Body templates (`{message}` placeholder, `\n` line breaks), a Test button that previews the real formatting, SMTP-style send-in modes, and one-message-per-group-call batching (one label per line)
- Pushover/Telegram line breaks: a `\n` in the template renders as one clean line break in both plain and HTML mode (use `\n\n` for a blank separator line); both sinks default to bold `<b>{message}</b>\n{label}`

### v3.5.9 (June 2026)
- **Per-capcode match state in group calls** — each member in the `subscribers` JSON now carries its own `match_type` (filtered / monitor-only / no match), so a viewer renders every capcode in the same pane the PDW window does
- Shutdown hardening for the log manager, MQTT and webhook senders

### v3.5.7 (June 2026)
- **SQLite output feed** — single local file, no DLL, LowWrite mode, optional auto-purge

### v3.5.6 (June 2026)
- **MySQL output feed** — no external DLLs required, three schema modes
- **RX Quality Alert** — e-mail notification when signal quality is poor for a sustained period
- SMTP crash on rapid Test-button clicks fixed
- Raw feed mode added to MQTT, Webhook, and Telnet
- Date-stamped log files for all output feeds

### v3.4 (2026)
- **Telnet server** on port 8024 — streams decoded messages in a structured wire-format (custom internal feature)
- RX Quality (`<RXQ:NN>`), watchdog (`<WD>`), RS232 and Audio presence markers
- Reconnect backlog replay window (60 s)

### v3.3 (2026)
- **High-DPI support** — crisp layout on 4K and HiDPI monitors
- **SMTP split Subject/Body** — choose which fields appear in subject vs. body
- SMTP encryption combobox (Auto / STARTTLS / SSL)
- SMTP error logging to disk

### v3.2 → v3.3 (2026)
- **MQTT output** with PDW-native and flat/Node-RED JSON formats
- **Webhook HTTP(S)** output
- **Windows 11 toast notifications**
- FLEX multi-frame message reassembly
- SMTP STARTTLS (port 587) and implicit TLS (port 465)
- x64 build target added
- COM port numbers ≥ 10 supported
- Exclusive COM port access — while PDW runs, no other program can hijack its port (prevents stream-splitting on virtual COM/Moxa redirectors)
- Filter label length increased to 256 characters

---

## License

PDW is licensed under the **GNU General Public License v3.0** (GPL-3.0) — see `LICENSE` for the full text. All additions in this repository are released under the same GPL-3.0 terms.

PDW was originally developed by **Jason Petty** (2001–2004) and **Peter Hunt** (2004–2010), who open-sourced it in 2013. This repository builds on the community fork maintained at [github.com/Discriminator/PDW](https://github.com/Discriminator/PDW). Many thanks to the contributors who kept the codebase alive over the years:

Discriminator · andrey2805 · evroza · Muspah · lt-holman · senf666

---

