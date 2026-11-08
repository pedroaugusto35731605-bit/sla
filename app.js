// Avaliador de Hot Wheels — tudo roda no navegador.

// ---------- Tabela de preços (R$, mercado brasileiro, aproximado) ----------
// min/max = carrinho lacrado, cartela em bom estado.
const TIPOS = {
  basico:      { nome: "Básico (mainline comum)",               min: 10,  max: 20 },
  licenciado:  { nome: "Básico de marca famosa (licenciado)",   min: 15,  max: 35 },
  th:          { nome: "Treasure Hunt (chama na cartela)",      min: 30,  max: 70 },
  sth:         { nome: "Super Treasure Hunt ($TH)",             min: 250, max: 700 },
  zamac:       { nome: "ZAMAC / exclusivo de loja",             min: 25,  max: 60 },
  premium:     { nome: "Premium (Car Culture, Pop Culture...)", min: 50,  max: 120 },
  boulevard:   { nome: "Boulevard / Fast & Furious Premium",    min: 60,  max: 140 },
  transport:   { nome: "Team Transport (com caminhão)",         min: 120, max: 220 },
  rlc:         { nome: "Red Line Club (RLC)",                   min: 300, max: 900 },
  convencao:   { nome: "Edição de convenção / Elite 64",        min: 250, max: 800 },
  redline:     { nome: "Redline antigo (1968–1977)",            min: 150, max: 1500 },
  antigo:      { nome: "Antigo (anos 80/90)",                   min: 20,  max: 80 },
};

const ESTADOS = {
  lacrado:    { nome: "Lacrado, cartela perfeita",     fator: 1.0 },
  cartela_ok: { nome: "Lacrado, cartela com marcas",   fator: 0.75 },
  cartela_ruim:{ nome: "Lacrado, cartela amassada/rasgada", fator: 0.55 },
  solto_novo: { nome: "Solto, como novo",              fator: 0.5 },
  solto_uso:  { nome: "Solto, pequenas marcas de uso", fator: 0.3 },
  solto_gasto:{ nome: "Solto, bem gasto",              fator: 0.15 },
};

const EXTRAS = {
  popular:    { nome: "modelo muito procurado", fator: 1.3 },
  erro:       { nome: "erro de fábrica",        fator: 2.0 },
  primeira:   { nome: "primeira edição",        fator: 1.15 },
  realriders: { nome: "rodas de borracha",      fator: 1.2 },
};

// ---------- Utilitários ----------
const $ = (id) => document.getElementById(id);
const brl = (n) => n.toLocaleString("pt-BR", { style: "currency", currency: "BRL", maximumFractionDigits: 0 });
const arred = (n) => (n < 50 ? Math.round(n) : Math.round(n / 5) * 5);

function lerJSON(chave, padrao) {
  try { return JSON.parse(localStorage.getItem(chave)) ?? padrao; } catch { return padrao; }
}
function gravar(chave, valor) {
  try { localStorage.setItem(chave, typeof valor === "string" ? valor : JSON.stringify(valor)); return true; }
  catch { return false; }
}

// ---------- Estado ----------
let fotoBase64 = null;     // JPEG reduzido, sem o prefixo data:
let fotoMini = null;       // miniatura para a coleção
let estimativaIA = null;   // { min, max, texto } quando a IA dá um preço
let ultimoCalculo = null;

// ---------- Preencher selects ----------
for (const [k, v] of Object.entries(TIPOS)) $("tipo").add(new Option(`${v.nome}`, k));
for (const [k, v] of Object.entries(ESTADOS)) $("estado").add(new Option(v.nome, k));

// ---------- Foto ----------
async function reduzirImagem(arquivo, lado) {
  const bmp = await createImageBitmap(arquivo).catch(async () => {
    // Alguns navegadores antigos não têm createImageBitmap para todos os formatos
    const img = new Image();
    img.src = URL.createObjectURL(arquivo);
    await img.decode();
    return img;
  });
  const escala = Math.min(1, lado / Math.max(bmp.width, bmp.height));
  const c = document.createElement("canvas");
  c.width = Math.round(bmp.width * escala);
  c.height = Math.round(bmp.height * escala);
  c.getContext("2d").drawImage(bmp, 0, 0, c.width, c.height);
  return c.toDataURL("image/jpeg", 0.85);
}

async function aoEscolherFoto(e) {
  const arquivo = e.target.files?.[0];
  if (!arquivo) return;
  try {
    const grande = await reduzirImagem(arquivo, 1280);
    fotoMini = await reduzirImagem(arquivo, 160);
    fotoBase64 = grande.split(",")[1];
    $("previewImg").src = grande;
    $("previewImg").hidden = false;
    $("preview").classList.remove("vazio");
    estimativaIA = null;
    atualizarBotaoIA();
    calcular();
  } catch (err) {
    mostrarStatus("Não consegui abrir essa imagem. Tente outra foto.", "erro");
  }
  e.target.value = "";
}
$("inputCamera").addEventListener("change", aoEscolherFoto);
$("inputGaleria").addEventListener("change", aoEscolherFoto);

// ---------- Cálculo manual ----------
function calcular() {
  const tipo = TIPOS[$("tipo").value];
  const estado = ESTADOS[$("estado").value];
  let fator = estado.fator;
  const extrasUsados = [];
  for (const [k, v] of Object.entries(EXTRAS)) {
    if ($(k).checked) { fator *= v.fator; extrasUsados.push(v.nome); }
  }

  let min = tipo.min * fator;
  let max = tipo.max * fator;
  let texto = `Base: ${tipo.nome} (${brl(tipo.min)}–${brl(tipo.max)} lacrado).\n` +
              `Estado: ${estado.nome} (×${estado.fator}).`;
  if (extrasUsados.length) texto += `\nExtras: ${extrasUsados.join(", ")}.`;

  // Se a IA reconheceu o modelo e deu preço, mistura com a tabela (pesa mais a IA)
  if (estimativaIA) {
    min = estimativaIA.min * 0.7 + min * 0.3;
    max = estimativaIA.max * 0.7 + max * 0.3;
    texto = `🤖 ${estimativaIA.texto}\n\n${texto}`;
  }

  min = Math.max(2, arred(min));
  max = Math.max(min, arred(max));
  const medio = arred((min + max) / 2);

  $("valor").textContent = `≈ ${brl(medio)}`;
  $("faixa").textContent = `entre ${brl(min)} e ${brl(max)}`;
  $("explicacao").textContent = texto;

  const nome = $("nome").value.trim();
  if (nome) {
    $("linkML").href = "https://lista.mercadolivre.com.br/" + encodeURIComponent(`hot wheels ${nome}`);
    $("linkML").hidden = false;
  } else {
    $("linkML").hidden = true;
  }

  ultimoCalculo = { nome: nome || "Hot Wheels sem nome", tipo: tipo.nome, estado: estado.nome, min, max, medio };
}
$("form").addEventListener("input", calcular);
$("form").addEventListener("change", calcular);

// ---------- Chave da API ----------
const CHAVE_KEY = "hw_api_key";
// Chaves da Anthropic começam com "sk-ant-"; qualquer outra é tratada como Gemini.
const usaClaude = (chave) => chave.startsWith("sk-ant-");
function chaveAtual() { try { return localStorage.getItem(CHAVE_KEY) || ""; } catch { return ""; } }
function atualizarBotaoIA() {
  $("btnIA").disabled = !(fotoBase64 && chaveAtual());
  $("btnIA").title = !chaveAtual() ? "Configure a chave da API primeiro" : !fotoBase64 ? "Tire uma foto primeiro" : "";
}
// Mostra só o fim da chave salva, para conferir sem expor ela inteira
function mostrarChaveSalva() {
  const c = chaveAtual();
  $("apiKey").value = "";
  $("apiKey").placeholder = c
    ? `✅ Chave salva (${usaClaude(c) ? "Claude" : "Gemini"}, termina em …${c.slice(-4)})`
    : "Cole aqui sua chave do Gemini ou do Claude";
}
$("salvarChave").addEventListener("click", () => {
  const v = $("apiKey").value.trim();
  if (!v) return;
  gravar(CHAVE_KEY, v);
  mostrarChaveSalva();
  $("chaveBox").open = false;
  mostrarStatus(`Chave salva neste aparelho (termina em …${v.slice(-4)}).`, "ok");
  atualizarBotaoIA();
});
$("apagarChave").addEventListener("click", () => {
  try { localStorage.removeItem(CHAVE_KEY); } catch {}
  mostrarChaveSalva();
  mostrarStatus("Chave apagada.", "ok");
  atualizarBotaoIA();
});
mostrarChaveSalva();

function mostrarStatus(msg, tipo = "") {
  const s = $("statusIA");
  s.hidden = false;
  s.className = "status " + tipo;
  s.textContent = msg;
}

// ---------- Identificação com IA (Gemini ou Claude) ----------
const ESQUEMA = {
  type: "object",
  additionalProperties: false,
  required: ["e_hot_wheels", "nome", "ano_aproximado", "tipo", "estado", "popular", "erro_fabrica",
             "primeira_edicao", "real_riders", "preco_min", "preco_max", "confianca", "comentario"],
  properties: {
    e_hot_wheels:    { type: "boolean" },
    nome:            { type: "string" },
    ano_aproximado:  { type: "string" },
    tipo:            { type: "string", enum: Object.keys(TIPOS) },
    estado:          { type: "string", enum: Object.keys(ESTADOS) },
    popular:         { type: "boolean" },
    erro_fabrica:    { type: "boolean" },
    primeira_edicao: { type: "boolean" },
    real_riders:     { type: "boolean" },
    preco_min:       { type: "number" },
    preco_max:       { type: "number" },
    confianca:       { type: "string", enum: ["baixa", "media", "alta"] },
    comentario:      { type: "string" },
  },
};

const PROMPT = `Você é um especialista em colecionismo de Hot Wheels no Brasil.
Olhe a foto e identifique o carrinho. Responda em português.

- e_hot_wheels: false se a foto não mostrar um carrinho Hot Wheels (ou similar 1:64).
- nome: nome do modelo como aparece na cartela/coleção (ex.: "'95 Mazda RX-7"). Se não souber, descreva (ex.: "Muscle car laranja").
- tipo: uma das chaves: ${Object.entries(TIPOS).map(([k, v]) => `${k} = ${v.nome}`).join("; ")}.
  Treasure Hunt comum tem um símbolo de chama dentro de um círculo atrás do carro na cartela.
  Super Treasure Hunt tem pintura Spectraflame, rodas de borracha (Real Riders) e logo "TH" no carro.
- estado: uma das chaves: ${Object.entries(ESTADOS).map(([k, v]) => `${k} = ${v.nome}`).join("; ")}.
- popular: true para modelos muito procurados (JDM como Skyline/Supra/RX-7, Porsche, Kombi/Fusca, Velozes e Furiosos, Batmóvel etc.).
- preco_min / preco_max: faixa realista em reais (R$) pela qual esse carrinho, NESSE estado, costuma ser vendido no Brasil (Mercado Livre, grupos de colecionadores).
- confianca: o quanto você tem certeza da identificação.
- comentario: 1 ou 2 frases curtas explicando a identificação e o preço.`;

let AnthropicSDK = null;
async function carregarSDK() {
  if (!AnthropicSDK) {
    const mod = await import("./vendor/anthropic-sdk.js?v=3");
    AnthropicSDK = mod.default;
  }
  return AnthropicSDK;
}


async function identificarClaude(chave) {
  const Anthropic = await carregarSDK();
  const client = new Anthropic({ apiKey: chave, dangerouslyAllowBrowser: true });
  const resposta = await client.beta.messages.create({
    model: "claude-opus-5-5",
    max_tokens: 16000,
    betas: ["server-side-fallback-2026-07-01"],
    fallbacks: "default",
    output_config: {
      effort: "medium",
      format: { type: "json_schema", schema: ESQUEMA },
    },
    messages: [{
      role: "user",
      content: [
        { type: "image", source: { type: "base64", media_type: "image/jpeg", data: fotoBase64 } },
        { type: "text", text: PROMPT },
      ],
    }],
  });
  if (resposta.stop_reason === "refusal") {
    throw new Error("A IA não conseguiu analisar essa foto. Preencha os detalhes manualmente.");
  }
  if (resposta.stop_reason === "max_tokens") throw new Error("A resposta veio incompleta. Tente de novo.");
  const bloco = resposta.content.find((b) => b.type === "text");
  if (!bloco) throw new Error("A IA não devolveu resposta. Tente de novo.");
  return JSON.parse(bloco.text);
}

// Tenta o modelo mais novo do Gemini Flash; se não existir, usa o 2.5 Flash.
const MODELOS_GEMINI = ["gemini-flash-latest", "gemini-2.5-flash"];

async function identificarGemini(chave) {
  const { additionalProperties, ...schema } = ESQUEMA; // o Gemini não aceita esse campo
  let ultimoErro = new Error("Nenhum modelo do Gemini disponível para essa chave.");
  for (const modelo of MODELOS_GEMINI) {
    const resp = await fetch(`https://generativelanguage.googleapis.com/v1beta/models/${modelo}:generateContent`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "x-goog-api-key": chave },
      body: JSON.stringify({
        contents: [{
          role: "user",
          parts: [
            { inline_data: { mime_type: "image/jpeg", data: fotoBase64 } },
            { text: PROMPT },
          ],
        }],
        generationConfig: { responseMimeType: "application/json", responseSchema: schema },
      }),
    });
    const dados = await resp.json().catch(() => ({}));
    if (resp.status === 404) { ultimoErro = new Error(dados?.error?.message || "Modelo não encontrado."); continue; }
    if (!resp.ok) {
      const e = new Error(dados?.error?.message || `Erro ${resp.status}`);
      e.status = resp.status;
      throw e;
    }
    const cand = dados.candidates?.[0];
    if (!cand || cand.finishReason === "SAFETY" || cand.finishReason === "PROHIBITED_CONTENT") {
      throw new Error("A IA não conseguiu analisar essa foto. Preencha os detalhes manualmente.");
    }
    if (cand.finishReason === "MAX_TOKENS") throw new Error("A resposta veio incompleta. Tente de novo.");
    const texto = (cand.content?.parts || []).map((p) => p.text || "").join("");
    if (!texto) throw new Error("A IA não devolveu resposta. Tente de novo.");
    return JSON.parse(texto);
  }
  throw ultimoErro;
}

$("btnIA").addEventListener("click", async () => {
  if (!fotoBase64 || !chaveAtual()) return;
  $("btnIA").disabled = true;
  mostrarStatus("Analisando a foto… (pode levar uns 20 segundos)");

  try {
    const chave = chaveAtual();
    const r = usaClaude(chave) ? await identificarClaude(chave) : await identificarGemini(chave);

    if (!r.e_hot_wheels) {
      estimativaIA = null;
      mostrarStatus("Não parece ser um Hot Wheels. Tente outra foto, mais de perto e com boa luz.", "erro");
      calcular();
      return;
    }

    // Preenche o formulário com o que a IA encontrou
    $("nome").value = r.nome;
    if (TIPOS[r.tipo]) $("tipo").value = r.tipo;
    if (ESTADOS[r.estado]) $("estado").value = r.estado;
    $("popular").checked = r.popular;
    $("erro").checked = r.erro_fabrica;
    $("primeira").checked = r.primeira_edicao;
    $("realriders").checked = r.real_riders;

    const min = Number(r.preco_min), max = Number(r.preco_max);
    estimativaIA = (min > 0 && max >= min)
      ? { min, max, texto: `${r.nome}${r.ano_aproximado ? ` (${r.ano_aproximado})` : ""} — ${r.comentario} IA sugeriu ${brl(min)}–${brl(max)}.` }
      : null;

    mostrarStatus(`Identificado: ${r.nome} (certeza ${r.confianca}). Confira e ajuste os detalhes se precisar.`, "ok");
    calcular();
  } catch (err) {
    let msg = err?.message || String(err);
    if (err?.status === 401 || (err?.status === 400 && /api key/i.test(msg))) msg = "Chave da API inválida. Confira se copiou ela inteira.";
    else if (err?.status === 403) msg = "Essa chave não tem permissão para usar a IA. Confira no painel onde você criou a chave.";
    else if (err?.status === 429) msg = "Muitas análises seguidas. Espere um pouco e tente de novo.";
    else if (err?.status >= 500) msg = "O serviço da IA está instável agora. Tente em alguns minutos.";
    else if (/Failed to fetch|NetworkError|import/i.test(msg)) msg = "Sem conexão com a IA. Verifique a internet.";
    mostrarStatus("⚠️ " + msg, "erro");
  } finally {
    atualizarBotaoIA();
  }
});

// Ao mudar algo à mão depois da IA, mantém a sugestão dela mas recalcula
// (se o usuário trocar o tipo, a sugestão da IA deixa de valer).
$("tipo").addEventListener("change", () => { estimativaIA = null; calcular(); });

// ---------- Coleção ----------
const COLECAO_KEY = "hw_colecao";
let colecao = lerJSON(COLECAO_KEY, []);

function desenharColecao() {
  const ul = $("lista");
  ul.innerHTML = "";
  if (!colecao.length) {
    const li = document.createElement("li");
    li.className = "vazia";
    li.textContent = "Sua coleção está vazia. Avalie um carrinho e toque em “Salvar”.";
    ul.append(li);
  }
  for (const item of colecao) {
    const li = document.createElement("li");
    const foto = item.foto ? Object.assign(document.createElement("img"), { src: item.foto, alt: "" })
                           : Object.assign(document.createElement("div"), { className: "semfoto", textContent: "🏎️" });
    const info = document.createElement("div");
    info.className = "info";
    const b = document.createElement("b"); b.textContent = item.nome;
    const s = document.createElement("span"); s.textContent = `${brl(item.medio)} · ${item.estado}`;
    info.append(b, s);
    const del = document.createElement("button");
    del.textContent = "🗑️";
    del.setAttribute("aria-label", "Remover " + item.nome);
    del.addEventListener("click", () => {
      if (!confirm(`Remover "${item.nome}" da coleção?`)) return;
      colecao = colecao.filter((x) => x.id !== item.id);
      gravar(COLECAO_KEY, colecao);
      desenharColecao();
    });
    li.append(foto, info, del);
    ul.append(li);
  }
  const total = colecao.reduce((s, x) => s + x.medio, 0);
  $("qtd").textContent = `${colecao.length} ${colecao.length === 1 ? "carrinho" : "carrinhos"}`;
  $("totalColecao").textContent = brl(total);
}

$("btnSalvar").addEventListener("click", () => {
  if (!ultimoCalculo) return;
  const item = { id: Date.now(), foto: fotoMini, ...ultimoCalculo };
  colecao.unshift(item);
  if (!gravar(COLECAO_KEY, colecao)) {
    // Sem espaço: salva sem foto
    item.foto = null;
    if (!gravar(COLECAO_KEY, colecao)) { colecao.shift(); alert("Não foi possível salvar (armazenamento cheio)."); return; }
  }
  desenharColecao();
  $("btnSalvar").textContent = "✅ Salvo!";
  setTimeout(() => ($("btnSalvar").textContent = "⭐ Salvar na minha coleção"), 1500);
});

// ---------- Início ----------
atualizarBotaoIA();
calcular();
desenharColecao();
