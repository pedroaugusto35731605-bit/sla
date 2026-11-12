// Roda no contexto da própria página do xbox.com (mundo MAIN), antes do
// player do Xbox Cloud Gaming ser criado. Intercepta o RTCPeerConnection do
// stream para:
//   1. reduzir o buffer de jitter (o "atraso de segurança" que o navegador
//      segura antes de mostrar cada quadro) ao mínimo;
//   2. escolher o codec/perfil que a placa de vídeo do PC decodifica por
//      hardware (ou, sem hardware, o perfil mais leve de decodificar);
//   3. medir e mostrar a latência real (ping, buffer, decodificação, tela...).
(() => {
  if (window.__xclInstalled) return;
  window.__xclInstalled = true;

  const OrigPC = window.RTCPeerConnection;
  if (!OrigPC) return;

  let settings = { enabled: true, minBuffer: true, codec: "hw", overlay: true, lite: true };
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

  // ---------- 2. codec e decodificação ----------
  // Pergunta ao navegador, para cada codec que ele aceita, se a decodificação
  // em 1080p60 é feita pela placa de vídeo ("powerEfficient" = hardware).
  const decodeInfo = new Map(); // "mime|fmtp" -> { powerEfficient, smooth }
  const isRealCodec = (c) => !/rtx|red|ulpfec|flexfec/i.test(c.mimeType);
  const key = (c) => c.mimeType.toLowerCase() + "|" + (c.sdpFmtpLine || "");
  const h264Profile = (c) => {
    if (!/h264/i.test(c.mimeType)) return "";
    const m = /profile-level-id=([0-9a-f]{2})/i.exec(c.sdpFmtpLine || "");
    const p = m ? m[1].toLowerCase() : "";
    return p === "42" ? "baseline" : p === "4d" ? "main" : p === "64" ? "high" : p === "f4" ? "high444" : "";
  };

  const hwReady = (async () => {
    if (!navigator.mediaCapabilities || !window.RTCRtpReceiver || !RTCRtpReceiver.getCapabilities) return;
    const caps = (RTCRtpReceiver.getCapabilities("video") || { codecs: [] }).codecs.filter(isRealCodec);
    await Promise.all(caps.map(async (c) => {
      const video = { width: 1920, height: 1080, bitrate: 20e6, framerate: 60 };
      for (const contentType of [c.mimeType + (c.sdpFmtpLine ? ";" + c.sdpFmtpLine : ""), c.mimeType]) {
        try {
          const r = await navigator.mediaCapabilities.decodingInfo({ type: "webrtc", video: { ...video, contentType } });
          if (r.supported) {
            decodeInfo.set(key(c), { powerEfficient: !!r.powerEfficient, smooth: !!r.smooth });
            return;
          }
        } catch (_) {}
      }
    }));
  })();

  const isHw = (c) => !!(decodeInfo.get(key(c)) || {}).powerEfficient;
  const h264HasHw = () => {
    for (const [k, v] of decodeInfo) if (k.startsWith("video/h264|") && v.powerEfficient) return true;
    return false;
  };

  // Modo efetivo de ordenação a partir da configuração.
  //   "hw"            -> codec com decodificação por hardware; se o H.264 não
  //                      tiver hardware, usa o perfil Baseline (mais leve no software)
  //   "h264"/"h264-high" -> H.264 perfil High (melhor imagem)
  //   "h264-baseline" -> H.264 perfil Baseline (decodificação mais leve)
  //   "auto"          -> não mexe
  function codecMode() {
    if (!settings.enabled) return "auto";
    const m = settings.codec === "h264" ? "h264-high" : settings.codec;
    if (m === "hw") return decodeInfo.size && !h264HasHw() ? "hw-baseline" : "hw-high";
    return m;
  }

  const PROFILE_ORDER = {
    high: { high: 4, main: 3, baseline: 2, high444: 1 },
    baseline: { baseline: 4, main: 3, high: 2, high444: 1 }
  };

  function sortCodecs(codecs) {
    const mode = codecMode();
    if (mode === "auto") return codecs;
    const prof = mode.endsWith("baseline") ? "baseline" : "high";
    const useHw = mode.startsWith("hw");
    const score = (c) => {
      if (!isRealCodec(c)) return 0; // rtx/red/fec ficam no fim, como de costume
      let sc = 0;
      if (useHw && isHw(c)) sc += 100;
      if (/h264/i.test(c.mimeType)) sc += 10 + (PROFILE_ORDER[prof][h264Profile(c)] || 0);
      return sc + 1;
    };
    return codecs
      .map((c, i) => [c, i])
      .sort((x, y) => score(y[0]) - score(x[0]) || x[1] - y[1])
      .map((x) => x[0]);
  }

  function applyCodecPrefs(pc) {
    if (codecMode() === "auto") return;
    if (!window.RTCRtpReceiver || !RTCRtpReceiver.getCapabilities) return;
    const caps = RTCRtpReceiver.getCapabilities("video");
    if (!caps) return;
    pc.getTransceivers().forEach((t) => {
      const kind = t.receiver && t.receiver.track && t.receiver.track.kind;
      if (kind !== "video" || !t.setCodecPreferences) return;
      try {
        origSetCodecPrefs.call(t, sortCodecs(caps.codecs));
      } catch (_) {}
    });
  }

  // Se o próprio site escolher codecs, reordena a lista dele.
  const TP = window.RTCRtpTransceiver && RTCRtpTransceiver.prototype;
  const origSetCodecPrefs = TP && TP.setCodecPreferences;
  if (origSetCodecPrefs) {
    TP.setCodecPreferences = function (codecs) {
      const isVideo = this.receiver && this.receiver.track && this.receiver.track.kind === "video";
      return origSetCodecPrefs.call(this, isVideo && Array.isArray(codecs) ? sortCodecs(codecs) : codecs);
    };
  }

  // ---------- intercepta o RTCPeerConnection ----------
  // Espera (no máximo 300 ms) o teste de hardware terminar antes de negociar.
  const waitHw = () => Promise.race([hwReady, new Promise((r) => setTimeout(r, 300))]);
  function wrapNegotiation(name) {
    const orig = OrigPC.prototype[name];
    OrigPC.prototype[name] = function (...args) {
      if (typeof args[0] === "function") { // API antiga com callbacks
        applyCodecPrefs(this);
        return orig.apply(this, args);
      }
      return waitHw().then(() => {
        applyCodecPrefs(this);
        return orig.apply(this, args);
      });
    };
  }
  wrapNegotiation("createOffer");
  wrapNegotiation("createAnswer");

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

  // Mede, quadro a quadro, quanto tempo passa entre o quadro chegar pela rede
  // e ser exibido (buffer + decodificação + desenho na tela).
  const watchedVideos = new WeakSet();
  let toScreenSum = 0, toScreenCount = 0;
  function watchVideo() {
    if (!HTMLVideoElement.prototype.requestVideoFrameCallback) return;
    document.querySelectorAll("video").forEach((v) => {
      if (watchedVideos.has(v) || !(v.srcObject instanceof MediaStream)) return;
      watchedVideos.add(v);
      const onFrame = (_now, meta) => {
        if (meta.receiveTime && meta.expectedDisplayTime) {
          const ms = meta.expectedDisplayTime - meta.receiveTime;
          if (ms >= 0 && ms < 2000) { toScreenSum += ms; toScreenCount++; }
        }
        if (v.isConnected) v.requestVideoFrameCallback(onFrame);
        else watchedVideos.delete(v);
      };
      v.requestVideoFrameCallback(onFrame);
    });
  }

  // Hardware ou software? Usa o que o navegador informar nas estatísticas;
  // se ele esconder, usa a previsão do teste feito ao carregar a página.
  const SW_DECODERS = /libvpx|ffmpeg|openh264|dav1d|libaom|software/i;
  function decoderKind(video, codec) {
    if (typeof video.powerEfficientDecoder === "boolean") return video.powerEfficientDecoder ? "Hardware" : "Software";
    if (video.decoderImplementation && SW_DECODERS.test(video.decoderImplementation)) return "Software";
    if (codec) {
      const info = decodeInfo.get(codec.mimeType.toLowerCase() + "|" + (codec.sdpFmtpLine || ""));
      if (info) return info.powerEfficient ? "Hardware*" : "Software*";
    }
    return "?";
  }

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
    const dropped = secs > 0 ? d("framesDropped") / secs : NaN;
    watchVideo();
    const toScreen = toScreenCount ? toScreenSum / toScreenCount : NaN;
    toScreenSum = toScreenCount = 0;
    // Atraso total ≈ ida e volta na rede + do quadro chegar até aparecer.
    const total = (rtt || 0) + (Number.isFinite(toScreen) ? toScreen : (buffer || 0) + (decode || 0));
    last = video;

    const kind = decoderKind(video, codec);
    const prof = codec ? h264Profile(codec) : "";
    const codecName = codec ? codec.mimeType.replace("video/", "") + (prof ? " " + prof : "") : "–";

    const el = ensureOverlay();
    if (!el) return;
    const rows = [
      ["Total aprox.", fmt(total) + " ms", color(total, 60, 110)],
      ["Ping (RTT)", fmt(rtt) + " ms", color(rtt, 30, 60)],
      ["Buffer", fmt(buffer) + " ms", color(buffer, 15, 40)],
      ["Decodificação", fmt(decode, 1) + " ms", color(decode, 5, 12)],
      ["Chegada→tela", fmt(toScreen) + " ms", color(toScreen, 25, 50)],
      ["Decodificador", kind, kind.startsWith("Hardware") ? "#6f6" : kind === "?" ? "#fff" : "#fd5"],
      ["Codec", codecName],
      ["FPS", fmt(video.framesPerSecond)],
      ["Descartados", fmt(dropped, 1) + " /s", color(dropped, 0, 2)],
      ["Taxa", fmt(mbps, 1) + " Mb/s"],
      ["Perda pacotes", fmt(lostPct, 2) + " %", color(lostPct, 0.1, 1)]
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
    hint.textContent = (kind.endsWith("*") ? "* previsto pelo navegador\n" : "") + "Alt+L esconde";
    hint.style.color = "#777";
    frag.appendChild(hint);
    el.replaceChildren(frag);
    placeOverlay();
    el.dataset.has = "1";
    updateOverlayVisibility(true);
  }

  setInterval(() => { collect().catch(() => {}); }, 1000);
})();
