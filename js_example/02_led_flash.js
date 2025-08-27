const LED_PIN = 2;          // 假设 LED 在 GPIO2
pinMode(LED_PIN, "output");

var state = false;
setInterval(function() {
  state = !state;                   // 翻转 LED 状态
  digitalWrite(LED_PIN, state);     // 写入引脚
  console.log("Hello World - LED is", state ? "ON" : "OFF");
}, 1000);