# ESP32 Penguin Controller
An 8 keys chunithm controller using ESP32-S3
如果你想看中文版 [這裡](README-TW.md)

# Software Part
Firstly, I use **vibe coding** (because I'm suck at coding C qq), so if you think the code is suck don't blame me, it's all GPT-5.3-Codex·Xhigh's fault :D <br>

But whatever, it works well for me(yeah I ensure all commits is working version inside my environment), so if your's don't work, go ask AI :DDDDDD <br>

We got two parts: ESP32 firmware and chuniio, ESP32 read and output the raw values of touch panel and IR Phototransistors, and chuniio use serial to get them, process it into segatools readable value. I don't really know how dll injection works but again AI has done it and I can play my pjsk waiting for compile and flashing XD. <br>

## ESP32 Firmware
If you're using vscode, just install esp-idf, select your board(I'm using ESP32-S3-JTAG... but since I didn't use HID, any ESP32 would work I guess) and COM port, open this folder as a project, and use the bottom button to build, flash, DONE🥳 If something doesn't work, like missing dependancy or missing me...(?how, go ask AI, or [samuelhsieh_ai](https://github.com/samuelhsieh0829/samuelhsieh)🗣️

If you like cli, then type in some esp-idf command
```bash
idf.py set-target esp32s3
idf.py build
```
My copilot say it work, I trust him, thought I'm pretty sure it skip a lot of environment setup and would cause failure.

## Chuniio.dll
I assume you got a gcc, run this:
```bash
$oldPath = $env:PATH; $env:PATH = 'C:\msys64\mingw32\bin;' + $oldPath; & 'C:\msys64\mingw32\bin\gcc.exe' -shared -O2 -Wall -Wextra -std=c11 -static-libgcc -o .\chuniio\chuniio.dll .\chuniio\chuniio.c; $code = $LASTEXITCODE; & 'C:\msys64\mingw32\bin\objdump.exe' -f .\chuniio\chuniio.dll | Out-String; Write-Output ('exit=' + $code); $env:PATH = $oldPath
```
Hmmm kinda sus, maybe the only part you need is:
```bash
'C:\msys64\mingw32\bin\gcc.exe' -shared -O2 -Wall -Wextra -std=c11 -static-libgcc -o .\chuniio\chuniio.dll .\chuniio\chuniio.c
```
I'm not really sure how does it work, but AI always run the upper one, choose the one you like whatever. <br>

Alright now you have a chuniio.dll, place it into your Chunithm HDD, the path is: `bin/chuniio/chuniio.dll` or you can put it wherever you like bc you set the actual path in segatools.ini :P

Now time to set you segatools.ini, you have to change these settings below:
```ini
[chuniio]
; Uncomment this if you have custom chuniio implementation comprised of a single 32bit DLL.
; (will use chu2to3 engine internally)
path={Your chuniio.dll absolute path}
com_port={Your ESP32 COM PORT in integer}
baud_rate=2000000
auto_baud=1
debug_window=1
touch_deadzone=24 ; uhhh idk how to change it
touch_on_threshold=200 ; This one either
touch_off_threshold=70 ; ...?
touch_scale=8 ; AI help me pls
stale_timeout_ms=250 ; ?
air_higher_is_blocked=1 ; ?
air_threshold=3400 ; ?
air_hysteresis=80 ; ?
air1_threshold=3300 ; Finally something I understand
air2_threshold=3400 ; You got 6 IR right?
air3_threshold=3600 ; It stands for the value to trigger air switch
air4_threshold=3700 ; Set the ADC value of each one (raw value could be 0~4095)
air5_threshold=4050 ; You can test it by...
air6_threshold=4060 ; Oh I don't have that file saved, go to your test mode and try

[slider]
; Not sure if it's needed
enable=1
```

You say you don't have Chunithm HDD? ~~Maybe you need to learn how to google~~ Maybe someday I will make it supports some simulator thought I don't even know any one just now.

# Hardware Part
The current version supports 8 keys touch input using ESP32's touch GPIO, why not 16? ESP32 only has 14 built-in touch pins. I'm waiting for my mpr121 shipping, after that I'll make a 16 keys version or even 32 keys depends on my time to solder another 16 wires and make it not look like a mess😵‍💫
 
So the component I use is here:
|name|quantity|
|---|---|
|ESP32-S3|1|
|IR LED|6|
|IR Phototransistor|6|
|1M ohm resistor|6|
|33 ohm resistor|2|
|Diff colors Wires|7 (8 would be ideal)|
|copper tape|a lot|
|Random Costco box|1|
|Paper|2|

Paste your copper tape on a surface like acrylic or cardboard in the shape of rectangle (about 34cm\*9cm for my 23 inch 1080P monitor or 45cm\*10cm to fit the official size, about a 32 inch monitor), use a knife to split them into 8 indepandent pieces not conducting each others, and then solder each pieces of them to wires:
<img src="https://cdn.discordapp.com/attachments/1189565314736857180/1490790310685577226/IMG_4655.jpg?ex=69d5562b&is=69d404ab&hm=aa68dc379b73889ae7f3f0586caeae84d4a6889934ce25d66b62abc5c3beec97&" alt="Touch sensor example">
As you can see, I soldered two wires to each one parts, it is for the future to cut them into 16 parts, every part only has one wire working now. After that, wire these 8 wires to the GPIO 1 to 8 from left to right.

Next up is AIR sersor, this part I really hate AI, it says IR phototransistor requires about 30k ohm resistor to make the pull-up resistor circuit, and 100 ohm for each IR LED, but as I test, the distance of those requires **1M ohm each for IR phototransistor** and **33 ohm for IR LED (maybe less)** to make it possible to stably work, in simple words, AI provides a value that IR LED is too weak to let transistor even know it exists, and it acts like your hands is constantly on the air. So back to circuit, we need to let LEDs and transistors face to face(?, I mean they have to almost be in a same line, my method is to draw each point (in a same line, each distance about 3cm, just like yours😯) at a paper, and then cover the other side with another paper (to draw mirrored), align them and face to light, copy it, Done. Now paste it on both sides of your 3D cardboard box (that's why I use Costco box, I don't need to build a standing thing myself), use something to drill holes and put IR LEDs and transistors into these.
<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790313176862720/IMG_4658.jpg?ex=69d5562c&is=69d404ac&hm=bdba74dce1872f288973338fba80af40c6e47f5ffa7c2c398e0f7b819642d1d5&=&format=webp&width=558&height=744" alt="IR sensor example">
Solder all GND Pins all together, one vcc on LEDs for one side, one wire for one IR transistor
<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790312405237830/IMG_4657.jpg?ex=69d5562c&is=69d404ac&hm=a0ba65c66f2a662cc8ab3bee93c76199b77dae8cadd07ab5fb8ddfcad0c64eb3&=&format=webp&width=558&height=744" alt="IR sensor example">
Circuit:
```
5V----------------33 resistor--Left side LED VCC
   |            |-33 resistor--right side LED VCC
1M resistor(x6)                            |
   /        \                              |
GPIO9~14  IR Phototransistor 1~6----------GND
```
So abstracting, I guess you'll understand right?
<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790311667171560/IMG_4656.jpg?ex=69d5562b&is=69d404ab&hm=951be3fa71db2918001004d05f6fd1e41cc08842588b9bd90df51f094e8a20fd&=&format=webp&width=558&height=744" alt="All circuit">
OwO you just successfully make a chunithm controller🥳
<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790313814524046/IMG_4659.jpg?ex=69d5562c&is=69d404ac&hm=3f97148cfa5e8bbc0112aef8525c5fd33e02fc5dbd831ac9d5ce72b406492a51&=&format=webp&width=558&height=744" alt="Finished Product">

# Resource references
https://github.com/Yona-W/OpeNITHM
https://github.com/Yona-W/OpeNITHM/issues/8
https://github.com/Sucareto/Arduino-Chunithm-Controller
https://github.com/whowechina/chu_pico
https://home.gamer.com.tw/artwork.php?sn=6049032