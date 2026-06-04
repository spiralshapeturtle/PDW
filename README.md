# PDW — Paging Decoder for Windows

**Version 3.5.8** | Windows 7–11 | Win32 + x64 | Visual Studio 2017+

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

Filter labels support up to 256 characters; COM ports ≥ 10 are supported. Search-while-typing is available in the filter list. The filter list font follows the main window font setting.

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
| `match_type` | TINYINT | `0` = no match · `1` = filtered · `2` = monitor-only |
| `label_color` | VARCHAR(7) | `#RRGGBB` of the label; empty when none |

`received`, `address`, and `match_type` are always written. `mode`, `msg_type`, `bitrate`, `message`, and `label` are written only when enabled in the PDW field-bitmask setting. `subscribers` and `label_color` are written only when non-empty.

**Group calls (`subscribers`):**

FLEX group calls store the individual paged addresses as a JSON array:

```json
[
  {"address": "1234567", "label": "Ambulance 1", "color": "#1565c0"},
  {"address": "1234568", "label": "Ambulance 2"}
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

The PDW executable is intentionally large. OpenSSL, Paho MQTT, and all other third-party libraries are **statically linked** directly into the binary — there are no separate DLLs to install or keep in sync. Upgrading is a single-file operation: copy the new `PDW.exe` and you are done.

---

## Changelog highlights

### v3.5.7 (June 2026)
- **SQLite output feed** — single local file, no DLL, LowWrite mode, optional auto-purge

### v3.5.6 (June 2026)
- **MySQL output feed** — no external DLLs required, three schema modes
- **RX Quality Alert** — e-mail notification when signal quality is poor for a sustained period
- SMTP crash on rapid Test-button clicks fixed
- Raw feed mode added to MQTT, Webhook, and Telnet
- Date-stamped log files for all output feeds

### v3.4 (2026)
- **Telnet server** on port 8024 — CS FlexDecoder-compatible wire-format
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

