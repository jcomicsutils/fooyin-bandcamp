# fooyin-bandcamp

A [fooyin](https://github.com/ludouzi/fooyin) plugin that lets you stream Bandcamp albums and tracks directly inside the player.

---

> [!CAUTION]
> **This plugin was written entirely by Claude AI.** It was made for personal use by someone who knows nothing about C++. It works on my machine, but I cannot vouch for the code quality or safety. **Please read and understand the source before building or running it.** The usual caveats around AI-generated systems code apply, there's a big chance of bugs, memory issues, or other problems that I wouldn't know how to spot.

---

> [!WARNING]
> **Known bug: SIGSEGV on rapid track changes or playing new tracks after closing fooyin.**
> If you switch tracks too quickly the player will segfault and crash. **If you can fix it, please open a PR, it would be genuinely appreciated.**

---

## Features

- Album art, artist, title, duration, and track numbers all populated
- Add tracks to an existing fooyin playlist or create a new one
- Stream URLs are re-resolved at decode time (Bandcamp CDN links expire after ~1 hour)
- fooyin is able to scrobble loaded tracks

---

## Building from source

```bash
git clone https://github.com/jcomicsutils/fooyin-bandcamp.git
cd fooyin-bandcamp

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/fooyin/install

cmake --build build --parallel
cp build/bandcampinput.so ~/.local/lib/fooyin/plugins/
```

---

## Usage

1. Open fooyin and go to **Library → Stream from Bandcamp…**
2. Paste an album or track URL
3. Click **Fetch** and wait for the tracklist to load
4. Select a target playlist from the dropdown, then click **Add to Playlist** or click **Add to New Playlist** to create one on the fly
5. Play tracks as normal

---

## License

see [LICENSE](LICENSE).
