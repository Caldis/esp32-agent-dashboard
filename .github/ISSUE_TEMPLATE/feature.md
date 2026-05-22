---
name: Feature request
about: Suggest a new scene, hook, or protocol capability
title: 'feat: '
labels: enhancement
assignees: ''
---

## What problem are you trying to solve

Describe the situation — the workflow you tried, the thing you
couldn't see on the dashboard, the integration that was missing.

> "I'd like to see when Claude is waiting on me vs. when it's busy
> thinking" is a concrete problem. "Add more scenes" isn't.

## What you'd like

A clear description of the feature. If it's a new scene, sketch what
it shows. If it's a new wire-protocol verb, sketch the payload:

```json
// e.g. new `dash health` verb
{ "battery_pct": 87, "wifi_dbm": -52, "uptime_s": 1843 }
```

```c
// or a new harness primitive you'd use
harness_dashboard_register_scene("my_scene", ...);
```

## Have you checked the roadmap?

- [ ] v0.1 — USB-Serial multi-agent (current)
- [ ] v1.0 — BLE NUS for Claude Desktop pairing
- [ ] v2.0 — WiFi push for headless dev boxes

If your request is on the roadmap, comment on the existing milestone
issue instead of opening a new one.

## Alternatives considered

Other ways you've thought of solving the same problem. Workarounds you
tried. Reasons they didn't work.

## Is this really an esp-harness issue?

If your feature is "the framework should expose X" (a new toolkit
command, a console-protocol extension), file it against
[esp-harness](https://github.com/Caldis/esp-harness/issues/new?template=feature.md)
and link from here.

## Additional context

Screenshots, mockups (a quick whiteboard photo is fine), links to
similar features in other projects, etc.
