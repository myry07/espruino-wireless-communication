const btn = document.getElementById("btn");
const title = document.getElementById("title");

btn.addEventListener("click", () => {
  title.style.color = `hsl(${Math.random() * 360}, 80%, 40%)`;
});
