# Test data

`sine440_44k.mp3`, `sine440_48k.mp3`, `sine440_32k.mp3`: 1 s of a 440 Hz sine, stereo,
128 kbit/s, at 44.1, 48 and 32 kHz. Made with:

    ffmpeg -f lavfi -i sine=f=440:d=1 -ar <rate> -ac 2 -b:a 128k -map_metadata -1 sine440_<rate>.mp3
