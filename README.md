# multiaudio

Play the same sound through **every** output device at once, so two pairs of
headphones (or headphones + speakers) hear the same thing.

A small console program: about 1,700 lines of C++ with no dependencies beyond
Windows itself. No installer, no driver, no admin rights.

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
[Setup B](#setup-b-with-a-virtual-cable) below. `multiaudio` then does the
fan-out part that the cable does not do.

## Quick start

Build it (see [Building](#building)), then:

```
multiaudio --list      show the playback devices
multiaudio             mirror the default device to all the others
```

Plug in both pairs of headphones, run `multiaudio`, and play something. Ctrl+C
stops it. Each device keeps its own volume slider in the Windows volume mixer,
so that is where you balance them.

### Setup A: no install (default)

Your normal playback device stays the default. Windows plays to it as usual;
`multiaudio` copies that audio to every other device.

```
multiaudio                                mirror to everything else
multiaudio --to "HyperX" --to "Realtek"   mirror to just these two
multiaudio --exclude "HDMI"               mirror everywhere except HDMI
multiaudio --source 2                     mirror device 2 from --list
```

The one asymmetry: the source device plays with no added delay, and the
mirrored devices are ~40 ms behind it. Two people wearing separate headphones
will not notice. If both outputs are audible in the same room, lower it with
`--latency 15`, or use Setup B.

### Setup B: with a virtual cable

If you want a device to select in Windows that feeds *all* of your real
outputs equally:

1. Install [VB-CABLE](https://vb-audio.com/Cable/) (free, signed driver).
   It adds a playback device called **CABLE Input**.
2. Make **CABLE Input** the default playback device.
3. Run:

   ```
   multiaudio --source "CABLE Input"
   ```

Now everything Windows plays goes into the cable, and `multiaudio` fans it out
to every real device with identical latency — nothing is "first". Rename
CABLE Input to "All Outputs" in Sound settings if you like.

## Options

```
--list                 Show the playback devices and exit.
--source <device>      Device to mirror from: "default", a number from --list,
                       or part of a device name. Default: the Windows default.
--to <name>            Only mirror to devices whose name contains <name>.
                       Repeatable. Default: every other playback device.
--exclude <name>       Never mirror to devices matching <name>. Repeatable.
--latency <ms>         How far the mirrored devices lag the source, 5-500.
                       Lower is tighter but more likely to crackle.
                       Default: 40.
--no-follow-default    Do not reconnect when the default device changes.
--verbose              Print extra detail.
--help                 Show help.
```

Name matching is case-insensitive and matches any part of the name, so
`--to hyperx` is enough for "Headset Earphone (HyperX Cloud II)".

Devices can be plugged in and unplugged while it runs: the streams are
reopened automatically a moment later, and new devices are picked up.

## Building

Nothing to install beyond a compiler; it links only `ole32` and `avrt`, both
part of Windows.

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
x86_64-w64-mingw32-g++ -std=c++17 -municode -O2 -static \
    -o multiaudio.exe src/*.cpp -lole32 -lavrt
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

Source layout: `main.cpp` (command line), `mirror.cpp` (capture + the sink
streams, the interesting part), `devices.cpp` (endpoint enumeration),
`audio.cpp` (format conversion), `ring_buffer.h`, `util.h`.

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
* Mirrored devices lag the source by `--latency`. This is not an A/V sync
  problem for the source device itself, which is untouched.

## License

MIT — see [LICENSE](LICENSE).
