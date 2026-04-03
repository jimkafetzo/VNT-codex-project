VNT is a custom audio codec that is designed for embedded systems with limited resources. It is event driven and takes samples only when above a certain threshold. 

* it has 12-bit stereo sound
* is compressed with delta encoding
* it uses bit-packing to be as memory efficient as possible
* it is very lightweight requiring just 8 bytes of ram for playback and having at least 100 times less computational load than a conventional format like mp3

How it works: 

Its based on the <stdint.h> library and uses linear interpolation, thus securing compatibility with most devices.
For now it only supports clean 16bit wav files at 44.1kHz with stereo or mono sound

