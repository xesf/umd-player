# Vita audio benchmark

This app measures how fast the ATRAC3plus decoder runs on the Vita CPU, and lets you hear its output.

For every ISO in `ux0:umd/`, the app decodes the first 60 seconds of audio track 0 of the movie, once at 444 MHz and once at 333 MHz. For the first disc, it then plays 15 seconds through the speaker and saves the first 10 seconds as a WAV file.

## Build

```
export VITASDK=~/vitasdk PATH=~/vitasdk/bin:$PATH
cmake -S tests/vita/at3p_bench -B build/vita-at3p_bench
cmake --build build/vita-at3p_bench
```

The output is `build/vita-at3p_bench/at3p_bench.vpk`.

## Run

1. Copy one or more ISO files to `ux0:umd/`.
2. Install `at3p_bench.vpk` with VitaShell and start **UMD audio benchmark**. The screen stays black.
3. After the benchmark, you hear 15 seconds of the first disc's audio. Then press any button to exit.
4. Copy `ux0:data/umdplayer/at3p_bench.txt` and `ux0:data/umdplayer/at3p_bench.wav` back to your computer.

## Reading the log

Each disc has one line per clock speed. The last number is the share of one CPU core that the decoder needs to keep up with playback. The player has a budget of 10% at 333 MHz.

To check the Vita output against the Mac build, decode the same 10 seconds on the Mac and compare:

```
./build/host/umddemux umd/<disc>.iso /tmp/d -n 11
./build/host/at3pdec /tmp/d/audio_00.oma /tmp/mac.wav
```
