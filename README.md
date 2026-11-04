# Professor Pixel

Professor Pixel is a kindly old hologram professor who lives in your GameCube. Ask him anything with the keyboard controller, and he answers out loud, floating in a beam of light, using the AI of your choice: **Google Gemini**, **Claude** or **ChatGPT**.

- He's drawn in 3D on the GameCube's own graphics chip: soft lighting, a hologram shimmer, a glow, sparkles and a reflection on the floor. He blinks, raises his eyebrows, puts his spectacles on to think, and his mouth moves with his voice.
- His voice is made on the GameCube itself with [Flite](https://github.com/festvox/flite), the speech engine from Carnegie Mellon University: no account or internet needed for it. He starts speaking a sentence at a time, and the speech is kept in the GameCube's audio memory (ARAM).
- The AI is reached directly over HTTPS through the network adapter, with your own API key.

It runs on the GameCube only.

## What you need

- A network adapter (the Broadband Adapter, or a WIZnet-based one such as the ETH2GC) connected to your router.
- An SD card adapter (SD2SP2, SD Gecko or GC Loader).
- The ASCII keyboard controller made for Phantasy Star Online, in any controller port, to type your questions. (The GameCube Microphone is coming in a later version.)
- An API key for one of the AI services (see below). Google Gemini has a free allowance; Claude and ChatGPT are pay as you go, and a short chat costs a fraction of a cent.

## Installing

Download `pixel.zip` from the Releases page and copy the `pixel` folder in it to your SD card, where your Launcher looks for apps. It holds `default.dol` and `cacert.pem` (the certificates the secure connection is checked with).

## Signing in

The first time, the professor's head is a little foggy: he needs a brain. Choose Google Gemini, Claude or ChatGPT, and a sign-in page appears with a QR code. Scan it with your phone (or open the address on a computer on the same network). The page links to that company's own site, where you sign in with your account and make an API key; paste the key into the page and press Send. You can also type the key with the keyboard controller (Y).

The key is saved in `pixel.ini` beside the app on the SD card and is only ever sent to the company it's for. You can switch to a different AI in Settings > Brain.

## Controls

| Button | |
| --- | --- |
| Keyboard | Start typing to ask a question; Enter asks it |
| A | Ask a question |
| Control Stick, + Control Pad, C-Stick | Move; scroll his answer |
| X | Stop him talking |
| B | Back, or stop him talking; on the main screen, twice to exit to the Launcher |
| START | Settings |

## Settings

- **Resolution**: Auto (480p with a component cable), 480i or 480p. A new resolution goes back to the old one after 10 seconds unless you press A.
- **Screen**: 4:3, or 16:9 if your TV stretches the picture to fill a widescreen.
- **Picture width**: Full uses the whole width of the TV picture; Standard leaves a border.
- **Overscan**: pulls everything in from the edges if your TV cuts them off.
- **Voice volume**: how loud he speaks, from Off to 10.
- **Menu sounds**: how loud the sounds are when you move and choose, from Off to 10.
- **Captions**: shows his answers in a bubble as he speaks.
- **Brain**: which AI he uses, and signing in to it.
- **Launcher**: which .dol at the root of the SD card Exit to Launcher starts.
- **Exit to Launcher**: leaves Professor Pixel.

A few more can be set by hand in `pixel.ini`: `gemini.model`, `claude.model` and `openai.model` choose a different model for each service (empty for the usual one), and `display.glow=0` turns the glow off.

## Building

It builds with devkitPPC and [libogc2](https://github.com/extremscorner/libogc2). `get-deps.sh` downloads Flite and mbedTLS into `third_party`:

```
sh get-deps.sh
docker run --rm -v "$PWD:/src" -w /src ghcr.io/extremscorner/libogc2:latest make
```

Every push to GitHub builds it too (see the Actions tab), and the result is a zip with the `pixel` folder ready for the SD card. The fonts are made by `tools/make_assets.py` (needs Python 3 with Pillow and NumPy), which writes `source/ui_assets.c` and `.h`.

## Credits

- Flite: Language Technologies Institute, Carnegie Mellon University (a BSD-style license)
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0) for the secure connection; `cacert.pem` holds Mozilla's trusted root certificates
- libogc2 and libdvm: Extrems and the devkitPro team
- Font: M PLUS Rounded 1c (SIL Open Font License, see `tools/fonts/OFL.txt`)

Professor Pixel isn't affiliated with Google, Anthropic, OpenAI or Nintendo.
