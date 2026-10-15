# Test media

Generated for these tests; no third-party audio.

- `sine-1k-44k1-stereo.mp3`: one second of a 1 kHz sine, 44.1 kHz stereo, 128 kb/s, made with
  `ffmpeg -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=1" -ac 2 -c:a libmp3lame -b:a 128k -id3v2_version 0 -map_metadata -1`.
