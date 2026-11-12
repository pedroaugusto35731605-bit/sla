# XOne-Emu

Emulador experimental de Xbox One: núcleo em **C++20** (CPU x86-64, memória
paginada, GPU via **Vulkan**, BIOS UEFI simulado, parser de imagens) e uma
interface de biblioteca de jogos em **Python + PyQt6** com tema escuro inspirado
no Xbox.

> **Estado atual: base de pesquisa, não roda jogos comerciais.**
> Os jogos de Xbox One vêm em pacotes XVD/XVC **criptografados** com chaves do
> console e dependem de um sistema operacional próprio (hypervisor + Host OS +
> Game OS). Hoje o XOne-Emu dá boot UEFI, executa código x86-64 homebrew
> (subconjunto de instruções), atende chamadas de kernel por HLE, processa
> comandos de GPU PM4 → Vulkan, lê imagens XDVDFS/GDF e cabeçalhos XVD, e
> gerencia a biblioteca. Veja o [Roadmap](#roadmap).

## Estrutura

```
xone-emulator/
├── core/
│   ├── hardware_spec.hpp   constantes de hardware (CPU, GPU, RAM, SSD, mapas de memória)
│   ├── memory.cpp/hpp      8 GB unificados + page tables x86-64 de 4 níveis + TLB
│   ├── cpu.cpp/hpp         decoder x86-64, interpretador, HLE do kernel, cluster 8 núcleos
│   ├── jit_engine.cpp/hpp  JIT tier-1: cache de blocos básicos pré-traduzidos
│   ├── gpu.cpp/hpp         Command Processor PM4 (GCN) + contexto Vulkan
│   ├── bios.cpp/hpp        UEFI simulado (SEC/PEI/DXE/BDS) + ROM de diagnóstico
│   └── main.cpp            CLI `xone-emu` (também é o backend da UI)
├── filesystem/
│   ├── iso_parser.cpp/hpp  XDVDFS/GDF + cabeçalho XVD/XVC, metadados e capa
│   └── content_store.cpp/hpp  biblioteca (library.json) compartilhada com a UI
├── ui/
│   ├── main_window.py      janela principal (PyQt6)
│   └── game_library.ui     layout (Qt Designer)
├── tests/core_tests.cpp    testes do núcleo
├── tools/make_test_iso.py  gera uma imagem XDVDFS de teste (sem conteúdo protegido)
├── build/                  diretório de compilação
├── CMakeLists.txt
└── requirements.txt
```

## Compilar o núcleo

Requisitos: CMake ≥ 3.20, compilador C++20 com `std::format`
(GCC 13+, Clang 17+ ou MSVC 2022), e opcionalmente o Vulkan SDK.

**Linux**
```bash
sudo apt install build-essential cmake libvulkan-dev   # Debian/Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Windows** (Visual Studio 2022 + [Vulkan SDK](https://vulkan.lunarg.com/))
```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release
```

Sem Vulkan SDK o núcleo compila mesmo assim, com GPU *headless*
(`-DXONE_ENABLE_VULKAN=OFF` força esse modo). Se o SDK existe mas não há driver,
o boot detecta e cai para headless automaticamente.

## Usar a CLI

```bash
./build/xone-emu spec                  # hardware emulado
./build/xone-emu boot                  # boot UEFI + ROM de diagnóstico com trace
./build/xone-emu boot --jit --quiet    # mesmo programa pelo JIT
./build/xone-emu boot --image app.xbin # binário homebrew cru carregado em 0x400000
./build/xone-emu info jogo.iso --files # metadados em JSON
./build/xone-emu import jogo.iso       # adiciona à biblioteca
./build/xone-emu library
./build/xone-emu extract jogo.iso MicrosoftGame.config saida.xml
```

Trecho do `boot`:
```
[XONE-BIOS] ExitBootServices: core0 RIP=0x400000 RSP=0x7FFEFFF0, cores 1-7 estacionados
[XONE-CORE] Executing instruction at address 0x0000000000400023 | core0 | 48 01 C8 | add rax, rcx
[XONE-CORE] Executing instruction at address 0x0000000000400026 | core0 | 48 FF C9 | dec rcx
[XONE-CORE] Executing instruction at address 0x0000000000400029 | core0 | 75 F8    | jne 0x400023
[XONE-KRNL] XDbgPrintHex(core0): 0x37 (55)
[XONE-KRNL] XGpuSubmitCommandBuffer(va=0x400130, dwords=18)
[XONE-GPU ] DrawCall: prim=4 vertices=6 instancias=4 (auto)
[XONE-GPU ] Frame 1 apresentado via Vulkan 1.2: 2 draws, clear=#107C10FF
```

## Interface gráfica

```bash
pip install -r requirements.txt
python ui/main_window.py
```

- **Grade de capas** com busca, ordenação e filtros (biblioteca / recentes / downloads).
- **Importar ISO**: chama `xone-emu import`, que lê Title ID, nome, editora e a capa da imagem.
- **⬇ Baixar ISO**: cole uma URL direta `.iso`/`.xvc`; aparece uma barra de progresso
  animada e, ao terminar, o jogo entra na biblioteca sem reiniciar o app.
  **O download é simulado**: nenhum dado é baixado (o jogo fica marcado como
  `"mock": true` no `library.json`).
- **Detalhes**: resolução alvo, FPS alvo, Title ID, formato, tamanho, origem.
- **▶ Jogar**: faz o boot do núcleo e mostra o console com o trace da CPU.
- Atalhos: `Ctrl+D` baixar, `Ctrl+O` importar, `Ctrl+F` buscar, `F5` recarregar.

A biblioteca fica em `~/.xone-emu/library.json` (Windows: `%APPDATA%\XOne-Emu`),
ou em `$XONE_HOME`. Núcleo e UI leem e gravam o mesmo arquivo; chaves que um
lado não conhece são preservadas pelo outro.

Para testar sem ter jogos:
```bash
python tools/make_test_iso.py demo.iso
./build/xone-emu import demo.iso      # ou "Importar ISO" na UI
```

## Arquitetura do núcleo

| Componente | O que faz hoje |
|---|---|
| **Memória** | Reserva 8 GB contíguos (`mmap`/`VirtualAlloc`, commit sob demanda). Page tables x86-64 reais (PML4→PDPT→PD→PT) dentro da RAM do convidado, bits P/W/U/NX, TLB em software de 1024 entradas, `#PF` em acesso inválido. |
| **CPU** | Decoder com REX: MOV (reg/imm/mem), ADD, SUB, XOR, CMP, TEST, INC, DEC, PUSH, POP, CALL, RET, JMP, Jcc, SYSCALL, HLT, NOP. Flags CF/ZF/SF/OF, escrita de 32 bits zera a metade alta. 8 núcleos; cada núcleo ativo roda numa thread do host. Instrução fora do subconjunto gera `#UD` com endereço e bytes. |
| **Kernel (HLE)** | `XDbgPrint`, `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `KeQueryPerformanceCounter`, `XGpuSubmitCommandBuffer`, `NtTerminateProcess` via `SYSCALL` (convenção própria do XOne-Emu). |
| **JIT** | Tier-1: traduz blocos básicos uma vez para uma sequência de handlers e reaproveita por endereço, com invalidação por faixa. O campo `TranslatedBlock::native` é reservado para o codegen nativo. |
| **GPU** | Command Processor com pacotes PM4 tipo 3 (`SET_CONTEXT_REG`, `SET_UCONFIG_REG`, `NUM_INSTANCES`, `DRAW_INDEX_AUTO`, `DRAW_INDEX_2`, `EVENT_WRITE_EOP`…). Vulkan: instância, escolha de GPU, fila graphics+compute, device, command pool, render target 1920×1080 e submissão por frame com fence. |
| **BIOS** | POST por amostragem, mapa de memória EFI publicado na RAM, "drivers" DXE, carga da imagem, pilha e ring da GPU, reset do core 0. |
| **ISO** | XDVDFS/GDF nos offsets XISO/XGD1/XGD2/XGD3; metadados de `MicrosoftGame.config`, `AppxManifest.xml` ou `default.xbe`; capa pelo `StoreLogo`. XVD: magic `msft-xvd`, VDUID, tipo de conteúdo, Product ID, versão (layout do cabeçalho segundo a documentação pública do xvdtool). |

### Sobre as especificações

`hardware_spec.hpp` usa o perfil pedido para o projeto (2,3 GHz, 12 TFLOPS,
SSD NVMe), mas isso mistura gerações: o Xbox One original tem Jaguar a
1,75 GHz e GPU de 1,31 TFLOPS; 2,3 GHz é o Xbox One X (6 TFLOPS); 12 TFLOPS e
SSD NVMe são do Series X. Os valores reais estão documentados em `kRetail`.

## Roadmap

1. **CPU**: decoder x86-64 completo (SIB, RIP-relative, prefixos 66/F2/F3), SSE/AVX
   (o Jaguar tem AVX), e codegen nativo no JIT (x86-64 → x86-64/ARM64).
2. **Formato de executável**: loader PE/COFF para executáveis de homebrew do Xbox One
   (modo Dev Mode/UWP), com resolução de imports por HLE.
3. **GPU**: recompilador de shaders GCN → SPIR-V, pipelines, swapchain com janela,
   detiling de texturas e emulação da ESRAM.
4. **Sistema**: threads (IPI para os núcleos 1-7), sincronização, áudio, input.
5. **Download real** opcional (com hash/verificação), substituindo o modo simulado.

## Aviso legal

O XOne-Emu não contém código, chaves ou firmware da Microsoft. Use apenas
software que você tem direito de executar (homebrew ou cópias dos seus próprios
jogos). Xbox é marca registrada da Microsoft; este projeto não tem relação com ela.
