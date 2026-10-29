# Installation

This covers installing a prebuilt release and the on-device setup. To
build from source instead, see [build.md](build.md). For
a project overview, see the [main README](../README.md).

## Install from a release

If you just want to run the mod, grab a prebuilt binary from this
repository's Releases page. Each release has two archives:
`oem-aa-mod-<version>.zip` and `oem-aa-mod-<version>-debug.zip`.
Use `oem-aa-mod-<version>.zip`; only grab the `-debug` one if you're
actually debugging.

The archive has the same layout — the shims under their canonical names
plus the patched system-attribute XMLs and the sample config — so it's
everything the on-device install needs:

```
oem-aa-mod-<version>/
  libpatch-blmjciaapa.so              # -> /data_persist/oem-aa-mod/
  libpatch-svcjcinavi.so              # -> /data_persist/oem-aa-mod/ (optional; HUD merge + compass)
  libpatch-aap_service.so             # -> /data_persist/oem-aa-mod/ (optional; protocol v1.6)
  libdbapiaamod.so                    # -> /jci/dbapi/plugins/ (optional; on-unit settings screen)
  resources/
    aap_system_attributes.xml         # -> /etc/
    aap_system_attributes_UCP.xml     # -> /etc/
    libpatch.conf                     # -> /data_persist/oem-aa-mod/ (optional; runtime config)
    gui/aaModSettings.js              # -> /jci/gui/apps/syssettings/aamod/ (optional; settings screen)
    gui/syssettingsApp.js.patch       # one added line in the OEM Settings app
    dbapi/plugins.xml.patch           # one added line in /jci/dbapi/plugins.xml
```

These shims are per-service `LD_PRELOAD` libraries, each scoped to the
single `sm_svclauncher` PID hosting its target service. Installing is
two on-device steps: drop the binaries somewhere `sm` can read them at
boot, then tell `sm.conf` to preload each one for its service
(`jciAAPA` for the Android Auto shim, and optionally `jcinavi` for the
HUD-merge shim).

> All commands below assume a root shell on the CMU (telnet/SSH after
> the usual jailbreak; the standard JCI dev shell works).

### 1. Place the binaries in `/data_persist/`

`/data_persist/` survives firmware updates and factory resets, which
keeps the mod from getting wiped by routine OEM activity. Use a
dedicated subdirectory so the mod's files are easy to find and remove:

```sh
mkdir -p /data_persist/oem-aa-mod
cp /mnt/sdb1/libpatch-blmjciaapa.so /data_persist/oem-aa-mod/
# Optional: the HUD-merge shim (only useful with the nav SD card; see
# hud_transport=svcnavi under Configuration below).
cp /mnt/sdb1/libpatch-svcjcinavi.so /data_persist/oem-aa-mod/
# Optional: the protocol v1.6 shim (only needed for use_protocol_v1_6=true;
# enables lane guidance on the HUD; see Configuration below). Inert unless
# that key is set.
cp /mnt/sdb1/libpatch-aap_service.so /data_persist/oem-aa-mod/
# Optional: runtime config (see Configuration below). Omit it to use
# the built-in defaults.
cp /mnt/sdb1/libpatch.conf /data_persist/oem-aa-mod/
chmod 0644 /data_persist/oem-aa-mod/libpatch-*.so
sync
```

`sync` is important — `/data_persist/` is backed by flash with deferred
writes, and without an explicit flush a power cycle before the next
write-back can leave the file truncated or missing.

The `libpatch-svcjcinavi.so` and `libpatch.conf` lines are optional:
copy `libpatch-svcjcinavi.so` if you want the HUD-merge shim or
`compass_always_on` (it self-disables in any process that isn't the nav
service), and
`libpatch.conf` only if you want to override defaults. The two libraries
load into different services — `libpatch-blmjciaapa.so` is preloaded for
`jciAAPA` and `libpatch-svcjcinavi.so` for `jcinavi`, each via its own
`<service>` block in `sm.conf` (step 2 below).

> **EU firmware:** the OEM nav service blanks the HUD street line in this
> market, so the street name will be missing even with everything else
> working. To show it, deploy `libpatch.conf` and set
> **`force_street_name = true`** — see
> [Configuration](#configuration-libpatchconf) for details. NA units
> show the street with the defaults and need no change.

SD cards mount under `/mnt/sd<x><n>` (e.g. `/mnt/sda1`, `/mnt/sdb1`, …)
depending on which USB/SD slot the kernel enumerates first on your
unit. Check `mount | grep /mnt/sd` if unsure.

### 2. Inject `LD_PRELOAD` via `sm.conf`

`sm` builds each spawned process's environment from `<environ_var>`
elements inside the matching `<service>` block, then `posix_spawn`s the
binary with that envp. That envp is given to the per-service
`sm_svclauncher` *before* it `dlopen`s the OEM `.so`, so `LD_PRELOAD` is
honoured at the right moment and the shim ends up mapped into exactly
one PID.

Edit `/jci/sm/sm.conf` and add an `<environ_var>` to each service you
want to patch.

**`jciAAPA`** (required — Android Auto shim). Find the
`<service … name="jciAAPA" …>` block and add the env var inside it:

```xml
<service type="jci_service" name="jciAAPA"
         path="/jci/aapa/blmjciaapa.so" …>
    <!-- existing <dependency>/<param>/… entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"/>
</service>
```

**`jcinavi`** (optional — needed for the HUD-merge shim, which in turn
needs `hud_transport = svcnavi` and the navigation SD card, and/or for
`compass_always_on`, which needs neither). Find the
`<service … name="jcinavi" …>` block and add the same env var inside it:

```xml
<service type="jci_service" name="jcinavi" path="/jci/navi/svcjcinavi.so" …>
    <!-- existing <dependency>/<param>/… entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-svcjcinavi.so"/>
</service>
```

**`aap_service`** (only needed for Android Auto GAL 1.6 — i.e.
`use_protocol_v1_6 = true`). Find the `<service … name="aap_service" …>` block
and add the same env var inside it:

```xml
<service type="process" name="aap_service" path="/usr/bin/aap_service" …>
    <!-- existing entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-aap_service.so"/>
</service>
```

For 1.6 HUD guidance BOTH `jciAAPA` (blmjciaapa) and `aap_service` must be
preloaded: the aap_service shim advertises 1.6 and forwards the decoded
navigation to the blmjciaapa shim, which renders it on the HUD. With
`use_protocol_v1_6 = false` (the default) the aap_service shim is inert, so there
is no reason to preload it unless you are enabling 1.6.

(Leave the existing `<dependency>`/`<connection>`/attributes exactly as
they are — only the `<environ_var>` line is added.) Skip this block
entirely if you aren't using the svcnavi HUD transport; the shim
self-disables in any process that isn't the nav service, but there's no
reason to preload it where it does nothing.

### 3. Update the AAP system attribute XMLs in `/etc`

`blmjciaapa.so` reads its one-time configuration from
`/etc/aap_system_attributes.xml` and `/etc/aap_system_attributes_UCP.xml`
at startup. The patched copies live in [resources/](../resources/) — back
up the originals and overwrite:

```sh
cp /etc/aap_system_attributes.xml          /etc/aap_system_attributes.xml.orig
cp /etc/aap_system_attributes_UCP.xml      /etc/aap_system_attributes_UCP.xml.orig
cp /mnt/sdb1/aap_system_attributes.xml     /etc/aap_system_attributes.xml
cp /mnt/sdb1/aap_system_attributes_UCP.xml /etc/aap_system_attributes_UCP.xml
sync
```

(Adjust the source path to wherever you staged the files from
[resources/](../resources/) — SD card mount, `scp`, etc.)

### 4. Restart the services (or reboot)

Restart just the services you changed:

```sh
smctl -r -n jciAAPA
smctl -r -n jcinavi    # only if you patched jcinavi in step 2
```

…or reboot the unit.

## Configuration (`libpatch.conf`)

Runtime behaviour is controlled by an optional `libpatch.conf` placed in
the same folder as the libraries (`/data_persist/oem-aa-mod/` — step 1
copies it there). It is the common config for the whole mod: every
library reads the same file and acts on the keys it understands,
ignoring the rest. A sample is shipped at
[resources/libpatch.conf](../resources/libpatch.conf).

The file is entirely optional — if it is missing, or a key is omitted,
the built-in default is used. Syntax is one `key = value` per line; `#`
begins a comment; whitespace around keys/values is ignored; keys are
case-insensitive.

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `touch` | `true` / `false` | `true` | Enable the Android Auto touch-input passthrough. |
| `hud` | `true` / `false` | `true` | Enable HUD guidance forwarding (turn arrow + distance to the head-up display). |
| `hud_transport` | `svcnavi` / `vbs` | `svcnavi` | Which path HUD guidance takes (only relevant when `hud = true`). |
| `force_street_name` | `true` / `false` | `false` | Force the Android Auto street name onto the HUD street line even where the OEM blanks it (see the EU note below). |
| `hud_fold_latin` | `true` / `false` | `true` | Fold HUD-unrenderable precomposed Latin letters in street names to their base forms (see the note below). |
| `compass_always_on` | `true` / `false` | `false` | Keep the instrument-cluster compass alive at all times, instead of only above ~9 km/h and only while the NVRAM speed restriction is enabled. Read by the `svcjcinavi` shim; requires it to be preloaded (see the note below). |
| `use_protocol_v1_6` | `true` / `false` | `false` | Advertise Android Auto GAL 1.6 so the phone sends the 1.6 navigation protocol (maneuver / lanes / distance) for the HUD. Read by the `aap_service` shim; requires it to be preloaded (see the note below). |
| `roundabout_prepend_exit_number` | `true` / `false` | `false` | For a roundabout that gives an exit number but no exit angle (GAL 1.6 only), prepend its number to the HUD street name. |
| `aa_audio_low_latency` | `true` / `false` | `false` | Fix Android Auto guidance-audio clipping at the head, beginning, and tail of prompts — one switch for the whole audio-cutoff fix. Needs **both** the `aap_service` and `blmjciaapa` shims preloaded (see the note below). |

Booleans are lenient — `true`/`1`/`yes`/`on` and `false`/`0`/`no`/`off`
are all accepted.

`hud_transport` picks the route for HUD frames:

- **`svcnavi`** (default) — emit guidance to the OEM `svcjcinavi`
  navigation service, which becomes the single writer of the HUD frame.
  Required for the speed-limit display and clean coexistence with OEM
  nav, but needs the navigation SD card.
  The *HUD-merge* part of `libpatch-svcjcinavi.so` only matters with this
  transport; its `compass_always_on` part is independent of both the
  transport and the SD card. (`svcjcinavi` is accepted as an alias.)
- **`vbs`** — write the HUD frame directly to `com.jci.vbs.navi`. Works
  with no navigation SD card, but it is one of two writers, so it can
  conflict with OEM nav guidance.

- **`force_street_name`** controls whether the Android Auto street name is
shown on the HUD's secondary line:

  - On **NA** firmware the street name already appears with the defaults,
    so this option can be left `false`.
  - On **EU** firmware the OEM navigation service blanks the HUD street
    line for the head-up display types used in that market — the turn
    arrow and distance still show, but the street stays empty. Set
    **`force_street_name = true`** to make the merge shim re-assert the
    real Android Auto street onto that line. This is the specific setting needed for street-name display on EU units; it has no effect on the maneuver arrow or distance.

The option only applies with `hud_transport = svcnavi` (the
`libpatch-svcjcinavi.so` merge shim must be installed and patched into
`jcinavi`, as in step 2). It is harmless on firmware that does not blank
the street, so it is safe to enable everywhere if preferred.

- **`compass_always_on`** fixes the instrument-cluster compass, which is dead
  far more often than most owners realise.

  The nav service only publishes a heading while one internal flag is set, and
  that flag follows a vehicle-speed signal computed by the VIP: it goes to `1`
  above **9.00 km/h** and back to `0` below **7.00 km/h**. So on a stock unit the
  compass works *only while driving*. On top of that, the head unit pins the same
  flag to `0` outright whenever the NVRAM key `bus_bcm_speed_restriction` is set
  to `disable` — which is exactly what the usual touchscreen-while-driving tweak
  does — and then the compass never appears at any speed.

  Turning the speed restriction back on to recover the compass has a price: the
  same setting re-arms the OEM navigation app's own lockout, so a destination can
  no longer be entered while moving. That is the trade this option removes. It
  forces the flag non-zero **inside the nav service only**, so the NVRAM key can
  stay `disable` — Android Auto, CarPlay, DVD, LVDS and the nav app all stay
  unrestricted — and the compass still runs. Genuine non-zero readings are passed
  through untouched; only zeros are lifted.

  Requires `libpatch-svcjcinavi.so` preloaded into `jcinavi` (step 2). **No
  navigation SD card is needed** — this works without one.

  While stationary there is no valid heading to publish (the source is a GPS
  course-over-ground, meaningless at rest), and the cluster usually shows `N` after
  start — the same as the OEM navigation app does.

- **`hud_fold_latin`** handles a HUD-font limitation: the head-up display's
  ECU font only has glyphs for Unicode below roughly U+0800, so any
  3-byte-UTF-8 character renders as a blank space. For **precomposed
  Latin letters** — the Latin Extended Additional block (`U+1E00`–`U+1EFF`),
  where each character is a base letter plus diacritics — we can fold to
  the renderable base instead of losing them. When enabled, each such
  letter is folded to its base, keeping a renderable
  accent where the letter is defined by one (`ă â ê ô ơ ư`) and dropping
  only the marks the font can't draw: `ễ`→`ê`, `ớ`→`ơ`, `ậ`→`â`,
  `ḍ`→`d`, `ṛ`→`r`, `ẁ`→`w`. So a name the font shows as `Nguy n Phư c`
  reads `Nguyên Phươc`. It affects only the HUD street strip; all other
  text — ASCII, the already-renderable `ă â ê ô ơ ư đ`, and non-Latin
  ≥U+0800 scripts (CJK, Thai, …, which have no Latin base and stay blank)
  — is passed through unchanged, so it is harmless everywhere. Set
  `false` to send names verbatim. Applies to both HUD transports.

- **`use_protocol_v1_6`** advertises Android Auto **GAL 1.6** to the phone (the
  OEM advertises 1.5). At 1.5 the phone only sends the deprecated turn events; at
  1.6 it sends the full navigation protocol — maneuver, **lanes**, distance — which
  is what feeds the richer HUD guidance. The `aap_service` shim catches and decodes
  the 1.6 frames and forwards them to the `blmjciaapa` shim, which renders them on
  the HUD via the same `hud` / `hud_transport` path as the 1.5 guidance. So this
  needs the `aap_service` shim preloaded (step 2) **in addition to** `jciAAPA`; the
  other libraries ignore the key. The GAL version is negotiated once per session,
  so a change takes effect on the next Android Auto connection (restart `jciAAPA`
  + `aap_service`, or reconnect the phone). Default `false` = stock 1.5.
  Lane guidance is part of the 1.6 protocol, so it is sent automatically
  whenever `use_protocol_v1_6 = true` (and never at stock 1.5).

- **`aa_audio_low_latency`** fixes Android Auto **guidance-audio clipping** — one
  opt-in switch covering all three edges of a spoken prompt. It is `false` by default
  because it touches the live audio path; off, the unit behaves stock.

  - **Head / start** — AA playback's per-client ALSA start threshold is lowered from the
    full negotiated buffer to one period, and the guidance pipeline self-activates after a
    short pre-roll, so dense / back-to-back prompts don't clip their first few
    milliseconds.
  - **Beginning / cold open** — one silent `dmix` client is held open inside `jciAAPA`
    (0 % CPU — it streams nothing, just holds the fd) so the shared `hw:0,0` output device
    stays warm. Without it the first prompt over a tuner (FM / DAB) pays a multi-second
    cold open and loses its opening; because a tuner never keeps that device open, the
    cost otherwise recurs on *every* prompt.
  - **Tail / end** — the too-short end-of-stream drain wait is extended so the tail plays
    out instead of being flushed, and the amplifier mix is held until the drain actually
    finishes (coupled by a cross-process "tail drained" event), so the amp releases exactly
    when the audio ends rather than after a guessed delay — no chopped last syllable, no
    dead-air.

  The head/start and tail/end halves live in the `aap_service` shim (`audio.cpp`,
  `goactive.cpp`, `sem_clockfix.cpp`); the beginning cold-open and amp-hold halves live in
  `blmjciaapa` (`audio_keepalive.cpp`, `audio_stopdelay.cpp`). So it needs **both** shims
  preloaded (step 2). Every piece is gated by this one key — there is no separate audio
  switch.

After editing `libpatch.conf`, restart the affected service(s) —
`jciAAPA`, `jcinavi` if you patched it, and `aap_service` if you enabled
`use_protocol_v1_6` or `aa_audio_low_latency` — or reboot for the change to take effect.
The config is read once at library load.

## Optional: edit `libpatch.conf` from the head unit

Everything above configures the mod by editing `libpatch.conf` over ssh. You can
instead get a **Settings → System → Android Auto Mod** screen on the unit that
reads and writes the same file, so the on/off keys can be flipped from the
touchscreen.

It is entirely optional and independent of the shims: nothing above needs it, and
it changes no shim behaviour — it only edits the file the shims already read.

### How it works

The Opera GUI has no filesystem access; it can only talk to its three
WebSockets. One of them, `ws://127.0.0.1:2766`, is **`jciDbapi`**, and `jciDbapi`
is a plugin host: it reads `/jci/dbapi/plugins.xml` and `dlopen`s
`/jci/dbapi/plugins/libdbapi<name>.so` for each `<plugin name="<name>"/>`, then
calls into it whenever a GUI app does
`framework.sendRequestToDbapi(uiaId, cb, "<name>", "<method>", params)`.

So there are three pieces, and no new daemon or listening port:

| Piece | Where it goes | What it does |
| --- | --- | --- |
| `libdbapiaamod.so` | `/jci/dbapi/plugins/` | The plugin. `GetConfig` returns the exposed keys with their current effective values; `SetConfig` validates one key/value against that same list and rewrites the line(s) of `libpatch.conf` that set it. |
| `plugins.xml` line | `/jci/dbapi/` | Registers the plugin under the api name `aamod`. |
| `aaModSettings.js` | `/jci/gui/apps/syssettings/aamod/` | The screen. Adds the row, renders whatever `GetConfig` hands back, calls `SetConfig` on a toggle. |

The screen does **not** add an MMUI context — that would mean patching
`libjciuiasyssettings.so`. It borrows the OEM `About` context: the new row sends
the OEM's own `SelectAbout` event, MMUI performs its ordinary
`SystemTab → About` transition, and the list is dressed with the mod's title and
rows for that one visit. So the back button, focus stack and at-speed lockout all
behave exactly as the OEM's. The real About screen is untouched — its own row
still opens version info.

The mod's change to the OEM `syssettingsApp.js` is **one added `loadScript`
line**; every wrapper lives in `aaModSettings.js`.

### What it exposes

On this branch, **every key** `libpatch.conf` has: the thirteen booleans — `touch`,
`hud`, `force_street_name`, `hud_fold_latin`, `use_protocol_v1_6`,
`roundabout_prepend_exit_number`, `compass_always_on`,
`bt_pairing_bypass_all_devices`, `bt_pairing_show_device_notification`,
`mute_pauses_phone`, `unmute_starts_playback`, `block_headunit_media_play`,
`aa_audio_low_latency` — plus the one two-way choice,
`hud_transport` (`svcnavi` / `vbs`).

`hud_maneuver_max_distance_m` is **shown but not editable**: the row reads its
current value (in metres, or "Always" for 0), and the plugin refuses any write to it.
Change it in the file.

Free-text keys are deliberately **not** editable here and stay file-only: they
would need the OEM keyboard control on a second borrowed MMUI context, and a
typo'd value entered on a touchscreen is a worse failure than editing the file
over ssh. (There are none on this branch — the wireless-AA branches add
`aa_ssid`, `aa_psk`, `aa_ap_ip`, … which the screen leaves alone.)

`SetConfig` rejects any key not in the plugin's own schema, so the screen cannot
write arbitrary config. The schema lives in one table in
[`mazda/dbapi/aamod.cpp`](../mazda/dbapi/aamod.cpp) — exposing a newly added key
is a change to that table and nothing else, since the GUI renders whatever
`GetConfig` hands it and never names a key itself.

Because the key set differs per branch, `test/gui/aamod_settings_test.js` audits
it: it fails if the screen exposes a key `common/config.h` does not parse (a
toggle that writes a line no shim reads), if an exposed key has no English label,
or if a label survives for a key that is gone — and it prints which keys
`config.h` parses but the screen leaves file-only. Run it after any rebase.

Keys the file does not set are shown at their built-in default, and changing one
appends it (under a marker comment) rather than rewriting the file — your
comments, ordering and spacing are preserved byte-for-byte.

### Language

The screen ships **English** and **Czech**; the Czech is a draft and has not been
reviewed by a native speaker. It follows the unit's language setting, falling
back to English per string, so a partial translation is fine.

Translations live GUI-side in `resources/gui/aaModSettings.js`, in one `STRINGS`
table keyed by the `LANG_CODES` values from the framework's `Localization.js`
(`LANGS_DE_GERMAN`, `LANGS_PL_POLISH`, …). **To add a language**, copy the
`LANGS_CZ_CZECH` block, change the key, translate the values — nothing else to
touch. Keep labels short: they render in a narrow list row, and a label that fits
in English often does not in translation.

The OEM route (`text1Id` plus a string in each `syssettingsAppDict_<locale>.js`)
is deliberately not used — there are 42 of those files, and editing them all is
the wholesale change to JCI's code that the one-line hook exists to avoid. The
one string that *is* localised the OEM way is the dialog's OK button
(`common.Ok`), which every language already has.

`test/gui/aamod_settings_test.js` audits the table: it fails on a translated
string id the English base does not define (otherwise a typo silently falls back
to English forever) or on a translation that drops a `{0}`/`{1}` placeholder, and
prints per-language coverage.

### Install

```sh
mount -o remount,rw /

# 1. the plugin, straight into the directory the host builds its path from.
cp /mnt/sdb1/libdbapiaamod.so /jci/dbapi/plugins/
chmod 0644 /jci/dbapi/plugins/libdbapiaamod.so

# 2. register it: adds <plugin name="aamod"/> to plugins.xml
cp /jci/dbapi/plugins.xml /jci/dbapi/plugins.xml.orig
cd /jci/dbapi && patch -p1 < /mnt/sdb1/plugins.xml.patch

# 3. the screen
mkdir -p /jci/gui/apps/syssettings/aamod
cp /mnt/sdb1/aaModSettings.js /jci/gui/apps/syssettings/aamod/

# 4. the hook in syssettingsApp.js that loads the screen
cp /jci/gui/apps/syssettings/js/syssettingsApp.js \
   /jci/gui/apps/syssettings/js/syssettingsApp.js.orig
cd /jci/gui/apps/syssettings/js && patch -p1 < /mnt/sdb1/syssettingsApp.js.patch

sync
```

Steps 2 and 4 run BusyBox `patch` **in the target file's own directory**: the
patch headers name plain basenames (`a/plugins.xml`), so `-p1` resolves to the
file sitting right there. The `.patch` file itself can be anywhere.

Two things about BusyBox 1.19.2's `patch`, which is much stricter than GNU's and
is what runs on the car:

- It does **not** strip a trailing CR from the filename in the `---`/`+++`
  headers. `syssettingsApp.js` is a CRLF file, so a diff of it legitimately has
  CR at the end of every payload line — but any tool that CR-terminates the
  *header* lines too makes `patch` try to open `syssettingsApp.js<CR>` and fail
  with `No such file or directory`, printed with the CR still in it so the
  message overwrites itself on screen. The shipped patches keep headers LF-only
  and `test/gui/aamod_settings_test.js` fails if that ever regresses. If you
  regenerate a patch yourself, watch for this.
- It has no fuzz, and `-l` does not rescue a line-ending mismatch. So **patch a
  pristine copy of the file, not one that has been through an editor** that
  normalised CRLF to LF — that fails with *"different line endings"*.

The stock file to patch against is:

```
md5  49e9d40b1a389ee47533006a59e6df31
size 276511 bytes
```

Worth checking your unit matches before you start (BusyBox has `md5sum`):

```sh
md5sum /jci/gui/apps/syssettings/js/syssettingsApp.js
```

**If the checksum differs, stop** — something else has already edited the file,
or it is a firmware build the screen was not made for. **If `patch` refuses**,
put the backup back and stop:

```sh
cp /jci/gui/apps/syssettings/js/syssettingsApp.js.orig \
   /jci/gui/apps/syssettings/js/syssettingsApp.js
```

Do not edit the file by hand instead: the screen wraps internals of this exact
Settings app, and on a different one it can break the whole Settings app, not
just the mod screen. Apply each patch once only.

Then restart the two services (or reboot):

```sh
smctl -r -n jciDbapi
smctl -r -n Opera
```

`jciDbapi` picks up the plugin at start, and `Opera` re-reads the GUI files.

### Using it

**Settings → System → Android Auto Mod.** Toggling a row only *records* the
change — the **Apply** row at the bottom shows how many are pending and is greyed
out when there are none. Apply writes them all, then reports once. Leaving the
screen without applying discards the edits (nothing was written), and re-entering
always re-reads the file, so the screen never shows state the file does not have.

Because every shim reads `libpatch.conf` once at load, a change only takes effect
after a restart, so the dialog after Apply asks for a **CMU reboot (hold NAV +
MUTE)** — the one action that reliably restarts every affected service. That
dialog is navigable with the rotary commander as well as by touch.

Rows are locked out while the vehicle is moving, the same as the OEM rows around
them.

### Notes

- `/jci` is not on `/data_persist`, so — exactly like the `sm.conf` edit in step
  2 of the main install — all four pieces (the plugin, the `plugins.xml` line,
  `aaModSettings.js`, the `syssettingsApp.js` line) are lost on a firmware update
  and need reapplying.
- The plugin deliberately goes **in `/jci/dbapi/plugins/`** rather than living in
  `/data_persist` behind a symlink. `jciDbapi` is an early `autorun` service, and
  a symlink would make it depend on `/data_persist` already being mounted when
  the host `dlopen`s it — if it isn't, the plugin is silently absent until
  something restarts the service. `/jci` is on the rootfs and always there.
  `plugins.xml` has to be edited in that same directory anyway, and is lost on a
  firmware update either way, so the symlink buys nothing.
- Verify the plugin loaded by grepping the device log for
  `[libpatch-dbapiaamod]`; `jciDbapi` also logs
  `Plugin aamod (...) loaded and initialized.` The GUI side logs under `aaMod:`.
- If the plugin is missing or failed to load, the screen still opens and reports
  the failure in a dialog (after a 10 s wait for the answer) rather than showing
  values it cannot back up.
- `jciDbapi` is a `reset_board` service: if it crashes, the **whole unit
  reboots**. The plugin runs inside it, so it keeps its footprint small — it
  binds the JSON library once at start and refuses to initialise if that fails
  (`jciDbapi` then skips it and carries on). If you would rather not have code of
  ours in that process, skip this optional part.

## Uninstall / disable

Remove (or comment out) the `<environ_var>` line(s) you added in
`/jci/sm/sm.conf` (the `jciAAPA` one, the `jcinavi` one if present, and the
`aap_service` one if you enabled 1.6), restore the original
`/etc/aap_system_attributes*.xml` files from the `.orig` backups, then restart
the affected services (`smctl -r -n jciAAPA` / `smctl -r -n jcinavi` /
`smctl -r -n aap_service`) or reboot. The
`/data_persist/oem-aa-mod/` directory can be deleted afterwards.

If you installed the Settings screen, also restore
`/jci/dbapi/plugins.xml` and `/jci/gui/apps/syssettings/js/syssettingsApp.js`
from their `.orig` backups, remove
`/jci/dbapi/plugins/libdbapiaamod.so` and the
`/jci/gui/apps/syssettings/aamod/` directory, then restart `jciDbapi` and
`Opera` (or reboot). Dropping just the `plugins.xml` line is enough to disable
the plugin; dropping just the `syssettingsApp.js` line is enough to remove the
screen.
