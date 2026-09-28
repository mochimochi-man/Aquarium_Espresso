# Aquarium Espresso

![Aquarium Espresso](screenshot.jpg)



A small tropical fish tank on an ESP32-S3 + ST7789 (320x240, landscape).

The fish are listed below. They are chosen at random.

Guppy

Neon tetra

Black tetra

Corydoras

Amano shrimp

Silver hatchetfish　* Left alone, it may jump out and disappear.

Translucent glass catfish

Sailfin molly

Green spotted puffer

Platy

Aplocheilus lineatus

Clown loach

Nothobranchius rachovii

(Secret)

There is nothing for you to do.

It is just a small world to watch, and relax.



## Wiring

|ST7789|ESP32-S3|
|-|-|
|SCLK|GPIO12|
|MOSI|GPIO11|
|DC|GPIO9|
|CS|GPIO10|
|RST|tied to 3V3 (set `PIN\_TFT\_RST` to 8 to use it)|
|BLK|tied to 3V3 (use `PIN\_TFT\_BLK` to drive it from a GPIO)|

|microSD|ESP32-S3|
|-|-|
|SCLK|GPIO5|
|MISO|GPIO6|
|MOSI|GPIO7|
|CS|GPIO4|



## Build / flash



Build and flash with the Arduino IDE.

The settings you need are:



PSRAM=OPI

FlashSize=16M

PartitionScheme=huge\_app



You also need to install LovyanGFX as a library.



## Swimming your own character



Make a transparent PNG of 126px x 128px, save it on the SD card as fish.png, and one to three of the guppies are replaced by your character.

If you do not want your own character, you do not have to connect the SD card reader at all.



## License



Copyright of this project belongs to mochimochi-man / Uh (X : calorie0).

It is MIT licensed, so please feel free to use it.

The bundled images were generated with AI, using Grok and Gemini.
