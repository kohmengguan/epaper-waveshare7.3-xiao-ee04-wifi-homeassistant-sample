# epaper-waveshare7.3-xiao-ee04-wifi-homeassistant-sample
Minimal sample Code for Waveshare 7.3 using Xiao-ee04, fetching from home assistant using pyscript with an automatic 4hour refresh. 

Based on sample from https://github.com/Hilko113/epaper_imageclient_spectra6
and comparison algorithm from https://github.com/Toon-nooT/PhotoPainter-E-Ink-Spectra-6-image-converter

If the colours are off, check the colour palette order in py file as waveshare have different versions of the 7.3 inch display

To run code on home assistant, update configuration.yaml using studio code server in HA or equivalent, and copy the py file to custom components

To adapt to other boards, update the pins

Verified Pins for FireBeetle2 ESP32-C6 + DESPI-C73 (Potential brownout issue connecting to wifi especially for wifi 6, requires additional capacitor to solve)
#define EPD_CS    1
#define EPD_DC    2
#define EPD_RST   3
#define EPD_BUSY  4
#define EPD_SCK   23
#define EPD_MOSI  22



