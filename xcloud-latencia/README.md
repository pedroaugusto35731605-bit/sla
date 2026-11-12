# xCloud Baixa Latência

Extensão para Chrome/Edge que diminui o atraso do **Xbox Cloud Gaming** (Game Pass na nuvem) jogado no navegador, em `xbox.com/play`.

## O que ela faz

| Ajuste | Como ajuda |
|---|---|
| **Buffer de vídeo mínimo** | Por padrão o navegador guarda alguns quadros antes de exibir, para absorver variações da rede. A extensão zera esse buffer (`jitterBufferTarget = 0`), então cada quadro aparece assim que chega. É o maior ganho, geralmente algumas dezenas de ms. |
| **Preferir H.264** | Coloca o H.264 em primeiro na negociação do stream. Ele é decodificado pela placa de vídeo em praticamente qualquer PC, com decodificação mais rápida e estável. |
| **Painel de latência** | Mostra na tela o ping até o servidor, o tempo de buffer, o tempo de decodificação, FPS, taxa de dados, perda de pacotes e codec. Assim dá pra ver o que está causando o atraso. `Alt+L` mostra/esconde. |
| **Modo leve** | Desliga desfoques e animações da interface do site, que disputam a GPU com o vídeo. |

Tudo pode ser ligado e desligado clicando no ícone da extensão.

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
