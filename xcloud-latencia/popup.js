const KEYS = Object.keys(XCL_DEFAULTS);

function render(s) {
  KEYS.forEach((k) => {
    const el = document.getElementById(k);
    if (!el) return;
    if (el.type === "checkbox") el.checked = !!s[k];
    else el.value = s[k];
  });
  document.body.classList.toggle("off", !s.enabled);
}

chrome.storage.sync.get(XCL_DEFAULTS, render);

KEYS.forEach((k) => {
  const el = document.getElementById(k);
  if (!el) return;
  el.addEventListener("change", () => {
    const value = el.type === "checkbox" ? el.checked : el.value;
    chrome.storage.sync.set({ [k]: value });
    if (k === "enabled") document.body.classList.toggle("off", !value);
  });
});
