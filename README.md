# yabridge

> [!NOTE]
> This is a modified version of
> [robbert-vdh/yabridge](https://github.com/robbert-vdh/yabridge) that adds
> support for 32-bit Windows plugins with Wine's new WoW64 mode, which is what
> Arch Linux' `wine` and `wine-staging` packages use, along with fixes for
> 32-bit plugins in Renoise and REAPER and for crashes when the Wine plugin host
> dies. See [Changes in this fork](#changes-in-this-fork) for everything it
> adds, and [32-bit bitbridge](#32-bit-bitbridge) for build instructions.
> Please don't report issues with this version to the upstream project. Use
> this repository's [issue
> tracker](https://github.com/jartificer/yabridge-custom/issues) instead.

> [!CAUTION]
> The upstream yabridge project's GitHub page is its only official page. If you
> came from a website claiming to be related to yabridge, then that website was
> likely fake and trying to spread malware.

Yet Another way to use Windows audio plugins on Linux. Yabridge seamlessly
supports using both 32-bit and 64-bit Windows VST2, VST3, and CLAP plugins in
64-bit Linux plugin hosts as if they were native plugins, with optional support
for [plugin groups](#plugin-groups) to enable inter-plugin communication for
VST2 plugins and quick startup times. Its modern concurrent architecture and
focus on transparency allows yabridge to be both fast and highly compatible,
while also staying easy to debug and maintain.

![yabridge screenshot](https://raw.githubusercontent.com/robbert-vdh/yabridge/master/screenshot.png)

### Table of contents

- [**Changes in this fork**](#changes-in-this-fork)
  - [Environment variables](#environment-variables)
  - [Tested with](#tested-with)
- [**Usage**](#usage)
  - [Bitbridge](#bitbridge)
  - [Wine prefixes](#wine-prefixes)
  - [Using different Wine versions](#using-different-wine-versions)
  - [Drag-and-drop](#drag-and-drop)
  - [Input focus grabbing](#input-focus-grabbing)
- [Configuration](#configuration)
  - [Plugin groups](#plugin-groups)
  - [Compatibility options](#compatibility-options)
  - [Example](#example)
- [**Known issues and fixes**](#known-issues-and-fixes)
- [**Troubleshooting common issues**](#troubleshooting-common-issues)
- [**Performance tuning**](#performance-tuning)
  - [Environment configuration](#environment-configuration)
- [Building](#building)
  - [32-bit bitbridge](#32-bit-bitbridge)
- [Debugging](#debugging)
  - [Attaching a debugger](#attaching-a-debugger)

## Changes in this fork

This fork is based on upstream yabridge at commit
[`b580a9f7`](https://github.com/robbert-vdh/yabridge/commit/b580a9f7). Its main
goal is making 32-bit Windows plugins work reliably on modern Wine builds, and
in hosts like Renoise and REAPER that call into plugins from several threads at
once. Everything below is in addition to what upstream yabridge does. The
[changelog](CHANGELOG.md) lists the same changes in release note form.

### 32-bit plugins with Wine's new WoW64 mode

Upstream yabridge builds its 32-bit plugin host (`yabridge-host-32.exe`) as a
32-bit Winelib application. Wine's new WoW64 mode, which Arch Linux' `wine` and
`wine-staging` packages use, no longer has a 32-bit Linux side, so that host
can't run there at all ([Wine
#58377](https://bugs.winehq.org/show_bug.cgi?id=58377)). This fork builds the
32-bit host as a plain 32-bit Windows (PE) executable with MinGW-w64 instead,
which runs on both new and old style WoW64 Wine. The native plugin library
starts it through `wine` (or `$WINELOADER`). A PE executable can only use the
Windows API, so everything the host used to do through Linux APIs had to be
replaced:

- **Loopback TCP instead of Unix domain sockets.** Wine's Winsock doesn't
  support `AF_UNIX`, so the 32-bit host talks to the native plugin over TCP
  sockets bound to `127.0.0.1`. Endpoints are still identified by paths: the
  listening side writes its port number to the endpoint path. Since any user on
  the machine can connect to a loopback port, every connection starts with a
  challenge-response handshake. The listening side sends a random nonce and the
  connecting side answers with `SHA256("yabridge-tcp-auth" || token || nonce)`,
  where the token is a 64 byte secret stored in
  `$XDG_RUNTIME_DIR/yabridge-tcp-token-<uid>`, readable only by your user, and
  passed to the Wine host through the `YABRIDGE_TCP_TOKEN` environment
  variable.
- **Fewer Winsock round trips.** Every Winsock call in Wine involves a round trip
  to `wineserver`. Messages are now written with a single gather write (size and
  payload together), and the PE host reads through a read-ahead buffer so most
  messages arrive with one `recv()` call.
- **Shared audio buffers through Win32 file mappings.** The `/dev/shm` audio
  buffers are opened with `CreateFileMapping()`/`MapViewOfFile()`, which Wine
  backs with a `MAP_SHARED` mapping of the same file the native plugin uses.
- **An X11 relay for editor embedding.** The PE host can't open an X11
  connection itself. It contains a minimal implementation of the parts of libxcb
  yabridge uses (`src/wine-host/xcb-pe`), which talks to a small relay in the
  native plugin library (`src/plugin/x11-relay.cpp`) that forwards the traffic
  to the real X11 server. Editor embedding and drag-and-drop work as before.
- **Realtime scheduling through raw system calls.** Wine only maps Windows
  thread priorities to nice values, so the 32-bit host requests `SCHED_FIFO` by
  calling `sched_setscheduler()` directly with `int 0x80`. On x86_64 Wine traps
  system calls made from Windows code using Linux' syscall user dispatch, which
  would crash the host, so the host briefly flips Wine's per-thread syscall
  dispatch selector in the 64-bit TEB to let the call through. A vectored
  exception handler catches the fault on kernels without 32-bit syscall
  emulation, and `YABRIDGE_NO_LINUX_SYSCALLS=1` turns this off entirely.

See [32-bit bitbridge](#32-bit-bitbridge) for build instructions.

### Futex based audio processing for the 32-bit host

Waking up a thread that's blocked on a Winsock socket in Wine goes through
`wineserver`. For audio processing that meant extra latency on every cycle and
spikes of tens of milliseconds, which caused xruns. For VST2 plugins in the
32-bit host, audio processing requests now bypass the sockets entirely:

- Each plugin instance gets a 64 KiB shared memory object with a small header
  (`ProcessSyncHeader` in `src/common/process-sync.h`). The native plugin writes
  the serialized request, increments `request_seq` and wakes a futex. The Wine
  host's audio thread waits on that futex, processes the request, sets
  `response_seq` to the same value and wakes the other futex. Audio samples
  still go through the existing shared audio buffers, so only small requests
  pass through this channel.
- Both sides use Linux futexes directly. The 32-bit host makes the
  `futex()` system calls through `int 0x80`, the same way as the scheduling
  call above. Its waits always use a timeout, because an interrupted futex wait
  with a timeout returns `-EINTR` after a signal handler ran instead of being
  restarted by the kernel, and a restarted system call would be trapped again
  since Wine's signal handlers re-enable syscall trapping.
- MIDI events the DAW sends with `effProcessEvents()` are queued (up to 1024
  events) and sent along with the next processing request instead of needing
  their own socket round trip on the audio thread. The Wine host passes them to
  the plugin right before processing. SysEx events and very large batches still
  go through the regular dispatch path, and queued events are dropped when the
  plugin gets suspended.
- The channel is negotiated during startup: the host announces it before
  sending the plugin's `AEffect` struct, and the native plugin accepts it before
  sending its configuration back. Mismatched yabridge versions therefore keep
  using the socket. `YABRIDGE_NO_FUTEX=1` on either side also disables it.

VST3 and CLAP plugins in the 32-bit host still use the socket for audio
processing.

### Hosts that call plugins from multiple threads (Renoise)

Hosts like Renoise (with plugin sandboxing disabled) and REAPER call a plugin's
`dispatcher()` from several threads at the same time, for instance
`effProcessEvents()` from the audio thread for every note while the GUI thread
asks for program names. Yabridge handles overlapping calls by opening an extra
ad hoc connection to the Wine host for each one. With the 32-bit host's TCP
sockets this used to break 32-bit plugins in Renoise after a few notes:

- Setting up an ad hoc connection could fail spuriously, for instance with
  `connect()` returning `ECONNABORTED`, or because a signal interrupted the
  handshake's `poll()`, which was never restarted. Yabridge then concluded the
  Wine host had crashed and the plugin went silent for good. Connection setup
  is now retried (after 0, 1 and 10 ms), interrupted waits in the handshake and
  in `accept()` are retried, and if a connection still can't be made the request
  waits up to two seconds for the primary socket instead.
- Ad hoc connections are now pooled. After a request has been handled, its
  connection goes back into a pool of up to 8 idle connections and gets reused
  for the next overlapping request, and the receiving side keeps serving
  requests on it until it's closed. This avoids a TCP handshake, an
  authentication handshake and a new thread on the Wine side for every
  overlapping call.

### REAPER audio thread fixes

- REAPER calls `effVendorSpecific('PreS', 'GRdB')` from its audio thread on
  every processing cycle to ask for the plugin's gain reduction. Upstream runs
  all `effVendorSpecific()` calls on the Wine host's GUI thread, because Renoise
  uses PreSonus' `'PreS'`/`'AeCs'` extension through that opcode to set the
  editor's content scale. Every processing cycle then had to wait for the GUI
  thread, causing xruns whenever the plugin's editor kept that thread busy. Now
  only the content scale call runs on the GUI thread.
- Almost no plugins support the gain reduction query. Once a plugin has answered
  it with 0 64 times in a row, the native plugin library answers it by itself
  without a round trip to the Wine host. Any other answer resets the count, so
  plugins that do support it keep getting the query.

### Crash and hang resilience

Upstream yabridge takes the DAW down with it in several situations where the
Wine plugin host fails. This fork handles those situations gracefully:

- **The host dies before connecting.** If the Wine host exits before it has
  connected its sockets (for instance because Wine can't start in the plugin's
  prefix), waiting for the connection is aborted and the plugin fails to load.
  Upstream calls `std::terminate()` here, which crashes the DAW.
- **The host dies while the plugin is in use.** The next call from the DAW used
  to throw an uncaught `std::system_error` (`read: End of file`) into the DAW's
  thread. The VST2, VST3 and CLAP bridges now catch lost connections in their
  send paths. The first time this happens yabridge logs an error and shows a
  desktop notification, and from then on the plugin outputs silence and returns
  default values without contacting the host again.
- **The host hangs.** With the futex channel there's no socket that gets closed
  when the host crashes, and Wine may attach a debugger that keeps a crashed
  process alive. The native plugin checks once per second whether the host is
  still running, and gives up on a host that takes longer than 10 seconds for
  a single processing cycle. The plugin then goes silent as above.
- **The DAW closes a plugin mid-call.** `effClose()` now waits up to two
  seconds for other calls into the plugin that are still in progress (as
  REAPER's audio thread can be) before freeing what those calls use, and it
  no longer talks to a host that has already been lost.
- **Removing a plugin with a hung host.** The Wine host now gets `SIGKILL` if
  it hasn't exited two seconds after `SIGINT`, so removing the plugin doesn't
  hang the DAW.

### Packaging and testing

- [`tools/arch/PKGBUILD`](tools/arch/PKGBUILD) builds and installs the native
  libraries, both plugin hosts and `yabridgectl` from the local checkout on
  Arch Linux. Makepkg's Linux `CFLAGS`/`LDFLAGS` are kept away from the MinGW
  build, where options like `-z relro` would break the linker.
- `yabridgectl status` and its post-sync checks understand that the 32-bit host
  is a plain Windows executable without an accompanying `.exe.so` file.
- [`tools/smoke-test`](tools/smoke-test/run-smoke-test.sh) contains an
  end-to-end test with a minimal 32-bit and 64-bit Windows VST2 plugin and a
  minimal native VST2 host. It covers both plugin hosts, 32-bit and 64-bit
  plugins in the same process, concurrent `dispatcher()` calls interrupted by
  signals, the gain reduction query, and MIDI delivery through the futex
  channel.

### Environment variables

Besides the variables upstream yabridge supports (see
[Debugging](#debugging) and [Troubleshooting](#troubleshooting-common-issues)),
this version reads the following ones. They're only meant for working around
problems, and you normally don't need to set any of them.

| Variable                       | Effect                                                                                                                                                                                                    |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `YABRIDGE_NO_FUTEX=1`          | Send VST2 audio processing requests for the 32-bit host through the socket instead of the futex based shared memory channel.                                                                             |
| `YABRIDGE_NO_LINUX_SYSCALLS=1` | Stop the 32-bit host from making Linux system calls directly. This disables `SCHED_FIFO` realtime scheduling for its audio threads, and with it the futex channel, since that also needs those calls. |
| `WINELOADER`                   | As with upstream, the Wine binary to use. In this version it's also used to start the 32-bit host, which is a regular Windows executable that has to be run through Wine.                               |

`YABRIDGE_TCP_TOKEN` is also set by yabridge itself to pass the TCP
authentication token to the 32-bit host. Don't set it yourself.

### Tested with

This version has been developed and tested on Arch Linux with `wine-staging`
11.19 in Wine's new WoW64 mode, with **REAPER 7.82**, and with the 32-bit and
64-bit plugins from the [smoke test](tools/smoke-test/run-smoke-test.sh). The
Renoise specific fixes (calls into a plugin from several threads at once) are
covered by the smoke test. Other hosts that work with upstream yabridge, like
Bitwig Studio, Carla, Ardour, and Renoise, should work the same way, since the
64-bit plugin path is unchanged apart from the crash fixes above.

> [!IMPORTANT]
> Like upstream yabridge, this doesn't work with DAWs installed as `flatpak`
> packages. Use native packages of your DAW instead.

## Usage

0. You'll need a recent version of Wine (Staging). Unlike upstream yabridge,
   this version also supports 32-bit plugins with Wine builds that use the new
   WoW64 mode, like Arch Linux' `wine` and `wine-staging` packages, so any
   current Wine package from your distro or from WineHQ will do.

   For a general overview on how to use Wine to install Windows applications,
   check out Wine's [user
   guide](https://wiki.winehq.org/Wine_User%27s_Guide#Using_Wine).

1. This version of yabridge isn't available as a distro package or as a
   prebuilt download, so you'll need to build it yourself. Don't install it
   alongside your distro's `yabridge` package, since both would try to manage
   the same plugins.

   - On **Arch Linux**, the [`PKGBUILD`](tools/arch/PKGBUILD) in `tools/arch`
     builds and installs the native plugin libraries, both plugin hosts, and
     `yabridgectl` from your local checkout. It replaces the `yabridge` and
     `yabridgectl` packages if you have those installed.

     ```shell
     git clone https://github.com/jartificer/yabridge-custom.git
     cd yabridge-custom/tools/arch
     makepkg -si
     ```

     This installs everything under `/usr`, where yabridge and yabridgectl find
     it automatically. Rerun `makepkg -si` after pulling new changes, and then
     run `yabridgectl sync` to finish updating.

   - On **other distros**, follow the instructions in the
     [Building](#building) section, including the [32-bit
     bitbridge](#32-bit-bitbridge) part if you want to use 32-bit plugins.
     Then copy the build results to `~/.local/share/yabridge`:

     ```shell
     mkdir -p ~/.local/share/yabridge
     cp build/libyabridge-*.so build/yabridge-host.exe build/yabridge-host.exe.so \
       build-32/yabridge-host-32.exe tools/yabridgectl/target/release/yabridgectl \
       ~/.local/share/yabridge/
     ```

     **Whenever any step after this mentions running `yabridgectl <something>`,
     then you should run `~/.local/share/yabridge/yabridgectl <something>`
     instead.**

     Alternatively, you can also add that directory to your shell's search
     path. That way you can run `yabridgectl` directly. If you don't know what
     that means, then add `export PATH="$PATH:$HOME/.local/share/yabridge"` to
     the end of `~/.bashrc` and reopen your terminal.

2. Setting up and updating yabridge for your plugins is done though the
   `yabridgectl` command line utility. The basic idea is that you first install
   your Windows plugins to their default locations within a Wine prefix just
   like you would on regular Windows. and yabridgectl then manages those plugin
   directories for you. You then tell yabridgectl where it can find those
   plugins so it can manage them for you. That way you only ever need to run a
   single command whenever you install or remove a plugin. Both yabridge and
   yabridgectl will automatically detect your yabridge installation if you used
   one of the installation methods from step 1.

   To tell yabridgectl where it can find your Windows VST2, VST3, and CLAP
   plugins, you can use yabridgectl's `add`, `rm` and `list` commands to add,
   remove, and list the plugin directories yabridgectl is managing for you. You
   can also use `yabridgectl status` to get an overview of the current settings
   and the installation status for all of your plugins.

   1. To add the most common VST2 plugin directory in the default Wine prefix, use
      `yabridgectl add "$HOME/.wine/drive_c/Program Files/Steinberg/VstPlugins"`.
      This directory may be capitalized as `VSTPlugins` on your system, and some
      plugins may also install themselves to a similar directory directly inside
      of Program Files. 32-bit VST2 plugins are usually installed to the same
      directories under `Program Files (x86)` instead, so add that one as well
      if you use them.
   2. VST3 plugins under Windows are always installed to
      `C:\Program Files\Common Files\VST3`, and you can use
      `yabridgectl add "$HOME/.wine/drive_c/Program Files/Common Files/VST3"` to
      add that directory to yabridge.
   3. CLAP plugins under Windows are always installed to
      `C:\Program Files\Common Files\CLAP`, and you can use
      `yabridgectl add "$HOME/.wine/drive_c/Program Files/Common Files/CLAP"` to
      add that directory to yabridge.

3. Finally, you'll need to run `yabridgectl sync` to finish setting up yabridge
   for all of your plugins. After doing so, your VST2, VST3, and CLAP plugins
   will be set up in `~/.vst/yabridge`, `~/.vst3/yabridge`, and
   `~/.clap/yabridge` respectively. Make sure your DAW searches `~/.vst`,
   `~/.vst3`, and `~/.clap` for VST2, VST3, and CLAP plugins and you will be
   good to go.

### Bitbridge

Yabridge can also load 32-bit Windows plugins so you can use them in your 64-bit
Linux DAW. Yabridge will automatically detect whether a plugin is 32-bit or
64-bit on startup and it will handle it accordingly. 32-bit and 64-bit plugins
can be used side by side in the same DAW session. This requires the 32-bit
plugin host, `yabridge-host-32.exe`, which the Arch `PKGBUILD` always builds, and
which you can build yourself as described under [32-bit
bitbridge](#32-bit-bitbridge).

In this version of yabridge the 32-bit plugin host (`yabridge-host-32.exe`) is a
regular 32-bit Windows executable instead of a 32-bit Winelib application. This
makes it work with Wine's new WoW64 mode, which is what Arch Linux' `wine` and
`wine-staging` packages have been using since mid 2025. In that mode Wine no
longer has a 32-bit Linux side, so the old 32-bit Winelib host could not run
there. See [32-bit bitbridge](#32-bit-bitbridge) for more information.

### Wine prefixes

It is also possible to use yabridge with multiple Wine prefixes at the same
time. Yabridge will automatically detect and use the Wine prefix the Windows
plugin's `.dll`, `.vst3`, or `.clap` file is located in. Alternatively, you can
set the `WINEPREFIX` environment variable to override the Wine prefix for _all
yabridge plugins_.

With Wine's new WoW64 mode, 32-bit only Wine prefixes (created with
`WINEARCH=win32`) can't be used at all. Install your 32-bit plugins into a
regular 64-bit prefix instead, where they'll end up under `Program Files (x86)`.
yabridge then uses the 32-bit host for those plugins automatically.

### Using different Wine versions

If the `WINELOADER` environment variable is set, then yabridge will use that
binary to run Wine. Otherwise it will use whatever `wine` binary is available on
the search path instead.

See
[`yabridge-bottles-wineloader`](https://github.com/microfortnight/yabridge-bottles-wineloader)
for a community project to integrate yabridge with
[Bottles](https://usebottles.com/).

### Drag-and-drop

Yabridge supports drag-and-drop both from a native (X11) Linux application to
plugins running under yabridge, as well as from yabridge plugins to native X11
applications like your DAW or your file browser. When dragging things from a
plugin to your DAW, then depending on which DAW you're using it may look like
the drop is going to fail while you're still holding down the left mouse button.
That's expected, since yabridge's and Wine's own drag-and-drop systems are
active at the same time. If you're using yabridge in _REAPER_ or _Carla_, then
you may need to enable a [compatibility option](#compatibility-options) to
prevent those hosts from stealing the drop.

### Input focus grabbing

Yabridge tries to be clever about the way grabbing and releasing input focus for
a plugin works. One important detail here is that when grabbing input focus,
yabridge will always focus the _parent window_ passed by the host for the plugin
to embed itself into. This means that hosts like Bitwig Studio can still process
common key bindings like <kbd>Space</kbd> for play/pause even while you are
interacting with a plugin's GUI. The downside of this approach is that this also
means that in those hosts you simply cannot type a space character, as the key
will always go to the host.

For the very specific situations where you may want to focus the plugin's editor
directly so that all keyboard input goes to Wine, you can hold down the
<kbd>Shift</kbd> key while entering the plugin's GUI with your mouse. This will
let you type spaces in text fields in **Bitwig Studio**, type text into the
settings and license dialogs in **Voxengo** plugins, and it will also allow you
to navigate dropdowns with the keyboard.


## Configuration

Yabridge can be configured on a per plugin basis to host multiple plugins within
a single process using [plugin groups](#plugin-groups), and there are also a
number of [compatibility options](#compatibility-options) available to improve
compatibility with certain hosts and plugins.

Configuring yabridge is done by creating a `yabridge.toml` file located in
either the same directory as the bridged plugin `.so` or `.clap` file you're
trying to configure, or in any of its parent directories. In most cases, this
file should be created as either `~/.vst/yabridge/yabridge.toml`,
`~/.vst3/yabridge/yabridge.toml`, or `~/.clap/yabridge/yabridge.toml` depending
on the type of plugin you want to configure.

Configuration files contain several _sections_. Each section can match one or
more plugins using case sensitive
[glob](https://www.man7.org/linux/man-pages/man7/glob.7.html) patterns that
match paths to yabridge `.so` and `.clap` files relative to the `yabridge.toml`
file, as well as a list of options to apply to the matched plugins. These glob
patterns can also match entire directories, in which case the settings are
applied to all plugins under that directory or one of its subdirectories. To
avoid confusion, only the first `yabridge.toml` file found and only the first
matching glob pattern within that file will be considered. See below for an
[example](#example) of a `yabridge.toml` file. To make debugging easier,
yabridge will print the used `yabridge.toml` file and the matched section within
it on startup, as well as all of the options that have been set.

### Plugin groups

| Option  | Values            | Description                                                            |
| ------- | ----------------- | ---------------------------------------------------------------------- |
| `group` | `{"<string>",""}` | Defaults to `""`, meaning that the plugin will be hosted individually. |

Some plugins have the ability to communicate with other instances of that same
plugin or even with other plugins made by the same manufacturer. This is often
used in mixing plugins to allow different tracks to reference each other without
having to route audio between them. Examples of plugins that do this are
FabFilter Pro-Q 3, MMultiAnalyzer and the iZotope mixing plugins. In order for
this to work, all instances of a particular plugin will have to be hosted in the
same process.

Yabridge has the concept of _plugin groups_, which are user defined groups of
plugins that will all be hosted inside of a single process. Plugins groups can
be configured for a plugin by setting the `group` option of that plugin to some
name. All plugins with the same group name will be hosted within a single
process. Of course, plugin groups with the same name but in different Wine
prefixes and with different architectures will be run independently of each
other. See below for an [example](#example) of how these groups can be set up.

_Note that because of the way VST3 and CLAP work, multiple instances of a single
VST3 or CLAP plugin will always be hosted in a single process regardless of
whether you have enabled plugin groups or not._ _The only reason to use plugin
groups with those plugins is to get slightly lower loading times the first time
you load a new plugin._

### Compatibility options

| Option                        | Values                  | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ----------------------------- | ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `disable_pipes`               | `{true,false,<string>}` | When this option is enabled, yabridge will redirect the Wine plugin host's output streams to a file without any further processing. See the [known issues](#known-issues-and-fixes) section for a list of plugins where this may be useful. This can be set to a boolean, in which case the output will be written to `$XDG_RUNTIME_DIR/yabridge-plugin-output.log`, or to an absolute path (with no expansion for tildes or environment variables). Defaults to `false`.           |
| `editor_disable_host_scaling` | `{true,false}`          | Disable host-driven HiDPI scaling for VST3 and CLAP plugins. Wine currently does not have proper fractional HiDPI support, so you might have to enable this option if you're using a HiDPI display. In most cases setting the font DPI in `winecfg`'s graphics tab to 192 will cause plugins to scale correctly at 200% size. Defaults to `false`.                                                                                                                                  |
| `editor_force_dnd`            | `{true,false}`          | This option forcefully enables drag-and-drop support in _REAPER_. Because REAPER's FX window supports drag-and-drop itself, dragging a file onto a plugin editor will cause the drop to be intercepted by the FX window. This makes it impossible to drag files onto plugins in REAPER under normal circumstances. Setting this option to `true` will strip drag-and-drop support from the FX window, thus allowing files to be dragged onto the plugin again. Defaults to `false`. |
| `frame_rate`                  | `<number>`              | The rate at which Win32 events are being handled and usually also the refresh rate of a plugin's editor GUI. When using plugin groups all plugins share the same event handling loop, so in those the last loaded plugin will set the refresh rate. Defaults to `60`.                                                                                                                                                                                                               |
| `hide_daw`                    | `{true,false}`          | Don't report the name of the actual DAW to the plugin. See the [known issues](#known-issues-and-fixes) section for a list of situations where this may be useful. This affects VST2, VST3, and CLAP plugins. Defaults to `false`.                                                                                                                                                                                                                                                   |
| `vst3_prefer_32bit`           | `{true,false}`          | Use the 32-bit version of a VST3 plugin instead the 64-bit version if both are installed and they're in the same VST3 bundle inside of `~/.vst3/yabridge`. You likely won't need this.                                                                                                                                                                                                                                                                                              |

These options are workarounds for issues mentioned in the [known
issues](#known-issues-and-fixes) section. Depending on the hosts
and plugins you use you might want to enable some of them.

### Example

All of the paths used here are relative to the `yabridge.toml` file. A
configuration file for VST2 plugins might look a little something like this:

```toml
# ~/.vst/yabridge/yabridge.toml

["FabFilter Pro-Q 3.so"]
group = "fabfilter"

["MeldaProduction/Tools/MMultiAnalyzer.so"]
group = "melda"

# Matches an entire directory and all files inside it, make sure to not include
# a trailing slash
["ToneBoosters"]
group = "toneboosters"

["Chromaphone 3.so"]
hide_daw = true

["sforzando VST_x64.so"]
editor_force_dnd = true
frame_rate = 24

["Loopcloud*"]
disable_pipes = true

# Simple glob patterns can be used to avoid unneeded repetition
["iZotope*/Neutron *"]
group = "izotope"

# Since this file has already been matched by the above glob pattern, this won't
# do anything
["iZotope7/Neutron 2 Mix Tap.so"]
group = "This will be ignored!"

# Of course, you can also add multiple plugins to the same group by hand
["iZotope7/Insight 2.so"]
group = "izotope"

# This would cause all plugins to be hosted within a single process. Doing so
# greatly reduces the loading time of individual plugins, with the caveat being
# that plugins are no longer sandboxed from each other.
#
# ["*"]
# group = "all"
```

For VST3 plugins you should just match the directory instead of the `.so` file
deep within in, like this:

```toml
# ~/.vst3/yabridge/yabridge.toml

["FabFilter*.vst3"]
group = "fabfilter"
editor_disable_host_scaling = true

["Chromaphone 3.vst3"]
hide_daw = true

["Misstortion2.vst3"]
editor_disable_host_scaling = true

["*/*Spectral*.vst3"]
vst3_prefer_32bit = true

# These options would be applied to all plugins that do not already have their
# own configuration set
["*"]
editor_force_dnd = true
editor_disable_host_scaling = true
```

With CLAP plugins, you match on the Linux `.clap` plugin file, just like
matching on `.so` files for a VST2 config file:

```toml
# ~/.clap/yabridge/yabridge.toml

["fb799964.clap"]
hide_daw = true
```

## Known issues and fixes

Any plugin should function out of the box, although some plugins will need some
additional dependencies for their GUIs to work correctly. Notable examples
include:

- If plugins have missing, invisible, or misaligned text, then installing
  `corefonts` or `allfonts` through `winetricks` may help.
- If a plugin seems to work fine except for the fact that the GUI never seems to
  update when you interact with it, then try installing
  [DXVK](https://github.com/doitsujin/dxvk). Many recent JUCE-based plugins
  don't redraw anymore when using WineD3D. Make sure you also install Vulkan
  drivers if you don't already have those set up.
- **Serum 1** requires you to disable `d2d1.dll` in `winecfg` and to install
  `gdiplus` through `winetricks`. You may also want to disable the tooltips by
  going to the global settings tab, unchecking 'Show help tooltips', and
  clicking on the save icon next to 'Preferences'.
- **Native Instruments** plugins work, but the latest version of Native Access
  needs some extra work to run under wine. See the [wineHQ page](https://appdb.winehq.org/objectManager.php?sClass=version&iId=41820)
  for information on how to get it running.

  The legacy version `1.X` still can be installed directly. You can find it on the
  [legacy installers](https://support.native-instruments.com/hc/en-us/articles/360000407909-Native-Access-1-Legacy-Installers-for-Older-Operating-Systems)
  page on Native Instruments' website. To get the installer to finish correctly,
  open `winecfg` and set the reported Windows version to Windows 10. Otherwise
  the installer will be stuck on installing an ISO driver. To work around this
  you can open the .iso file downloaded to your downloads directory and run the
  installer directly.

  Some plugins or sound libraries will install as expected, but if you get an
  'Error while mounting disk image' installation failure, then you will need to
  install the plugin or sound library manually. You will find a .iso file in
  your downloads directory that you can mount and then run the installer from.
  However some of those Native Instruments .iso files contain hidden files, and
  the installer on the .iso file will fail to install unless you mount the .iso
  file with the correct mounting options to unhide those files. To do this,
  first run `udisksctl loop-setup -f ~/Downloads/<filename>.iso` to load the
  .iso file, and then use `udisksctl mount -t udf -o unhide -b /dev/loopX` where
  `/dev/loopX` corresponds to the loop device printed by the `loop-setup`
  command to mount the .iso file to a directory in `/run/media`.

  If you're using an older distro and you're getting a
  `Mount option 'unhide' is not allowed` error when trying to mount the file,
  then you may need to manually create or edit `/etc/udisks2/mount_options.conf`
  first, adding the following to the file:

  ```conf
  [defaults]
  udf_allow=uid=$UID,gid=$GID,iocharset,utf8,umask,mode,dmode,unhide,undelete
  ```

- If **Spitfire Audio** plugins like **BBC Symphony Orchestra** and **LABS** are
  unable to load their sample libraries (_Error #X: Something went wrong_), then
  you can try reinstalling those plugins to a new, clean Wine prefix. To avoid
  potential confusion, make sure to uninstall the Spitfire software along with
  the VST2 and VST3 plugins from your main Wine prefix first.
- Several **JUCE** based plugins have an issue under Wine where the mouse cursor
  will disappear after interacting with certain UI elements. This can usually be
  fixed by mousing over the resize handle in the bottom right corner.
- Several (JUCE-based) plugins like **Arturia**'s plugins, Sonic Academy's
  **Kick 2** and Cytomic's **The Drop** have an issue where the GUI freezes when
  it's trying to display a tooltip. This can be fixed by enabling the '_Hide
  Wine version from applications_' option in the Staging tab of winecfg. If a
  plugin seems to function normally but then freezes when clicking on something,
  then try enabling this option.
- The GUI in **Sforzando** may appear to not respond to mouse clicks depending
  on your Wine and system configuration. This is actually a redrawing issue, and
  the GUI will still be updated even if it doesn't look that way. Dragging the
  window around or just clicking anywhere in the GUI will force a redraw and
  make the GUI render correctly again.
- **MeldaProduction** plugins have minor rendering issues when GPU acceleration
  is enabled. This can be fixed by disabling GPU acceleration in the plugin
  settings. I'm not sure whether this is an issue with Wine or the plugins
  themselves. Notable issues here are missing redraws and incorrect positioning
  when the window gets dragged offscreen on the top and left sides of the screen.
- Knobs in **Tokyo Dawn Records** plugins may not behave as expected when
  dragging long distances. Setting the 'Continuous Drag' option in the plugin's
  options to 'Linear' fixes the issue.
- Similarly, the knobs in **Voxengo** plugins behave better when you enable the
  'Radial knob mode' setting in the global settings.
- If **Scaler 2**'s interface lags, blacks out, or otherwise renders poorly,
  then you can try enabling [software
  rendering](https://forum.scalerplugin.com/t/scaler-2-black-empty-window/3540/8)
  to fix these issues.
- **ujam** plugins and other plugins made with the Gorilla Engine, such as the
  **LoopCloud** plugins, will throw a `JS_EXEC_FAILED` error when trying to load
  the plugin. Enabling the `disable_pipes` [compatibility
  option](#compatibility-options) for those plugins will fix this.
- Plugins by **KiloHearts** have file descriptor leaks when _esync_ is enabled,
  causing Wine and yabridge to eventually stop working after the system hits the
  open file limit. To fix this, either unset `WINEESYNC` while using yabridge or
  switch to using [_fsync_](#performance-tuning) instead.
- **PSPaudioware** and **Soundtoys** plugins with expandable GUIs, such as E27
  and Crystallizer, may have their GUI appear in the wrong location after the
  GUI has been expanded. You can enable an alternative [editor hosting
  mode](#compatibility-options) to fix this.
- When using recent _Applied Acoustics_ plugins like **Chromaphone 3** under
  _Bitwig Studio_, text entry will cause the plugin to crash because Chromaphone
  uses a different text entry method when it detects Bitwig. You can use the
  `hide_daw` [compatibility option](#compatibility-options) to work around this.
- VST2 plugins like **FabFilter Pro-Q 3** that can share data between different
  instances of the same plugin plugins have to be hosted within a single process
  for that functionality to work. See the [plugin groups](#plugin-groups)
  section for instructions on how to set this up. This is not necessary for VST3
  plugins, as multiple instances of those plugins will always be hosted in a
  single process by design.
- Some hosts, particularly _Ardour_, _REAPER_, _Qtractor_, will by default not
  unload VST3 modules after you close the last plugin. This means that the
  associated `yabridge-host.exe` process will keep running until you close the
  project. For REAPER there's an option called
  `Allow complete unload of VST plug-ins` in the `VST` tab of the settings
  dialog to disable this behaviour.
- _Drag-and-drop_ to the plugin window under **REAPER** doesn't work because of
  a long standing issue in REAPER's FX window implementation. You can use a
  compatibility option to [force drag-and-drop](#compatibility-options) to work
  around this limitation.

Aside from that, these are some known caveats:

- **iZotope** plugins can't be authorized because of missing functionality in
  Wine's crypt32 implementation.
- **D16 Group** plugins also can't be authorized in current versions of Wine as
  they don't recall their authorization status correctly.
- **Waves** V13 VST3 plugins have memory issues, at least under Wine. They will
  likely randomly crash at some point. If you can avoid Waves, that would be for
  the best. Otherwise, try the V12 versions of the plugins if you still have a
  license for them.
- MIDI key labels for VST2 plugins (commonly used for drum machines and
  multisamplers) will not be updated after the host first asks for them since
  VST 2.4 has no way to let the host know that those labels have been updated.
  Deactivating and reactivating the plugin will cause these labels to be updated
  again for the current patch.

There are also some (third party) plugin API extensions that have not been
implemented yet. See upstream's [roadmap](./ROADMAP.md) for a list of those.

## Troubleshooting common issues

If your problem is not listed here, then feel free to post on this
repository's [issue
tracker](https://github.com/jartificer/yabridge-custom/issues). Also check the
[known issues and fixes](#known-issues-and-fixes) section above for help with
plugin-specific issues.

- This version includes upstream's fix for Wine 9.22 and later, so there's no
  need to downgrade Wine to use it.

- If a plugin suddenly goes silent and yabridge shows a desktop notification
  saying that it lost the connection to the Wine plugin host, then that host
  crashed, exited, or stopped responding. This version keeps your DAW running
  in that case instead of crashing it, but the plugin stays silent until you
  remove it and load it again. The log output from before the notification
  usually shows why the host died.

- If a plugin fails to load with an error saying the Wine plugin host exited
  before it could connect to yabridge, then Wine could not start the host at
  all. A common cause is a 32-bit only Wine prefix, see [Wine
  prefixes](#wine-prefixes). Running the host directly, as described below,
  usually shows the reason.

- If you run into audio problems specific to 32-bit VST2 plugins, then try
  starting your DAW with `YABRIDGE_NO_FUTEX=1` to rule out the futex based
  audio channel, and with `YABRIDGE_NO_LINUX_SYSCALLS=1` to rule out the 32-bit
  host's direct Linux system calls. See [Environment
  variables](#environment-variables).

- Both yabridgectl and yabridge try to diagnose many common issues for you. If
  you're running into crashes or other issues, then try launching your DAW from
  a terminal and reading the log output for any clues. Bitwig Studio writes
  plugin output to `~/.BitwigStudio/log/engine.log`, so you may need to look
  there instead.

- Try to use a clean Wine prefix when testing misbehaving plugins. Either
  temporarily rename `~/.wine` to something else, or set the `WINEPREFIX`
  environment variable to a directory path to have Wine use that as a prefix.
  Don't forget to unset it before starting your DAW or all plugins will use that
  prefix.

- If you have the `WINEPREFIX` environment variable set and you _don't_ want all
  of your plugins to use that specific Wine prefix, then you should unset it to
  allow yabridge to automatically detect Wine prefixes for you.

- If you get a warning about a low `RLIMIT_RTTIME` value of 200000 microseconds,
  then your DAW is running in an environment where _rtkit_ is active. Rtkit is
  used to grant realtime scheduling privileges to applications in environments
  where users can't do that themselves, but it also imposes severe limitations
  on what those applications can do. Applications known for doing this are:

  - **PipeWire**. With PipeWire versions above 0.3.44, you simply need to make
    sure your user has realtime priviliges. Follow the instructions from the
    section below to enable this and then reboot your system.
  - **GNOME 45+**. Recent GNOME shell versions started using rtkit in the shell
    itself. As far as I'm aware, this happens unconditionally, and masking the
    rtkit service to prevent it from running is the only workaround. Make sure
    your user has realtime priviliges set up according to the instructions from
    the section below, and then run `sudo systemctl mask rtkit-daemon.service`.
    The warning should disappear after rebooting. Please let me know if anyone
    knows a better solution for this problem!

- If yabridge prints errors or warnings about memory locking limits, then that
  means that you have not yet set up realtime privileges for your user. Setting
  the memlock limit to unlimited (or -1) is usually part of this process. How
  you should do this will depend on your distro. On _Arch_ and _Manjaro_, you
  will need to install the `realtime-privileges` package, add your user to the
  `realtime` group with `sudo gpasswd -a "$USER" realtime`, and then reboot.
  _Fedora_ does the same thing with their `realtime-setup` package, which also
  sets up a `realtime` group that you will need to add your user to. On
  _Debian_, _Ubuntu_, and distros based on those, the `jackd2` package usually
  sets this up for the `audio` group instead. If
  `/etc/security/limits.d/audio.conf` exists, then you can simply add yourself
  to the `audio` group and reboot. In any other case you may need to [set this
  up yourself](https://jackaudio.org/faq/linux_rt_config.html).

- If you're seeing errors related to Wine either when running `yabridgectl sync`
  or when trying to load a plugin, then it can be that your installed version of
  Wine is much older than the version that yabridge has been compiled for.
  Yabridgectl will automatically check for this when you run `yabridgectl sync`
  after updating Wine or yabridge. You can also manually verify that Wine is
  working correctly by running one of the Wine plugin host applications.
  Assuming that yabridge is installed under `~/.local/share/yabridge`, then
  running `~/.local/share/yabridge/yabridge-host.exe` directly (so _not_
  `wine ~/.local/share/yabridge/yabridge-host.exe`, that won't work) in a terminal
  should print a few messages related to Wine's startup process followed by a
  line starting with:

  ```
  Usage: yabridge-host.exe <plugin_type> <plugin_location> ...
  ```

  The 32-bit host is a regular Windows executable, so that one _does_ need to
  be run through Wine: `wine ~/.local/share/yabridge/yabridge-host-32.exe` (or
  `wine /usr/bin/yabridge-host-32.exe` when installed with the `PKGBUILD`)
  should print the same kind of usage line.

  If you're seeing a `002b:err:module:__wine_process_init` error instead, then
  your version of Wine is too old for this version of yabridge and you'll have
  to upgrade your Wine version. Instructions for how to do this on Ubuntu can be
  found on the [WineHQ website](https://wiki.winehq.org/Ubuntu).

  If you're getting a `0024:err:process:exec_process` error, then your Wine
  prefix is set to 32-bit only and it won't be possible to run 64-bit
  applications like `yabridge-host.exe`. With WoW64 Wine such a prefix can't be
  used for 32-bit plugins either, so use a regular 64-bit prefix instead.

- Sometimes left over Wine processes can cause problems. Run `wineserver -k` to
  terminate Wine related in the current or default Wine prefix.

- If plugin windows show up as a large overlay over the entire screen, covering
  up other windows and making it impossible to interact with anything else
  without Alt+Tabbing to them, then make sure the 'Allow the window manager to
  control the windows' checkbox in winecfg's Graphics tab is checked.

- If you're using a _lot_ of plugins and you're unable to load any new plugins,
  then you may be running into Xorg's limit of 256 clients. The exact number of
  plugins it takes for this to happen will depend on your system and the other
  applications running in the background. An easy way to check if this is the
  case would be to try and run `wine cmd.exe` from a terminal. If this prints a
  message about the maximum number of clients being reached (or if you are not
  able to open the terminal at all), then you might want to consider using
  [plugin groups](#plugin-groups) to run multiple instances of your most
  frequently used plugins within a single process. And if you're using many
  instances of a single VST2 plugin, using the VST3 or CLAP version of that
  plugin may also help since they'll share a single process.

  Alternatively you can try increasing Xorg's limit itself.
  First, check what your current limit is:
  In a terminal, run: `less /var/log/Xorg.0.1` (or use any other text editor)
  Search for a line containing "MaxClients". This then states the currently set
  limit.
  Then check if higher values are supported:
  In a terminal, run: `/usr/lib/Xorg -maxclients 9999`
  This should give an error message and show some information like this:

  > maxclients must be one of 64, 128, 256, 512, 1024 or 2048

  Let's say we pick 1024. Here's how to apply it.
  Create this file (will require sudo): `/etc/X11/xorg.conf.d/99-maxclients.conf`
  Add this content:

  ```
  Section "ServerFlags"
      Option "MaxClients" "1024"
  EndSection
  ```

  Save and reboot your system. Once you are logged back in, you can verify that
  the setting has been applied by using the same approach for checking the
  previously set limit (see above). Now it should be less likely to run into the
  previous issue regarding "too many clients" when opening lots of plugins.

- If you're using a `WINELOADER` that runs the Wine process under a separate
  namespace while the host is not sandboxed, then you'll have to use the
  `YABRIDGE_NO_WATCHDOG` environment variable to disable the watchdog timer. If
  you know what this means then you probably know what you're doing. In that
  case, you may also want to use `YABRIDGE_TEMP_DIR` to choose a different
  directory for yabridge to store its sockets and other temporary files in.

## Performance tuning

Running Windows plugins under Wine should have a minimal performance overhead,
but you may still notice an increase in latency spikes and overall DSP load.
Luckily there are a few things you can do to get rid of most or all of these
negative side effects:

- First of all, you'll want to make sure that you can run programs with realtime
  scheduling. Note that on Arch and Manjaro this does not necessarily require a
  realtime kernel as they include the `PREEMPT` patch set in their regular
  kernels. You can verify that this is working correctly by running `chrt -f 10 whoami`,
  which should print your username, and running `uname -a` should print
  something that contains `PREEMPT` in the output.

  If the `uname -a` output contains `PREEMPT_DYNAMIC`, then run either
  `zgrep PREEMPT /proc/config.gz` or `grep PREEMPT "/boot/config-$(uname -r)"`
  depending on your distro. If `CONFIG_PREEMPT` is not set, then either add the
  `preempt=full` kernel parameter or better yet, switch to a kernel that's
  optimized for low latencies.

- You can also try enabling the `threadirqs` kernel parameter and using which
  can in some situations help with xruns. After enabling this, you can use
  [rtirq](https://github.com/rncbc/rtirq#rtirq) to increase the priority of
  interrupts for your sound card.

- Make sure that you're using the performance frequency scaling governor, as
  changing clock speeds in the middle of a real time workload can cause latency
  spikes. Since Linux 5.9 it's possible to do this by setting the
  `cpufreq.default_governor=performance` to the kernel's command line in your
  boot loader configuration.

- The last but perhaps the most important thing you can do is to use a build of
  Wine compiled with Proton's fsync or FUTEX2 patches. This can improve
  performance significantly when using certain multithreaded plugins. If you are
  running Arch or Manjaro, then you can use [Tk-Glitch's Wine
  fork](https://github.com/Frogging-Family/wine-tkg-git) for a customizable
  version of Wine with the fsync patches included. Make sure to follow the
  instructions in the readme to build a version of wine-tkg using the default
  profile and don't try to use the prebuilt releases as they will have fshack
  enabled which tends to break many plugins that use Direct3D for their
  rendering. You'll also want to make sure you're running Linux kernel 5.16 or
  newer as those include support the `_fsync_futex_waitv` option that's enabled
  by default though wine-tkg's `customization.cfg`. Finally, you'll have to set
  the `WINEFSYNC` environment variable to `1` to enable fsync. See the
  [environment configuration](#environment-configuration) section below for more
  information on where to set this environment variable so that it gets picked
  up when you start your DAW.

- If you have the choice, the VST3 version of a plugin will likely perform
  better than the VST2 version. And if there is a CLAP version, then that may
  perform even better.

- If the plugin doesn't have a VST3 or CLAP version, then [plugin
  groups](#plugin-groups) can also greatly improve performance when many
  instances of same VST2 plugin. _VST3 and CLAP plugins have similar
  functionality built in by design_. Some plugins, like the BBC Spitfire
  plugins, can share a lot of resources between different instances of the
  plugin. Hosting all instances of the same plugin in a single process can in
  those cases greatly reduce overall CPU usage and get rid of latency spikes.

- For 32-bit VST2 plugins this version avoids `wineserver` on the audio thread
  by using Linux futexes directly. That needs the kernel's 32-bit syscall
  emulation (`CONFIG_IA32_EMULATION`), which is enabled in all common distro
  kernels. If it's disabled, or if `ia32_emulation=0` is set on the kernel's
  command line, then the 32-bit host falls back to the slower socket based path
  and won't get realtime scheduling. 32-bit VST3 and CLAP plugins always use the
  socket based path, so for 32-bit plugins the VST2 version may perform better.

### Environment configuration

This section is relevant if you want to configure environment variables in such
a way that they will be set when you launch your DAW from the GUI instead of
from a terminal. You may want to enable `WINEFSYNC` for fsync support with a
compatible Wine version and kernel, or you may want to change your search `PATH`
to allow yabridge to find the `yabridge-*.exe` binaries if you're using yabridge
directly from the `build` directory. To do this you'll need to change your
_login shell's_ profile, which is different from the configuration loaded during
interactive sessions. And some display manager override your login shell to
always use `/bin/sh`, so you need to be careful to modify the correct file or
else these changes won't work. You can find out your current login shell by
running `echo $SHELL` in a terminal.

- First of all, if you're using GDM, LightDM or LXDM as your display manager
  (for instance if you're using GNOME, XFCE or LXDE), then your display manager
  won't respect your login shell and it will always use `/bin/sh`. In that case
  you will need to add the following line to `~/.profile` to enable fsync:

  ```shell
  export WINEFSYNC=1
  ```

- If you are using the default **Bash** shell and you're not using any of the
  above display managers, then you will want to add the following line to
  `~/.bash_profile` (or `~/.profile` if the former does not exist):

  ```shell
  export WINEFSYNC=1
  ```

- If you are using **Zsh**, then you can add the following line to `~/.zprofile`
  (`~/.zshenv` should also work, but some distros such as Arch Linux overwrite
  the environment after this file has been read):

  ```shell
  export WINEFSYNC=1
  ```

- If you are using **fish**, then you can add the following line to either
  `~/.config/fish/config.fish` or some file in `~/.config/fish/conf.d/`:

  ```shell
  set -gx WINEFSYNC 1
  # Or if you're changing your PATH:
  set -gp fish_user_paths ~/directory/with/yabridge/binaries
  ```

_Make sure to log out and log back in again to ensure that all applications pick
up the new changes._

## Building

To compile yabridge, you'll need [Meson](https://mesonbuild.com/index.html) and
the following dependencies:

- GCC 10+
- A Wine installation with `winegcc` and the development headers
- libxcb
- The `i686-w64-mingw32` MinGW-w64 cross compiler, for the 32-bit plugin host
  (see [32-bit bitbridge](#32-bit-bitbridge))
- Rust and Cargo, for `yabridgectl`

The following dependencies are included in the repository as a Meson wrap:

- [Asio](http://think-async.com/Asio/)
- [bitsery](https://github.com/fraillt/bitsery)
- [function2](https://github.com/Naios/function2)
- [`ghc::filesystem`](https://github.com/gulrak/filesystem)
- [tomlplusplus](https://github.com/marzer/tomlplusplus)
- Version 3.7.7 of the [VST3 SDK](https://github.com/robbert-vdh/vst3sdk) with
  some [patches](https://github.com/robbert-vdh/yabridge/blob/master/tools/patch-vst3-sdk.sh)
  to allow Winelib compilation
- Version 1.1.9 of the [CLAP headers](https://github.com/free-audio/clap).

The project can then be compiled with the command below. You can remove or
change the unity size argument if building takes up too much RAM, or you can
disable unity builds completely by getting rid of `--unity=on` at the cost of
slightly longer build times.

```shell
meson setup build --buildtype=release --cross-file=cross-wine.conf --unity=on --unity-size=1000
ninja -C build
```

`yabridgectl` is built separately with Cargo:

```shell
cd tools/yabridgectl
cargo build --release
```

After you've finished building you can follow the instructions under the
[usage](#usage) section on how to set up yabridge.

### 32-bit bitbridge

To use 32-bit Windows plugins, such as old SynthEdit plugins, you'll also need
to build the 32-bit plugin host `yabridge-host-32.exe`. This allows yabridge to
act as a bitbridge, letting you run old 32-bit only Windows plugins in a modern
64-bit Linux plugin host alongside your 64-bit plugins.

Upstream yabridge built this host as a 32-bit Winelib application, which is not
compatible with Wine's new WoW64 mode ([Wine
#58377](https://bugs.winehq.org/show_bug.cgi?id=58377)). Distros like Arch
Linux now ship Wine in that mode. This version of yabridge instead builds the
32-bit host as a regular 32-bit Windows executable using MinGW-w64, which works
with both new and old style WoW64 Wine builds. For this you'll need the
`i686-w64-mingw32` MinGW-w64 cross compiler with POSIX threads:

- On Arch Linux: `sudo pacman -S mingw-w64-gcc`
- On Debian and Ubuntu: `sudo apt install g++-mingw-w64-i686-posix`, and use
  `cross-mingw-i686-posix.conf` instead of `cross-mingw-i686.conf` below

Meson can only use one cross compiler per build directory, so the 32-bit host
needs its own build directory:

```shell
# The native plugin libraries and the 64-bit plugin host, as described above
meson setup build --buildtype=release --cross-file=cross-wine.conf --unity=on --unity-size=1000
ninja -C build

# The 32-bit plugin host
meson setup build-32 --buildtype=release --cross-file=cross-mingw-i686.conf --unity=on --unity-size=1000
ninja -C build-32

# yabridge looks for the plugin hosts next to the plugin libraries, so they
# should end up in the same directory
cp build-32/yabridge-host-32.exe build/
```

Then install `build/yabridge-host-32.exe` alongside the other files as described
in the [usage](#usage) section, for instance in `~/.local/share/yabridge`. On
Arch Linux you can also use the `PKGBUILD` in
[`tools/arch`](tools/arch/PKGBUILD), which builds and installs everything
including `yabridgectl`:

```shell
cd tools/arch
makepkg -si
```

Yabridge will detect whether the plugin you're trying to load is 32-bit or
64-bit, and will run either the regular version or the `*-32.exe` variant
accordingly. The 32-bit host is started through `wine` (or `$WINELOADER`, if
that's set).

The `bitbridge` Meson option from upstream yabridge no longer builds anything.
Use the `cross-mingw-i686.conf` cross file as shown above instead.

There are a couple of differences between the two hosts, since the 32-bit host
can only use the Windows API:

- Wine's Winsock implementation does not support Unix domain sockets, so the
  32-bit host talks to the native plugin over TCP sockets on the loopback
  interface instead. Every connection is authenticated with a challenge-response
  handshake using a secret that's only readable by your user
  (`$XDG_RUNTIME_DIR/yabridge-tcp-token-<uid>`). Waiting on a Winsock socket in
  Wine involves `wineserver`, which adds latency and occasional spikes of tens
  of milliseconds. Because of that, VST2 audio processing requests (and MIDI
  events sent along with them) don't use a socket. They're passed through a
  small shared memory object instead, with both sides waiting on Linux futexes
  directly, so `wineserver` isn't involved. Set `YABRIDGE_NO_FUTEX=1` to use
  the socket instead. VST3 and CLAP plugins in the 32-bit host still use the
  socket for audio processing.
- The 32-bit host can't connect to the X11 server directly, so its X11
  connections for editor embedding and drag-and-drop go through a small relay
  in the native plugin library.
- Wine only maps Windows thread priorities to nice values. To still get
  `SCHED_FIFO` realtime scheduling for the audio threads, the 32-bit host makes
  the `sched_setscheduler()` Linux system call directly. Set
  `YABRIDGE_NO_LINUX_SYSCALLS=1` to disable this if it ever causes problems.
  This also disables the futex based audio processing described above.

[`tools/smoke-test`](tools/smoke-test/run-smoke-test.sh) contains an end-to-end
test that builds a minimal 32-bit and 64-bit Windows VST2 plugin and a minimal
native VST2 host, and then uses those to test both plugin hosts, including
running 32-bit and 64-bit plugins at the same time:

```shell
tools/smoke-test/run-smoke-test.sh build build-32
```

## Debugging

Wine's error messages and warning are usually very helpful whenever a plugin
doesn't work right away. However, with some hosts it can be hard read a plugin's
output. To make it easier to debug malfunctioning plugins, yabridge offers these
two environment variables to control yabridge's logging facilities:

- `YABRIDGE_DEBUG_FILE=<path>` allows you to write yabridge's debug messages as
  well as all output produced by the plugin and by Wine itself to a file. For
  instance, you could launch your DAW with
  `env YABRIDGE_DEBUG_FILE=/tmp/yabridge.log <daw>`, and then use
  `tail -F /tmp/yabridge.log` to keep track of the output. If this option is not
  present then yabridge will write all of its debug output to STDERR instead.
- `YABRIDGE_DEBUG_LEVEL={0,1,2}{,+editor}` allows you to set the verbosity of
  the debug information. You can set a debug level, optionally followed by
  `+editor` to also get more debug output related to the editor window handling.
  Each level increases the amount of debug information printed:

  - A value of `0` (the default) means that yabridge will only log the output
    from the Wine process and some basic information about the
    environment, the configuration and the plugin being loaded.
  - A value of `1` will log detailed information about most events and function
    calls sent between the plugin host and the plugin. This filters out some
    noisy events such as `effEditIdle()` and `audioMasterGetTime()` since those
    are sent multiple times per second by for every plugin.
  - A value of `2` will cause all of the events to be logged without any
    filtering. This is very verbose but it can be crucial for debugging
    plugin-specific problems.

  More detailed information about these debug levels can be found in
  `src/common/logging.h`.

See the [bug report
template](.github/ISSUE_TEMPLATE/bug_report.yml) for an example of how to use
this. Output from the 32-bit host ends up in the same place as the 64-bit
host's.

Wine's own [logging facilities](https://wiki.winehq.org/Debug_Channels) can also
be very helpful when diagnosing problems. In particular the `+message`,
`+module` and `+relay` channels are very useful to trace the execution path
within the loaded plugin itself.

### Attaching a debugger

To debug the plugin, you can just attach gdb to the host. Debugging the Wine
plugin host is a bit trickier. Wine comes with a GDB proxy for winedbg, but it
requires a little bit of additional setup and it expects the command line
arguments to be a valid Win32 command line. You'll also need to launch winedbg
in a seperate detached terminal emulator so it doesn't terminate together with
the plugin, and winedbg can be a bit picky about the arguments it accepts. I've
already set this up behind a feature flag for use in KDE Plasma. Other desktop
environments and window managers will require some slight modifications in
`src/plugin/host-process.cpp`. To enable this, simply run the follow and then
rebuild yabridge:

```shell
meson configure build --buildtype=debug -Dwinedbg=true
```
