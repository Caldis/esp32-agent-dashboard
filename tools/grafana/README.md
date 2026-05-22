# Grafana dashboard for esp32-agent-dashboard

A pre-built Grafana dashboard that visualises rolling telemetry from
one or more devices. Reads `.jsonl` files written locally by the
bridge — no remote service, no third-party collector.

The dashboard is opt-in. The firmware never enables telemetry by
itself; see [`docs/TELEMETRY_SPEC.md`](../../docs/TELEMETRY_SPEC.md)
for the privacy posture.

---

## Three-step setup

### 1. Make sure local telemetry files exist

Run the bridge for a few hours, or sample manually:

```bash
$ ls ~/.claude-buddy/telemetry/
2026-05.jsonl
```

Each line is one 6-hourly envelope (see TELEMETRY_SPEC §4 for the
shape). If the directory is empty, the bridge isn't writing yet —
make sure `dash health` returns rolling samples before continuing.

You can also `tail -f` the file while you work to confirm appends:

```bash
$ tail -f ~/.claude-buddy/telemetry/2026-05.jsonl | jq -c .
```

### 2. Install Grafana + the JSON/Infinity datasource

Grafana >= 10. With Docker:

```bash
docker run -d --name=grafana \
  -p 3000:3000 \
  -v ~/.claude-buddy/telemetry:/var/lib/grafana/telemetry:ro \
  -e GF_INSTALL_PLUGINS=yesoreyeram-infinity-datasource \
  grafana/grafana-oss
```

Open `http://localhost:3000` (admin/admin), then:

- **Connections → Add new connection → Infinity** → Add datasource.
- **Name**: `dash-telemetry` (the UID `dash-telemetry` is what
  `dashboard.json` references — keep this exact).
- **Authentication**: None.
- **Allowed hosts**: leave default (we'll be reading via the
  `file://` URL scheme).
- **Security → Security settings**: enable **Allow local file
  access** (Infinity calls this "Allowed URL schemes"; tick `file`).
- Save & test.

If you're not using Docker, just install Grafana natively
(`brew install grafana` / official .deb / etc.) and install the
plugin via the UI; the file-access toggle is the same.

### 3. Import the dashboard

In Grafana:

- **Dashboards → New → Import**.
- Upload `tools/grafana/dashboard.json`, or paste its contents.
- When prompted for the datasource, pick `dash-telemetry`.
- Click **Import**.

You should land on a 7-panel dashboard. If the panels read "No
data", check:

1. `${jsonl_root}` variable points at the directory containing
   your `.jsonl` files. The default is `~/.claude-buddy/telemetry`;
   under Docker you'll want to change it to
   `/var/lib/grafana/telemetry` (the bind-mount destination from
   step 2).
2. The time range selector covers a window your data actually
   spans. Telemetry buckets are 6 hours wide; "last 5 minutes"
   will be empty.
3. Pick a device under the **Device** variable. With one device
   it should auto-populate.

---

## Panel inventory

| # | Panel | What it answers |
|---|---|---|
| 1 | Device uptime | "Has it been rebooting?" |
| 2 | Heap free (p50 + min) | "Is anything leaking?" |
| 3 | FPS p50 / p95 | "Is the UI still smooth?" |
| 4 | Prompt latency histogram | "Is the round-trip still snappy?" |
| 5 | Error rate | "Did my last bridge change regress?" |
| 6 | Agent count (p50 / p95) | "How parallel is my usage?" |
| 7 | Crashes (24h) | "Did anything die?" |

Each panel has a description (hover the title) that names the
exact envelope field it reads.

---

## Adapting the dashboard

The dashboard is intentionally vanilla — no Prometheus, no
Loki, no buy-in to a metrics ecosystem. The cost is that Infinity
re-reads the JSONL file on every refresh. That's fine up to ~10 k
envelopes (= ~7 years × 1 device, or ~6 months × 12 devices).
Beyond that, convert your `.jsonl` files into a real metrics
store on a cron and point the datasource there instead — every
panel target uses identical field names, so the only edit is the
datasource UID.

To add a custom panel:

1. Open the dashboard in **Edit** mode.
2. Duplicate an existing panel (preserves the Infinity config).
3. Change the `columns[].selector` to the field you want.

Every documented envelope field is at the top level — drilling
in via `root_selector` is only used for the bucketed histogram
in panel 4.

---

## Not seeing what you expected?

- **Panel 4 always reads zero**: the bridge hasn't ingested any
  prompt-latency benchmark data yet. Run
  `tools/claude_buddy_bridge.py --bench-prompts N` first.
- **Panel 7 never increments**: that's the goal, but if you
  intentionally crashed the device for testing, also confirm
  `dash dump crash` returned something — if it didn't, the boot
  count bumped without a panic record (e.g. you pressed reset).
- **Heap-free shows fewer points than expected**: check that the
  bridge's `--telemetry-window-h` setting matches the 6-hour
  default. Half-buckets get dropped from the rolling stats.

---

## Privacy reminder

Everything this dashboard renders is local — Grafana reads files
on your own disk. Nothing is exfiltrated. If you also enabled
the *remote* telemetry endpoint, that's a separate code path with
the same envelope shape but pointed at the URL you configured.

To make this dashboard work over an opt-in remote endpoint,
swap the Infinity datasource URL from `file://...` to the
endpoint's JSON-API; everything else carries over.
