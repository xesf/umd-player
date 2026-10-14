# Vita decoder test

This app checks whether the Vita hardware H.264 decoder (`sceAvcdec`) can decode UMD Video streams, including the field-coded discs.

For every ISO in `ux0:umd/`, the app seeks to 25% into the longest title and decodes 600 access units straight to the screen. If a disc fails, the app tries it again after calling `sceAvcdecSetInterlacedStreamMode`. That function is not in the VitaSDK headers, and its signature is a guess.

## Build

```
export VITASDK=~/vitasdk PATH=~/vitasdk/bin:$PATH
cmake -S tests/vita/avcdec -B build/vita-avcdec
cmake --build build/vita-avcdec
```

The output is `build/vita-avcdec/avcdec_test.vpk`.

## Run

1. Copy one or more ISO files to `ux0:umd/`. Include a field-coded disc, for example *Rhyme and Reason* or *The Up in Smoke Tour*.
2. Install `avcdec_test.vpk` with VitaShell and start **UMD decoder test**.
3. Video plays in the top-left corner of the screen for each disc. When the screen stops changing, press any button to exit.
4. Copy `ux0:data/umdplayer/avcdec_test.txt` back to your computer.

## Reading the log

Each disc has a `result:` line. A disc passes when it has 0 errors and more than 0 frames out. For a field-coded disc, about 300 frames from 600 access units means the decoder combines field pairs into frames.

If the app stops during a disc, the log ends at the last step that worked.
