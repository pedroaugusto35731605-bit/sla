# 🏎️ Avaliador de Hot Wheels

Site para tirar foto dos seus Hot Wheels e ver quanto eles valem, mais ou menos, no mercado brasileiro.

## Como funciona

1. **Foto** – tire uma foto com a câmera do celular ou escolha da galeria.
2. **Identificar com IA (opcional)** – com uma chave do Gemini (Google) ou do Claude (Anthropic), a IA olha a foto,
   reconhece o modelo, a linha (básico, Treasure Hunt, Super Treasure Hunt, Premium…) e o estado,
   e sugere uma faixa de preço.
3. **Detalhes** – confira ou preencha à mão: tipo, estado (lacrado/solto) e extras
   (modelo procurado, erro de fábrica, primeira edição, rodas de borracha).
4. **Valor estimado** – mostra o valor médio e a faixa, com link para comparar no Mercado Livre.
5. **Minha coleção** – salve os carrinhos e veja o valor total. Fica salvo só no seu aparelho.

Sem chave o site funciona normalmente pela tabela de preços.

## Como usar

É um site estático (HTML + CSS + JS), sem instalação.

- **GitHub Pages:** Settings → Pages → Deploy from branch → escolha a branch e a pasta `/ (root)`.
  O site fica em `https://<seu-usuario>.github.io/sla/`.
- **No computador:** `python3 -m http.server` na pasta e abra `http://localhost:8000`.

A câmera precisa de HTTPS (o GitHub Pages já usa).

## Chave da API

- **Gemini:** crie grátis em [aistudio.google.com](https://aistudio.google.com/apikey).
- **Claude:** crie em [console.anthropic.com](https://console.anthropic.com) (chaves que começam com `sk-ant-`).

Cole a chave no campo "Configurar chave da API" do site. Ela fica salva só no navegador do seu
aparelho e é enviada direto para o Google ou a Anthropic. Nunca coloque a chave dentro do código:
o repositório é público.

## Preços

A tabela de preços está no começo do `app.js` (`TIPOS`, `ESTADOS`, `EXTRAS`) e pode ser ajustada.
Os valores são aproximados e o preço real varia com a procura.

`vendor/anthropic-sdk.js` é o SDK oficial `@anthropic-ai/sdk` 1.0.0 empacotado para o navegador (licença em `vendor/anthropic-sdk.LICENSE`).
