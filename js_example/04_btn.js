var BUTTON = D0;
pinMode(BUTTON, "input_pulldown");

// 当按钮从低到高（按下）触发
setWatch(function(e) {
  console.log("Button pressed! Time:", e.time);
}, BUTTON, { edge: 'rising', repeat: true, debounce: 50 });

// 当按钮从高到低（松开）触发
setWatch(function(e) {
  console.log("Button released! Time:", e.time);
}, BUTTON, { edge: 'falling', repeat: true, debounce: 50 });