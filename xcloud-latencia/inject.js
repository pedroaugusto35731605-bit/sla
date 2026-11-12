// Roda no contexto da própria página do xbox.com (mundo MAIN), antes do
// player do Xbox Cloud Gaming ser criado. Intercepta o RTCPeerConnection do
// stream para:
//   1. reduzir o buffer de jitter (o "atraso de segurança" que o navegador
//      segura antes de mostrar cada quadro) ao mínimo;
//   2. preferir H.264, que quase sempre é decodificado pelo hardware da placa
//      de vídeo, com menos atraso de decodificação;
//   3. medir e mostrar a latência real (ping, buffer, decodificação, FPS...).
(() => {
  if (window.__xclInstalled) return;
  window.__xclInstalled = true;

  const OrigPC = window.RTCPeerConnection;
  if (!OrigPC) return;

  let settings = { enabled: true, minBuffer: true, codec: "h264", overlay: true, lite: true };
  const peers = new Set();

  // ---------- configurações vindas da extensão ----------
  document.addEventListener("xcl:settings", (e) => {
    try {
      settings = Object.assign(settings, JSON.parse(e.detail));
    } catch (_) {
      return;
    }
    peers.forEach(tunePeer);
    updateOverlayVisibility();
  });
  document.dispatchEvent(new CustomEvent("xcl:hello"));

  // ---------- 1. buffer mínimo ----------
  function tuneReceiver(receiver) {
    if (!receiver) return;
    const target = settings.enabled && settings.minBuffer ? 0 : null;
    try {
      // API atual (Chrome/Edge 114+): alvo do jitter buffer em milissegundos.
      if ("jitterBufferTarget" in receiver) {
        if (receiver.jitterBufferTarget !== target) receiver.jitterBufferTarget = target;
      } else {
        // API antiga do Chromium, em segundos.
        receiver.playoutDelayHint = target === null ? undefined : 0;
      }
    } catch (_) {
      /* navegador não suporta: ignora */
    }
  }

  function tunePeer(pc) {
    if (pc.connectionState === "closed") return;
    try {
      pc.getReceivers().forEach(tuneReceiver);
    } catch (_) {}
  }

  // ---------- 2. preferência de codec ----------
  function sortCodecs(codecs) {
    if (!settings.enabled || settings.codec !== "h264") return codecs;
    const isH264 = (c) => /h264/i.test(c.mimeType);
    // Entre os H.264, prefere o perfil "High" (4d/64) que tem decodificação
    // por hardware e boa qualidade; depois os demais.
    const score = (c) => {
      if (!isH264(c)) return 0;
      const p = /profile-level-id=([0-9a-f]{2})/i.exec(c.sdpFmtpLine || "");
      const prof = p ? p[1].toLowerCase() : "";
      return prof === "64" || prof === "4d" ? 3 : 2;
    };
    return codecs.slice().sort((a, b) => score(b) - score(a));
  }

  function applyCodecPrefs(pc) {
    if (!settings.enabled || settings.codec !== "h264") return;
    if (!window.RTCRtpReceiver || !RTCRtpReceiver.getCapabilities) return;
    const caps = RTCRtpReceiver.getCapabilities("video");
    if (!caps || !caps.codecs.some((c) => /h264/i.test(c.mimeType))) return;
    pc.getTransceivers().forEach((t) => {
      const kind = t.receiver && t.receiver.track && t.receiver.track.kind;
      if (kind !== "video" || !t.setCodecPreferences) return;
      try {
        origSetCodecPrefs.call(t, sortCodecs(caps.codecs));
      } catch (_) {}
    });
  }

  // Se o próprio site escolher codecs, reordena para colocar H.264 na frente.
  const TP = window.RTCRtpTransceiver && RTCRtpTransceiver.prototype;
  const origSetCodecPrefs = TP && TP.setCodecPreferences;
  if (origSetCodecPrefs) {
    TP.setCodecPreferences = function (codecs) {
      const isVideo = this.receiver && this.receiver.track && this.receiver.track.kind === "video";
      return origSetCodecPrefs.call(this, isVideo && Array.isArray(codecs) ? sortCodecs(codecs) : codecs);
    };
  }

  // ---------- intercepta o RTCPeerConnection ----------
  const origCreateOffer = OrigPC.prototype.createOffer;
  OrigPC.prototype.createOffer = function (...args) {
    applyCodecPrefs(this);
    return origCreateOffer.apply(this, args);
  };
  const origCreateAnswer = OrigPC.prototype.createAnswer;
  OrigPC.prototype.createAnswer = function (...args) {
    applyCodecPrefs(this);
    return origCreateAnswer.apply(this, args);
  };

  function track(pc) {
    peers.add(pc);
    pc.addEventListener("track", (e) => tuneReceiver(e.receiver));
    pc.addEventListener("connectionstatechange", () => {
      if (pc.connectionState === "closed" || pc.connectionState === "failed") peers.delete(pc);
    });
  }

  window.RTCPeerConnection = new Proxy(OrigPC, {
    construct(target, args, newTarget) {
      const pc = Reflect.construct(target, args, newTarget);
      track(pc);
      return pc;
    }
  });
  if (window.webkitRTCPeerConnection) window.webkitRTCPeerConnection = window.RTCPeerConnection;

  // O player pode redefinir o buffer depois; reaplica periodicamente.
  setInterval(() => peers.forEach(tunePeer), 2000);

  // ---------- 3. painel de latência ----------
  let overlay = null;
  let hiddenByUser = false;
  let last = null;

  function ensureOverlay() {
    if (overlay || !document.body) return overlay;
    overlay = document.createElement("div");
    overlay.id = "xcl-overlay";
    overlay.style.cssText = [
      "position:fixed", "top:8px", "left:8px", "z-index:2147483647",
      "font:12px/1.35 ui-monospace,Consolas,monospace", "color:#fff",
      "background:rgba(0,0,0,.65)", "padding:6px 9px", "border-radius:6px",
      "pointer-events:none", "white-space:pre", "display:none"
    ].join(";");
    document.body.appendChild(overlay);
    return overlay;
  }

  // Em tela cheia só o elemento em fullscreen aparece: move o painel para dentro dele.
  function placeOverlay() {
    if (!overlay) return;
    const fs = document.fullscreenElement;
    const parent = fs && fs.tagName !== "VIDEO" ? fs : document.body;
    if (parent && overlay.parentNode !== parent) parent.appendChild(overlay);
  }
  document.addEventListener("fullscreenchange", placeOverlay);

  function updateOverlayVisibility(hasData) {
    if (!overlay) return;
    const show = settings.enabled && settings.overlay && !hiddenByUser && (hasData ?? overlay.dataset.has === "1");
    overlay.style.display = show ? "block" : "none";
  }

  // Alt+L mostra/esconde o painel durante o jogo.
  window.addEventListener("keydown", (e) => {
    if (e.altKey && (e.key === "l" || e.key === "L")) {
      hiddenByUser = !hiddenByUser;
      updateOverlayVisibility();
    }
  }, true);

  const color = (v, good, ok) => (v <= good ? "#6f6" : v <= ok ? "#fd5" : "#f66");
  const fmt = (v, d = 0) => (Number.isFinite(v) ? v.toFixed(d) : "–");

  async function collect() {
    const live = [...peers].filter((p) => p.connectionState === "connected");
    if (!live.length) {
      if (overlay) { overlay.dataset.has = "0"; updateOverlayVisibility(false); }
      last = null;
      return;
    }
    if (!settings.enabled || !settings.overlay || hiddenByUser) return;

    // Usa a conexão que está recebendo vídeo (o stream do jogo).
    let stats, pair, video, codec;
    for (const pc of live) {
      stats = await pc.getStats();
      pair = video = undefined;
      stats.forEach((s) => {
        if (s.type === "candidate-pair" && s.nominated && s.state === "succeeded") pair = s;
        if (s.type === "inbound-rtp" && s.kind === "video") video = s;
      });
      if (video) break;
    }
    if (!video) return;
    if (video.codecId) codec = stats.get(video.codecId);

    const now = video.timestamp;
    const d = (k) => (last ? (video[k] || 0) - (last[k] || 0) : NaN);
    const rtt = pair && pair.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : NaN;
    const emitted = d("jitterBufferEmittedCount");
    const buffer = emitted > 0 ? (d("jitterBufferDelay") / emitted) * 1000 : NaN;
    const decoded = d("framesDecoded");
    const decode = decoded > 0 ? (d("totalDecodeTime") / decoded) * 1000 : NaN;
    const secs = last ? (now - last.timestamp) / 1000 : NaN;
    const mbps = secs > 0 ? (d("bytesReceived") * 8) / secs / 1e6 : NaN;
    const lostPct = (() => {
      const lost = d("packetsLost"), recv = d("packetsReceived");
      return lost + recv > 0 ? (lost / (lost + recv)) * 100 : 0;
    })();
    const total = (rtt || 0) + (buffer || 0) + (decode || 0);
    last = video;

    const el = ensureOverlay();
    if (!el) return;
    const rows = [
      ["Total aprox.", fmt(total) + " ms", color(total, 60, 110)],
      ["Ping (RTT)", fmt(rtt) + " ms", color(rtt, 30, 60)],
      ["Buffer", fmt(buffer) + " ms", color(buffer, 15, 40)],
      ["Decodificação", fmt(decode, 1) + " ms", color(decode, 5, 12)],
      ["FPS", fmt(video.framesPerSecond)],
      ["Taxa", fmt(mbps, 1) + " Mb/s"],
      ["Perda pacotes", fmt(lostPct, 2) + " %", color(lostPct, 0.1, 1)],
      ["Codec", codec ? codec.mimeType.replace("video/", "") : "–"]
    ];
    // Montado com DOM (sem innerHTML) por causa de políticas Trusted Types.
    const frag = document.createDocumentFragment();
    rows.forEach(([label, value, c]) => {
      const row = document.createElement("div");
      const a = document.createElement("span");
      a.textContent = label.padEnd(14, " ");
      a.style.color = "#aaa";
      const b = document.createElement("span");
      b.textContent = value;
      b.style.color = c || "#fff";
      row.append(a, b);
      frag.appendChild(row);
    });
    const hint = document.createElement("div");
    hint.textContent = "Alt+L esconde";
    hint.style.color = "#777";
    frag.appendChild(hint);
    el.replaceChildren(frag);
    placeOverlay();
    el.dataset.has = "1";
    updateOverlayVisibility(true);
  }

  setInterval(() => { collect().catch(() => {}); }, 1000);
})();
