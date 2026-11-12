# SeriesS-Emu

Base experimental de um emulador de **Xbox Series S**, com foco no que define o
console: **memória apertada** (orçamento rígido para o jogo) e **I/O de SSD muito
rápido com descompressão em hardware**, que compensa a GPU de 4 TFLOPS.

Núcleo em **C++20** (otimizado para Zen 2) e biblioteca de jogos em **Python + PyQt6**.

> **Estado: base de pesquisa, não roda jogos comerciais.** Jogos de Series X|S vêm
> em pacotes XVC **criptografados** com chaves do console e dependem do sistema
> operacional próprio (hypervisor + Game OS). Hoje o SeriesS-Emu dá boot UEFI,
> executa código x86-64 homebrew (subconjunto de instruções), atende chamadas de
> kernel por HLE, aplica o orçamento de 7 GB, simula o SSD/DirectStorage com
> leitura e descompressão reais, faz streaming de texturas sob pressão de memória
> e envia comandos de GPU PM4 para o Vulkan, com upscaling de 1440p para 4K.

## Estrutura

```
xs-s-emulator/
├── core/
│   ├── hardware_spec.hpp     constantes do Series S (CPU, GPU, RAM 7+3, SSD, vídeo, mapas)
│   ├── memory_manager.*      10 GB unificados: 7 GB título + 3 GB sistema, pressão, compressão,
│   │                         scatter-gather, page tables x86-64 com páginas de 2 MB
│   ├── ssd_io.*              NVMe 2,4 GB/s + descompressor HW (até 8 GB/s), fila DirectStorage
│   ├── codec.*               XLZ: codec LZ77 em blocos de 64 KiB (no papel do Kraken/BCPack)
│   ├── cpu.* / jit_engine.*  decoder/interpretador x86-64 + JIT de blocos (16 threads lógicas)
│   ├── gpu.*                 Command Processor PM4 (RDNA 2) → Vulkan, render 1440p → saída 4K
│   ├── bios_uefi.*           firmware UEFI (SEC/PEI/DXE/BDS) + kernel do Xbox OS (HLE)
│   ├── log.hpp               logger ([INFO]/[WARN]…)
│   └── main.cpp              CLI xss-emu
├── filesystem/
│   ├── iso_parser.*          XDVDFS/GDF + cabeçalho XVD/XVC, metadados, capa e tags
│   ├── texture_streamer.*    streaming de mips sob demanda via SSD, integrado à pressão de memória
│   └── image_builder.*       gera uma imagem de demonstração (sem conteúdo protegido)
├── ui/
│   ├── main_window.py        biblioteca (PyQt6), tema escuro minimalista
│   └── game_library.ui       layout (Qt Designer)
├── tests/core_tests.cpp
├── CMakeLists.txt
└── requirements.txt
```

## Compilar

Requisitos: CMake ≥ 3.20, compilador C++20 com `std::format` (GCC 13+, Clang 17+,
MSVC 2022) e, opcionalmente, o Vulkan SDK.

```bash
# Linux (Debian/Ubuntu): sudo apt install build-essential cmake libvulkan-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

```powershell
# Windows: Visual Studio 2022 + Vulkan SDK
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

**Otimização para Zen 2.** O build Release usa `-O3 -march=znver2 -mtune=znver2`
(GCC/Clang) ou `/O2 /arch:AVX2` (MSVC), além de LTO. O binário resultante usa
instruções que **CPUs Intel e AMD antigas não têm** (ex.: SSE4a) e pode fechar com
"illegal instruction" nelas. Para gerar um binário portável:

```bash
cmake -S . -B build -DXSS_NATIVE_ZEN2=OFF
```

Sem Vulkan SDK o núcleo compila com GPU *headless*. Com o SDK mas sem driver, o
boot detecta isso e cai para headless sozinho.

## CLI

```bash
./build/xss-emu spec                      # hardware emulado
./build/xss-emu boot                      # UEFI + ROM de diagnóstico com trace da CPU
./build/xss-emu boot --jit --quiet
./build/xss-emu memtest                   # estoura o orçamento de 7 GB de propósito
./build/xss-emu mkimage demo.iso          # imagem de teste: 24 texturas 2048² + nível
./build/xss-emu info demo.iso --files     # metadados + tags em JSON
./build/xss-emu bench demo.iso            # carga DirectStorage: NVMe × SATA × HDD
./build/xss-emu stream demo.iso           # streaming de texturas disputando os 7 GB
```

`SERIESS_LOG=quiet` mostra só avisos; `SERIESS_LOG=debug` mostra também o JIT e as
alocações espalhadas.

## Memória: 7 GB + 3 GB

| Faixa física | Partição | Pool |
|---|---|---|
| 0 – 2 GB | System Reserved | lento, 56 GB/s |
| 2 – 3 GB | System Reserved | rápido, 224 GB/s |
| 3 – 10 GB | **User Space (título)** | rápido, 224 GB/s |

- O título só aloca no User Space. As page tables e as estruturas do kernel são
  cobradas do sistema.
- **Pressão** (fração dos 7 GB): **Elevated ≥ 80 %** faz o streaming passar a pedir
  texturas com metade da resolução. **Critical ≥ 95 %** comprime os dados frios
  marcados como `Compressible` e reduz as texturas para ¼.
- **Acima de 100 %**, a alocação tenta, em ordem: (1) *reclaim*, em que os donos de
  recursos (o streamer) despejam o que está frio; (2) compressão forçada de dados
  frios. Se mesmo assim faltar memória:
  ```
  [WARN] Memory threshold exceeded on Series S backend.
  ```
  e a alocação falha (`OutOfMemory` no C++, `STATUS_NO_MEMORY` para o título).
- **Eficiência:** a reserva de 10 GB é virtual, então o host só gasta RAM nas páginas
  tocadas, e frames liberados voltam ao host (`MADV_DONTNEED`/`MEM_DECOMMIT`). Buffers
  grandes usam páginas de 2 MB. Com o espaço físico fragmentado, a alocação é montada
  em várias extensões (scatter-gather) que a page table une num intervalo virtual
  contínuo, então fragmentação não causa falha. `lock()`/`unlock()` impedem que o
  host perca um ponteiro para memória que a pressão comprimiria.

## SSD e DirectStorage

Cada requisição passa por um pipeline com relógio simulado:

```
comando NVMe (~30 µs, até QD 64 sobrepostos) → barramento 2,4 GB/s → descompressor HW (≤ 8 GB/s)
```

Os bytes são **lidos e descomprimidos de verdade** na RAM do convidado. O relógio
simulado diz quanto tempo isso levaria no console. O mesmo lote é re-simulado
com HDD e SSD SATA, que não têm descompressor: neles a CPU descomprime, e usamos o
tempo real medido no host.

Exemplo de `bench` com a imagem de demonstração (576 MiB de assets → 167 MiB no disco):

```
Series S NVMe + HW decomp   166.67 MiB lidos → 575.88 MiB (3.46×) |   84 ms | efetivo 7.19 GB/s
SSD SATA III                166.67 MiB lidos → 575.88 MiB (3.46×) | 1092 ms | efetivo 0.55 GB/s
HDD 5400 RPM (Xbox One)     166.67 MiB lidos → 575.88 MiB (3.46×) | 4252 ms | efetivo 0.14 GB/s
```

**Sobre o codec:** o Series S descomprime zlib e BCPack em hardware, e muitos
jogos usam Oodle Kraken (proprietário). O XLZ é um codec próprio da família LZ4,
com blocos independentes de 64 KiB. Ele reproduz o que importa para a simulação
(paralelismo por bloco, decodificação rápida, taxa de 2–4× em dados de jogo), mas
não lê arquivos Kraken/BCPack.

## Interface

```bash
pip install -r requirements.txt
python ui/main_window.py
```

- Lista de jogos com capa, Title ID, tamanho e tags (`4TFLOPS`, `Zen 2`, `1440p`, `RDNA 2`…).
- **Baixar ISO:** cole a URL direta `.iso`/`.xvc` e clique em **Download & Install**.
  A barra de progresso simula a gravação no SSD a ~2,4 GB/s, e o jogo aparece na
  lista ao terminar. **O download é simulado:** nada é baixado, o tamanho é
  sintético e o jogo fica marcado com `"mock": true`. O espaço é checado contra
  os 512 GB do SSD.
- **Importar ISO:** o núcleo lê metadados, tags e capa (`xss-emu info --thumbnail`).
- **Iniciar** faz o boot do núcleo e mostra o console. **Benchmark de I/O** roda o
  `bench` na imagem importada.

A biblioteca fica em `~/.seriess-emu/library.json` (Windows: `%APPDATA%\SeriesS-Emu`),
ou em `$XSS_HOME`.

## Especificações: pedido × console real

| Item | Pedido no projeto | Usado | Observação |
|---|---|---|---|
| CPU | Zen 2, 8C/16T, 3,4 GHz | igual | 3,6 GHz sem SMT |
| GPU | RDNA 2, 20 CUs, 1050 MHz, 4 TFLOPS | **1,565 GHz** | 20×64×2×1,05 GHz = 2,69 TFLOPS; com 1,565 GHz fecha 4,0 |
| RAM | 10 GB GDDR6, 7 jogo + 3 SO | igual | o console real libera ~8 GB aos jogos (8 GB a 224 GB/s + 2 GB a 56 GB/s) |
| SSD | 512 GB, 2,4 GB/s, até 8 GB/s comprimido | igual | a Microsoft divulga 4,8 GB/s típico |
| Vídeo | 1440p → 4K | igual | |

## Roadmap

1. CPU: decoder completo (SIB, RIP-relative, SSE/AVX2) e codegen nativo no JIT.
2. Loader PE/COFF para homebrew, com resolução de imports por HLE.
3. GPU: recompilador de shaders RDNA → SPIR-V, swapchain, Sampler Feedback real.
4. Reclaim em duas fases: perguntar quanto cada handler pode liberar antes de despejar
   (hoje, um pedido impossível ainda despeja as texturas frias antes de falhar).
5. Download real opcional, com verificação de hash.

## Aviso legal

O projeto não contém código, chaves nem firmware da Microsoft. Use apenas software
que você tem direito de executar. Xbox e Xbox Series S são marcas da Microsoft; este
projeto não tem relação com ela.
