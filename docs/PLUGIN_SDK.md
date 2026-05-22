# Plugin SDK (v1.1.0 — DRAFT, scaffolded ahead of cycle)

> Status: **scaffold**, shipped by PLUG1 ahead of the v1.1.0 cycle.
> Wired into the build by F2 when v1.1.0 actually lands. Not yet
> referenced by `main/CMakeLists.txt`; the example plugin under
> `examples/sdk_example_scene/` does NOT compile into the default
> firmware. See "Integration checklist" at the bottom.

This document defines the public contract a third party uses to
extend an `esp32-agent-dashboard` device with their own scenes and
console commands, without forking firmware.

---

## 1. What a plugin IS

A **plugin** is a *signed component pack* that adds capability to a
running dashboard firmware. Concretely it ships:

| Required | Description |
|---|---|
| `manifest.toml` | identity, ABI level, signing keys, exports |
| at least one `scene_t` registered via `PLUGIN_SCENE_REGISTER(...)` | otherwise the plugin has no UI |
| an ed25519 *author* signature over the manifest + sources | proves "this code came from author X" |
| (countersigned) an ed25519 *user-trust* signature | proves "this device's owner accepts plugin X" |

| Optional | Description |
|---|---|
| one or more `plugin_console_register(...)` calls in the plugin's `on_init` hook | adds `plugin.<name>.<verb>` console commands |
| static assets (icons, fonts) embedded via `idf_component_register(EMBED_FILES ...)` | bundled inside the component, accessed via standard `_binary_*_start` symbols |
| a README under `examples/<plugin>/README.md` | what it does + what stub data it expects |

A plugin is **NOT**:

- a separate ELF / binary loaded at runtime (see §2)
- allowed to mutate `agent_state` directly (read-only access via the SDK)
- allowed to register `dash *` commands (those are reserved for the
  bridge protocol; plugins use `plugin.<name>.*`)
- allowed to install console commands without going through
  `plugin_console_register` (so we can keep an audit log of who
  registered what)

---

## 2. Load-time vs runtime extension

**v1 is LOAD-TIME ONLY.**

Plugins are linked into the firmware image at build time, in one of
two shapes:

### 2a. In-tree component (fork model)

The plugin lives under `./components/<plugin-name>/` in a forked
checkout. The user's `main/CMakeLists.txt` lists it in `REQUIRES` (or
it's auto-discovered via `EXTRA_COMPONENT_DIRS`). The build produces
ONE monolithic `app.bin` that has all plugins baked in.

This is what `examples/sdk_example_scene/` demonstrates.

### 2b. OTA partition (drop-in model, v1.1.0 stretch)

A second app partition (`plugin_a` in `partitions.csv`, **NOT YET
ADDED**) holds a plugin pack signed by author + user. At boot,
`plugin_loader_init` mmap's the partition, validates signatures,
runs each plugin's init. This requires partition-table changes that
SEC1 must co-design — see "Future wire changes" §8 for the
provisional layout.

### 2c. Runtime hot-load — *v2 stretch, NOT shipping*

A hot-loadable dlopen-style flow (à la Linux `.so`) is *not* in
scope for v1. ESP-IDF has no dynamic loader, and we explicitly do
NOT want one — it would let an attacker who pwns the bridge push
arbitrary code without a power-cycle, which violates SEC1's threat
model. Plugins must require a reboot to install.

---

## 3. The manifest format

`manifest.toml` ships at the root of every plugin component. Example:

```toml
# manifest.toml — esp32-agent-dashboard plugin manifest, v1
[plugin]
name             = "weather"           # snake_case, [a-z0-9_], unique within firmware image
display_name     = "Weather"           # shown in scene picker, status surfaces
version          = "0.1.0"             # semver, plugin's own version
description      = "Four-day forecast for a configured city."

[abi]
plugin_abi_version = 1                 # see §6 ABI compatibility
min_firmware       = "1.1.0"           # refuse to load below this
max_firmware       = "<2.0.0"          # refuse to load at or above this

[exports]
scenes           = ["scene_weather"]   # symbol names registered via PLUGIN_SCENE_REGISTER
console_commands = ["plugin.weather.set_city",
                    "plugin.weather.refresh"]

[signing]
author_key_id    = "ed25519:plug-author-001"
# Signature blobs live next to manifest as:
#   manifest.toml.sig.author
#   manifest.toml.sig.user
# Each is a 64-byte detached ed25519 signature over the canonicalised
# manifest + a Merkle root of the source tree (see §5).

[assets]
embed = []                              # files passed to EMBED_FILES at build
```

Fields whose value is malformed cause `plugin_loader_init` to log
`[plugin:<name>] reject: <reason>` and **skip** the plugin (do not
panic the device).

---

## 4. Plugin lifecycle hooks

A plugin scene's lifecycle is a strict superset of the harness's
existing `scene_t` lifecycle (see
`components/aurora-harness/include/harness/scene_framework.h`):

| Hook | Harness equiv | When fires | Plugin use |
|---|---|---|---|
| `on_init(scene, parent)` | `init()` | Once, after signatures verify and parent LVGL container is allocated. | Build LVGL tree, `plugin_console_register(...)`, allocate `user_data`. |
| `on_show(scene)` | `on_show()` | Every time this scene becomes visible. | Resume timers, refresh any cached agent_state. |
| `on_hide(scene)` | `on_hide()` | Every time this scene leaves the screen. | Pause timers, stop animations. |
| `on_tick(scene, t_ms)` | `frame()` | If the plugin requested ticking. Shares the framework's 60 Hz timer. | Animate, poll borrowed agent_state, redraw. |
| `on_dash_command(scene, verb, args)` | *new* | Fires when the bridge issues `dash plugin <plugin-name> <verb> <json>`. | Plugins receive structured input from the host without re-implementing the line parser. |

`on_dash_command` is the v1 plugin-protocol hook — it is dispatched
inside the existing `dash *` command family rather than as a new
top-level verb, so plugins never have to touch the global console
registry directly. See PROTOCOL.md "Future wire changes" §8.

All hooks are called on the **LVGL task**. They MUST NOT block, MUST
NOT take `agent_state_lock()` for longer than ~1 ms, and MUST NOT
call `vTaskDelay` for more than 0 ticks.

---

## 5. Security contract

> *Cross-references SEC1's work in `tools/sign/` (in progress at the
> time of writing). This section is the agreed contract between
> PLUG1 and SEC1; the actual keypair tooling lives on SEC1's side.*

A plugin's signatures live next to its manifest:

```
plugins/<name>/manifest.toml
plugins/<name>/manifest.toml.sig.author    # 64 B ed25519, by author_key_id
plugins/<name>/manifest.toml.sig.user      # 64 B ed25519, by user-trust key
```

The bytes signed are **NOT** just the TOML file — they are a
canonical concatenation:

```
H = sha256( canonicalised_manifest.toml )
M = sha256( merkle_root_of_source_tree )           # see tools/sdk/sign_plugin.py
P = "esp32-agent-dashboard.plugin.v1\n" || H || M
sig_author = ed25519_sign(author_secret, P)
sig_user   = ed25519_sign(user_secret,   P)
```

The user-trust key is generated and stored on the device's NVS
namespace `plug_keys` (single key — the device's owner). User trust
is granted via `dash plugin trust <author_key_id>` from the bridge,
which writes a co-signing record to NVS. The **device** never holds
the user-trust **secret** — only the public key for verification.

Both signatures must verify at `plugin_loader_init`:

```
verify(author_pub,    P, sig_author) == OK
verify(user_pub,      P, sig_user)   == OK
```

Failure modes:

| Failure | Action | EVT |
|---|---|---|
| Manifest parse error | log + skip | `EVT: plugin_reject name=<n> reason=manifest` |
| Author signature missing or invalid | log + skip | `EVT: plugin_reject name=<n> reason=sig_author` |
| User countersignature missing or invalid | log + skip | `EVT: plugin_reject name=<n> reason=sig_user` |
| ABI mismatch | log + skip | `EVT: plugin_reject name=<n> reason=abi` |
| Init hook returns non-zero / panics | log, mark slot dead, continue | `EVT: plugin_reject name=<n> reason=init` |

There is NO "skip the user signature in debug builds" backdoor.
Developers building their own plugin lock-step generate their own
user-trust keypair via `python tools/sdk/sign_plugin.py --self-trust`
and store the private half under `~/.esp32-agent-dashboard/keys/`.

### Threat model alignment with SEC1

SEC1 owns the *firmware* OTA threat model (signed updates, anti-
rollback). The plugin SDK reuses SEC1's primitives:

- Same ed25519 keypair management tooling (`tools/sign/`), same
  `mbedtls_pk_*` verify path.
- Same "two keys, two parties" structure: SEC1 requires `author`
  (firmware vendor) + `user` (device owner) co-signatures for OTA;
  we do the same for plugins.
- Same partition-encryption baseline: a plugin-OTA partition (§2b)
  inherits the same flash-encryption enabled-or-not status as the
  app partitions.

This means a future attacker who can write arbitrary bytes to a
plugin partition (e.g. via a compromised bridge) still can't get
them executed without controlling **both** the author secret AND
the user-trust secret — the same bar as a malicious OTA update.

---

## 6. ABI compatibility rules

`plugin_abi_version` follows a flat integer scheme:

| Firmware ABI | Plugin ABI | Outcome |
|---|---|---|
| `N` | `N` | load |
| `N` | `< N` | load if firmware advertises `plugin_abi_compat_min <= plugin_abi`, else reject |
| `N` | `> N` | reject — plugin too new |
| any | any, but `min_firmware`/`max_firmware` mismatch | reject |

We start at `plugin_abi_version = 1`. The ABI is the union of:

- `plugin_api.h` exported symbols + their signatures
- the layout of `plugin_agent_state_t` (the read-only snapshot type)
- the exact dispatch shape of `on_dash_command(verb, argc, argv)`

Anything that changes any of the above bumps `plugin_abi_version`
and **forbids** the previous version unless we explicitly maintain
`plugin_abi_compat_min`. Plugins themselves use **semver** for
their own `version`; that's unrelated to ABI.

---

## 7. The SDK toolchain

Files in `tools/sdk/`:

- `scaffold.py <name>` — generate `./plugins/<name>/` from template:
  manifest, hello scene, sample console command, CMakeLists.txt,
  README. Idempotent on re-run (won't clobber edits; will refresh
  files marked `# AUTO-GENERATED`).
- `sign_plugin.py <plugin-dir>` — produce `manifest.toml.sig.author`
  + (`--self-trust`) `manifest.toml.sig.user`. Requires SEC1's
  `tools/sign/` keys to exist; will error with an actionable hint if
  not.

Workflow:

```
$ python tools/sdk/scaffold.py weather
created ./plugins/weather/{manifest.toml,scene.c,scene.h,CMakeLists.txt,README.md}

$ # edit scene.c, iterate locally

$ python tools/sdk/sign_plugin.py plugins/weather --self-trust
signed: manifest.toml.sig.author (ed25519:plug-author-001)
signed: manifest.toml.sig.user   (ed25519:user-self-trust)

$ # add to project's main/CMakeLists.txt REQUIRES list, idf.py build
```

---

## 8. Future wire changes (reserved, NOT yet in PROTOCOL.md)

> PLUG1 deliberately did not edit `PROTOCOL.md` (that's reserved
> ground). Reserving the shape here so when v1.1.0 lands, F2/H4 can
> lift this section into PROTOCOL.md verbatim.

New `dash plugin` verbs requested:

- `dash plugin list` → OK + payload of installed plugins with name,
  version, abi, signature status.
- `dash plugin <name> <verb> <json>` → routes `on_dash_command`
  to the named plugin. The plugin's `<verb>` namespace is
  plugin-local; no risk of colliding with `dash snapshot` etc.
- `dash plugin install <name> <ota-slot>` (v1.1.0 stretch; OTA path)
  — bridge streams a signed pack to a plugin partition, device
  verifies, marks pending, reboot to activate.
- `dash plugin trust <author_key_id>` — owner action; bridge prompts
  user; device persists the user-trust pubkey + records the
  countersignature.

New EVTs:

- `EVT: plugin_loaded name=<n> version=<v> abi=<n>`
- `EVT: plugin_reject name=<n> reason=<r>`
- `EVT: plugin_dash_unknown name=<n> verb=<v>` (plugin didn't
  implement `on_dash_command` for that verb)

These need a partition-table addition for §2b OTA flow:

```
# partitions.csv (PROPOSED, not yet applied)
plugin_a, app,  ota_0,  ..,  1M,
plugin_b, app,  ota_1,  ..,  1M,
plug_keys,data, nvs,    ..,  0x2000,
```

---

## 9. Integration checklist (for F2 when v1.1.0 lands)

- [ ] `main/CMakeLists.txt`: add `main/plugin/plugin_loader.c` to SRCS
- [ ] `main/CMakeLists.txt`: add `"plugin"` to INCLUDE_DIRS
- [ ] `main/esp32_agent_dashboard_main.c`: call `plugin_loader_init()`
      AFTER `agent_state_init()` + `theme_init()` + `console_protocol_init()`
      but BEFORE `agent_commands_register()` (plugins may want to
      register `on_dash_command` listeners before snapshots start flowing).
- [ ] `PROTOCOL.md`: lift §8 "Future wire changes" into PROTOCOL.md
      proper, mark wire version v1.1.
- [ ] `partitions.csv`: add `plugin_a`, `plugin_b`, `plug_keys` per §8.
- [ ] `examples/sdk_example_scene/`: add to `EXTRA_COMPONENT_DIRS`
      in a non-default build profile for "plugin demo" smoke runs.
- [ ] `tools/smoke.ps1` (G-6): add `dash plugin list` + signature-
      reject path to the dashboard-consumer smoke suite.
- [ ] Verify with SEC1 that `tools/sign/` key paths match
      `tools/sdk/sign_plugin.py`'s assumed layout.

---

## 10. References

- `main/plugin/plugin_api.h` — the C header plugin authors #include.
- `main/plugin/plugin_loader.{h,c}` — boot-time enumeration + verify.
- `tools/sdk/scaffold.py` — new-plugin generator.
- `tools/sdk/sign_plugin.py` — signature helper, integrates with SEC1.
- `examples/sdk_example_scene/` — fully-buildable reference plugin.
- `components/aurora-harness/include/harness/scene_framework.h` —
  the underlying `scene_t` shape that `PLUGIN_SCENE_REGISTER` wraps.
- `components/aurora-harness/include/harness/console_protocol.h` —
  the underlying console API that `plugin_console_register` wraps.
