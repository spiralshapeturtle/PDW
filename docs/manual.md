# PDW User Manual

**Version 3.5.8** | Windows 7–11 | FLEX / ReFLEX / POCSAG / ACARS / MOBITEX / ERMES decoder

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Legal notice](#2-legal-notice)
3. [Requirements](#3-requirements)
4. [Getting started](#4-getting-started)
   - 4.1 [Sound card input](#41-sound-card-input)
   - 4.2 [Serial port input](#42-serial-port-input)
   - 4.3 [Discriminator tap](#43-discriminator-tap)
5. [Main window](#5-main-window)
   - 5.1 [Signal indicator / RX Quality bar](#51-signal-indicator--rx-quality-bar)
   - 5.2 [Message columns](#52-message-columns)
6. [File menu](#6-file-menu)
7. [Monitor menu](#7-monitor-menu)
   - 7.1 [Statistics window](#71-statistics-window)
8. [Interface menu](#8-interface-menu)
   - 8.1 [Sound card setup](#81-sound-card-setup)
   - 8.2 [Serial port setup](#82-serial-port-setup)
9. [Filters](#9-filters)
   - 9.1 [Filter fields](#91-filter-fields)
   - 9.2 [Filter actions](#92-filter-actions)
   - 9.3 [Search while typing](#93-search-while-typing)
10. [Options menu](#10-options-menu)
    - 10.1 [SMTP e-mail alerts](#101-smtp-e-mail-alerts)
    - 10.2 [Webhook](#102-webhook)
    - 10.3 [MQTT](#103-mqtt)
    - 10.4 [Telnet server](#104-telnet-server)
    - 10.5 [MySQL output](#105-mysql-output)
    - 10.6 [SQLite output](#106-sqlite-output)
    - 10.7 [RX Quality Alert](#107-rx-quality-alert)
    - 10.8 [Log files and write buffering](#108-log-files-and-write-buffering)
    - 10.9 [Program options](#109-program-options)
11. [Display menu](#11-display-menu)
12. [Protocols decoded](#12-protocols-decoded)
    - 12.1 [POCSAG](#121-pocsag)
    - 12.2 [FLEX and ReFLEX](#122-flex-and-reflex)
    - 12.3 [ACARS](#123-acars)
    - 12.4 [MOBITEX](#124-mobitex)
    - 12.5 [ERMES](#125-ermes)
13. [Paging message format](#13-paging-message-format)
14. [Log file format](#14-log-file-format)
15. [Support files](#15-support-files)
16. [Windows notifications](#16-windows-notifications)
17. [High-DPI support](#17-high-dpi-support)
18. [Multi-instance / title bar](#18-multi-instance--title-bar)
19. [COM ports >= 10](#19-com-ports--10)
20. [Troubleshooting](#20-troubleshooting)
21. [Changelog](#21-changelog)
22. [Credits and license](#22-credits-and-license)

---

## 1. Introduction

PDW is a software paging decoder that turns a sound card or serial port into a full FLEX / ReFLEX / POCSAG / ACARS / MOBITEX / ERMES receiver. It decodes, filters, and distributes paging messages to a wide range of output channels — from simple on-screen display and e-mail alerts to MQTT brokers, webhooks, Telnet clients, MySQL databases, and local SQLite files.

This version (3.5.8) builds on the classic PDW 3.2 codebase and adds five years of production-hardened improvements. The main additions over the original release are listed in [section 21](#21-changelog).

---

## 2. Legal notice

Decoding paging signals may be illegal in your country. This software is provided on an as-is basis. The authors cannot be held responsible for any legal action that results from its use.

---

## 3. Requirements

| Item | Minimum |
|------|---------|
| OS | Windows 7, 8, 10, or 11 (32-bit or 64-bit) |
| CPU | Any modern CPU; a 1 GHz Pentium-class is sufficient |
| RAM | 32 MB |
| Disk | 10 MB for the application; extra for log files and the optional database |
| Sound card | Any Windows-compatible sound card with a line-in or microphone input |
| Radio | Any scanner or receiver covering the frequency band you want to decode |
| Runtime | Microsoft Visual C++ Redistributable 2017 or later (x86 for the Win32 build, x64 for the 64-bit build). Download from Microsoft if not already installed. |

A serial port interface (hardware slicer) can be used instead of a sound card — see [section 8.2](#82-serial-port-setup). Serial port input works on all supported Windows versions including Windows 10 and 11.

---

## 4. Getting started

### 4.1 Sound card input

1. Make a cable to connect your radio or scanner to the **line-in** (or microphone) socket of your sound card. Use a mono plug on the radio end and a stereo plug on the sound card end, wiring the mono signal to both left and right channels.
2. Launch PDW and open **Interface → Setup**. Select **Soundcard** and choose a configuration:
   - **Discriminator** — best quality; requires a discriminator tap (see [4.3](#43-discriminator-tap))
   - **Earphone** — use the radio's headphone socket; good for most setups
   - **Speaker Out** — tap the speaker output
   - **Tape/Rec Out** — use the radio's tape or record output
3. Open **Interface → Volume** to open the Windows mixer. Make sure **Line In** or **Mic** is selected as the recording source.
4. Switch your radio on, set the squelch to its lowest position, and tune to white noise on the VHF band. The signal indicator (top right of the toolbar) should start moving. If it does not, increase the radio volume.
5. From the **Monitor** menu, select the protocol you want to decode.
6. Tune to a known frequency in your area. Messages should appear within a minute. If they do not, try adjusting the volume or try a different configuration in step 2.

**Custom threshold / centering / resync settings**

If you can decode one baud rate but not another (e.g. POCSAG 1200 works but 2400 does not):

1. Select **Custom** in the Interface Setup dialog and click **Set Custom**.
2. Try setting the **Threshold** for the problem baud rate to 1. Click OK and test.
3. If that does not help, try values 2 through 9.
4. Repeat for **Centering** and **Resync** if threshold changes alone are not enough.

### 4.2 Serial port input

A hardware slicer (serial port interface) converts the discriminator audio to a two- or four-level digital signal and feeds it into the PC's COM port. This gives the best possible signal quality and works on all Windows versions including Windows 10 and 11.

1. Build or obtain a serial port slicer for 2-level (POCSAG / ACARS / ERMES) or 4-level (FLEX / MOBITEX) FSK.
2. Connect the slicer to a spare COM port.
3. Open **Interface → Setup**, select **Serial** and choose the correct COM port number. COM ports 10 and higher are supported — see [section 19](#19-com-ports--10).
4. Connect the slicer's audio input to your radio's discriminator output or earphone socket.
5. Select the protocol from the **Monitor** menu and tune to the appropriate frequency.

### 4.3 Discriminator tap

If sound card input gives poor results at any configuration setting, a discriminator tap is the solution. This mod takes the signal directly from the FM discriminator IC of your scanner before the de-emphasis filter, giving a flat frequency response ideal for data decoding.

The tap is a **0.1 uF** capacitor soldered to the discriminator output pin. A shielded wire runs from the capacitor to the sound card line-in or serial port interface. This tap always works when done correctly.

> Discriminator tap modification opens the radio and may void its warranty. If you are not comfortable with soldering, use the earphone output instead.

---

## 5. Main window

The main window shows a scrolling list of decoded messages. Each row is one paging message.

### 5.1 Signal indicator / RX Quality bar

The bar at the right of the toolbar shows the current receive quality as a percentage (0–100 %). A higher value means a cleaner signal. This value is also streamed to Telnet clients as `<RXQ:NN>` (see [section 10.4](#104-telnet-server)).

### 5.2 Message columns

| Column | Content |
|--------|---------|
| Time | Decode time (HH:MM:SS) |
| Date | Decode date (DD-MM-YY) |
| Mode | Protocol and bit rate, e.g. `FLEX-1600`, `POCSAG-1200` |
| Type | `ALPHA` / `NUMERIC` / `TONE` / `GROUP` |
| Bitrate | Numeric bit rate |
| Capcode | Pager address (capcodes may have leading zeros) |
| Label | Filter label assigned to this capcode |
| Message | Decoded text |

Click a column header to sort by that column. Double-click a message row to open the detail view.

---

## 6. File menu

| Item | Description |
|------|-------------|
| Open/Close Logfile | Start or stop writing decoded messages to a `.log` file; also opens write-buffering and ISO timestamp options |
| Open/Close Filter Log | Start or stop writing filter-matched messages to a separate `.flt` file |
| Filters | Open the filter editor (also Ctrl+F) |
| Print | Print the current message list |
| Exit | Close PDW |

Log files are date-stamped (`YYMMDD_*.log`) and rotate automatically at midnight. See [section 10.8](#108-log-files-and-write-buffering) for write buffering and the ISO timestamp option.

---

## 7. Monitor menu

Select which protocol PDW should decode. Only one mode is active at a time.

| Menu item | Description |
|-----------|-------------|
| **POCSAG/FLEX** | Decode POCSAG (512 / 1200 / 2400 baud) and FLEX / ReFLEX (1600 / 3200 / 6400 baud) simultaneously |
| **ACARS** | Aircraft Communications Addressing and Reporting System (2400 baud) |
| **MOBITEX** | Mobile packet data network protocol |
| **ERMES** | European Radio Messaging System |
| **Statistics...** (Alt+S) | Open the statistics window (see [7.1](#71-statistics-window)) |

### 7.1 Statistics window

Open via **Monitor → Statistics** or press **Alt+S**.

Shows hourly and daily message counts per protocol and type (Alpha / Numeric). Statistics are tracked for all nine protocol/rate combinations: FLEX 6400 / 3200 / 1600, POCSAG 2400 / 1200 / 512, ACARS 2400, MOBITEX, and ERMES. Statistics can be saved to a `.st` file.

---

## 8. Interface menu

### 8.1 Sound card setup

See [section 4.1](#41-sound-card-input) for the initial setup. The **Custom** option in this dialog lets you fine-tune threshold, centering, and resync per baud rate. After changes, click **OK** to apply without restarting PDW.

**Auto Invert** — when enabled, PDW automatically detects and corrects inverted FSK polarity. For most setups this works correctly; if decoding is erratic, try setting it manually.

### 8.2 Serial port setup

Select the COM port number matching your hardware slicer. COM port numbers 10 and higher can be entered directly in the field. The port is opened immediately after clicking OK.

---

## 9. Filters

Filters let you assign labels, colours, alerts, and separate log files to specific capcodes, label patterns, or message text patterns. Every incoming message is checked against all active filters in order. There is no hard limit on the number of filters.

Open the filter editor via **File → Filters** or press **Ctrl+F**.

### 9.1 Filter fields

| Field | Description |
|-------|-------------|
| Capcode | Exact capcode number, or a prefix/wildcard match |
| Label | Descriptive name shown in the Label column |
| Colour | Background colour for matched messages |
| Type | ALPHA / NUMERIC / TONE / Any |

### 9.2 Filter actions

Each filter can independently trigger any combination of the following:

| Action | Description |
|--------|-------------|
| **Sound** | Play a WAV file |
| **E-mail** | Send an SMTP alert (uses settings from Options → SMTP) |
| **Command** | Run an external program or script |
| **Log 1 / 2 / 3** | Write matched messages to up to three separate log files |
| **Monitor only** | Show on screen but exclude from main log and feed outputs |
| **Reject** | Suppress this message entirely — do not show or log |

Filter labels can be up to **256 characters** long.

### 9.3 Search while typing

In the filter editor, start typing in the search box to filter the displayed entries in real time. This is useful when you have hundreds of filters.

---

## 10. Options menu

### 10.1 SMTP e-mail alerts

Configure via **Options → SMTP Settings**.

PDW includes a fully self-contained SMTP client (no external library).

| Setting | Description |
|---------|-------------|
| Server | Hostname or IP of your SMTP server |
| Port | 465 for implicit TLS; 587 or 25 for STARTTLS |
| Username / Password | LOGIN or PLAIN authentication |
| From / To | Sender and recipient addresses; multiple recipients separated by semicolons |
| Subject / Body fields | Choose which message fields appear in the subject line and which in the body |

**Encryption is automatic:** port 465 uses implicit TLS (SSL from the first byte); ports 587 and 25 use STARTTLS with a mandatory second EHLO over the encrypted channel. No separate setting is needed.

**Split Subject / Body mode** lets you choose different fields for the subject line and the message body independently. This is useful for push notification services (such as pushover.net) where the subject becomes the notification title and the body becomes the detail text.

SMTP activity is logged to `YYMMDD_mail.log`. Click **Test** in the dialog to send a test message.

### 10.2 Webhook

Configure via **Options → Webhook**.

Sends an HTTP or HTTPS POST to any URL on every decoded message (or on filtered messages only).

| Setting | Description |
|---------|-------------|
| URL | Target endpoint (http:// or https://) |
| JSON format | PDW-native (nested) or Flat / Node-RED format |
| Send filter | All messages / Filtered only / Filtered + Monitor / Raw feed |
| Trust self-signed | Skip certificate verification for HTTPS with self-signed certs |
| Pad capcode | Left-pad capcode to 9 digits with zeros |

Three delivery attempts with exponential backoff (1 s, 2 s, 4 s). Activity logged to `YYMMDD_webhook.log`.

**Flat JSON example:**
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

### 10.3 MQTT

Configure via **Options → MQTT**.

Publishes every decoded page to an MQTT broker. Uses the static-linked Paho C library — no external DLLs required.

| Setting | Description |
|---------|-------------|
| Broker | Hostname or IP |
| Port | Default 1883; use 8883 for TLS |
| Topic | Base topic |
| Client ID | MQTT client identifier |
| Fields bitmask | Choose which fields to publish |
| JSON format | PDW-native or Flat / Node-RED |
| Send filter | All / Filtered / Filtered + Monitor / Raw feed |

**Published fields:**

| Field | Description |
|-------|-------------|
| `message` | Decoded text |
| `address` | Capcode |
| `label` | Matched filter label |
| `time` | HH:mm:ss |
| `date` | DD-MM-YY |
| `timestamp` | Unix epoch (seconds) |
| `mode` | FLEX / POCSAG / ... |
| `type` | ALPHA / NUMERIC / TONE |
| `bitrate` | 1600 / 3200 / 6400 (FLEX) or 512 / 1200 / 2400 (POCSAG) |
| `subscribers` | JSON array of `{capcode, label}` for FLEX group calls |

Click **Test connection** in the dialog to verify broker connectivity. Activity logged to `YYMMDD_mqtt.log`.

### 10.4 Telnet server

Configure via **Options → Telnet Server**.

PDW includes a built-in Telnet server (default port **8024**) that streams every decoded message in the wire-format used by CS FlexDecoder. Clients built for CS FlexDecoder — such as p2kflexMonitor — connect without any reconfiguration.

| Setting | Description |
|---------|-------------|
| Port | Default 8024 |
| Bind address | Default 0.0.0.0 (all interfaces) |
| Max clients | Default 25 |
| Watchdog interval | Seconds between `<WD>` heartbeat messages |
| Backlog window | Seconds of messages replayed to a reconnecting client (default 60 s) |

**Wire-format messages:**

| Message | Meaning |
|---------|---------|
| `CC/FFF -ALPHA- capcode text` | FLEX alpha (CC=cycle, FFF=frame) |
| `-ALPHA- capcode-N text` | POCSAG alpha (N=function 0–3) |
| `<TX_START>` / `<TX_STOP>` | Transmission start / end |
| `<RXQ:NN>` | RX quality 0–100 % |
| `<WD>` | Watchdog heartbeat |
| `<RS232:0>` / `<RS232:1>` | Serial data lost / recovered |
| `<AUDIO:0>` / `<AUDIO:1>` | Audio signal lost / recovered |
| `<BUFFER_START>` / `<BUFFER_STOP>` | Reconnect replay window |

Events are logged to `YYMMDD_telnet_server.log`; the raw wire stream is logged to `YYMMDD_telnet_traffic.log`.

### 10.5 MySQL output

Configure via **Options → MySQL**.

Persists all decoded messages to a MySQL or MariaDB database. No external DLLs or MySQL client libraries are required — the MySQL wire protocol is implemented directly inside PDW.

| Setting | Description |
|---------|-------------|
| Host / Port | Database server address (default port 3306) |
| Database | Database name (auto-created if it does not exist) |
| Username / Password | MySQL credentials |
| Schema | Classic / Extended / Optimized (see below) |
| Fields | Choose which message fields to write |
| Activity log | Enable `pdw_mysql.log` |

**Three schema modes:**

**Classic** — minimal, three columns:
```sql
CREATE TABLE `alarmeringen` (
    `id`        INT(11)     NOT NULL AUTO_INCREMENT,
    `timestamp` TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `capcode`   VARCHAR(10) NOT NULL DEFAULT '',
    `melding`   TEXT        NOT NULL,
    `label`     TEXT        NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**Extended** — all fields as text:
```sql
CREATE TABLE `alarmeringen` (
    `id`        INT(11)     NOT NULL AUTO_INCREMENT,
    `timestamp` TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `address`   VARCHAR(20) NOT NULL DEFAULT '',
    `msg_time`  VARCHAR(10) NOT NULL DEFAULT '',
    `msg_date`  VARCHAR(12) NOT NULL DEFAULT '',
    `mode`      VARCHAR(15) NOT NULL DEFAULT '',
    `msg_type`  VARCHAR(20) NOT NULL DEFAULT '',
    `bitrate`   VARCHAR(10) NOT NULL DEFAULT '',
    `message`   TEXT        NOT NULL,
    `label`     TEXT        NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**Optimized** — typed columns with indexes; recommended for new installations:
```sql
CREATE TABLE `alarmeringen` (
    `id`          BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
    `ontvangen`   DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `capcode`     CHAR(9)           NOT NULL DEFAULT '',
    `mode`        VARCHAR(15)       NOT NULL DEFAULT '',
    `msg_type`    VARCHAR(10)       NOT NULL DEFAULT '',
    `bitrate`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `message`     TEXT              NOT NULL,
    `label`       VARCHAR(256)      NOT NULL DEFAULT '',
    `subscribers` TEXT              NOT NULL DEFAULT '',
    `match_type`  TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    `label_color` VARCHAR(7)        NOT NULL DEFAULT '',
    PRIMARY KEY (`id`),
    INDEX `idx_capcode`   (`capcode`),
    INDEX `idx_ontvangen` (`ontvangen`),
    INDEX `idx_match`     (`match_type`),
    INDEX `idx_label`     (`label`(64)),
    FULLTEXT `ft_message` (`message`, `label`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
```

**Column reference (Optimized schema):**

| Column | Type | Notes |
|--------|------|-------|
| `id` | BIGINT | Monotonic auto-increment. Use for live polling: `WHERE id > :since` |
| `ontvangen` | DATETIME | Message time in local timezone |
| `capcode` | CHAR(9) | Zero-padded pager address. Leading zeros are preserved. FLEX group capcodes are `2029568`–`2029583` |
| `mode` | VARCHAR | Protocol + rate, e.g. `FLEX-1600`, `POCSAG-1200` |
| `msg_type` | VARCHAR | `ALPHA` / `NUMERIC` / `TONE` / `GROUP` |
| `bitrate` | SMALLINT | 512 / 1200 / 2400 (POCSAG) or 1600 / 3200 / 6400 (FLEX) |
| `message` | TEXT | Decoded text. `>>` (byte 0xBB) marks a line break — render as newline |
| `label` | VARCHAR | Filter label; empty when no rule matched |
| `subscribers` | TEXT | JSON array of group members (see below); empty for non-group messages |
| `match_type` | TINYINT | `0` = no match, `1` = filtered, `2` = monitor-only |
| `label_color` | VARCHAR(7) | `#RRGGBB`; empty when none |

**FLEX group calls** store member addresses in `subscribers`:
```json
[
  {"capcode": "1234567", "label": "Ambulance 1", "color": "#1565c0"},
  {"capcode": "1234568", "label": "Ambulance 2"}
]
```

**Useful queries:**
```sql
-- Latest 100 messages
SELECT * FROM alarmeringen ORDER BY id DESC LIMIT 100;

-- Live polling (newer than the last seen id)
SELECT * FROM alarmeringen WHERE id > :since ORDER BY id DESC LIMIT 50;

-- One capcode
SELECT * FROM alarmeringen WHERE capcode = '1234567' ORDER BY id DESC;

-- Matched/filtered messages only
SELECT * FROM alarmeringen WHERE match_type >= 1;

-- Full-text search
SELECT * FROM alarmeringen
WHERE MATCH(message, label) AGAINST ('brandweer' IN BOOLEAN MODE)
ORDER BY id DESC LIMIT 50;
```

Only `mysql_native_password` authentication is supported. On MySQL 8.0+, configure the account with `ALTER USER ... IDENTIFIED WITH mysql_native_password`.

### 10.6 SQLite output

Configure via **Options → SQLite**.

Persists all decoded messages to a local SQLite database file. No server, no installer, no external DLLs — the SQLite engine is compiled directly into PDW.

| Setting | Description |
|---------|-------------|
| File path | Default: `<PDW directory>\pdw.db` |
| Fields | Choose which message fields to write |
| LowWrite mode | Reduce NVMe write amplification by batching commits every ~15 s |
| PurgeDays | Delete rows older than N days (off by default) |
| MaxSizeMB | Delete oldest rows when file exceeds this size (off by default) |
| Activity log | Enable `YYMMDD_pdw_sqlite.log` |

**Schema:**
```sql
CREATE TABLE IF NOT EXISTS "alarmeringen" (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ontvangen   TEXT    NOT NULL DEFAULT '',   -- 'YYYY-MM-DD HH:MM:SS'
    capcode     TEXT    NOT NULL DEFAULT '',   -- leading zeros preserved
    mode        TEXT    NOT NULL DEFAULT '',
    msg_type    TEXT    NOT NULL DEFAULT '',
    bitrate     INTEGER NOT NULL DEFAULT 0,
    message     TEXT    NOT NULL DEFAULT '',
    label       TEXT    NOT NULL DEFAULT '',
    subscribers TEXT    NOT NULL DEFAULT '',
    match_type  INTEGER NOT NULL DEFAULT 0,
    label_color TEXT    NOT NULL DEFAULT ''
);
```

The column names and content are identical to the MySQL Optimized schema; the only differences are that `capcode` is `TEXT` instead of `CHAR(9)` and there is no FULLTEXT index (use `LIKE '%term%'` for text search).

**SQLite PRAGMA settings applied automatically:**

| PRAGMA | Normal mode | LowWrite mode |
|--------|------------|---------------|
| `journal_mode` | WAL | WAL |
| `synchronous` | NORMAL | OFF |
| `auto_vacuum` | INCREMENTAL | INCREMENTAL |
| Commit cadence | Every message | ~15 s batched |

**LowWrite mode** significantly reduces write amplification on SSDs. The trade-off is that up to 15 seconds of messages may be lost on a hard crash or power failure.

**Auto-maintenance** (off by default): PurgeDays and MaxSizeMB both default to disabled. PDW never deletes data without explicit configuration. Maintenance runs once per hour inside the worker thread.

Click **Test** in the dialog to verify the file can be opened. The database can be inspected with any SQLite browser (e.g. DB Browser for SQLite).

### 10.7 RX Quality Alert

Configure via **Options → RX Quality Alert**.

Sends an e-mail alert when the receive quality stays below a threshold for a sustained period.

| Setting | Default | Description |
|---------|---------|-------------|
| Threshold | 80 % | Quality below this level starts the timer |
| Recovery level | 90 % | Quality above this level cancels a pending alert |
| Minimum duration | 15 min | How long below threshold before sending the alert |
| Cooldown | 120 min | Minimum time between repeated alerts |

Uses the same SMTP settings as filter-based alerts. The recipient address can be configured separately from the filter mail recipient.

### 10.8 Log files and write buffering

Configure via **File → Open/Close Logfile**.

All PDW log files use the date-stamped naming convention `YYMMDD_<type>.log` and rotate automatically at midnight.

| Log file | Content |
|----------|---------|
| `YYMMDD_monitor.log` | All decoded messages |
| `YYMMDD_filter.log` | Filter-matched messages |
| `YYMMDD_mail.log` | SMTP send/error log |
| `YYMMDD_mqtt.log` | MQTT publish log |
| `YYMMDD_webhook.log` | Webhook send log |
| `YYMMDD_telnet_server.log` | Telnet connection events |
| `YYMMDD_telnet_traffic.log` | Telnet wire-format stream |
| `YYMMDD_pdw_sqlite.log` | SQLite feed activity |
| `pdw_mysql.log` | MySQL feed activity |

**ISO timestamp format** — enable in the Logfile dialog to write `YYYY-MM-DD HH:MM:SS` timestamps inside monitor and filter log lines (the file names on disk are unchanged). When enabled, the separate Time and Date column checkboxes are grayed out.

**Write buffering (NVMe protection)** — enable via **"Reduce disk writes (buffer)"** in the Logfile dialog:

| Setting | Default | Description |
|---------|---------|-------------|
| Flush interval | 500 ms | How often the buffer is written to disk |
| Buffer slots | 512 | Maximum entries held before an early flush |

Recommended for busy POCSAG/FLEX networks where many messages per second can cause high write amplification on an SSD. The maximum log loss on a hard crash equals the flush interval.

### 10.9 Program options

| Option | Description |
|--------|-------------|
| Auto scroll | Keep the latest message visible |
| Show duplicates | Show repeated messages or suppress them |
| Beep on message | Audible beep for every decoded message |
| Start minimized | Start PDW in the system tray |
| Save position | Restore window position on next start |

---

## 11. Display menu

| Item | Description |
|------|-------------|
| Font | Change the message list font |
| Columns | Show or hide individual columns |
| Colours | Edit the colour scheme |
| Word wrap | Wrap long messages inside the message column |

---

## 12. Protocols decoded

### 12.1 POCSAG

POCSAG (Post Office Code Standardisation Advisory Group) is the most common paging protocol worldwide.

| Rate | Usage |
|------|-------|
| 512 baud | Older systems |
| 1200 baud | Most common |
| 2400 baud | High-speed systems |

PDW decodes all three rates simultaneously when POCSAG/FLEX mode is active. Message types: **Alpha**, **Numeric**, **Tone**.

Typical paging frequencies (Europe):

| Range | Notes |
|-------|-------|
| 136–139 MHz NFM | |
| 153–154 MHz NFM | |
| 454–455 MHz NFM | |
| 466–467 MHz NFM | P2000 / Semafoon (NL/BE) |
| 35–36 MHz NFM | |
| 43–44 MHz NFM | |

### 12.2 FLEX and ReFLEX

FLEX is a high-speed paging protocol developed by Motorola.

| Rate | Notes |
|------|-------|
| 1600 baud | 2-level FSK |
| 3200 baud | 4-level FSK |
| 6400 baud | 4-level FSK |

PDW decodes all three rates simultaneously. Message types: **Alpha**, **Numeric**, **Tone**, **Short-Instruction**, **Frame-Info**, **Group calls**.

**Multi-frame reassembly:** long FLEX alpha messages that span multiple frames are accumulated and displayed as a single complete string.

**Group calls:** a group capcode (range 2029568–2029583) addresses multiple individual pagers simultaneously. PDW displays all subscriber capcodes and their labels together with the group message.

**ReFLEX** is an extended version of FLEX supporting two-way paging. PDW decodes ReFLEX using the same decoder path.

### 12.3 ACARS

ACARS (Aircraft Communications Addressing and Reporting System) is used for data exchange between aircraft and ground stations.

PDW decodes ACARS at **2400 baud** via sound card input.

**ACARS frequencies:**

| Frequency | Region |
|-----------|--------|
| 131.550 MHz | Primary — USA, Canada, Asia/Pacific |
| 131.725 MHz | Primary — Europe |
| 130.025 MHz | Secondary — USA |
| 129.125 MHz | Tertiary — USA |
| 131.475 MHz | Air Canada (private) |
| 131.525 MHz | Tertiary — Europe |
| 131.450 MHz | Primary — Japan |

**ACARS database files:** PDW can look up airline codes, aircraft types, ground station IDs, and route information from optional database files placed in the PDW application directory:

| File | Content |
|------|---------|
| `label.df` | Message label descriptions |
| `aircraft.df` | Aircraft type codes |
| `country.df` | Country codes |
| `airline.df` | Airline codes |
| `ground.df` | Ground station IDs |
| `routes.df` | Flight route information |

These files are optional. If not present, PDW still decodes ACARS messages but without the lookup labels.

### 12.4 MOBITEX

MOBITEX is a mobile packet data network protocol used primarily for paging and data terminals.

**First-time setup — frame sync:**

When decoding a Mobitex network for the first time you need to set the correct frame sync for your local network:

1. Tune to an active Mobitex signal.
2. Set the **Invert data** option in Interface Setup manually (Auto does not work until the frame sync is known).
3. Watch the 4-digit frame sync numbers on the left side of the display. The number that appears most often and occasionally has message data alongside it is the correct one. The common European frame sync is **EB90**.
4. Open **Options → Program** and enable **Check Frame Sync**.
5. Enter your 4-digit frame sync number in the **Frame Sync** field.

**Base station labels (base-ids.txt):** PDW reads `base-ids.txt` from the application directory to display base station names in the title bar. The format is one entry per line:

```
# base-ids.txt
0001=London
001B=Amsterdam
011B=Zoetermeer
```

Lines starting with `#` are comments. The ID is the hexadecimal Base-ID as shown in the PDW display. If the file is not present, the raw numeric Base-ID is shown instead.

### 12.5 ERMES

ERMES (European Radio Messaging System) is a pan-European paging standard operating at 6.25 kHz channel spacing. PDW decodes ERMES tone, numeric, and alphanumeric messages including error detection and correction.

---

## 13. Paging message format

A typical decoded message line:

```
10:24:31  03-06-26  FLEX-1600  ALPHA  1600  0012345  Ambulance  Patient contact req
```

| Part | Example | Meaning |
|------|---------|---------|
| Time | `10:24:31` | Decode time |
| Date | `03-06-26` | DD-MM-YY |
| Mode | `FLEX-1600` | Protocol and bit rate |
| Type | `ALPHA` | Message type |
| Bitrate | `1600` | Bit rate in bps |
| Capcode | `0012345` | Pager address (leading zeros preserved) |
| Label | `Ambulance` | Filter label (empty if no filter matched) |
| Message | `Patient contact req` | Decoded text |

---

## 14. Log file format

Monitor log (`.log`) lines:
```
10:24:31 03-06-26 FLEX-1600 ALPHA 0012345 Patient contact req
```

With ISO timestamp option enabled:
```
2026-06-03 10:24:31 FLEX-1600 ALPHA 0012345 Patient contact req
```

Filter log (`.flt`) lines follow the same format but include the matched label.

---

## 15. Support files

PDW reads several optional support files from the application directory. None are required for basic operation.

| File | Used by | Description |
|------|---------|-------------|
| `filters.ini` | Filter system | Your filter rules. Created and updated by PDW when you save filters. |
| `pdw.ini` | All | Main settings file. Created by PDW on first run. |
| `language.df` | UI | Interface language strings. Place in the PDW directory to override default English text. |
| `base-ids.txt` | MOBITEX | Maps hexadecimal Base-ID codes to readable station names. Format: `ID=Name` per line, `#` for comments. |
| `label.df` | ACARS | ACARS message label descriptions. |
| `aircraft.df` | ACARS | Aircraft type code lookup. |
| `country.df` | ACARS | Country code lookup. |
| `airline.df` | ACARS | Airline code lookup. |
| `ground.df` | ACARS | Ground station ID lookup. |
| `routes.df` | ACARS | Flight route lookup. |

---

## 16. Windows notifications

Press **Ctrl+T** to send a test Windows toast notification. PDW uses the native `IUserNotification` COM interface (Action Center toast), not the obsolete balloon-tip API that Windows 10 and Windows 11 no longer display.

The system tray icon provides:
- Minimize-to-tray by closing the window
- Click the tray icon to restore the window
- Optional per-message toast notifications for filter-matched messages

---

## 17. High-DPI support

PDW declares `System DPI Aware` in its application manifest. Fonts, toolbar, and layout are recalculated from the actual display DPI at startup. PDW displays correctly on 125 %, 150 %, and 200 % scaled (4K / HiDPI) monitors without blurring or clipping.

---

## 18. Multi-instance / title bar

When running two PDW windows simultaneously (for example one decoding audio and one on a serial port), the title bar shows the active **[MODE]** — FLEX or POCSAG — so you can immediately see which window is which.

---

## 19. COM ports >= 10

COM port numbers 10 and higher are fully supported. Enter the port number directly in the Interface Setup dialog. PDW opens the port using the `\\.\COMn` notation required by Windows for high-numbered ports.

---

## 20. Troubleshooting

**No messages are decoded**
- Check that the signal indicator bar (top right) is moving. If not, the audio signal is not reaching PDW. Increase radio volume or check the cable.
- Make sure the correct input source (Line In / Mic) is selected in the Windows recording mixer.
- Check that the correct protocol is selected in the Monitor menu.
- Try all four configurations in Interface Setup (Discriminator, Earphone, Speaker Out, Tape/Rec Out).
- A discriminator tap gives the best signal quality and often resolves persistent issues.

**Poor decode rate / many errors**
- Try the Custom threshold settings (see [section 4.1](#41-sound-card-input)).
- Reduce other audio sources that may interfere.
- A discriminator tap will usually solve persistent quality issues.

**MOBITEX decodes nothing**
- Verify the frame sync number is set correctly for your network (see [section 12.4](#124-mobitex)).
- Make sure Invert data is set correctly — Auto does not work until the frame sync is known.

**ACARS: no airline / aircraft labels**
- The ACARS database files (`label.df`, `airline.df`, etc.) are missing from the PDW directory. PDW still decodes messages but cannot look up labels.

**SMTP test fails**
- Verify server, port, username, and password.
- Port 465 = implicit TLS; port 587 = STARTTLS. Do not mix them.
- Check `YYMMDD_mail.log` in the PDW directory for detailed error messages.

**Telnet clients do not connect**
- Check that the Telnet server is enabled in Options and that the port (default 8024) is not blocked by a firewall.
- Check `YYMMDD_telnet_server.log` for connection and error messages.

**SQLite / MySQL feed not writing**
- Click **Test** in the respective settings dialog to verify connectivity.
- Check the feed's activity log file.
- For MySQL: ensure the account uses `mysql_native_password`.

**RX Quality is always low after PDW restart**
- This is normal: the quality buckets are empty at startup and fill up over the first few minutes. The `<RXQ:NN>` value in the Telnet stream will stabilise once the decoder has seen a full cycle.

**High-DPI layout looks wrong**
- Make sure you are running the release build. Debug builds may not include the DPI-aware manifest.

---

## 21. Changelog

### v3.5.8
- **Central Log Manager** — all log output flows through one consistent path; uniform `YYYY-MM-DD HH:MM:SS.mmm` timestamps; daily rotation for every log type
- **Write buffering** — coalesce log writes into timed batches (500 ms / 512 slots by default) to reduce SSD write amplification on busy networks
- **ISO timestamp option** — `YYYY-MM-DD HH:MM:SS` inside monitor and filter log lines (sortable)

### v3.5.7
- **SQLite output feed** — single local database file; no server, no DLL; LowWrite mode; optional auto-purge

### v3.5.6
- **MySQL output feed** — no external DLLs; three schema modes (Classic / Extended / Optimized)
- **RX Quality Alert** — e-mail notification on sustained signal quality drop
- SMTP crash on rapid Test-button clicks fixed
- Raw feed mode added to MQTT, Webhook, and Telnet
- Date-stamped log files for all output feeds

### v3.4
- **Telnet server** on port 8024 — CS FlexDecoder-compatible wire-format
- `<RXQ:NN>`, `<WD>`, `<RS232:x>`, `<AUDIO:x>` markers
- Reconnect backlog replay window (60 s)
- RX Quality corrected for POCSAG channels

### v3.3
- **High-DPI support** — crisp layout on 4K and HiDPI monitors
- **SMTP split Subject / Body** — independent field selection for subject and body
- SMTP error logging to disk

### v3.2 → v3.3
- **MQTT output** with PDW-native and flat / Node-RED JSON formats
- **Webhook HTTP(S)** output
- **Windows 11 toast notifications** (Action Center)
- FLEX multi-frame message reassembly
- SMTP STARTTLS (port 587) and implicit TLS (port 465)
- x64 build target
- COM ports >= 10 supported
- Filter label length increased to 256 characters

---

## 22. Credits and license

PDW is licensed under the **GNU General Public License v3.0** (GPL-3.0). All additions in this repository are released under the same terms. See `LICENSE` for the full text.

PDW was originally developed by **Jason Petty** (2001–2004) and **Peter Hunt** (2004–2010), who open-sourced it in 2013. This repository builds on the community fork at [github.com/Discriminator/PDW](https://github.com/Discriminator/PDW).

Contributors to the Discriminator fork: Discriminator, andrey2805, evroza, Muspah, lt-holman, senf666.

Further development in this fork by Rob de Hoog.
