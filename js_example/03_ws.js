var neopixel = require("neopixel");

var pin = D12;   // 控制引脚
var leds = 1;    // 只有一个灯

var data = new Uint8ClampedArray(leds * 3);

// 红色 (R=255, G=0, B=0)
data.set([20, 10, 30], 0);  

neopixel.write(pin, data);