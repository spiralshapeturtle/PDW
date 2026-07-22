# PDW 4.0.4 - Release Notes

PDW 4.0.4 is a combined stability and hardening release: a full source-code audit (memory safety,
buffer handling, and rare corner-case defects across the decoders, the output feeds, the input
paths, and the screen/GUI code), the new high-resolution toolbar, a title-bar/corner display-glitch
fix, a window-position safety fix, and one small new option (a menu-bar toggle). Most fixes target
unusual conditions - corrupt or truncated over-the-air frames, feeds under stress, reconnect timing,
and misconfigured or corrupt `pdw.ini` / `filters.ini` files - so day-to-day behaviour is unchanged.
No decoder output or configuration format changes; existing `pdw.ini` and `filters.ini` files work
unchanged.

## Health trend: 4- and 8-hour windows + worst-dip band (FIX [HealthSparkLong])
The Health panel's trend sparkline can now look back over a full night: the window menu adds
"4 hours" and "8 hours" alongside the existing 1/5/15/60-minute options (the trend history buffer
was grown from 1 hour to 8 hours to match). Over such a long window each pixel spans several minutes,
and a plain per-pixel average would smooth short reception drops away entirely - exactly the events
you want to catch when reviewing the past night. The sparkline now draws the average as the trend
line (as before) and adds a bold, colour-coded band that hangs down from the line to the *worst*
value in each pixel's slice, so a brief RX collapse shows up as a red spike stabbing downward even
while the averaged line stays high. The hover tooltip reports the window's lowest value and when it
happened, e.g. "lowest 40% at 03:15", so you can pinpoint an overnight problem without reading the
graph pixel by pixel. Purely a display/history change; no decoder, feed or configuration format is
affected, and short windows look the same as before (the band only appears where there is a real dip).

## Antivirus false-positive: version-resource metadata (FIX [AvMetadata])
Some antivirus products (notably Windows Defender, as `Trojan:Win32/Wacatac.C!ml`) flagged the
unsigned binary on a heuristic/reputation basis - not on any actual behaviour. A source audit found
no networking, obfuscation, reconnaissance or persistence code behind the detection. Two things in
the executable's version resource were needlessly feeding the heuristic and have been corrected: the
`CompanyName` no longer contains a substring that reputation systems associate with piracy, and the
`OriginalFilename` now matches the shipped executable name (a mismatch there mimics self-renaming
malware). A `LegalCopyright` line and more descriptive product strings were added. No behaviour, no
decoder output and no configuration changed. The most effective remaining step is Authenticode code
signing of the released binaries (see the build workflow).

## Telnet Penalty-system RX score over-strict on POCSAG (FIX [RxqSyncThreshold])
The Health panel's "Penalty system" score (and the telnet wire-format RXQ it shares) could degrade
faster on POCSAG than intended, even while messages decoded and displayed perfectly. PDW's POCSAG
sync/idle-word detector is deliberately more tolerant of noise than the reference algorithm this
score was ported from (a long-standing, unrelated PDW characteristic, unchanged here); a borderline
sync/idle match was still scored as a full error by the RX-quality tracker, something the reference
algorithm never does because it would have rejected that same match outright. The score now skips
that credit on a borderline match, exactly like the reference algorithm does, while decoding itself -
what you see on screen - is completely unaffected. Only the "Penalty system" health source is
affected; the classic RX-Q needle score never looked at this and needs no change. FLEX was audited
alongside this and found to already match its reference algorithm.

## Unreadable POCSAG capcode now shown in red (FIX [PocsagCapcodeColor])
When a POCSAG address word can't be corrected, PDW already replaced the capcode with `???????`
instead of showing a wrong-but-plausible number (FIX [PocsagCapcodeGuard]). That placeholder is now
colored red, the same color already used for corrupted characters in message text, making it
immediately clear at a glance that a capcode is unreliable rather than just unusual-looking. Purely
a screen color change - the underlying value, filter matching, logging and output feeds are all
unaffected.

## Stray RX-Q "100%" under the combined-layout needle (FIX [HealthComboRxqLeak], [HealthFitSizeInvalidate])
With the Health panel's third layout - "Show RX needle alongside", which keeps the classic RX needle
in its old top-right slot next to the panel - the old green RX-Q "100%" box could flash under the
needle right after maximizing the window and then quickly disappear. The panel decides whether it
fits the available toolbar width and caches that verdict; `HealthPanel_Active()` returns the cached
value, and the cache was only refreshed when the panel itself repainted. Within one title-bar repaint
the classic pieces (the PANE1 header width and the RX-Q "100%"-box suppression) read that cache
*before* the panel's own paint refreshed it, so a "did not fit" -> "fits" transition on a resize
(e.g. maximizing from a narrower window where the combined layout did not fit) made them draw one
frame using the stale "did not fit" verdict - painting the classic RX-Q box under the needle until
the next full repaint erased it. The fit is now re-evaluated once up front on every title-bar repaint
(so all readers in that pass agree) and the cache is invalidated on any window-size change (so a
pre-resize verdict can never leak into the new layout). Only the combined "panel + needle" layout was
affected; the classic corner, the panel-only layout, decoding and the on-screen RX-quality all behave
exactly as before.

## Default message font (FIX [DefaultFontConsolas])
The built-in default message-display font changed from Courier New (8pt) to **Consolas 12pt**. This
only affects a fresh install or a `pdw.ini` with no `Font.*` keys yet - anyone with an existing
`pdw.ini` keeps whatever font they already have configured. The shipped release `pdw.ini`
(`release-template/pdw.ini`) was updated to match (`Font.FaceName=Consolas`, `Font.Height=-16`).

## Toolbar Health panel (FIX [HealthPanel]/[HealthSource]/[FeedStatus]/[FeedStatusConn]/[HealthPanelCorner]/[HealthSparkColor]/[HealthDotGreenStart]/[HealthPanelTips2]/[FeedDotLatch]/[FeedLastError]/[FeedTransitionLog]/[HealthSparkThreshold]/[HealthDotShapes]/[HealthNeedleCombo])
A new, compact status strip on the right side of the toolbar band. It is a true switchover with the
classic corner: while the panel is shown it **replaces** the needle meter and the RX-Q percentage box
(so nothing is displayed twice); hide the panel (right-click menu) and the classic needle + RX-Q corner
come back exactly as before. On windows too narrow for the panel the classic corner is shown as well.
- **Health score with colour coding.** Shows the active RX-health score (0-100%) in green/orange/red.
  Two selectable sources: the classic RX-Q score (default, backward compatible) or the stricter
  "Penalty system" score ("instant drop, slow recovery") that PDW already computed internally for the
  telnet wire format - it works regardless of the telnet server being enabled.
  Right-click the panel to switch; the choice is saved to `pdw.ini` (`[HealthPanel] Source`).
- **The low-quality mail alert follows the selected source.** The existing RX-quality e-mail alert
  now reads the same score the panel shows, so switching the source also switches what the alert
  reacts to. Thresholds, hysteresis and cooldown behave exactly as before; the alert mail now also
  names the active source.
- **Trend sparkline.** A small graph of the health score over a configurable window (1/5/15/60
  minutes, right-click menu; `[HealthPanel] SparkMinutes`). The line is drawn in status colours
  (FIX [HealthSparkColor]): a healthy period is a solid green line, degradation shows as orange, and
  below the mail-alert threshold the line turns red.
- **COM link dot.** When serial input is enabled: green = port open and receiving, orange = open but
  no data (stall/warmup), red = port not open.
- **Output-feed dots.** One dot + short tag per *enabled* output feed (SMTP `SM`, webhook `WH`,
  Telegram `TG`, Pushover `PO`, MQTT `MQ`, MySQL `MY`, SQLite `SQ`, telnet server `TS`): green = no
  known problem (FIX [HealthDotGreenStart]: this includes "enabled but nothing delivered yet" - the
  per-message feeds can legitimately go days before their first push), orange = retrying, red = last
  delivery/connection failed. Disabled feeds are hidden so the strip stays compact. For the
  connection-oriented feeds the dot also tracks the *connection itself* (FIX [FeedStatusConn]): MQTT
  turns green as soon as the warm broker connection is established (and red while the broker is
  unreachable), and MySQL turns green on a successful (re)connect instead of waiting for the next
  INSERT - so the panel shows a sensible state right after startup.
- **Hover tooltips (FIX [HealthPanelTips2]).** Every panel entry explains itself on mouse-over: the
  score shows its active source, the sparkline its window and alert level, the COM dot the link
  state, and each feed dot its full status (see the "last problem" item below). An earlier attempt
  ([HealthPanelTipsDisabled]) attached a second, independent tooltip control to the toolbar with its
  own message-subclassing hook - a fragile combination that was pulled after a crash report. The new
  implementation registers the hover regions with the toolbar's *own* tooltip control instead (the
  one that already serves the button tooltips), so no extra window and no second subclassing hook
  are involved.
- **Tooltips no longer clip off the right edge (FIX [HealthPanelTipPos]/[HealthPanelTipPos2]).** The
  panel hugs the right window edge, so the rightmost feed tooltips are drawn to the *left* of the
  cursor. They were still running off the screen because the bubble was measured before the tooltip
  control had laid out the new text (so an old, too-small width was used). The tooltip is now shown
  first, then measured, then positioned, with a hard clamp to the monitor work area - so every tip
  stays fully on-screen regardless of how wide it is or how close to the edge the dot sits.
- **Steady feed dots - no more green/orange/red strobing (FIX [FeedDotLatch]).** The feed dot now
  shows the last *outcome*, not the instantaneous state of the current attempt. Previously a
  continuously failing feed cycled on every message: a new send briefly flashed the dot green
  ("sending"), then orange per retry, then red again. Two rules stop that: an in-flight send never
  changes the dot (activity is not an outcome), and a retry never downgrades a red dot back to
  orange - once red, only a real successful delivery/connection turns the dot green again.
- **Feed dots remember what went wrong (FIX [FeedLastError]).** Each feed records a short "last
  problem" description (e.g. `API rejected message (HTTP 401)`, `broker unreachable`, `queue full -
  message dropped`) plus the time of the last status change. The tooltip of an orange/red dot shows
  it: `Telegram: FAILED since 14:02 - API rejected message (HTTP 401)`. Cleared automatically on the
  next successful delivery.
- **Health log: status transitions on disk (FIX [FeedTransitionLog]).** Every feed status *change*
  (and every COM link state change) is written to a new daily log file `{date}_health.log` via the
  central log manager, e.g. `TG (Telegram): OK -> FAILED - API rejected message (HTTP 401)`. A dot
  that was red during the night is now diagnosable the next morning even though the panel itself is
  long green again. Steady states write nothing, so the file stays tiny.
- **Health log: no more routine "idle -> OK" noise (FIX [HealthLogNoIdleOk]).** A feed's first
  successful contact after startup (or after MQTT's proactive idle-reconnect) is not a failure and
  used to log anyway, cluttering the file with harmless entries. That specific transition is now
  skipped; every transition that involves an actual problem - going into RETRYING/FAILED, and the
  later recovery back to OK - still logs exactly as before.
- **Alert-threshold marker in the sparkline (FIX [HealthSparkThreshold]).** A thin dotted line marks
  the mail-alert level inside the trend graph, so you can see how much headroom the score has before
  the alert would fire, instead of only the colour flip.
- **Feed/COM dot size tuned (FIX [HealthDotSize]).** Went 7px -> 10px (too small on hi-DPI/RDP) ->
  8px, which reads as the sweet spot between the two. The layout recalculates itself around the new
  size, and the retry ring / error "minus" bar scale with it automatically.
- **Re-tuned status colours (FIX [HealthStatusTriad], supersedes [HealthGreenLighter]).** The green,
  orange and red now form one coherent triad tuned for a status strip that sits permanently on the
  light-gray toolbar band. The green is darker and more saturated than the earlier light green, which
  read too faint on the "100%" score text; the red is a strong, deep red so a fault reads instantly;
  orange stays clearly distinct from both. The message-text colour palette (used for on-screen paging
  text) is untouched - these are panel-local colours only.
- **Colour-blind-friendly dot shapes (FIX [HealthDotShapes]/[HealthDotNoBar]).** Status is not
  colour-only: a retrying feed draws as a *ring* (hollow), while healthy and failed are both solid
  discs (green and red). The COM dot uses the same coding (ring = open but no data, solid red = not
  open). The earlier white "minus" bar on the failed dot was dropped: a red disc with a horizontal
  white bar reads as the universal "no-entry / disabled / off" glyph, so an errored feed looked
  switched-off rather than faulty. Colour + shape (ring vs disc) + the hover tooltip carry the state.
- **Smooth, round status dots (FIX [HealthDotAA]).** The small dots were drawn with the plain GDI
  circle primitive, which does no antialiasing - at ~8px the outline came out angular ("cog"-like).
  Each dot is now rendered at 4x and smoothly downscaled (the same technique the signal meter and
  toolbar icons already use), so the dots read as clean, round discs at any DPI.
- **Dot size and alignment polish (FIX [HealthDotSize]/[HealthDotVAlign]).** The dots are one pixel
  larger and nudged down slightly so they sit optically centred against their tag text instead of
  riding high.
- The panel adapts to the free space (labels drop first, then the sparkline) and hides itself on very
  narrow windows. Right-click the panel area to hide it; right-click the same spot to show it again
  (`[HealthPanel] Visible`). Fully DPI-aware, drawn with the same GDI style as the rest of the toolbar.
- **Optional "panel + needle" layout (FIX [HealthNeedleCombo]).** A third right-click option, *Show RX
  needle alongside*, brings the classic RX signal-strength needle back into its old far-right corner
  slot while keeping the Health panel. The panel shrinks just enough to make room, so the RX-health
  percentage stays where it is and the needle sits to its right. The old RX-Q percentage box and
  warning square stay hidden (the panel's own score replaces them). Saved to `pdw.ini`
  (`[HealthPanel] ShowNeedle`, default off); turning it on while the panel is hidden also shows the
  panel.
- **Dead-receiver detection for the Penalty-system source (FIX [HealthRxqStale]).** The penalty score
  is only recomputed when frames are actually decoded, so on dead air it froze at its last (typically
  healthy) value: a receiver that died after a healthy period kept showing a green ~100% and the
  low-quality mail alert could never fire for this source. The Health panel and the alert now treat a
  penalty score that has not been recomputed for 2 minutes as dead air and read 0%. The telnet wire
  score itself is untouched (it keeps exact p2kflex parity and still freezes, like the reference
  implementation). Note: on networks with legitimate multi-minute transmission gaps this source dips
  to 0% between transmissions - the classic needle source is the better pick for such channels.
- **COM dot no longer green on a dead-but-reopenable link (FIX [Rs232LinkRealData]).** The serial
  link dot was derived from the reconnect watchdog's own clocks, and every successful automatic
  reopen reset the "just opened" grace period - so a link that reopens fine but delivers nothing
  (e.g. a Moxa NPort whose TCP tunnel is down: the virtual port opens, reads return 0 bytes) showed
  a green "receiving" dot indefinitely. The dot is now judged by actually received data: the warmup
  grace applies only after a user-initiated open, and the dot turns orange as soon as no data has
  arrived for more than 5 seconds.
- **Toolbar corner can no longer go blank under GDI pressure (FIX [HealthPanelFitsFail]).** If the
  panel's off-screen back-buffer could not be created (GDI-handle exhaustion, display churn) the
  panel drew nothing while still claiming the corner, leaving neither the panel nor the classic
  needle/RX-Q box visible until resources recovered. The claim is now withdrawn on failure so the
  classic corner renders again on the next repaint.
- **Overall-status accent bar (FIX [HealthRollupBar]).** A full-height colour strip on the panel's
  left edge is the "one light to glance at": it shows the worst of everything - the RX health, the
  COM input link and every enabled feed. Green when the whole chain is healthy, orange/red the moment
  anything degrades or fails, so you no longer have to scan all the individual dots. Being part of the
  panel edge rather than an inline dot next to the number, a red bar reads as "the strip has a problem"
  and is never mistaken for the RX score itself being red. Its tooltip summarises the active
  faults/warnings.
- **"RX" caption on the score (FIX [HealthScoreLabel]).** A small `RX` label precedes the percentage
  so the number is unmistakably the RX-health score and not confused with the RX-Q box it replaces.
- **Click an entry to open its settings (FIX [HealthClickConfig]).** Left-click a feed dot to open
  that feed's configuration dialog, the COM dot to open Interface setup, or the score to open the
  System Alerts dialog. The tooltips mention the click target. Right-click still opens the panel's
  own menu (source / trend window / hide).
- **Soft area fill under the trend line (FIX [HealthSparkFill]).** The sparkline now has a light tint
  under the curve in the current status colour, making the trend direction easier to read at a glance;
  the bold status line and the dotted alert-level marker stay crisp on top.

## COM link-lost e-mail alert + "System Alerts" dialog (FIX [ComLinkAlert])
A new, optional e-mail alert for the serial input, complementing the RX-quality alert. The RX-quality
alert watches *decode quality*; on a total loss of serial data (a COM/USB adapter unplugged, or a Moxa
NPort whose network tunnel drops) the classic RX-needle score simply freezes at its last value and the
quality alert never fires. This alert instead watches the physical link state directly and e-mails when
the serial input is enabled but no data has been received for a configurable number of minutes
(default 3). It recovers automatically when data resumes, and re-mails at most once per cooldown period
(default 120 min) while the link stays down.
- **The old "RX Quality Alert" menu/dialog is now "System Alerts"** and holds *both* alerts (RX quality
  and COM link-lost) in two clearly separated groups that share one recipient list. Enable the COM
  alert with the **Enable COM link-lost alert** checkbox and set its "no serial data for" and cooldown
  minutes - no `pdw.ini` editing required.
- Opt-in, off by default. Reuses the RX-quality alert's recipient list and SMTP host - no separate
  mail configuration. Settings persist in `pdw.ini` under `[ComLinkAlert]` (`Enabled`, `Minutes`,
  `Cooldown`). Only active when serial (COM) input is enabled; ignored for sound-card input.
- Note: a Moxa NPort keeps the virtual COM port *open* when its network side drops, so PDW sees "open
  but no data" (the COM dot goes orange, not red) - this alert treats both "open but no data" and "port
  not open" as the link being down.

## Output-feed & logging robustness audit
A focused concurrency/lifetime scrub of the multi-threaded output-feed and logging machinery
(producer/consumer ring buffers, worker-thread lifecycles, shutdown/reconfigure paths). Findings and
fixes:
- **Telnet wire-log no longer drops lines (FIX [WireLogMultiLine]).** When a single locked telnet
  operation emitted more than one wire-line (e.g. a `<TX_START>` immediately followed by the message
  line, or `<TX_STOP>`/`<RS232>`/`<AUDIO>` markers together in one watchdog tick), only the last line
  reached `telnet_traffic.log`. The per-thread staging buffer now accumulates the whole batch and
  flushes it intact. The live telnet stream to clients was never affected - this was disk-log fidelity
  only.
- **Webhook URL is no longer written to the log (FIX [WebhookUrlSecret]).** Webhook URLs commonly embed
  an auth token in the path/query; the log now records only the outcome (`SENT`/`ERROR` + HTTP status),
  never the URL.
- **Webhook group JSON stays valid on very large group calls (FIX [WebhookSubRollback]).** A group with
  enough subscribers to exceed the subscriber-list buffer could POST truncated (invalid) JSON. Each
  subscriber entry is now written all-or-nothing, so the array always closes cleanly.
- **Mail worker start is failure-safe (FIX [SmtpThreadStart]).** If the mail worker thread or its event
  could not be created, the feed now reverts to a clean stopped state instead of leaking the event and
  queuing mail to a worker that never runs.
- **SMTP test-dialog response handle cleared on close (FIX [SmtpRespWnd]).** Cancelling the SMTP setup
  dialog while a test mail was still sending could leave the worker writing to the closed dialog's list
  box; the handle is now cleared on close.
- **Additional hardening:** sqlite maintenance now aborts promptly on shutdown so exit/reconfigure
  can't stall (FIX [SqliteMaintCancel]); mqtt/webhook event-handle teardown and signalling are fully
  serialized under the feed lock (FIX [MqttEventRace]/[WebhookEventRace]); mysql/smtp snapshot the
  socket before `shutdown()` to avoid a descriptor-reuse race (FIX [MysqlShutdownFd]/[SmtpShutdownFd]);
  the mqtt/smtp run-flags use `std::atomic` like the other feeds (FIX [MqttAtomicRun]/[SmtpAtomicRun]);
  and a debug-only credential trace was removed from the base64 encoder (FIX [SmtpCredTrace]).

### Pre-release scrub (second pass)
A second adversarial pass over the same feed/logging layer, with every finding verified against the
code before fixing. None of these change day-to-day behaviour; they matter when an endpoint is down,
a queue is full, or PDW is closing.
- **SMTP: no more crash on exit while a mail was in flight (FIX [SmtpShutdownNullCfg]).** Closing PDW
  cleared the SMTP configuration pointers before the mail worker had stopped; a worker mid-send could
  then read a NULL server/username/password and crash during shutdown. The worker is now stopped and
  joined first, with defensive NULL guards as backup.
- **Exit/reconfigure no longer freezes on a dead endpoint (FIX [FlushBounded]).** The MQTT, webhook,
  Telegram and Pushover workers flush their remaining queue on shutdown, and each queued message got
  a full network attempt (10-20 s of timeouts) even when the endpoint was unreachable - a full queue
  could freeze the GUI for 10+ minutes on exit or a settings change. The flush now stops at the first
  transport failure and logs how many messages were dropped (`LOST`).
- **SQLite: database maintenance can no longer stall exit for minutes (FIX [SqliteMaintInterrupt]).**
  The hourly age purge ran as one unbounded DELETE (minutes on a multi-GB backlog) with no cancel
  point, and vacuum walked the whole freelist in one call. Maintenance is now batched and
  interruptible; a stop/reconfigure aborts it almost immediately, with the database left consistent.
- **SQLite: a failed COMMIT can no longer silently wedge the feed (FIX [SqliteCommitCheck]).** If a
  COMMIT failed with the transaction still open (e.g. a concurrent reader holding the database busy),
  the feed kept "inserting" into an orphan transaction that was rolled back at close - total silent
  data loss while reporting OK. The result is now checked, rolled back on failure, and the
  transaction state is resynced from the engine.
- **MySQL: the shutdown flush now actually delivers (FIX [MysqlStopDrain]).** Stop/reconfigure shut
  the socket down before the worker's final flush ran, so up to 63 queued rows were discarded
  unlogged on every stop. The worker now gets a 3 s grace window to flush over the live connection;
  anything still lost is logged.
- **MySQL: stop no longer waits out a connect in progress (FIX [MysqlConnAbort]).** A stop landing
  while the worker was mid-connect against a slow or dead server waited for the full
  DNS/connect/handshake timeouts (15-65 s of frozen GUI). The in-progress connect is now interrupted
  immediately, and the connect sequence checks the stop flag between phases.
- **SMTP: full-queue corner could corrupt the send queue (FIX [SmtpRingPublish]).** The lock-free
  mail queue published its indices in two steps; with the ring nearly full (hung mail server), a
  thread switch between the two steps could make the whole backlog unsendable or resend stale slots.
  Indices are now computed locally and published in a single store.
- **SMTP: recipient captured per queued mail (FIX [SmtpToSnapshot]).** The worker read the To-address
  from the live settings at send time; changing it in the settings dialog while a mail was in flight
  could tear the recipient string. The recipient is now captured when the mail is queued.
- **Telnet: reconnect replay of a completely full backlog delivered zero lines (FIX
  [TelnetReplayCount]).** A client reconnecting after missing more than a full backlog window got an
  empty `<BUFFER_START><BUFFER_STOP>` instead of the newest lines - exactly when it had missed the
  most. The replay now counts entries instead of comparing ring positions.
- **Telnet: event-log disk writes moved off the server lock (FIX [TsEventLogStaged]).** The same
  treatment the wire-log got earlier ([TsWireLogLockFree]): connect/disconnect/state event lines were
  still written to disk while the telnet lock was held, briefly stalling the decoder on a slow disk.
  They are now staged per-thread and written right after the lock is released.
- **Telnet: listen-socket close race closed (FIX [TelnetListenSockLock]).** Disable/shutdown closed
  the listening socket while the worker could still pick up the (recyclable) handle value; the handle
  is now detached under the server lock on both sides.
- **Telegram: supergroup migration no longer rewrites settings from the worker thread (FIX
  [TgProfileGuiSync]).** A group upgrading to a supergroup made the worker rewrite the stored chat-id
  list concurrently with the GUI - a torn chat list could be saved to `pdw.ini`. The update is now
  applied on the GUI thread.
- **Telegram: a rate limit on the plain-text fallback is retried (FIX [TgFallback429]).** A message
  whose HTML was rejected and whose plain-text resend hit HTTP 429 was dropped even though retry
  attempts remained; it now re-enters the retry loop.
- **Log settings: applying them no longer discards buffered log lines (FIX [LogReconfigureFlush]).**
  With write-buffering enabled, pressing OK in the log settings threw away any lines still in the
  buffer; they are now flushed to disk first. Event signalling and buffer publication in the log
  manager are also fully serialized (FIX [LogEventRace]).
- **Dropped messages are now always visible (FIX [QueueDropVisible], [ShutdownLost], [MysqlDropLog],
  [SqliteDrainLog]).** A queue-full drop now always turns the feed status red (previously invisible
  with per-feed logging off) and its log line is written outside the feed lock; every shutdown-time
  drop path - all feeds, including SMTP's new "N queued messages unsent at shutdown" line - now
  leaves a `LOST`/`DROP` log line.
- Small polish: the SMTP monitor-window handle is read once per response line (FIX
  [SmtpRespSnapshot]) and a few remaining em-dashes in runtime strings were replaced with plain ASCII
  (FIX [AsciiRuntime]).

## FLEX/POCSAG decode-path audit
A full adversarial pass over the primary decode path (FLEX and POCSAG vector/address parsing, BCH
error correction, and the shared display buffers) against corrupt/truncated over-the-air frames. The
path was already memory-safe; the fixes below correct one display defect and harden three
never-yet-triggered corner cases.
- **FLEX long-address capcode no longer wraps (FIX [FlexLongAddrOverflow]).** With **FLEX Group Mode
  turned off**, a long-address FLEX message whose capcode fell outside the 9-digit range could
  overflow a 32-bit intermediate and display a wrong (but plausible) capcode. The value is now
  computed in 64-bit and any capcode outside the valid 9-digit range is shown as `?????????` instead
  of a wrong number. (With Group Mode on - the default - long addresses are skipped, so this path was
  never reached in a default configuration.)
- **FLEX RX-quality reading corrected (FIX [FlexBiterrorDoubleCount]).** Each FLEX message word fed
  its bit-error count into the on-screen RX-Quality meter twice, making the FLEX reading slightly more
  pessimistic than POCSAG. Now counted once, matching POCSAG. The telnet RX-quality feed is a separate
  path and was never affected.
- **Defensive decoder bounds (FIX [PocsagNumBound], [FlexNumWordClamp], [EccPosGuard]).** Three array
  writes/reads that were safe only by an implicit invariant now carry an explicit bound (POCSAG numeric
  buffer, FLEX numeric word span, BCH error-position table index), so a future change to the
  surrounding logic cannot silently turn them into an out-of-bounds access.

## Show/hide the menu bar (FIX [MenuBarToggle])
New **Display > Show Menu Bar** checkbox hides the whole menu bar (File, Edit, Interface, ...) while
keeping the toolbar visible; the message panes reflow to use the freed space immediately, with no
leftover gap. Shortcut **Ctrl+Shift+M** toggles it either way (it keeps working even while the menu
bar itself is hidden, so it always gets you back). With the menu bar hidden, right-clicking anywhere
in the main window (including the toolbar) also offers a **Show Menu Bar** entry in the context
menu. The state is saved to `pdw.ini` as `ShowMenuBar` (default `1`, i.e. visible) and restored on
the next start.

## New high-resolution toolbar + themed dialogs (FIX [ToolbarHiResIcons])
The toolbar now uses 72x72 32-bit icons with a real alpha channel, box-filtered down to the current
DPI, and the application manifest opts in to Common Controls v6 - so every dialog also gets the modern
themed look instead of the classic Windows 2000 style.

## Toolbar icons squared up to one grid (FIX [ToolbarIconGrid])
Follow-up polish on the icon set so every button carries the same optical weight. All 13 glyphs are
now drawn to a single 54 px content box centred on the canvas: the folder and the two copy-pane
icons were enlarged to fill their box, the statistics bars were widened and raised to sit on-centre,
the pause bars were thickened so the two-bar shape no longer looks smaller than its neighbours, and
the help "?" was given a slightly larger ring with clear whitespace around the glyph (the round shape
gets a 1 px optical bump so it does not read small next to the square icons). Corner radii were
unified. The spacing between buttons is a single fixed 8 px grid gutter instead of a per-button
margin. Purely cosmetic - no button, tooltip or behaviour changed.

## Window never opens off-screen (FIX [WindowPosMinimized])
If PDW was closed while minimized, an older build could save an off-screen window position
(-32000,-32000) and start up invisible (only the tray icon reachable). The saved position is now
sanity-checked against the connected monitors, and a corrupt or zero window size falls back to the
default. PDW always opens on a visible part of the desktop.

## Title bar re-syncs after a Remote Desktop / theme change (FIX [ToolbarResync])
Long-standing display glitch: a full-width black band under the toolbar plus a broken divider line
by the signal meter, appearing after the machine had been left running - often seen over a Remote
Desktop session (e.g. a Retina client). Root cause: when Windows changes the theme or display mode
(which a Remote Desktop connect/disconnect does), the toolbar quietly resizes to a different height,
but PDW only recomputed its toolbar height on a window resize. The header, divider, RX-quality box
and signal meter are all positioned from that height, so they ended up several pixels below where
the toolbar now ended - a black gap that only a manual resize cleared. PDW now detects the
toolbar-height change on its own and re-aligns the whole layout automatically within about a second,
so the glitch corrects itself without any manual resize.

## Faster title-bar re-sync after Remote Desktop / theme change (FIX [ToolbarResyncProactive])
The [ToolbarResync] heal above ran on a once-per-second check, so the corner could stay visibly wrong
for up to a second after a Remote Desktop connect/disconnect. PDW now also reacts directly to the
system events that cause the resize (theme, display-mode, DPI, system-setting and colour changes):
a short, self-cancelling timer re-aligns the layout about 150 ms after the toolbar settles to its new
height, instead of waiting for the next one-second check. The once-per-second check stays as a
fallback. The result is that the meter, divider and header snap back into place almost immediately
when reconnecting or disconnecting a session.

## Title-bar header self-heals (FIX [TitleBarSelfHeal])
As an extra safety net, the whole header row under the toolbar (Time / Date / Address / Monitored
Messages and the divider line) is now refreshed once per second instead of only its RX-quality corner,
so a momentarily blanked header repairs itself within a second.

## Signal-meter stays aligned in the toolbar (FIX [SigindBandAlign])
The small signal-strength meter in the top-right corner is now vertically centered in the actual
toolbar band instead of being pinned to a fixed offset. Previously, once the toolbar settled to its
themed steady-state height (slightly shorter than at startup, also re-triggered by the repaint after a
brief COM-port drop), the meter could drift down past the divider into the title-bar band. It now
shares the same anchor as the divider and RX-Q box, so it stays put.

## Divider line stays continuous under the signal meter (FIX [SigindDividerClip])
The horizontal divider line directly under the toolbar could break off (leave a gap) beneath the
signal meter. Once the toolbar settled to its shorter themed steady-state, the meter's black
background box reached down onto the divider row and painted over that segment; because the meter is
repainted on toolbar hover without redrawing the divider, the gap stuck until the next full repaint.
The meter box is now kept strictly above the divider row, and the meter repaint also restores the
divider segment beneath it, so the line stays unbroken. Correct at startup as before.

## Divider line stays continuous left of the signal meter (FIX [RxqSquareBandClamp], [RxqSquareDividerClip])
Companion fix to [SigindDividerClip] above, for a similar break just LEFT of the signal meter. The
small RX-quality indicator square (light gray when reception is good, a warning icon when poor) sits
at a fixed vertical offset tuned for the taller toolbar band. Once the toolbar settled to its shorter
themed steady-state (e.g. after a Remote Desktop disconnect at high DPI), the square's bottom reached
a few pixels below the divider and painted over both the divider row and the top edge of the header
box underneath - a short gap in the line just left of the meter that every once-per-second repaint
re-created, so neither a resize nor the re-sync fixes above could heal it. The square (and the warning
icon) is now clamped to stay strictly above the divider, and the divider segment under its span is
re-asserted on every repaint. At the taller startup height the rendering is unchanged.

## Redesigned signal meter (FIX [SigindFlatGauge], [SigindGaugeGray], [SigindGaugeBigger], [SigindWiggleVisibility])
The signal meter is now a clean flat gauge - a rounded frame with a half-circle scale and a red needle
- drawn from a high-resolution source and scaled crisply to any DPI, in the same slate-gray tone and
stroke weight as the redesigned toolbar icons. The gauge fills more of its box so it reads better next
to the zoom text. The needle itself is drawn live over a cached back-buffer, so the scale can never be
nibbled by the moving needle and there is no flicker. The at-rest needle jitter - the small wiggle you
see when noise is present but no message is decoding - was made as visible as on the classic meter: the
needle was lengthened to fill the enlarged scale and its low end restored to the original hand-tuned
spacing, so it visibly snaps out of rest again. Signal-strength behaviour is unchanged; this is a visual
refresh only.

## Signal-meter/RX-Q bitmaps reload after a display change (FIX [DisplayBitmapReload])
Hygiene fix found while diagnosing the above: the signal-meter and RX-quality warning-icon bitmaps are
now recreated for the new display whenever Windows reports a display-driver change (e.g. an RDP
connect/disconnect), instead of only at startup.

## Window close no longer runs unrelated re-sync code (FIX [WmCloseFallthrough])
Internal correctness fix, no user-visible behaviour change: closing the main window shared a code path
with the toolbar re-sync handlers above (a C `switch` falls through case labels regardless of which
one matched), so every ordinary close briefly ran that handler's code before exiting. Harmless in
practice (its one-shot timer was already cancelled on window teardown) but fragile, so window close now
returns directly instead of falling through.

## Copy with a full scrollback could crash (FIX [VscrollClamp])
Completes [ClipboardBounds] below. With a full message buffer (normal after some uptime) and the
view scrolled up (e.g. Home key), every newly arriving line pushed the pane's scroll position one
step below zero. Using Copy Upper/Lower/Selection afterwards converted that negative position to a
huge unsigned offset and read far outside the message buffer - on the x64 build a likely crash.
The scroll position is now clamped where the buffer wraps, and the copy loop clamps defensively.

## Double-click word selection read outside the buffer (FIX [DblClickBounds])
Double-clicking a word had no bounds anywhere on the path: a window wider than the internal
180-character line, a click in the empty band below the last text line, or a word starting at
column 0 of the very first buffer line made the word-boundary scans walk outside the pane buffer
(garbage selection at best, crash at worst). Click coordinates, the target line and both scan
directions are now bounded; the Google Maps position lookup gets the same guards.

## Filter delete could corrupt the filter list (FIX [FilterDelReentry])
The filter-window delete loop processes pending window messages while it works. Pressing Delete/F8
again while a large multi-delete was still running re-entered the delete handler; when the nested
pass returned, the outer loop continued with a stale index and could erase past the end of the
filter list (memory corruption / crash). A delete request is now ignored while one is in progress.

## Mouse wheel direction and multi-monitor fixes (FIX [WheelDelta], [WheelCoordSign])
Only a wheel delta of exactly +120 counted as "scroll up", so precision touchpads (small deltas)
and fast wheel movements (coalesced +/-240, +/-360) always scrolled DOWN regardless of direction;
the sign of the delta is now used. Additionally, wheel coordinates were read as unsigned, so on a
monitor positioned left of or above the primary the inside-window test always failed and the wheel
did nothing; coordinates are now read signed.

## ACARS Colors dialog corrupted main screen colors (FIX [AcarsColorsInit])
Opening the ACARS Colors dialog as the FIRST color dialog since startup and clicking OK silently
saved the main Time/Date, Message and Bit-errors colors as black (the dialog wrote values it had
never initialized). The DBI and Labels fields also shared a single color slot, so editing either
overwrote the other on every OK. All values the dialog saves are now seeded when it opens, and DBI
has its own color slot (including in the preview and the Default reset).

## Logfile dialog no longer wipes the filename (FIX [LogDlgNameWipe])
With a fixed (non-date) logfile name configured, the filename field was always empty when the
dialog opened, and toggling ANY checkbox in the dialog (enable, date, ISO, log-rejected, buffer)
erased a filename the user had just typed - OK then failed with "You haven't entered a file
name!". A leftover unconditional field update overwrote the correct value; removed.

## POCSAG function-number list duplicated (FIX [FnuComboReset])
Browsing filters with Next/Previous re-initializes the filter-edit dialog, and every jump onto a
POCSAG filter appended another "All, 1, 2, 3, 4" set to the function-number dropdown. Picking a
duplicated entry then saved an invalid function number (e.g. `-8`) into the capcode, a filter that
never matched again. The list is now reset before it is filled.

## Config-file validation (FIX [ScreenColClamp], [FilterTypeClamp], [ReadFiltersBounds])
Hardening against hand-edited or corrupt configuration files:
- `ScreenColN` (column order) values outside 0..7 were written straight through the column-layout
  code as an out-of-bounds array WRITE at startup; the Columns dialog could also launder a bad
  value into -1 via an empty dropdown selection. Clamped at load and at save.
- `FilterDefaultType` outside 0..5 (or an empty dropdown selection in Filter Options) drove an
  out-of-bounds index into the capcode-length table when adding a new filter. Clamped at load and
  at save.
- A truncated `filters.ini` line made three field scanners walk past the end of the line buffer,
  and overlong last-hit date/time tokens overflowed their 10-byte fields into adjacent filter
  data. Scans now stop at end-of-string and the copies are bounded.

## Filtered-pane scrollbar accuracy (FIX [PaneFilterScrollbarSync])
The vertical scrollbar of the Filtered pane is now refreshed on every appended line, so it always
reflects whether older messages have scrolled off the top. The busy Monitored pane already did this
implicitly via its constant auto-scroll; the low-traffic Filtered pane could, in edge cases (after a
buffer wrap, or while scrolled up), be left with a scrollbar that lagged the true content extent.

## Group-call logging fix (FIX [GroupcallLogFilename])
During a FLEX group-call conversion the message/filter log could, for the 2nd and later members of a
group, write log lines using a stale internal filename - occasionally routing monitor-log lines into
the filter file. The log filename is now re-derived for every member. Screen and feed output were not
affected.

## Decoder robustness on corrupt frames
- **POCSAG uncorrectable capcodes (FIX [PocsagCapcodeGuard])** - a RIC address word with an
  uncorrectable bit error is again shown as `???????` instead of a plausible-but-wrong capcode. The
  guard had been rendered ineffective by an earlier change.
- **POCSAG long-message safety (FIX [PocsagAlpBound])** - a long run of message codewords without an
  intervening address word can no longer overrun the internal alpha buffer.
- **MOBITEX (FIX [MobitexMpakOverflow], [MobitexStaleMpak], [MobitexResCompare], [MobitexBaseIdParse])** -
  fixed a buffer overflow on certain rare MPAK service messages with a full ESN, a stale MPAK label
  that could carry over to the next frame, an RX/TX-MAN comparison bug, and hardened the optional
  `base-ids.txt` parser against malformed lines.
- **ACARS (FIX [AcarsBitrateTerm])** - the bitrate field is now terminated, so a previous protocol's
  value can no longer stick to it after a mode switch.

## Output-feed reliability
- **SMTP monitor-window freeze (FIX [SmtpAddRespDeadlock])** - disabling SMTP (or exiting) while the
  SMTP monitor/test window was open could deadlock the whole application. The status-line update is
  now non-blocking.
- **SMTP TLS + recipients (FIX [SmtpImplicitTlsFail], [SmtpEmptyRcpt])** - a failed implicit-TLS
  (port 465) handshake now fails cleanly instead of hanging on timeouts or silently falling back to
  plaintext; an empty recipient in a `;`-separated list is skipped instead of dropping the whole mail.
- **Pushover (FIX [PushoverFirstConnectRetry])** - a transient network hiccup on the first connection
  attempt no longer silently drops a single message; it now retries like the other feeds.
- **SQLite (FIX [SqliteSizeCap], [SqliteStopDrain])** - the optional maximum-size cap no longer risks
  emptying the whole table on a database without incremental auto-vacuum (it now measures real data
  size); queued rows are flushed on shutdown/reconfigure instead of being discarded.
- **MySQL (FIX [MysqlErrPktWedge])** - an oversized server error reply no longer wedges the feed in an
  endless reconnect loop behind one row.
- **Telnet server (FIX [TelnetPartialSend], [TelnetReplayDup])** - a slow client can no longer receive
  a torn, half-sent line, and backlog replay after a reconnect no longer duplicates the lines that
  arrive during the replay.
- **Telnet server wire-log latency (FIX [TsWireLogLockFree])** - the optional wire-log disk write ran
  while the telnet server's internal state lock was held (a synchronous file write by default, since
  write-buffering is off unless enabled in the Logfile dialog), briefly stalling every other telnet
  operation - client accept/send, RS232/AUDIO state updates - for the duration of that write. The line
  is now only formatted under the lock; the actual disk write happens right after the lock is released.
- **MQTT (FIX [MqttSubRollback])** - an oversized group-subscriber list degrades to a dropped entry
  instead of malformed JSON.
- **Telegram (FIX [TgMigrateToken])** - a chat migration now matches the exact chat id rather than any
  substring, so it can no longer corrupt a different id in the list.
- **Central log (FIX [ReconfigureLock])** - applying new log settings while a feed is writing can no
  longer produce a garbled log filename.

## Input-path hardening
- **COM port lock-out (FIX [ComPortReopenLeak])** - if the serial reader thread had to be force-stopped
  in the middle of a reconnect, an exclusively-opened COM handle could leak and lock PDW out of its own
  port until a restart. The in-flight handle is now always released.
- **Sound card (FIX [WaveInErrReset], [StopCapBufferLeak], [BuffersReadyReset])** - audio buffers are
  correctly reset/released on device errors and restarts, preventing a rare buffer-tracking overrun and
  stale-audio processing after a stop/start cycle.

## Low-severity hardening sweep (second fix round)
The remaining (low-severity) findings from the display-layer audit, all fixed:

- **Resource leaks** - the font dialog leaked one font handle per "OK" (FIX [FontDlgLeak]); every
  print job leaked the printer-settings handles, a failed StartDoc still "printed", and Cancel
  kept printing into the dead job without ever aborting it (FIX [PrinterJobGuards]); two clipboard
  paths freed memory owned by the clipboard, risking a later use-after-free (FIX [ClipOwnedFree]).
- **Selection & scrolling edge cases** - dragging a selection above/left of a pane fed negative
  coordinates into the selection as huge values (FIX [PaneSelCoordSign]); dragging the scrollbar
  thumb in a scrollback larger than 65535 lines jumped to the wrong line (FIX [ThumbTrack32]);
  Copy Selection after a click without drag left an inverted cell on screen and copied a stale
  one-cell rect (FIX [CopySelNoDrag]).
- **Scrollback resize is OOM-safe** - changing the scrollback size now allocates the new buffers
  BEFORE freeing the old ones; a failed allocation keeps the current buffer and size instead of
  falling back to unchecked mallocs while a message box pumped paints against inconsistent pane
  state (FIX [ScrollDlgOomSafe]).
- **Stale dialog state** - the filter window now restyles its list correctly when the color option
  was changed from the main window, and the menu variant of Filter Options goes through the same
  path as the button (FIX [FilterColorsStale]); the filter-edit dialog re-arms its init guard on
  every open, so init-time control echoes can no longer corrupt multi-edit sep-file fields
  (FIX [FilterEditInitGuard]); two uninitialized locals fixed (sort focus with a single selected
  row, backup-extension text when no filters.ini exists - FIX [SortFilterFocusInit],
  [ResetHitExtInit]).
- **Wide-window and highlight corner cases** - on a window wider than the internal 180-character
  line the final character of a message could escape the line-wrap bound; the wrap point is now
  clamped and the hard bound is a >= test (FIX [LastcharWrapEscape]); a text-filter match at the
  very first character of a message no longer disables the highlight for the whole message
  (FIX [HighlightPosZero]); right-to-left (Hebrew) mode now reverses the per-character colors
  together with the text (FIX [ReverseColorSync]).
- **Signal meter self-heal** - a failed bitmap/DC (re)load during display changes no longer leaves
  the meter blank until restart or presents an uninitialized meter face; the next repaint retries
  (FIX [GaugeSrcdcFail], [SigindReloadRetry], [ExclamLoadGuard]).
- **Bounded string assembly** - the title-bar text, the ".txt" append for the clipboard-save
  filename, and a path-helper length check are now bounded (FIX [WindowTextBound],
  [EditFileExtBound], [PathBufferOffByOne]).

## Miscellaneous safety fixes
- Bounds and termination hardening across clipboard copy, several dialogs (logfile, filter, scrollback
  options), the error dialog, filter-label substitution, and path building - all guarding against
  overly long paths, malformed `filters.ini` fields, or unusual selections.
- Printer output now paginates instead of clipping everything past the first page.
- Language table loading is read-only (works from a read-only install folder) and handles high-bit
  characters correctly.
- The About-dialog logo bitmap is freed on close (was leaking one handle per open).

---

# PDW 4.0.3 - Release Notes

PDW 4.0.3 includes a targeted reliability fix for the MQTT output feed, plus a small display option
that makes the on-screen fragment marker opt-in. On a quiet night the MQTT feed could occasionally
miss a single message (seen downstream as a short gap), while the webhook feed to the same
Node-RED/Home Assistant instance over the same network path stayed reliable. No decoder output or
configuration format changes; existing `pdw.ini` and `filters.ini` files work unchanged.

## Optional fragment marker (FIX [FragMarkerOptional])
When a long FLEX alpha message is split across multiple frames, PDW reassembles it into a single
complete message and, until now, always drew an asterisk (`*`) directly after the capcode to flag that
the message had been rebuilt from fragments. That marker prompted recurring questions from users who
did not know what it meant, so it is now **off by default** and controlled by a new checkbox in
**Screen Options**: "Mark reassembled fragmented messages with '*' after the capcode".

- The fragment-reassembly logic is completely unchanged - only the on-screen `*` is now gated behind
  the new `ShowFragMarker` option (default `0`). Existing configs without the key keep the marker off.
- The setting applies to both the classic (non-group) capcode column and the FlexGroupMode group view.
- Enable it if you want the at-a-glance indication of which messages arrived as fragments; leave it off
  for a cleaner display identical to a normal single-frame message.

## Pane layout polish (FIX [PaneBottomPad], FIX [PaneScrollbarAlign])
The two message panes (Monitored Messages / Filtered Messages) now size their text area to whole
lines and keep a small margin between the newest row and the pane edge below it.

- No more half-clipped bottom row: text is always drawn in whole lines, at any scroll position and
  any window size ([PaneBottomPad]).
- The bottom padding lives *inside* each pane window, so the standard vertical scrollbar - which
  always spans the full window height - stays flush with the pane footer: the Filtered Messages
  header bar for the top pane, the bottom window edge for the bottom pane ([PaneScrollbarAlign]).
- Purely cosmetic; message handling, filtering, logging and all output feeds are untouched.
- The RX-Q / percentage box at the right end of the Monitored Messages title bar now uses the exact
  same vertical position math as the rest of the title bar ([RxqTitleAlign]). At display scaling
  above 100% it sat 1px lower, which made the divider line above it look thicker and the "100%"
  label sit visibly lower than the other column labels.
- The gray separator line under the Monitored Messages title bar no longer has a gap under the
  RX-Q box: the box's fill (repainted every second) overwrote its segment of the line, so the
  separator stopped short of the right edge ([RxqBottomLine]).
- The small RX-quality warning square on the toolbar is now drawn with one fixed rectangle and pen
  in both quality states; the two branches used slightly different sizes and an unpredictable
  border pen ([RxqSquareRect]).
- Hovering the toolbar buttons no longer repaints both pane title bars on every hot-track
  notification - only the graphics that actually overlap the toolbar band (signal indicator and
  RX-quality square) are refreshed, removing hover flicker ([NotifyRedraw]).
- Defensive clamp for extremely short windows: Pane1's whole-line rounding can no longer claim
  more height than remains for Pane2, so Pane2 and its header stay visible down to the smallest
  usable window size ([PaneShrinkClamp]).

## Main window could start invisible / far off-screen (FIX [WindowPosMinimized])
Exiting PDW while the main window was minimized to the taskbar could make the next start appear
"windowless": the process ran (tray icon, dialogs and decoding all worked) but the main window was
nowhere to be seen. Cause: Windows parks a minimized window at the virtual position
-32000,-32000, PDW's `WM_MOVE` handler recorded that as the window position, and an exit while
minimized persisted it to `pdw.ini` - the next start then created the window 32000 pixels
off-screen. An existing exit-time guard only covered the minimize-to-**tray** path, not a plain
taskbar minimize. This was a long-standing latent bug, not tied to any recent feature.

- The window position is no longer recorded while the window is minimized or in the tray, so the
  last real on-screen position is what gets saved ([WindowPosMinimized] in `PDW.cpp`).
- Defense in depth at startup: if the position saved in `pdw.ini` does not touch any monitor
  (e.g. a config poisoned by an older build, or a saved position on a since-removed second
  monitor), PDW falls back to the top-left of the primary work area instead of creating the
  window out of sight ([WindowPosMinimized] in `Initapp.cpp`).

## High-resolution toolbar icons (FIX [ToolbarHiResIcons])
The 13 toolbar buttons have a completely new icon set. The old icons were 18x18 pixel, 16-color
bitmaps from the original PDW era; on scaled displays they were stretched up pixel-by-pixel, which
made them look blocky and blurry, and even at 100 % they showed their age.

- New assets: each icon is a 72x72, 32-bit image with a real (premultiplied) alpha channel, drawn
  in a clean modern outline style (folder, copy, pane-copy up/down, save, print, options, filter,
  statistics, pause, help, clear, mode).
- At startup the icons are smoothly **downscaled** (area-averaging box filter) to the exact
  DPI-scaled button size - 18/23/27/36 px at 100/125/150/200 % - instead of being stretched up,
  so they render crisp and anti-aliased at every scale factor. GDI's `StretchBlt` is deliberately
  not used for this: its fast mode is blocky and its smooth mode does not reliably preserve the
  alpha channel.
- The imagelist is now a true 32-bit alpha imagelist (no more color-key transparency keyed off the
  corner pixel), which removes the jagged icon outlines.
- `pdw.manifest` now declares the **Common Controls v6** dependency. This is required for
  alpha-blended toolbar imagelists and, as a side effect, gives all PDW dialogs the modern themed
  Windows look (visual styles) instead of the classic Windows-2000 style. Behavior of the dialogs
  is unchanged.
- The old 18x18 16-color icon set has been retired: on Windows 10/11 (and the themed Common
  Controls v6 look) the new icons fit the platform, and maintaining two render paths is not
  worth it. The original bitmaps remain available in the git history.

## MQTT feed reliability

## Why only MQTT, and only at night (root cause)
Every other MQTT client on the broker (and PDW's own webhook feed) holds a single connection open and
rides out brief broker hiccups within the keepalive grace. PDW's MQTT feed was the exception: it
proactively closed its connection after 3 minutes idle and cold-reconnected on the next message. On a
quiet night, where the only recurring traffic is a periodic test call every few minutes, that meant a
cold reconnect on *every* message - roughly 288 reconnects a day. A cold reconnect (name resolution,
TCP handshake, CONNECT/CONNACK, auth) is the single fragile operation, and PDW used a thinner
retry/timeout profile than the webhook. Over a week, one reconnect eventually coincided with a brief
broker stall (e.g. a Home Assistant add-on reload or backup window) and the message was dropped.

## Warm MQTT connection (FIX [MqttWarmConn], FIX [MqttKeepAlive])
- The 3-minute idle disconnect is removed so the connection is no longer torn down between messages.
- Keepalive is now actually driven. The Paho *synchronous* client does not send keepalive pings by
  itself - its own header states the application must call `MQTTClient_yield()` periodically to emit
  them. PDW, a pure publisher, never did, so broker logs showed the PDW session being reaped roughly
  every 1.5x the keepalive ("exceeded timeout") during any quiet gap, and the next message then hit a
  dead socket. The worker now calls `MQTTClient_yield()` about every 10 s while connected, so PINGREQ
  goes out and the session survives multi-minute quiet periods (keepalive lowered 60 s to 45 s, well
  under Mosquitto's 1.5x grace and any NAT idle timeout). This stays in synchronous mode, so the QoS
  delivery confirmation in the send path keeps working.
- A genuinely dead socket (real broker restart) is still detected and reconnected on the next publish.
- Verify after upgrading: the Mosquitto log should stop showing `Client PDW ... exceeded timeout`
  every few minutes, and PDW should stay connected across quiet periods.

## Proactive idle reconnect (FIX [MqttIdleReconnect])
- If the broker drops the connection while PDW is idle (broker restart, add-on update), the worker now
  re-establishes it proactively within at most 60 s, instead of leaving it dead until the next paging
  message pays the cold-connect cost. This completes the warm-connection story: after any broker blip
  the sparse nighttime heartbeat rides an already-warm session again.
- A healthy connection is never touched - the reconnect only runs when the session is already down.
- A broker that stays down is retried once per minute (no connect storm), logging one `WARN` on the
  first failure and a `CONNECT` line when the connection is restored. The very first message after
  PDW startup also benefits: the initial connection is now made right after the feed starts.

## Hardened MQTT reconnect (FIX [MqttReconnHarden])
- Connect timeout raised from 5 s to 10 s, matching the webhook feed's WinHTTP connect timeout.
- Publish attempts raised from 2 to 4 with exponential back-off (1/2/4 s between them), mirroring the
  webhook feed's retry profile, so a brief broker stall is ridden out instead of dropping the message.
- Shutdown and runtime reconfigure are unaffected: the back-off retries are skipped as soon as a stop
  is in progress, so teardown stays prompt.

## Retry on unconfirmed QoS delivery (FIX [MqttWaitRetry])
- With QoS 1 or 2, `MQTTClient_publish` can return success (the message is queued locally) while the
  broker acknowledgement never arrives - the classic half-open socket after an idle gap, where
  `waitForCompletion` returns `rc=-3` (disconnected). Previously this was logged as a warning and the
  message was dropped even though retries were still available.
- Such an unconfirmed delivery now drops the stale connection and retries on a fresh one, up to the
  same 4-attempt limit. Because QoS 1/2 is at-least-once, a rare duplicate is preferred over a miss.
  This was the exact failure that appeared in the logs once QoS was raised above 0.

## Self-explanatory MQTT log codes (FIX [MqttRcText])
- The MQTT log now prints a short label next to each Paho return code, e.g. `rc=-3 (DISCONNECTED)`
  and `rc=-1 (FAILURE)`, so a log line is readable without looking the code up.

## Recommended alongside this release
- Enable the MQTT log file (`MqttLogToFile=1`) to confirm behaviour: nighttime lines should now read
  `SENT` without a preceding `RECONNECT`, proving the session survives the gaps.
- If you monitor the feed for inactivity, keep the threshold comfortably above the heartbeat interval
  (at least two missed beats) so a single delayed message never trips a false alert.

---

# PDW 4.0.2 - Release Notes

PDW 4.0.2 is a stability release: no new features, only hardening fixes that came out of a full
release-readiness audit of the codebase (memory safety, bounds checking). None of them change any
decoder output or configuration format; existing `pdw.ini` and `filters.ini` files work unchanged.

## Bounded filename-extension append (FIX [ExtAppendBound])
- The Logfile, Filterfile and Statistics dialogs appended their default extension (`.log`, `.flt`,
  `.st`) with an unchecked `strcat`. A filename typed or pasted at the maximum length (the edit
  controls have no length limit) filled the buffer completely, and the append then wrote a few bytes
  past it - a stack buffer overrun triggerable from the GUI. The extension is now only appended when
  it still fits; nothing changes for normal-length names.
- The same pattern in the per-filter separate-filterfile fields could overrun the stored filter
  entry by up to 4 bytes when a name of 125-128 characters without an extension was entered. Also
  bounded now.

## Group-call sort: read past the group array (FIX [SortGroupBound])
- Sorting a FLEX group-call member list relied on a 0-terminator behind the last member. A group at
  the absolute maximum capacity (999 members) has no terminator, letting the sort read one integer
  past the array. Read-only and practically unreachable with real paging traffic, but now explicitly
  bounded.

## MOBITEX hardening (FIX [MobitexFilterLen], FIX [MobitexSweepBounds])
- A MOBITEX filter whose capcode was hand-edited in `filters.ini` to fewer than 2 characters made the
  filter matcher index before the start of the string. Such a capcode now safely falls back to the
  default address+mode match.
- The MOBITEX sweep-info decoder took the neighbour/slave channel count directly from a raw decoded
  byte (0-255) while the channel table holds at most 15 displayable entries; a corrupted frame could
  overrun the table and the display buffers. The count is now capped to the table capacity and the
  fill loops are explicitly bounded.

---

# PDW 4.0.1 - Release Notes

## Filter label colors - readability and uniqueness (FIX [FilterBlueContrast])
- The **Default** filter label color was pure blue (`0,0,255`), which is hard to read on the black
  message background. It is now a readable azure (`64,128,255`). This also applies to the "Reset all
  colors to Default" buttons in the color dialogs.
- With **Better contrast** enabled, three blue slots (Default, Light Blue, Blue) previously remapped to
  the exact same color. They are now distinct: Default = `140,180,255`, Light Blue = `80,148,255`
  (unchanged), Blue = `48,112,224`. Every label color in the enhanced palette is now unique.
- Note: the Default color is stored in `pdw.ini` (`Color.FilterLabel=`). Existing installations keep
  their saved value; use "Reset all colors to Default" (or start from a fresh `pdw.ini`) to pick up the
  new readable blue. The Light Blue and Blue slots are not stored and update automatically.

## Custom filter label colors via pdw.ini (FIX [FilterPaletteIni])
- All 17 filter label colors can now be overridden in `pdw.ini`. Slot 0 keeps the existing
  `Color.FilterLabel=R,G,B` key; slots 1-16 use `Color.FilterLabel1=` .. `Color.FilterLabel16=`.
- It is per-key autodetect: a missing, empty or invalid key keeps the built-in default, so an existing
  `pdw.ini` behaves exactly as before. No GUI switch and no "enable custom colors" flag - leave a key
  out and that slot stays default; add one and that slot is overridden.
- Filters still reference a color by its slot number (the `filters.ini` format is unchanged), so this
  is fully backward compatible. Custom colors are shown as-is; the "Better contrast" remap only adjusts
  the built-in defaults.

---

# PDW 4.0.0 - Release Notes (final)

PDW 4.0.0 is the first **final release** of the modernised fork. It does not add new features over
3.7.0 - it marks the point where five years of work on the classic PDW 3.2 codebase have settled into
one stable, dependency-free binary that has been hardened for unattended 24/7 operation. The version
number is bumped to 4.0.0 to reflect that maturity.

## What PDW does

PDW turns a sound card or a serial-port hardware slicer into a full paging receiver and message
distributor.

### Decoding
- **FLEX** 1600 / 3200 / 6400 bps - Alpha, Numeric, Tone, Short-Instruction, Frame-Info, group calls,
  with multi-frame alpha fragment reassembly
- **ReFLEX** - decoded through the same path (phase demux A/B/C/D)
- **POCSAG** 512 / 1200 / 2400 bps - Alpha, Numeric, Tone
- **ACARS**, **MOBITEX** and **ERMES** decoders
- Sound-card (WaveIn) or serial-slicer / RS232 input; the COM port is held with exclusive access so a
  second program cannot split the byte stream

### Filtering
Each filter matches on capcode, label or message text (`^` starts-with, `&` AND, `|` OR, `=` whole
word, up to 256 characters) and can independently set a custom label and colour, play a WAV alert,
send e-mail, run an external command, write to its own log file, route to specific output feeds, or
act as monitor-only / reject. FLEX group-call subscribers can be hidden from the screen per filter
("Ignore in Groupcall") while remaining in the logs and feeds.

### Output feeds
On-screen display, Windows 11 toast notifications, the message log, and eight distribution channels:
- **SMTP e-mail** - STARTTLS (587) and implicit TLS (465), RFC-compliant EHLO, split Subject/Body,
  reliable worker thread with TCP keepalive
- **MQTT** - PDW-native and flat/Node-RED JSON
- **Webhook** - HTTP(S) POST with CSV / per-capcode / subscriber-array modes
- **Telnet server** (port 8024) - streams decoded messages in a structured wire-format (a custom
  internal feature, not intended for general use), with half-open session cleanup via TCP keepalive
- **MySQL / MariaDB** - Classic / Extended / Optimized schemas, no external client DLL
- **SQLite** - a single local file, no server, LowWrite NVMe mode and optional auto-purge
- **Telegram** (Bot API) and **Pushover** - Title/Body templates, per-filter routing and overrides,
  one-message-per-group-call batching

### Supporting features
- Central **log manager** with date-rotated files, optional ISO timestamps, and write buffering to
  reduce SSD write amplification on busy networks
- **RX Quality Alert** - e-mail when signal quality stays poor for a sustained period
- **High-DPI** support, configurable colours/fonts, scrollback, system-tray operation
- A single binary under ~7 MB: OpenSSL, Paho MQTT, SQLite and the MySQL protocol are statically
  compiled in - no separate DLLs to deploy

## Changes since 3.7.0

None functionally - 4.0.0 is 3.7.0 promoted to a final release (version strings and resource version
info updated to 4.0.0). The feature delta that the 3.7.x line introduced over 3.6.x is summarised
below and detailed in the 3.7.0 notes that follow:

- Telnet server: half-open client sessions detected and cleaned up via TCP keepalive
  (FIX [TelnetStaleSlot])
- TCP keepalive on the SMTP connection (FIX [SmtpKeepAlive])
- Telegram: direct (1-on-1) chats no longer fail with HTTP 400 (FIX [TelegramThreadOverflow])
- Telegram: Discover now finds every chat, including your direct chat (FIX [TgDiscoverAllChats])
- Per-filter Telegram routing - TG topic / TG chat overrides (FIX [TelegramRouting])
- Pushover: HTML line breaks no longer add a stray space/blank line (FIX [PoHtmlNewlineSpace])
- Filter dialog layout fixes - hit counter and PO priority/sound labels (FIX [FilterHitsClip] /
  [FilterPoLabelClip])

---

# PDW 3.7.0 - Release Notes

## New in 3.7.0

### Telnet server

- **Half-open client sessions are now detected and cleaned up (FIX [TelnetStaleSlot]):** when a
  connected client dropped its link ungracefully - a router/NAT rebind, a crash, or a brief network
  blip that swallowed the TCP close - PDW never noticed the old socket was dead. It kept the slot
  "connected" and kept sending to it, and when the same client reconnected the new link was assigned a
  *second* slot. The result was two simultaneous sessions, both shown as active, while only one was
  actually processed downstream - so a receiver could look perfectly connected yet have its data
  silently ignored. PDW now enables TCP keepalive on every accepted connection, so a dead/half-open
  peer is detected within roughly a minute and its slot is freed for reuse. Multiple simultaneous
  connections from the same address (for example several clients behind one NAT, or test setups) are
  fully supported - each TCP connection is its own session.

### E-mail (SMTP)

- **TCP keepalive on the mail connection (FIX [SmtpKeepAlive]):** the SMTP socket now enables TCP
  keepalive so a server that disappeared without closing the link cannot leave a half-open
  connection lingering. This is a safety net on top of the existing 30 s receive timeout (which
  already detects a dead server on the next mail) and the server's own idle-session timeout.

### Telegram

- **A direct (1-on-1) chat in the chat list no longer fails with HTTP 400 (FIX [TelegramThreadOverflow]):**
  a forum topic / thread id (`message_thread_id`) is only valid for a group/supergroup. When a topic
  was configured (globally or per filter) it was previously applied to *every* destination, so a
  direct chat with the bot - which has no topics - was rejected by Telegram with HTTP 400 ("message
  thread not found") and showed up as `failed` in the log. The topic is now applied only to
  group/supergroup/channel destinations (negative chat ids); direct chats (positive ids) never get a
  topic and send normally. This covers both the global chat list and per-filter chat overrides.

- **Discover now finds every chat, including your direct chat (FIX [TgDiscoverAllChats]):** the
  Discover button used to return only the *last* chat seen in the bot's recent updates, so once the
  bot was added to an active group the group's messages always came last and a 1-on-1 chat was never
  offered. Discover now lists *all* distinct chats from the updates (id, type, and name), de-duplicated,
  and appends each new one to the chat-id field in a single click. The reply buffer was also enlarged
  so a busy bot's updates are no longer truncated. Note: Telegram only keeps recent un-acknowledged
  updates for about 24 hours - if a chat no longer appears, send a fresh message to the bot and click
  Discover again.

- **Per-filter Telegram routing (FIX [TelegramRouting]):** the Ctrl+F filter editor now has two new
  per-filter Telegram fields. *TG topic* sets a numeric forum topic / thread id
  (`message_thread_id`) so a capcode's messages land in a specific topic of a forum group (blank or 0
  = the chat's default thread). *TG chat* overrides the destination chat: leave blank to use the
  global chat id(s), or enter one or more targets such as `-1001234567890`, `@mychannel`, or several
  separated by `;` to send just that capcode somewhere else (a typed comma or space is normalised to
  `;` so the CSV line stays intact). Both fields are stored in `filters.ini` (new CSV fields 14 and 15);
  old files without them load with the global routing as before. The overrides also apply inside
  FLEX group calls (first non-zero topic and first non-empty chat override among the matched
  subscribers win). A topic id belongs to a specific chat - if you combine a chat override with a
  topic id, that topic must exist in the overridden chat. A failing override send is logged to
  `YYMMDD_telegram.log` and never blocks the rest of the Telegram stream; the bot token is never
  logged.

### Pushover

- **HTML line breaks no longer add a stray space/blank line (FIX [PoHtmlNewlineSpace]):** with HTML
  formatting on, a multi-line Pushover message (for example one capcode/label per line in a group
  call) showed an extra leading space - or with the interim build a full blank line - in front of each
  line in the Pushover app, while the lock-screen preview looked fine. PDW used to translate every
  `\n` into an HTML `<br>`, but Pushover already turns a bare newline into a line break in HTML mode
  too, so the explicit `<br>` stacked a second break on top of Pushover's own. PDW now sends the
  newlines verbatim in both plain and HTML mode: a single `\n` renders as one clean line break
  everywhere (app, lock-screen preview, and plain mode), and `<b>`/`<i>` styling still applies with
  HTML on. Use `\n\n` in the Title/Body template where you want a blank line of separation between the
  message and the label list.

### Filter dialog

- **Hit counter is fully visible again (FIX [FilterHitsClip]):** after the Telegram/Pushover layout
  rework the "Number of hits" field in the Ctrl+F filter editor had been narrowed so far that the
  prefix text left room for only a single digit - a filter with 2 or more hits showed nothing after
  "Number of hits:". The count was always read and stored correctly; only the on-screen field was too
  small. The hit-counter row is back to its original column widths so multi-digit counts and the last
  hit date/time are shown in full.
- **Pushover priority/sound labels no longer clipped (FIX [FilterPoLabelClip]):** the "PO priority"
  and "PO sound" labels in the same dialog were wide enough that their text ran underneath the combo
  box / edit field next to them, hiding the last characters. The labels were narrowed and the inputs
  nudged right so each label stays fully readable.

## New in 3.6.9

### FLEX group calls

- **Late / fragmented group messages now keep their subscriber list (FIX [GroupLateFrame] /
  [GroupAssembledLeak]):** a FLEX group call is announced by Short Instructions ("listen to group X,
  the message arrives in frame N") after which PDW collects the member capcodes and shows them under
  the group message. The match used to require the group message to arrive in *exactly* frame N. But
  per the FLEX spec a long message may be split into fragments sent "in following frames", and a busy
  frame can push the message one or more frames later than announced. When that happened PDW showed the
  group capcode bare (without its members), never cleared the collected members, and the stale list
  then leaked onto the next group call that reused the same group slot - appearing minutes later under
  the wrong group. PDW now accepts the group message anywhere within the same grace window it already
  used for missed-call detection (up to `GROUP_GRACE_FRAMES` after the announced frame, with
  end-of-cycle wrap handling), so the members are shown immediately under the correct message and the
  slot is cleared. A safety net also emits and clears the members for multi-fragment messages that
  complete beyond the grace window. As a side effect such late calls are no longer mis-counted as
  "missed group calls". Applies identically to all 16 group codes (2029568-2029583). The on-screen
  view, the Windows toast and the webhook/Telegram/Pushover/MQTT/MySQL/SQLite feeds all benefit; the
  telnet wire feed was already correct (it emits the raw instruction and message events separately).

## New in 3.6.8

### Filter overview

- **Telegram/Pushover state in the Ctrl+F overview (FIX [FilterWindowFeeds]):** with *Show extra info*
  enabled, each filter row now shows `TG`/`tg` and `PO`/`po` flags (uppercase = on, lowercase = off)
  next to the existing `CMD` / `LAB` / `SEP` / sound / `IGN-GRP` markers. The flags reflect the
  per-filter *Send Telegram* / *Send Pushover* checkboxes, giving an at-a-glance view of which capcodes
  are wired to each notification service - useful when scanning thousands of filters. The markers only
  appear for non-reject filters (a reject row shows `REJECT` instead) and only when the extra-info
  column is turned on.

### Per-filter notification overrides

- **Telegram silent per filter (FIX [TelegramSilent]):** the Ctrl+F filter editor has a new *Telegram
  silent* checkbox. When checked it forces `disable_notification=true` for every Telegram message that
  matches this filter, regardless of the global *Silent delivery* setting. When unchecked it defers to
  the global setting (it does not force audible). Useful for silencing routine status capcodes in an
  otherwise audible Telegram bot, or for keeping a critical alarm capcode audible even when global
  silent is on. The flag is stored as bit `0x100` of the existing filter flags field in `filters.ini`.
  It is disabled for reject rules (reject filters never produce Telegram output). In multi-edit mode
  (multiple filters selected) the checkbox switches to tri-state; an indeterminate state means "leave
  unchanged".

- **Per-filter overrides now apply inside FLEX group calls (FIX [GroupcallPerFilter]):** a P2000
  monitor capcode almost always arrives only as a *subscriber inside a group call*, never as an
  individual page. Earlier the per-filter Telegram-silent / Pushover-priority / Pushover-sound
  overrides were dropped for group calls (the single aggregated notification used the global settings),
  which made them useless for exactly the monitor-code use case they were built for. The overrides are
  now folded into the per-group accumulator and applied when the group is flushed as one notification:
  - **Pushover priority** - the most urgent priority among the matched subscribers wins (e.g. one
    subscriber set to *High* lifts the whole group notification to High).
  - **Pushover sound** - the first non-empty per-filter sound among the matched subscribers is used.
  - **Telegram silent** - the group is sent silent only when **every** matched subscriber requested
    silent; a single non-silent matched member keeps it audible, so a real alarm is never accidentally
    muted. The global silent setting is still applied on top.
  A subscriber with no override (priority "Global" / empty sound / silent off) does not affect the
  aggregate. The Test buttons in the Telegram and Pushover config dialogs now also include the
  configured priority/sound/silent so a test send previews the real notification.

- **Pushover priority and sound per filter (FIX [PushoverPerFilter]):** the filter editor has two new
  Pushover-specific controls:
  - *PO priority* dropdown: "Global" (use the Pushover config value), or an explicit override
    (-2 Lowest / -1 Low / 0 Normal / 1 High). Emergency priority 2 is not offered (requires
    receipt-polling infrastructure not yet present).
  - *PO sound* text field: leave blank to use the global configured sound; enter a Pushover sound name
    (up to 31 characters, e.g. `siren`, `alien`, `none`) to override it for this filter only.
  Both are disabled for reject rules and unavailable when the Pushover sink is off. In multi-edit mode
  the dropdown has a "Don't change" item and the sound field shows "(leave unchanged)" when selected
  filters differ. Overrides are stored as CSV fields 12 and 13 appended to the existing `filters.ini`
  line (`-9` / empty = use global). Older PDW versions that read the file stop at field 11 and ignore
  the new tail, so the format is backward-compatible. These overrides apply both to individual pages
  and inside FLEX group calls (see the group-call aggregation note above).

---

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
  - **Database feeds tag the member distinctly (FIX [GroupcallIgnoreFeed]):** in the MySQL and SQLite
    feeds an ignored group-call subscriber is written with a dedicated per-subscriber `match_type` value
    of **3** (alongside 1 = filtered, 2 = monitor-only, 0 = unfiltered member). The member stays fully
    present in the group row's `subscribers` JSON - it is not dropped - but the distinct value lets a
    website isolate, hide or restyle these "monitor codes" on demand, apart from both genuine matches and
    the many plain unfiltered members of a group. The group-row `match_type` column itself stays 0/1/2
    (an ignored member counts as 0 for the row aggregate), so existing row-level queries, badges and
    counters are unaffected. Earlier the ignored member collapsed to `match_type` 0 and was
    indistinguishable from any other unfiltered subscriber.
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
