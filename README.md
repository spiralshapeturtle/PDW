# PDW — Paging Decoder for Windows

> **Legal notice:** Receiving and decoding paging transmissions may be restricted or prohibited by law in your country or region. It is your sole responsibility to verify that your use of this software complies with all applicable local, national, and international laws and regulations. This software is provided "as is", without warranty of any kind, express or implied. The authors and contributors accept no liability whatsoever for any damages, legal consequences, or other losses arising from the use or misuse of this software. By using this software you accept full responsibility for ensuring its lawful use.

---

**Version 4.0.1** | Windows 7-11 | Win32 + x64 | Visual Studio 2017+

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
| **RX Quality Alerts** | Get an e-mail when signal quality drops below a threshold for too long |
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

### RX Quality monitoring

The on-screen RX Quality bar is also tracked over time. When signal quality stays below a configurable threshold for too long, PDW sends an e-mail alert.

**Settings (Options → RX Quality Alert):**

| Setting | Default | Description |
|---------|---------|-------------|
| Threshold | 80 % | Quality below this triggers the timer |
| Recovery level | 90 % | Quality above this cancels the timer |
| Minimum duration | 15 min | How long below threshold before sending |
| Cooldown | 120 min | Silence period between repeated alerts |

The alert uses the same SMTP worker as the filter-based mail, so no extra configuration is needed.

---

### Windows push notifications

Press **Ctrl+T** to send a test Windows toast notification (Windows 10/11). PDW uses the native `IUserNotification` API instead of the legacy tray balloon, so notifications appear correctly in the Action Center on Windows 10 and Windows 11.

The system tray icon provides minimize-to-tray, click-to-restore, and optional per-message notifications for filtered messages.

---

### High-DPI support

PDW declares `System DPI Aware` in its manifest. Fonts, toolbar, and layout are recalculated from the actual DPI at startup — no blurring or clipping on 125 %, 150 %, or 200 % scaled displays.

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

