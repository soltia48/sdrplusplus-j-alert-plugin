# sdrplusplus-j-alert-plugin

*日本語: [README.ja.md](README.ja.md)*

Out-of-tree decoder plugin for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)
that demodulates and decodes the **J-ALERT** (全国瞬時警報システム) satellite
downlink — recovering the ground-system-identical NowcastPacket straight from RF,
fully offline and with no reference to the terrestrial system. Gzipped JMA
telegram chunks are additionally inflated to their alert XML.

The J-ALERT satellite link is an unencrypted, standard COTS satellite-modem
waveform:

| Item | Value |
|---|---|
| Modulation | BPSK, continuous SCPC carrier |
| Symbol rate | 256 ksym/s (128 kbps after FEC) |
| FEC | Convolutional K=7, generators (171, 133)₈ ("Voyager"), rate 1/2 |
| Scrambler | IESS-308 self-synchronising, 1 + x³ + x²⁰ |
| Framing | HDLC (0x7E flags, bit-stuffing), CRC-16/X.25 FCS |
| Payload | NowcastPacket (chunked); JMA telegram chunks carry a gzip member → alert XML |
| Encryption | none |

## Signal chain

```
VFO IQ (1.024 MHz)
 └─ coarse carrier recovery  (block FFT-argmax on the squared signal → 2·fc)
 └─ polyphase resample to 1.024 MHz (4 sps × 256 ksym/s)
 └─ AGC
 └─ matched filter + symbol timing  (polyphase RRC β=0.35 + Müller-Müller TED)
 └─ decision-directed Costas loop  → soft symbols
 └─ soft-decision Viterbi  (K=7, (171,133), streaming traceback)
 └─ double-differential decode + invert      (resolves BPSK 180° ambiguity)
 └─ IESS-308 self-sync descramble  (s ⊕ s≪3 ⊕ s≪20)
 └─ HDLC deframe + destuff  (0x7E flags, CRC-16/X.25 validated)
 └─ NowcastPacket parse  (header + chunk-entries; per-chunk body CRC-32)
 └─ chunk reassembly  → recovered file, byte-exact with the ground system
                        (multi-chunk / multi-packet, by id + seq)
 └─ [optional] gzipped JMA telegram chunks (flags==3, wrmx/eprx/issx/ioeq):
        JMA Socket Packet header → gunzip → JMA alert XML
```

The byte-exact recovered artifact is the NowcastPacket / reassembled chunk file;
inflating it to XML is an optional final step that applies only to gzipped JMA
telegram chunks (`flags==3`). Non-gzipped or non-JMA chunks are still recovered
and reported, just not turned into XML.

The NowcastPacket / chunk parser is fully structured: `NWCS` header,
`chunk-entry` table, per-chunk `body_crc32` verification, chunk metadata
(`filename`, `flags==3` = gzipped), and the chunk-type-code
(`wrmx`/`eprx`/`issx`/`ioeq`) as the content category. Files split across chunks
or packets are reassembled by `(id, seq, num_chunks)`. The packet `header_crc32`
is intentionally ignored (a known protocol-converter bug makes it always wrong).

The pairing phase of the rate-1/2 code is the only residual ambiguity; the
receiver runs two Viterbi+frame chains in parallel (one per phase) and lets the
HDLC FCS pick the correct one — the wrong phase produces only CRC-failing
garbage. Carrier-phase / bit-polarity ambiguity is absorbed by the differential
decode, so no inversion search is needed.

## Repository layout

```
.
├── .devcontainer/        # VS Code Dev Container (Ubuntu 24.04 + cross toolchains + zlib)
├── cmake/toolchains/     # aarch64 / armhf CMake toolchain files
├── external/SDRPlusPlus/ # SDR++ source (scripts/fetch-sdrpp.sh)
├── scripts/              # fetch-sdrpp.sh, build-all.sh, build-windows.ps1, fetch-windows-deps.py
├── src/
│   ├── main.cpp          # SDR++ module + panel
│   ├── dsp/              # complex, agc, fft, resampler, RRC, PFB clock sync, BPSK demod
│   ├── fec/              # conv code + streaming soft Viterbi
│   ├── frame/            # differential, descrambler, HDLC, NowcastPacket
│   ├── decode/           # gunzip, alert model, bit pipeline, receiver
│   ├── sink/             # file / TCP JSONL sinks + alert JSON serializer
│   └── ui/               # sparkline, constellation view
├── CMakeLists.txt
└── README.md
```

## Building

### Dev Container (all targets)

```sh
scripts/build-all.sh                 # linux-x86_64, linux-aarch64, linux-armhf → dist/
TARGETS="linux-aarch64" scripts/build-all.sh
BUILD_TYPE=Debug scripts/build-all.sh
```

`build-all.sh` refreshes the SDR++ source, then configures/builds each target
with CMake (Ninja if available, otherwise Make) and collects the shared library
into `dist/<target>/`.

### Single native build

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The plugin only needs SDR++ headers plus zlib; `sdrpp_core` symbols are resolved
at load time.

## Releases

GitHub Actions ([.github/workflows/build.yml](.github/workflows/build.yml))
builds the plugin for Linux **x86_64**, **x86 (i686)**, **ARM64 (aarch64)**,
**ARM32 (armhf)** and **Windows x64** on every push and pull request, and
attaches the four `.so` files and the `.dll` to a GitHub Release whenever a `v*`
tag is pushed (e.g. `git tag v1.0.0 && git push --tags`). zlib is statically
linked, so each binary is self-contained.

The Windows job runs `scripts/build-windows.ps1` on `windows-2022`, linking
against the SDR++ nightly build current at the time of the run.

### Windows (x64, MSVC)

A Windows DLL cannot leave `sdrpp_core` symbols unresolved, so the plugin links
import libraries generated from the official SDR++ Windows build (nightly,
which tracks the same `master` commit as the fetched headers). Requirements:
Visual Studio 2022 or Build Tools with the C++ workload, git, and Python 3.14+.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
# options: -BuildType Debug, -SkipFetch (reuse external/ deps)
```

The script imports the MSVC environment, runs `scripts/fetch-windows-deps.py`
(SDR++ Windows zip → `.def` export lists, fftw3/volk headers from
conda-forge), builds zlib statically, and writes
`dist\windows-x64\j_alert_decoder.dll`. Copy it into the `modules` folder next
to `sdrpp.exe`. The DLL must match the SDR++ build it is loaded into; rebuild
after updating SDR++.

## Installing into SDR++

Copy `j_alert_decoder.so` into the SDR++ plugin directory (Linux default
`/usr/lib/sdrpp/plugins/`) — on Windows, `j_alert_decoder.dll` into the
`modules` folder next to `sdrpp.exe` — or load it from the Module Manager and create an
instance. Tune the VFO onto the J-ALERT carrier; the panel shows lock state, a
BPSK constellation, carrier/Costas offsets, the estimated bit error rate, HDLC
frame counts, and the latest decoded alert (title / headline / time). Recovered
files can be:

- streamed as JSONL to a **file** and/or a **TCP** port — every recovered file,
  with its payload inline (the alert XML for gzipped JMA telegrams, or base64 of
  the raw bytes for non-XML telegrams) — see
  [docs/JSONL_FORMAT.md](docs/JSONL_FORMAT.md)
  ([日本語](docs/JSONL_FORMAT.ja.md)),
- written to an **Output folder** (enable with the **File output** checkbox):
  decoded JMA telegrams as `<timestamp>.xml`, other (non-XML) telegrams as the
  raw bytes in `<timestamp>.<chunk_type>.bin`.

Output paths default to the SDR++ config root: the **Output folder** to
`~/.config/sdrpp/j_alert/` and the **JSONL file** to
`~/.config/sdrpp/j_alert/decoded.jsonl`. Both can be changed in the panel.

All output settings persist per instance in the SDR++ config.

## Platform support

| Target          | Supported |
|-----------------|-----------|
| Linux x86_64    | yes       |
| Linux aarch64   | yes       |
| Linux armhf     | yes       |
| Windows x64     | yes       |
| macOS           | no        |

## License

MIT License — see [LICENSE](LICENSE). Copyright (c) 2026 KIRISHIKI Yudai.
