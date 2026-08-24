# multiaudio

Play the same sound through **every** output device at once, so two pairs of
headphones (or headphones + speakers) hear the same thing.

A small tray app: one executable, about 3,000 lines of C++ with no dependencies
beyond Windows itself. No installer to download, no driver, no admin rights.

```
> multiaudio
Source: Speakers (Realtek(R) Audio)  [48000 Hz, 2 ch, float32]
  -> Headset Earphone (HyperX)  [48000 Hz, 2 ch]
  -> Digital Output (S/PDIF)  [44100 Hz, 2 ch]  (resampled)
Latency: about 40 ms behind the source. Press Ctrl+C to stop.
```

## About the "virtual device" part

The obvious design is a virtual playback device that shows up in the Windows
sound list and fans its audio out to every real device. That cannot be done
from a normal program: every entry in that list is published by a *driver*, so
a real virtual device means a kernel-mode audio driver, a WDK build, and a
code-signing certificate — the opposite of a small utility.

This tool gets to the same place from the other side. It captures whatever is
already playing on one device (WASAPI **loopback** capture, which needs no
driver and no "Stereo Mix" support) and plays that stream out on all the other
devices at the same time. Nothing has to be reconfigured: audio keeps going to
your normal device, and the other devices join in.

If you do want a device named something neutral like "All Headphones" in the
Windows list, pair this with a free virtual cable driver — see
[a virtual cable](#feeding-everything-equally-with-a-virtual-cable) below. `multiaudio` then does the
fan-out part that the cable does not do.

## Install it

Download or build `multiaudio.exe` and run it. The first time, it offers to
install itself: it copies into `%LOCALAPPDATA%\Programs\multiaudio`, adds a
Start Menu entry, and starts with Windows from then on. Everything is per-user,
so no admin rights are involved, and the tray menu can undo all of it.

From a command line, `multiaudio --install` and `multiaudio --uninstall` do the
same without asking. Starting with Windows is a shortcut in your Startup folder
(`Win+R`, `shell:startup`), so it is somewhere you can see it and delete it.

Then it lives in the notification area. Click the icon:

```
  ✓ Enabled
  ─────────────────────────────
  Mirroring Speakers (Realtek) to 2 devices
      Headset Earphone (HyperX)
      Digital Output (S/PDIF)
  ─────────────────────────────
  Mirror from   ▸   ● Windows default device
  Mirror to     ▸   ✓ Headset Earphone (HyperX)
  Latency       ▸     15 / 25 / ● 40 / 80 / 150 ms
  ─────────────────────────────
  ✓ Start with Windows
  Uninstall...
  ─────────────────────────────
  Exit
```

The icon is blue while audio is being mirrored and grey when it is off or
waiting; hovering over it shows what it is doing. Double-clicking toggles it.
Your choices are remembered in `HKCU\Software\multiaudio`.

## If Windows Defender quarantines it

A generic machine-learning detection such as `Trojan:Script/Wacatac.B!ml` is
the usual verdict on an executable that is unsigned, built with MinGW and
statically linked. It is a judgement about the shape of the file, not about
anything the program did: the quarantine happens on the downloaded file, before
it has ever run.

The dependable answer is to **build it yourself** from this repository - see
[Building](#building). Binaries from the Visual Studio compiler trip this far
less often, and you know what went into them. That is a better position to be
in than trusting a stranger's binary, whatever the scanner says about it.

If you would rather run a downloaded copy, restoring it is Windows Security →
Protection history → the item → Actions → Restore. Whether to do that is a
judgement about where the file came from, and only you can make it.

Reporting it helps: Microsoft's [false-positive
submission](https://www.microsoft.com/en-us/wdsi/filesubmission) is usually
turned around in a day or two.

The lasting fix is an Authenticode signature, which also stops SmartScreen
warning about the program having no reputation. Microsoft's Azure Trusted
Signing is the least painful route for an individual publisher.

For what it is worth, this program does do two things that a heuristic scanner
is entitled to be suspicious of: it copies itself into your program folder, and
it arranges to start with Windows. Both are visible and reversible - the copy
lands in `%LOCALAPPDATA%\Programs\multiaudio`, the startup entry is a shortcut
in your Startup folder, and `--uninstall` removes both.

## Plugging things in later

Nothing has to be plugged in when it starts, which matters when it starts with
Windows and the USB devices are still enumerating. The engine keeps watching:

* Start it with one pair of headphones plugged in and it waits. Plug in the
  second pair and mirroring begins on its own, a moment later.
* Unplug a pair and it drops out; the rest carry on playing.
* A device that appears is added **without interrupting** the ones already
  playing, and so is switching a destination on or off in the menu.
* Change the default playback device and it follows.
* If a device is held in exclusive mode by another program, it is skipped and
  retried every ten seconds rather than hammered.

Nothing in that list is an error state: the icon just goes grey and the tooltip
says what it is waiting for.

## Running it in a console instead

The same executable is still a command line tool, for setting things up and for
seeing what Windows reports:

```
multiaudio --list                          show the playback devices
multiaudio --console                       mirror in this window until Ctrl+C
multiaudio --console --to "USB"            mirror to just the matching devices
multiaudio --console --exclude "HDMI"      mirror everywhere but HDMI
multiaudio --source "CABLE Input"          mirror a virtual cable to everything
multiaudio --console --latency 80          more headroom, if it crackles
```

Full list with `multiaudio --help`. Name matching is case-insensitive and
matches any part of the name, so `--to hyperx` is enough for "Headset Earphone
(HyperX Cloud II)". Volume is per device: use the Windows volume mixer to
balance them.

## Feeding everything equally, with a virtual cable

If you want a device to select in Windows that feeds *all* of your real outputs
equally, rather than one real device leading and the others following ~40 ms
behind:

1. Install [VB-CABLE](https://vb-audio.com/Cable/) (free, signed driver).
   It adds a playback device called **CABLE Input**.
2. Make **CABLE Input** the default playback device.
3. Pick it under **Mirror from** in the tray menu.

Everything Windows plays now goes into the cable, and `multiaudio` fans it out
to every real device with identical latency - nothing is "first".

## Building

Nothing to install beyond a compiler; it links only libraries that ship with
Windows (`ole32`, `avrt`, `shell32`, `user32`, `gdi32`, `advapi32`).

**Visual Studio** — from an *x64 Native Tools Command Prompt*:

```
build.bat
```

**CMake** — any generator:

```
cmake -B build
cmake --build build --config Release
```

**MinGW-w64**, including cross-compiling from Linux:

```
x86_64-w64-mingw32-g++ -std=c++17 -municode -mwindows -O2 -static \
    -o multiaudio.exe src/*.cpp \
    -lole32 -lavrt -lshell32 -luser32 -lgdi32 -ladvapi32
```

Windows 7 or newer (Vista-era APIs only), 32- or 64-bit.

### Tests

The resampler, the channel map and the ring buffer are plain arithmetic, so
they are tested on their own and the test builds on any platform:

```
g++ -std=c++17 -O2 -o audio_test tests/audio_test.cpp && ./audio_test
```

It checks that resampling stays continuous across block boundaries (no gaps,
repeats or jumps), that a source clock running 500 ppm fast or slow is absorbed
without the buffer drifting or underrunning over a simulated minute, that a
1 kHz tone survives 48 kHz to 44.1 kHz intact, and that silence and recovery
behave. `ctest` runs it too.

## How it works

```
     source device                          every other device
  (loopback capture)                       (shared-mode render)

  IAudioCaptureClient  --> ring buffer -->  resample --> channel map --> IAudioRenderClient
   polled, float32          per sink        (drift-       (5.1 -> 2.0,
                                             trimmed)      mono -> 2.0, ...)
```

* **Capture.** The source is opened with `AUDCLNT_STREAMFLAGS_LOOPBACK`, which
  hands back exactly what Windows is mixing for that device. It is polled
  rather than event-driven, because event-driven loopback is not supported.
* **One thread per output.** Each destination runs its own event-driven
  shared-mode render stream, so a slow or hiccuping device cannot stall the
  others.
* **Drift correction.** Two sound cards never agree on what 48 kHz means —
  they differ by a few parts per million, which is enough to drain or overflow
  a fixed buffer within minutes. Each sink continuously trims its resampling
  ratio (by at most ±0.2%) to hold its buffer at the target fill level, so the
  streams stay locked together indefinitely instead of drifting apart and
  clicking. This is why the mirrored devices need a small buffer, and why that
  buffer is what `--latency` sets.
* **Format handling.** Sample rate, channel count and sample format are
  converted per device, so a 44.1 kHz S/PDIF output and a 48 kHz USB headset
  can run from the same 48 kHz source.

### How low the latency goes

Zero is not on the table, and not because of any detail of this program: a
mirrored device is downstream of the source's mix, so the audio has to be
captured before it can be played again. The floor is set by three things.

| Where the delay is | Roughly |
| --- | --- |
| Loopback capture picking up the mix | one audio-engine period, ~10 ms |
| Waiting in our ring, absorbing jitter and clock drift | half the setting, at least 15 ms |
| Queued at the destination device | half the setting, at least 2 periods |

The two floors are what stop it going lower, and both come from the Windows
shared-mode audio engine's 10 ms period rather than from anything here. In
practice that puts the floor around **35-45 ms**, and settings below that do
very little.

Getting meaningfully under that needs a different kind of stream:

* `IAudioClient3` shared-mode streams can run a 2.67 ms period where the
  driver supports it, which would bring the total into the 10-15 ms range.
* Exclusive-mode destinations are lower still, at the price of the device
  becoming unusable by every other program - including the Windows volume
  mixer.

Neither is implemented. If you want them, they are the next thing to do.

The number the tray tooltip shows is measured from the buffers actually in use,
not the setting you asked for, so it tells you what you are really getting.

The engine is written as a service rather than a pipeline that is set up once:
one pass of its state machine runs twice a second, opens whatever is missing,
drops whatever has gone, and publishes what it is doing. Every part of what it
needs is allowed to be absent.

Source layout: `main.cpp` (entry point and command line), `tray.cpp` (the
notification-area app), `mirror.cpp` (capture, the destination streams and the
service loop - the interesting part), `devices.cpp` (endpoint enumeration),
`audio.cpp` (format conversion), `settings.cpp` (what is remembered),
`install.cpp` (installing without an installer), plus the header-only
`resampler.h`, `channel_map.h`, `ring_buffer.h` and `util.h`.

## Troubleshooting

**"nothing to mirror to"** — only one playback device is active, or the
`--to`/`--exclude` filters excluded everything. Check `multiaudio --list`.
Devices that are disabled or unplugged do not appear.

**Crackling on one device** — raise the latency: `--latency 80`. USB and
Bluetooth devices often need more headroom than onboard audio. `--verbose`
reports underruns per device on exit.

**Bluetooth headphones lag badly** — that is the Bluetooth codec, not this
tool; the delay is in the headset. Nothing here can remove it.

**A device cannot be opened** — something is holding it in exclusive mode
(some DAW and audiophile players do this). The device is skipped with a
message; the rest still work.

**No sound on the mirrors, but the source is fine** — check the mirrored
device's own volume in the Windows volume mixer, and that its default format
in Sound settings is a normal shared-mode format.

## Limitations

* The source device cannot also be a destination — it is already playing the
  audio, and feeding it back would loop.
* Everything is shared-mode. A program that takes a device in exclusive mode
  (WASAPI exclusive, ASIO) is invisible to loopback capture and its device
  cannot be used as a destination while it is held.
* Mirrored devices lag the source; see [How low the latency
  goes](#how-low-the-latency-goes). The source device itself is untouched, so
  this is not an A/V sync problem for whoever is listening on it.

## License

MIT — see [LICENSE](LICENSE).
