# Videokey

A ESP32-S3 based Video player keyring. Written using Espressif's own SDK. Plays mjpeg files from an SD card.

This project is adapted for the [**Waveshare esp32-s3 LCD-1.47B**](https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B), however it can also be easily modified to work with any esp32 chip and ST7789 based display.

## Installation

1. [Install esp-idf SDK](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
2. Setup esp-idf enviornment
3. Plugin your board
4. Build and flash with one command `idf.py flash`
5. Enjoy

## Usage

### SD Card setup

1. Format the SD card as a FAT32 and use mbr partion table
2. Then create a `mjpeg` and `font` folder in the root
3. Place the `inconsolata.fnt` in the `font` folder of the SD card from the `font` folder of this project
4. Place any mjpeg in the `mjpeg` folder in the SD card

### Converting videos to mjpeg

You can use a simple ffmpeg command to generate a compatible mjpeg file.
```bash
ffmpeg -i "<input_file>" -vf "scale=172:320" -vcodec mjpeg -pix_fmt yuvj420p -q:v 6 -r 24 output.mjpeg
```
Simply replace `<input_file>` with the video file you want to convert, this should work on any video format including gifs.

#### Video details
- mjpeg format
- 24 fps
- dimensions: 172x320px (320x172px for landscape video)

### Button Guide

- Press for a short time goes to the next video file or goes to the next item in the menu
- Press and hold for half a second to bring up the menu or select an item in the menu

### Menu
- Battery and brightness information at the top
- Sleep option puts the esp32 to sleep, where it sips 5uA and switches off the display, pressing the reset button reboots the chip
- You can change the brightness with the plus and minus options
- The exit option exits the menu back to the video player

## License

This project is licensed under [AGPL 3.0](https://www.gnu.org/licenses/agpl-3.0.en.html).
