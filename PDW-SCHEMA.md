# PDW Alarmeringen - Database Schema

PDW's MySQL feed ("Optimized" schema) writes **one row per message** into a single
flat table. It is built for read-heavy websites: typed columns, one DATETIME,
FULLTEXT search, and group calls kept self-contained in a JSON column.

Host / database / table are configured inside PDW (this project assumes table
`alarmeringen`).

## CREATE TABLE

```sql
CREATE TABLE IF NOT EXISTS `alarmeringen` (
  `id`          BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
  `ontvangen`   DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `capcode`     INT UNSIGNED      NOT NULL DEFAULT 0,
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
  FULLTEXT `ft_message` (`message`,`label`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC;
```

## Columns

| Column | Type | Notes |
|--------|------|-------|
| `id` | BIGINT | Monotonic auto-increment. Use for paging / live polling (`WHERE id > :since`). |
| `ontvangen` | DATETIME | Message time, `YYYY-MM-DD HH:MM:SS`, in the PDW machine's local timezone (no offset stored). |
| `capcode` | INT | Pager address. FLEX group capcodes are `2029568`-`2029583`. |
| `mode` | VARCHAR | Protocol + rate, e.g. `FLEX-1600`, `POCSAG-1200`, `ACARS-2400`. Protocol = `mode` up to the `-`. |
| `msg_type` | VARCHAR | `ALPHA` / `NUMERIC` / `TONE` / `GROUP` / `TRANSP`. |
| `bitrate` | SMALLINT | 512 / 1200 / 2400 (POCSAG/ACARS) or 1600 / 3200 / 6400 (FLEX). |
| `message` | TEXT | Up to ~5120 bytes. `>>` (byte `0xBB`) marks a line break - render as a newline. |
| `label` | VARCHAR | Filter label assigned to this capcode; empty when no rule matched. |
| `subscribers` | TEXT | JSON array of group members (see below); empty for non-group messages. |
| `match_type` | TINYINT | `0` no match - `1` filtered - `2` monitor-only. |
| `label_color` | VARCHAR(7) | `#RRGGBB` of the label; empty when none. |

**Field selection:** `mode`, `msg_type`, `bitrate`, `message` and `label` are only
written when enabled in PDW's MySQL field settings. Enable all of them for a
website. `ontvangen`, `capcode` and `match_type` are always written;
`subscribers` and `label_color` are written only when non-empty.

## Group calls (`subscribers`)

Filled for FLEX group calls. JSON array of the individual addresses that were paged:

```json
[
  {"capcode": 1234567, "label": "Ambulance 1", "color": "#1565c0"},
  {"capcode": 1234568, "label": "Ambulance 2"}
]
```

- `capcode` int, `label` string (may be empty), `color` optional `#RRGGBB`.
- `color` was added recently - **older rows have no `color`**; fall back to a neutral chip.

Detect a group call: `capcode BETWEEN 2029568 AND 2029583` **or** `subscribers <> ''`.

## Common queries

```sql
-- Latest 100, newest first
SELECT * FROM alarmeringen ORDER BY id DESC LIMIT 100;

-- Live polling (only newer than the last seen id)
SELECT * FROM alarmeringen WHERE id > :since ORDER BY id DESC LIMIT 50;

-- One capcode (own messages)
SELECT * FROM alarmeringen WHERE capcode = :cc ORDER BY id DESC;
-- ...also as a group member (simple, indexless):
--   WHERE capcode = :cc OR subscribers LIKE CONCAT('%"capcode":', :cc, '%')

-- Only matched messages
SELECT * FROM alarmeringen WHERE match_type >= 1;

-- Full-text search in message + label
SELECT * FROM alarmeringen
WHERE MATCH(message,label) AGAINST (:q IN BOOLEAN MODE)
ORDER BY id DESC LIMIT 50;
```

## PHP (PDO)

```php
$pdo = new PDO('mysql:host=HOST;port=3306;dbname=DB;charset=utf8mb4',
               'USER', 'PASS', [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

$rows = $pdo->query('SELECT * FROM alarmeringen ORDER BY id DESC LIMIT 100')
            ->fetchAll(PDO::FETCH_ASSOC);

foreach ($rows as $r) {
    $subs    = $r['subscribers'] !== '' ? (json_decode($r['subscribers'], true) ?: []) : [];
    $isGroup = $subs !== [];
    // render $r['message'] (replace 0xBB with newline), $r['label']/$r['label_color'],
    // and for groups each $subs[i] capcode/label/color
}
```

## Possible future columns

Not written yet - candidates for richer dashboards (PDW already has the data):

| Column | Purpose | Effort |
|--------|---------|--------|
| `source` / `station` | Receiver/instance name for multi-receiver aggregation. | Low (config string + 1 column) |
| `rx_quality` TINYINT | Per-message signal quality 0-100 (`dRX_Quality` is already computed) for coverage dashboards. | Medium |
| subscriber `color` | Per-member label color in `subscribers` JSON. | **Done** (rebuild/deploy needed) |
