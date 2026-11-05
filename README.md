# BrainBox

BrainBox puts Professor Scotty, a kindly old hologram professor, in your GameCube. Ask him anything by talking into the GameCube Microphone or typing on the keyboard controller, and he answers out loud, floating in a beam of light, using the AI of your choice: open models on **Groq** (Llama and others), **Google Gemini**, **Claude** or **ChatGPT**.

- He's drawn in 3D on the GameCube's own graphics chip, as one smoothly sculpted head: soft lighting, a hologram shimmer, a glow, sparkles and a reflection on the floor. He blinks, raises his eyebrows, puts his spectacles on to think, and his mouth moves with his voice.
- His voice is made on the GameCube itself with [Flite](https://github.com/festvox/flite), the speech engine from Carnegie Mellon University, in its Scottish voice (awb): no account or internet needed for it. Sounds like "Hmm", everyday slang and abbreviations are tidied first so he says them naturally. He starts speaking a sentence at a time, and the speech is kept in the GameCube's audio memory (ARAM).
- The AI is reached directly over HTTPS through the network adapter, with your own API key.

It runs on the GameCube only.

## What you need

- A network adapter (the Broadband Adapter, or a WIZnet-based one such as the ETH2GC) connected to your router.
- An SD card adapter (SD2SP2, SD Gecko or GC Loader).
- To ask questions, either or both of:
  - The GameCube Microphone (the one that came with Mario Party 6 and 7), in memory card slot A or B. Hold its button and talk.
  - The ASCII keyboard controller made for Phantasy Star Online, in any controller port, to type.
- An API key for one of the AI services (see below). Groq and Google Gemini have free allowances; Claude and ChatGPT are pay as you go, and a short chat costs a fraction of a cent.

## Installing

Download `brainbox.zip` from the Releases page and copy the `brainbox` folder in it to your SD card, where your Launcher looks for apps. It holds `default.dol` and `cacert.pem` (the certificates the secure connection is checked with).

## Signing in

The first time, the professor's head is a little foggy: he needs a brain. Choose Groq, Google Gemini, Claude or ChatGPT, and a sign-in page appears with a QR code. Scan it with your phone (or open the address on a computer on the same network). The page links to that company's own site, where you sign in with your account and make an API key; paste the key into the page and press Send. You can also type the key with the keyboard controller (Y).

The GameCube's clock is set from the internet first (secure connections need the right date). The key is saved in `brainbox.ini` beside the app on the SD card and is only ever sent to the company it's for. You can switch to a different AI in Settings > Brain.

## Talking to him

With the microphone in a memory card slot, hold the button on the microphone (or A on the controller), ask your question, and let go. What you said goes to the AI to be written down (it shows in the "You" bubble), then on to him as the question. Up to 15 seconds at a time. While the microphone is being tried out, the last recording is kept as `heard.wav` beside the app, with a few details about it in `mic.txt`.

Groq, Google Gemini and ChatGPT can listen. When Groq is signed in, it always does the listening (with Whisper, which is quick and free), whichever AI answers. Claude can't take sound, so if Claude is his brain, also sign in to Groq (Settings > Brain, then switch back to Claude): Groq writes down what you say, and Claude answers.

## Controls

| Button | |
| --- | --- |
| Microphone button, or A (hold) | With the microphone: talk while you hold it; let go to ask |
| A | Without the microphone: type a question |
| Y | With the microphone and the keyboard: type a question |
| Keyboard | Start typing to ask a question; Enter asks it |
| Control Stick, + Control Pad, C-Stick | Move; scroll his answer |
| X | Stop him talking |
| B | Back, or stop him talking; on the main screen, twice to exit to the Launcher |
| START | Settings |

## Settings

- **Resolution**: Auto (480p with a component cable), 480i or 480p. A new resolution goes back to the old one after 10 seconds unless you press A.
- **Screen**: 4:3, or 16:9 if your TV stretches the picture to fill a widescreen.
- **Picture width**: Full uses the whole width of the TV picture; Standard leaves a border.
- **Overscan**: pulls everything in from the edges if your TV cuts them off.
- **Voice volume**: how loud he speaks, from Off to 10 (10 is a little louder than the voice as it's made).
- **Menu sounds**: how loud the sounds are when you move and choose, from Off to 10.
- **Captions**: shows his answers in a bubble as he speaks.
- **Brain**: which AI he uses (Groq, Google Gemini, Claude or ChatGPT), and signing in to it.
- **Updates**: checks for a newer BrainBox and installs it.
- **Launcher**: which .dol at the root of the SD card Exit to Launcher starts.
- **Exit to Launcher**: leaves BrainBox.

A few more can be set by hand in `brainbox.ini`: `groq.model`, `gemini.model`, `claude.model` and `openai.model` choose a different model for each service (empty for the usual one), `display.glow=0` turns the glow off, and `display.intro=0` skips the opening (Professor Scotty switching on as a hologram, with its sound).

## Updates

BrainBox checks GitHub quietly when it starts, and if there's a newer version it says so. Settings > Updates checks again, and installs the new version with A: it's downloaded and unpacked on the SD card, then replaces this one (your settings and keys are kept), and A again restarts into it. The version it replaced is removed the next time BrainBox starts.

Each release on GitHub has `brainbox.zip` attached, holding the `brainbox` folder (`default.dol`, `opening.bnr` and `cacert.pem`).

If you used it before it was called BrainBox, its settings are carried over from `pixel.ini` the first time.

## Building

It builds with devkitPPC and [libogc2](https://github.com/extremscorner/libogc2). `get-deps.sh` downloads Flite and mbedTLS into `third_party`:

```
sh get-deps.sh
docker run --rm -v "$PWD:/src" -w /src ghcr.io/extremscorner/libogc2:latest make
```

Every push to GitHub builds it too (see the Actions tab), and the result is a zip with the `brainbox` folder ready for the SD card. The fonts are made by `tools/make_assets.py` (needs Python 3 with Pillow and NumPy), which writes `source/ui_assets.c` and `.h`.

## Credits

- Flite: Language Technologies Institute, Carnegie Mellon University (a BSD-style license)
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0) for the secure connection; `cacert.pem` holds Mozilla's trusted root certificates
- libogc2 and libdvm: Extrems and the devkitPro team
- Font: M PLUS Rounded 1c (SIL Open Font License, see `tools/fonts/OFL.txt`)
- [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library): Project Nayuki (MIT)
- The globe's map: drawn from the [world.geo.json](https://github.com/johan/world.geo.json) country outlines (public domain)

BrainBox's own code is under the MIT License (see `LICENSE`).

BrainBox isn't affiliated with Groq, Meta, Google, Anthropic, OpenAI or Nintendo.
