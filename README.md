# BrainBox

BrainBox puts Professor Scotty, a kindly old hologram professor, in your GameCube. Ask him anything by talking into the GameCube Microphone or typing on the keyboard controller, and he answers out loud, floating in a beam of light, using the AI of your choice: open models on **Groq** (Llama and others), **Google Gemini**, **Claude** or **ChatGPT**.

- He's drawn in 3D on the GameCube's own graphics chip, as one smoothly sculpted head: soft lighting, a hologram shimmer, a glow, sparkles and a reflection on the floor. He blinks, raises his eyebrows, puts his spectacles on to think, and his mouth moves with his voice.
- His voice is made on the GameCube itself with [Flite](https://github.com/festvox/flite), the speech engine from Carnegie Mellon University, in its Scottish voice (awb): no account or internet needed for it. Sounds like "Hmm", everyday slang and abbreviations are tidied first so he says them naturally. He starts speaking a sentence at a time, and the speech is kept in the GameCube's audio memory (ARAM).
- The AI is reached directly over HTTPS through the network adapter, with your own API key.

It runs on the GameCube only.

## Two holograms

Settings > Hologram chooses who answers you: Professor Scotty, a kindly old Scottish professor, or Dr. Brahms, a bright young American scholar and librarian with a bun and oversized round glasses. Each has a voice of their own, made on the GameCube with Flite (Scotty: the awb voice; Dr. Brahms: the slt voice), and only the one in use is loaded. The voices are the two `.flitevox` files in the `brainbox` folder.

## What you need

- A network adapter (the Broadband Adapter, or a WIZnet-based one such as the ETH2GC) connected to your router.
- An SD card adapter (SD2SP2, SD Gecko or GC Loader).
- To ask questions, either or both of:
  - The GameCube Microphone (the one that came with Mario Party 6 and 7), in memory card slot A or B. Hold its button and talk.
  - The ASCII keyboard controller made for Phantasy Star Online, in any controller port, to type.
- An API key for one of the AI services (see below). Groq and Google Gemini have free allowances; Claude and ChatGPT are pay as you go, and a short chat costs a fraction of a cent.

## Installing

Download `brainbox.zip` from the Releases page and copy the `brainbox` folder in it to your SD card, where your Launcher looks for apps. It holds `default.dol`, `cacert.pem` (the certificates the secure connection is checked with) and the two voices, `voice_scotty.flitevox` and `voice_her.flitevox`.

## Signing in

The first time, the professor's head is a little foggy: he needs a brain. Choose Groq, Google Gemini, Claude or ChatGPT, and a sign-in page appears with a QR code. Scan it with your phone (or open the address on a computer on the same network). The page links to that company's own site, where you sign in with your account and make an API key; paste the key into the page and press Send. You can also type the key with the keyboard controller (Y).

The GameCube's clock is set from the internet first (secure connections need the right date). The key is saved in `brainbox.ini` beside the app on the SD card and is only ever sent to the company it's for. You can switch to a different AI in Settings > Brain.

## Talking to him

With the microphone in a memory card slot, hold the button on the microphone (or A on the controller), ask your question, and let go. What you said goes to the AI to be written down (it shows in the "You" bubble), then on to him as the question. Up to 15 seconds at a time. While the microphone is being tried out, the last recording is kept as `heard.wav` beside the app, with a few details about it in `mic.txt`.

Groq, Google Gemini and ChatGPT can listen. When Groq is signed in, it always does the listening (with Whisper, which is quick and free), whichever AI answers. Claude can't take sound, so if Claude is his brain, also sign in to Groq (Settings > Brain, then switch back to Claude): Groq writes down what you say, and Claude answers.

## The weather, the date and the time

Ask "what's the weather?", "will it snow tomorrow?" or "how cold is it in Paris?" and he looks it up and answers in his own words, with today's conditions and a three-day forecast. This works with every brain and needs no extra account: the weather comes from [Open-Meteo](https://open-meteo.com/), which is free and needs no key.

When you ask Professor Scotty about the weather somewhere, his globe turns to show the place, with a little glowing mark on it.

He also knows today's date and the local time, so "what day is it?" or "how long until Christmas?" just work.

For your own weather he needs to know where you are. Settings > Home town is Auto at first: BrainBox guesses your town from your internet connection (with GeoJS, or ipwho.is if that doesn't answer), and the row shows the town it found. Internet providers' guesses can be a town or two off, so if it's wrong, choose Home town and type yours with the keyboard controller ("Detroit", or "Paris, France" to be exact). X puts it back to Auto. Once found, the town is kept in `hometown.txt` beside the app, so it isn't looked up again each time BrainBox starts (on Auto it's checked again once a month, in case you've moved; delete the file to look it up again). Temperatures are in Fahrenheit in the US and Celsius elsewhere.

## Looking things up

Every brain can look things up on Wikipedia, with no extra account or key: when a question needs facts about a particular person, place, book, film or event, he looks it up and answers starting with something like "According to Wikipedia". "What's in the news?" (or "any news about space?") gets the latest headlines from Google News, read out in his own words.


With Google Gemini signed in (Settings > Brain; it can be signed in alongside Groq, whichever AI is his brain), Professor Scotty can look things up with Google Search when a question needs today's information: news, recent events, scores, prices. Your home town is passed along too, so "near me" questions get local answers. His brain decides when; while he searches, the bar says "Let me look that up on the web", and his answer starts with something like "According to Google". The searches use Gemini 2.5 Flash, which includes up to 500 searches a day on Google's free allowance. `chat.web_search=0` in `brainbox.ini` turns this off. Without a Gemini key, he says he can't look things up yet and tells you how to turn it on.

## Quiz night

Say "let's play a quiz" (or "quiz me on dinosaurs", or "trivia about space") and the hologram hosts a quiz game. Professor Scotty or Dr. Brahms asks the topic (unless you named one), how many are playing (up to four), each player's name, and how many questions each (five, ten or fifteen). Then the questions go round in turn: say your answer into the microphone (hold A or the mic's button) or type it, and the host judges it kindly, with small slips and other ways of saying the same thing allowed. "Pass" or "I don't know" moves on. X hears the question again, and B stops the quiz. The scores are kept on a little brass abacus that floats off to the host's left in place of the globe, with a gold bead sliding across for each point, and the winner is announced at the end. The questions are written fresh each time by the AI you chose, so every quiz is different. Answering needs the microphone or the keyboard controller; to answer out loud with Claude as his brain, also sign in to Groq (as for talking to him).

## He remembers you

BrainBox remembers a little about you from one day to the next: your name once you've told him, things you mention about yourself (your dog's name, the books you love, a trip you're planning), and the last few things you asked about. He brings them up now and then when they fit, and might greet you by name. It's kept in `memory.txt` beside the app on the SD card, and only ever sent to the AI you chose, along with your questions. Say "forget everything you know about me" to clear it, or delete the file; `chat.memory=0` in `brainbox.ini` turns it off.

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
- **Menu sounds**: how loud the sounds are when you move and choose, from Off to 10. The hologram's quiet background sound (a soft, slowly breathing chord with the odd far-off bell, made on the GameCube like his voice) follows it too: it dips while he talks and goes silent while the microphone listens.
- **Hologram**: Professor Scotty or Dr. Brahms.
- **Hologram sound**: the hologram's quiet background hum and far-off bells, On or Off (how loud it is follows Menu sounds).
- **Captions**: shows the answers in a bubble as they're spoken.
- **Brain**: which AI he uses (Groq, Google Gemini, Claude or ChatGPT), and signing in to it.
- **Home town**: where you are, for the weather and the local time. Auto guesses from your internet connection; A types a town (with the keyboard controller), X goes back to Auto.
- **Updates**: opens Software Update, to see what's new in a newer BrainBox, install it and restart into it.
- **Launcher**: which .dol at the root of the SD card Exit to Launcher starts.
- **Exit to Launcher**: leaves BrainBox.

A few more can be set by hand in `brainbox.ini`: `groq.model`, `gemini.model`, `claude.model` and `openai.model` choose a different model for each service (empty for the usual one), `display.glow=0` turns the glow off, `chat.web_search=0` stops him looking things up with Google, `chat.memory=0` stops him remembering you between visits, `display.fps=1` shows the frame rate and how each frame's time is spent, and writes `boot.txt` (how long starting up took) and `speech.txt` (the last answer, sentence by sentence) beside the app, for tracking down problems, and `display.intro=0` skips the opening (Professor Scotty switching on as a hologram, with its sound). Holding Z as BrainBox starts skips it just that once.

## Updates

BrainBox checks GitHub quietly when it starts, and if there's a newer version it says so. Settings > Updates opens the Software Update window: it checks again, shows what's new in the newest version (the notes from its GitHub release), and installs it with Install: it's downloaded and unpacked on the SD card, then replaces this one (your settings and keys are kept). Restart Now starts the new version straight away. An install carries on if you close the window, and the Updates row shows how it's going. The version it replaced is removed the next time BrainBox starts.

Each release on GitHub has `brainbox.zip` attached, holding the `brainbox` folder (`default.dol`, `opening.bnr`, `cacert.pem` and the two voice files).

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
- Facts from [Wikipedia](https://en.wikipedia.org/) (CC BY-SA 4.0); headlines from Google News (or BBC News)
- Weather data by [Open-Meteo.com](https://open-meteo.com/) (CC BY 4.0); town lookup from your connection by [GeoJS](https://www.geojs.io/) and [ipwho.is](https://ipwho.is/)
- The globe's map: drawn from the [world.geo.json](https://github.com/johan/world.geo.json) country outlines (public domain)

BrainBox's own code is under the MIT License (see `LICENSE`). The notices for the software and data above that are built into BrainBox are in `THIRD_PARTY_NOTICES.md`.

BrainBox isn't affiliated with Groq, Meta, Google, Anthropic, OpenAI or Nintendo.
