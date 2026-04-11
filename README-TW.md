# ESP32 企鵝控制器
一款基於 ESP32-S3 的8鍵中二手台
If you want to read English version [here](README.md)

# 軟體部分

首先，這整個專案的程式90%都是我**vibe coding**出來的(因為我根本不太會寫C qq，尤其是動態庫這我完全不懂，韌體只學過Arduino，esp-idf還太菜)，所以如果你覺得這是坨屎山，那不是我的問題，是 GPT-5.3-Codex·Xhigh的錯 :D <br>

但至少在我這裡跑得動(我基本上只會commit能動的版本)，所以如果你不能用，就去問 AI 吧 :DDDDDD <br>

軟體可以分為兩個部分：ESP32韌體和chuniio。 ESP32負責讀取並輸出觸控面板和紅外線光電晶體的原始數值，而chuniio透過序列埠取得這些值，轉換成中二軟體看得懂的格式然後用一些我不懂但聽起來很屌的方式注入近遊戲裡面。對我就說我不會dll injection，但反正AI幫我弄好了，我負責在旁邊玩世界計畫等它跑就好了XD<br>

如果你相信我，可以直接下載[release包](artifacts\chunithm-release.zip)，就不用自己編譯了(當然那個包也是AI幫我生好的)

## ESP32 韌體

如果你用VSCode，安裝 esp-idf的插件，選擇開發板(我用ESP32-S3-JTAG……，就是帶USB-OTG的版本，但因為我沒用到HID，所以理論上任何ESP32都應該要能運作)，以及COM口，將此資料夾作為Project打開，然後使用底部的按鈕進行build、flash就行啦🥳如果有啥不能運作，像是缺了什麼依賴、還是缺了有我的春天(?，那就去問AI吧，或這是問[samuelhsieh_ai](https://github.com/samuelhsieh0829/samuelhsieh)🗣️

如果您喜歡CLI，那AI說可以直接打esp-idf的指令：

```bash
idf.py set-target esp32s3

idf.py build
```

對Copilot說這樣可以就是可以，我對它非常的信任，因為它會幫我處理好依賴問題，我相信你各位一定不會有問題的對吧<br>對吧?

## Chuniio.dll

我想大家都有gcc吧，執行這段指令：

```bash
$oldPath = $env:PATH; $env:PATH = 'C:\msys64\mingw32\bin;' + $oldPath; & 'C:\msys64\mingw32\bin\gcc.exe' -shared -O2 -Wall -Wextra -std=c11 -static. $code = $LASTEXITCODE; & 'C:\msys64\mingw32\bin\objdump.exe' -f .\chuniio\chuniio.dll | Out-String; Write-Output ('exit=' + $code); $env:PATH = $oldPath
```

Hmmm也太長，感覺env很多餘?但我也不知道能不能改，或許大部分人執行這一行就好了?

```bash
'C:\msys64\mingw32\bin\gcc.exe' -shared -O2 -Wall -Wextra -std=c11 -static-libgcc -o .\chuniio\chuniio.dll .\chuniio\chuniio.c
```

我不太確定這樣對不對，AI都是跑上面的那個，反正你們應該比我聰明，就是編譯個dll對各位大佬來說有什麼難的🛐 <br>

好了，現在你有了chuniio.dll，把它放到你的Chunithm HDD裡面，路徑是：`bin/chuniio/chuniio.dll`，或者你可以把它放在任何地方，因為實際路徑是在segatools.ini裡設定的 :P

現在來處理segatools.ini，修改以下設定：

```ini
[chuniio]

path={chuniio.dll的絕對路徑}

com_port={ESP32的COM數字部分}

baud_rate=2000000

auto_baud=1

debug_window=1

touch_deadzone=24 ; 痾我不知道這能幹嘛

touch_on_threshold=200 ; 阿

touch_off_threshold=70 ; 這

touch_scale=8 ; 救命啊codex

stale_timeout_ms=250 ; 我真的

air_higher_is_blocked=1 ; 跨

air_threshold=3400 ; ㄅㄜˊ

air_hysteresis=80 ; 阿阿阿阿阿

air1_threshold=3300 ; 終於有我能理解的了

air2_threshold=3400 ; 6個紅外線感測

air3_threshold=3600 ; 這是觸發上滑的值

air4_threshold=3700 ; 設定每個開關的類比值

air5_threshold=4050 ; 反正就是找到你手放在感測器中間的質就對了

air6_threshold=4060 ; 痾但測試的程式被蓋掉了 你們加油

[slider]
; 其實我不確定這要不要改，反正我這樣子用是ok的
enable=1
```

你說你沒有Chunithm HDD？ ~~也許你需要學習怎麼Google~~ 也許有一天我會讓它支援模擬器，但前提是我要知道有什麼模擬器，對我甚至不知道中二有沒有像maimai的AstroDX這種存在

# 硬體部分

目前版本支援 ~~8~~16鍵觸控輸入，使用 ESP32 + MPR121 觸控模組。 ~~為什麼不是16鍵？因為ESP32只有14個內建觸控腳位。我在等MPR121寄到，之後會做16鍵甚至32鍵版本。~~

耶我終於收到 MPR121ㄌ（大概等了一個禮拜qq），所以現在有16鍵版本了🥳。舊的8鍵版本（使用ESP32內建觸控GPIO）我放在這個 [branch](https://github.com/samuelhsieh0829/esp32-chunithm-controller/tree/ESP32-on-board-touch-version-(8k))。

我用的零件如下：

|名稱|數量|
|---|---|
|ESP32-S3|1|
|MPR121 模組|2（如果你要做32鍵就3個）|
|紅外線LED|6|
|紅外線光電晶體管|6|
|1M歐姆電阻|6|
|33歐姆電阻|2|
|不同顏色的導線|7種顏色（8種會比較好，不管是用來區分IR或是觸控都很有用）|
|銅箔膠帶|一堆|
|Costco大箱子|1|
|紙|2|

## 觸控感測器
先把銅箔膠帶貼在壓克力或紙板上，做成一條長方形（我自己是34cm\*9cm，對應23吋1080P螢幕；官機大概45cm\*10cm，約32吋螢幕尺寸）。再用美工刀切成 ~~8~~16 塊彼此不導通的區域，然後每一塊焊一條導線：

<img src="https://cdn.discordapp.com/attachments/1189565314736857180/1490790310685577226/IMG_4655.jpg?ex=69d5562b&is=69d404ab&hm=aa68dc379b73889ae7f3f0586caeae84d4a6889934ce25d66b62abc5c3beec97&" alt="觸控感應器範例">

~~你可以看到我以前每塊焊兩條線，是為了之後從8鍵切成16鍵，當時每塊只有一條真的在用。~~ <br>
焊好之後可以再貼一層霧面膠帶，摸起來會比較滑，類似霧面手機膜的觸感，這樣就不用戴手套打ㄌ <br>
接著把16條觸控線接到兩顆MPR121（要做32鍵的話接法和程式都要自己改，建議直接fork自己改）

但在接線前，先處理其中一顆MPR121的位址：你要把其中一顆的addr接地連線切斷，翻到背面，用美工刀割掉PCB裡面那條線，然後讓它短接到SDA，這樣我們就有兩顆：
- 沒割線的叫 **mpr121-1**
- 割線後短接到SDA的叫 **mpr121-2**

觸控線由左到右接法是：`mpr121-1` 的 0~11，再接 `mpr121-2` 的 0~3。兩顆的 SDA 都接 ESP32 GPIO7，兩顆的 SCL 都接 ESP32 GPIO8。這裡附一個跟我的程式碼一樣醜但勉強能看的圖：

```
ESP32---3V3(3.3V), GND to VCC, GND (這個應該不用畫吧)
     |-GPIO8------------
     |          *       |
     GPIO7-----------|  |
     /\  「-----*    |  |
     ADD SDA SCL    SDA SCL
    mpr121-2      mpr121-1
...,3,2,1,0 ___12,...,1,0
    \ \ | |//----|   / /
     \ \|///......../ /
      IIIIIIIIIIIIIIII

對那個星星是傳送門 應該看得懂吧
```

<img src="https://cdn.discordapp.com/attachments/1189565314736857180/1492603980772147463/IMG_4786.jpg?ex=69dbef48&is=69da9dc8&hm=da6ad1d8a9e3d2b0b54d1e3cd794a26374cfdc3ffa2992bc9ac1cc70ff9d2948&" alt="觸控感測器接線範例">

## 空中感測器
接下來是AIR感測器，這部分AI真的拉完了，它說IR光電晶體管需要一個大約30k歐姆的電阻來組上拉電阻，每個IR LED需要100歐姆的電阻，但我測試發現，它們之間的距離需要**每個IR光電晶體管需要 1M 歐姆的電阻**，**每個IR LED需要33歐姆（可能更低）**才能穩定運作。簡單來說，照AI說的做會得到一個拍照永遠沒曝光的照片，你根本不知道是手遮住了鏡頭還是沒有燈光。回到電路，我們需要讓LED和電晶體位在同一直線上面對面，為了讓它們對齊，我的方法是先在一張紙上畫出每個點（在同一條斜直線上，每個點之間的距離大約3公分，跟鄉民們差不多😯），然後把另一張紙完整貼在另一面，將它們對齊並朝向光源，把第一張紙的點描到第二張，就可以描出鏡像的位置，也就是兩邊會是對齊的樣子。把它們貼到紙箱的兩面（這就是為什麼我用Costco的紙箱，這樣我就不用自己製作兩跟支架之類的了），用你螺絲起子或筆之類尖尖的東西(像你的下面一樣)鑽孔，然後把紅外線 LED 和晶體管放進去。

<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790313176862720/IMG_4658.jpg?ex=69d5562c&is=69d404ac&hm=bdba74dce1872f288973338fba80af40c6e47f5ffa7c2c398e0f7b819642d1d5&=&format=webp&width=558&height=744" alt="紅外線感測器範例">

將所有GND(短腳)引腳焊接在一起，把兩側的IR LED的長腳分別焊接成一條線(共兩條)，然後每一個IR晶體管長腳焊上一條線。

<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790312405237830/IMG_4657.jpg?ex=69d5562c&is=69d404ac&hm=a0ba65c66f2a662cc8ab3bee93c76199b77dae8cadd07ab5fb8ddfcad0c64eb3&=&format=webp&width=558&height=744" alt="紅外線感測器接線範例">

電路：
```
5V----------------33Ω電阻--左側LED VCC

      |         |-33Ω電阻--右側LED VCC

   1MΩ電阻(x6)                 |

    /     \                    |

GPIO9~14 IR光電電晶體1~6---GND
```
記得要把IR光電電晶體由下到上123456依序接GPIO9~14，不要變成135246

這麼抽象，希望大家看得懂XD

<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790311667171560/IMG_4656.jpg?ex=69d5562b&is=69d404ab&hm=951be3fa71db2918001004d05f6fd1e41cc08842588b9bd90df51f094e8a20fd&=&format=webp&width=558&height=744" alt="所有電路">
OwO 你成功製作了一個土炮版中二手台🥳
<img src="https://media.discordapp.net/attachments/1189565314736857180/1490790313814524046/IMG_4659.jpg?ex=69d5562c&is=69d404ac&hm=3f97148cfa5e8bbc0112aef8525c5fd33e02fc5dbd831ac9d5ce72b406492a51&=&format=webp&width=558&height=744" alt="成品">

最後，我自己錄了個示範影片兼S手元，打的是ラヴィ(Lavie)紫譜13，有98萬分，算是證明了這個土炮的東西有用(?，有興趣的話去看一下順便給我個讚:D 謝啦 [link](https://www.instagram.com/reel/DW_hd89kaQa/?utm_source=ig_web_copy_link&igsh=NTc4MTIwNjQ2YQ==)
# 參考資料
謝謝各位前輩大佬們留下的資源🛐⚡
https://github.com/Yona-W/OpeNITHM
https://github.com/Yona-W/OpeNITHM/issues/8
https://github.com/Sucareto/Arduino-Chunithm-Controller
https://github.com/whowechina/chu_pico
https://home.gamer.com.tw/artwork.php?sn=6049032