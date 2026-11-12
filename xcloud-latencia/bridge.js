// Roda no "mundo isolado" da extensão: lê as configurações salvas e
// repassa para o script da página (inject.js), que não tem acesso ao chrome.storage.
(() => {
  const DEFAULTS = { enabled: true, minBuffer: true, codec: "h264", overlay: true, lite: true };

  function send(settings) {
    const s = Object.assign({}, DEFAULTS, settings);
    document.documentElement.classList.toggle("xcl-lite", s.enabled && s.lite);
    document.dispatchEvent(new CustomEvent("xcl:settings", { detail: JSON.stringify(s) }));
  }

  // O inject.js pede as configurações quando termina de carregar.
  document.addEventListener("xcl:hello", () => chrome.storage.sync.get(DEFAULTS, send));

  chrome.storage.sync.get(DEFAULTS, send);
  chrome.storage.onChanged.addListener((_c, area) => {
    if (area === "sync") chrome.storage.sync.get(DEFAULTS, send);
  });
})();
