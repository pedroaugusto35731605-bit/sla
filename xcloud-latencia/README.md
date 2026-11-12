# xCloud Baixa Latência

Extensão para Chrome/Edge que diminui o atraso do **Xbox Cloud Gaming** (Game Pass na nuvem) jogado no navegador, em `xbox.com/play`.

## O que ela faz

| Ajuste | Como ajuda |
|---|---|
| **Buffer de vídeo mínimo** | Por padrão o navegador guarda alguns quadros antes de exibir, para absorver variações da rede. A extensão zera esse buffer (`jitterBufferTarget = 0`), então cada quadro aparece assim que chega. É o maior ganho, geralmente algumas dezenas de ms. |
| **Decodificação pelo hardware** | Ao abrir o site, a extensão pergunta ao navegador quais formatos de vídeo a sua placa decodifica sozinha em 1080p60 e coloca esses formatos em primeiro na negociação do stream. Se o H.264 não tiver decodificação por hardware no seu PC, ela pede o perfil **Baseline**, que é o mais leve de decodificar pelo processador. |
| **Painel de latência** | Mostra na tela o ping, o tempo de buffer, o tempo de decodificação por quadro, o tempo entre o quadro chegar e aparecer na tela, se o **decodificador é hardware ou software**, o codec/perfil, FPS, quadros descartados, taxa de dados e perda de pacotes. `Alt+L` mostra/esconde. |
| **Modo leve** | Desliga desfoques e animações da interface do site, que disputam a GPU com o vídeo. |

Tudo pode ser ligado e desligado clicando no ícone da extensão.

### Opções de codec/decodificação

- **Melhor para minha placa de vídeo** (padrão): escolhe sozinho conforme o teste de hardware.
- **H.264 High**: melhor qualidade de imagem; bom quando a GPU decodifica H.264.
- **H.264 Baseline**: imagem um pouco pior, mas decodificação mais leve. Útil em PCs fracos.
- **Não mexer**: deixa o Xbox escolher.

A troca de codec vale a partir do próximo jogo iniciado.

### Se o painel mostrar "Decodificador: Software"

A extensão não consegue ligar a decodificação por hardware sozinha. Isso é configurado no navegador:

1. Em `chrome://settings/system` (ou `edge://settings/system`), ligue **Usar aceleração de hardware quando disponível** e reinicie o navegador.
2. Abra `chrome://gpu` e confira se **Video Decode** está como *Hardware accelerated*.
3. Atualize o driver da placa de vídeo.
4. Em notebooks, use o carregador e o modo de energia "Melhor desempenho". No Windows, em Configurações → Sistema → Tela → Elementos gráficos, defina o navegador como "Alto desempenho".
5. Nas configurações de stream do próprio Xbox, baixar para 720p reduz bastante o tempo de decodificação.

Um asterisco (`Software*` / `Hardware*`) indica que o navegador não informou o decodificador em uso e o valor foi previsto pelo teste inicial.

## Como instalar

1. Baixe esta pasta (`xcloud-latencia`).
2. Abra `chrome://extensions` (ou `edge://extensions`).
3. Ative o **Modo do desenvolvedor**.
4. Clique em **Carregar sem compactação** e escolha a pasta `xcloud-latencia`.
5. Abra (ou recarregue) `https://www.xbox.com/play` e comece um jogo.

## Limites

Nenhuma extensão consegue reduzir o tempo que os dados levam pela internet até o datacenter da Microsoft. Isso depende da sua conexão e da distância até o servidor. A extensão tira o atraso que o **navegador** acrescenta. Para o resto, o que mais ajuda é:

- usar **cabo de rede**, ou Wi-Fi **5 GHz** perto do roteador;
- deixar a **aceleração de hardware** ligada no navegador;
- fechar downloads, streams e outras abas pesadas enquanto joga;
- usar controle com fio ou Bluetooth bem pareado.

Com o buffer mínimo, conexões instáveis podem ter pequenos engasgos. Se isso acontecer, desligue só essa opção.

## Arquivos

- `manifest.json`: configuração da extensão (Manifest V3)
- `inject.js`: roda na página do Xbox e ajusta a conexão WebRTC do stream, além de desenhar o painel
- `bridge.js`: repassa as configurações salvas para a página
- `lite.css`: estilos do modo leve
- `popup.html`, `popup.js`, `defaults.js`: janelinha de configurações
