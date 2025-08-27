var neopixel = require("neopixel");

var LED_PIN = D12;   // 控制 WS2812 的引脚
var NUM_LEDS = 1;    // 灯的数量
var data = new Uint8ClampedArray(NUM_LEDS * 3);

// 定义几个颜色（R,G,B）
var colors = [
  [25, 0, 0],    // 红
  [0, 25, 0],    // 绿
  [0, 0, 25],    // 蓝
  [25, 25, 25] // 白
];
var colorIndex = 0;

// 初始化灯
function showColor(idx) {
  data.set(colors[idx], 0);     // 设置第0个灯
  neopixel.write(LED_PIN, data);
  console.log("Changed color to:", colors[idx]);
}
showColor(colorIndex);

// 按钮引脚
var BTN = D0;
pinMode(BTN, "input_pulldown");

// 每次按下按钮 → 切换颜色
setWatch(function() {
  colorIndex = (colorIndex + 1) % colors.length;
  showColor(colorIndex);
}, BTN, { edge:'rising', repeat:true, debounce:50 });