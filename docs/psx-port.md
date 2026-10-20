# Porte do Mortal Kombat 4 (PS1) para o Lion — desenho da camada de runtime

Base: [alexbeavs-ps1-ports/mortal-kombat-4-recomp](https://github.com/alexbeavs-ps1-ports/mortal-kombat-4-recomp)
sobre o framework [mstan/psxrecomp](https://github.com/mstan/psxrecomp) (pin `eecf3b2a`, fork Alexbeav).
Este documento é o resultado da leitura do `runtime/` do psxrecomp; nada foi compilado ainda.

## Como o psxrecomp funciona (o que importa para o porte)

- `recompiler/` (C++, roda no PC) traduz MIPS R3000A → C: `generated/<serial>_full*.c` e `_dispatch.c`.
  O MK4 (`SLUS-00605`, entrada `0x00010008`, texto 1 MB) precisa do EXE do **disco do usuário**; o repositório
  não traz o código gerado (o Toy Story 2 traz, 62 MB de C). Sem o disco não há como gerar nem testar.
- O C gerado fala só com `CPUState` (gpr, hi/lo, cop0, GTE inline) e com poucas funções do runtime:
  `psx_dispatch_call`, `psx_syscall`, leitura/escrita de memória por ponteiros em `CPUState`, relógio
  (`psx_cycles.c`) e ganchos de depuração (que se desligam com `PSX_NO_DEBUG_TOOLS`).
- O runtime (`runtime/src`, ~33 mil linhas só nos módulos de hardware) simula: RAM 2 MB, GPU (`gpu.c` 8 k
  linhas + rasterizadores), SPU (`spu.c`), CD (`cdrom.c`), DMA, timers, SIO (pads/cartão), MDEC, interrupções.
  Tudo isso está acoplado a SDL3, OpenGL/Vulkan, fibers (`ucontext`), servidor TCP de depuração, mods e netplay.
- Três camadas de execução: estática (BIOS + EXE), **overlays nativos compilados em tempo de execução com
  gcc/tcc e carregados como DLL** (`overlay_loader.c`), e um interpretador MIPS para código novo na RAM
  (`dirty_ram_interp.c`).

## O que não serve no Lion

| Peça do psxrecomp | Problema no Lion | Decisão |
| --- | --- | --- |
| Overlays por DLL + gcc/tcc | Não há `dlopen` nem compilador no console | Assar todos os overlays no binário (modo "static" do `overlay_backend`); onde faltar, cair no interpretador |
| SDL3, GL, Vulkan, PGXP, HD textures, netplay, mods, debug TCP | Não existem | Fora; compilar com `PSX_NO_DEBUG_TOOLS` e sem esses módulos |
| `psx_fiber.c` (ucontext) | Sem libc de SO | Trocar por troca de contexto própria em PowerPC (uma `.S` pequena) ou desativar threads do kernel com o BIOS "bios_hle" |
| BIOS recompilado (`OpenBIOS_full.c` = 9 MB de C) | Grande demais para a SDRAM de 24 MB | Usar o modo `bios_hle` (pula o boot e intercepta serviços do kernel) e manter só o trecho do kernel que o jogo usa |
| Código gerado ≈ dezenas de MB de C | A SDRAM é de 24 MB | Medir o `.text` do clang PPC; se passar de ~8 MB, dividir em funções quentes (nativas) e frias (interpretadas) |

## A camada Lion (`psxlion/`)

Biblioteca freestanding, compilada com `clang --target=powerpc-unknown-none-elf -mcpu=7400`, no mesmo molde
de `unreal/` (`crt0.S`, `link.ld`, imagem em ISO). Cada módulo troca um chip do PS1 por um do Lion:

| PS1 | Lion | Como |
| --- | --- | --- |
| RAM 2 MB, scratchpad 1 KB | SDRAM | Bloco fixo; `read_word`/`write_word` de `CPUState` viram acessos diretos com máscara KUSEG/KSEG0/KSEG1 (sem tabela de páginas) |
| GPU: GP0/GP1, VRAM 1 MB | **Spike** + SGRAM (8 MB) | Ver abaixo |
| GTE | Tuffy (FPU/AltiVec) ou inteiro puro | `gte.cpp` já é C++ de ponto fixo; compilar como está. Otimizar com AltiVec depois |
| SPU: 24 vozes ADPCM, 512 KB RAM | **Tyke** (24 vozes PCM16) | Decodificar ADPCM PS1 → PCM16 ao carregar a RAM do SPU, ou no momento do "key on"; envelope ADSR e reverb no Tuffy; saída por vozes do Tyke |
| CD-ROM (setores 2352 B, XA, CDDA) | CD-ROM do Lion (ISO9660, 2048 B) | Ver abaixo |
| DMA, timers, IRQ | Tuffy + PIC | `dma.c`/`timers.c`/`interrupts.c` são lógica pura; só trocar o relógio por um contador do Tuffy (`mftb`) |
| Pads (SIO0) | Controles `0x1F05_0000` | Mapear os botões do Lion no protocolo digital do PS1 (L1/R1/L2/R2 etc.) |
| MDEC (FMV) | Tuffy (AltiVec) | `mdec.c` é C puro; vídeo do MK4 (intro) pode ser pulado na primeira fase |

### GPU → Spike

O ponto mais delicado. O `gpu.c` já separa a decodificação dos comandos GP0 da rasterização: ele chama um
renderizador (`gpu_sw_renderer.c`, `gpu_gl_renderer.c`). A camada nova implementa a mesma interface:

- Polígonos/triângulos/retângulos de GP0 → lista de comandos da Spike (Gouraud, textura, color key).
- Texturas: a VRAM do PS1 tem páginas de 4/8/15 bits com CLUT. A Spike lê RGB565; então uma cache converte
  cada (página, CLUT, janela) para uma textura RGB565 na SGRAM na primeira vez que é usada. Cor preta pura
  (`0x0000`) já é transparente na Spike, como no PS1; o bit STP/semitransparência exige blend (o Lion tem
  blend aditivo; subtrativo e média precisam de passe extra ou aproximação).
- Sem correção de perspectiva no PS1 (afim); a Spike faz perspectiva — ver o MK4 com a geometria já
  projetada pelo GTE: usar o `w` constante para reproduzir o visual afim.
- Cópias VRAM→VRAM, carga de imagem (GP0 0xA0) e leitura (0xC0): `memcpy` na SGRAM + invalidar a cache de textura.
- Display: GP1 (modo, área) → VI do Lion (240p/480i).

### CD

O MK4 lê arquivos do disco por LBA. Duas opções:

1. **Reempacotar** o disco: um passo no PC (`tools/`) extrai o ISO9660 do `.bin/.cue`, descarta o 2352→2048 e grava
   um ISO do Lion com o EXE e os arquivos; o runtime traduz LBA→arquivo por uma tabela. Mais simples.
2. Aceitar a imagem crua. Não vale: o Lion lê 2048 B.

Trilhas de áudio (CDDA) e XA: converter no PC para `.WAV` (o dashboard já toca WAV do disco).

## Ordem de trabalho

1. **No PC, sem o Lion:** compilar o `runtime` do psxrecomp com o MK4 e o disco do usuário; rodar. É a referência
   de correção (o runtime do psxrecomp tem um modo "headless" e comparação com o Beetle).
2. Montar `psxlion/` só com memória + CPU + dispatch e rodar o código gerado do BIOS/EXE no emulador Lion até a
   primeira escrita no GPU (GP1 reset). Log por UART/anel de log como no `unreal/`.
3. GPU → Spike: primeiro retângulos e polígonos sem textura (tela de boot), depois texturas com CLUT.
4. CD (leitura do EXE, depois dos arquivos), pad, timers/IRQ.
5. SPU → Tyke.
6. Overlays assados + medidas de tamanho/velocidade; MDEC por último.

## Estado (medido)

- Disco verificado: MD5 `3c3ddcb5…` = o do `game.toml`; `SLUS_006.05` (1 079 296 B, entrada `0x00010008`).
  Arquivos do disco: `FAT.OVL`, `GEO1/GEO2`, `ANI`, `SND`, `XAAUD`, `INTROS`, `ENDINGS`, `BLANK.DAT` (30 MB).
- O `psxrecomp-game` foi compilado com o cmake/g++ do sistema e gerou o C do MK4 em ~3 s: 188 shards,
  4729 funções, 280 MB de texto (10 M de linhas, quase tudo `#ifdef` de instrumentação).
- Runtime de referência no PC **compilado e rodando**: `MK4/work/mk4/build-rt/Mortal_Kombat_4` (SDL3 baixado
  pelo cmake; `-DPSX_REWIND=OFF -DPSX_RECOMP_UI=OFF`), com a OpenBIOS que vem no framework
  (`--bios psxrecomp/bios/openbios.bin`). Em 40 s executou 2313 quadros, com o PC dentro do código do jogo
  (`psx_last_run_report.json`). Ainda não olhei a imagem na tela.
- Tamanho do código do jogo: com `PSX_ENABLE_BLOCK_CYCLES` (como o runtime de referência compila) são **36 MB**
  em x86-64 -O3, grande demais para os 24 MB do Lion. Sem essa instrumentação de ciclos, um shard (o 00) cai de
  446 KB para 54–64 KB (-Os/-O3), ou seja, o jogo todo ficaria em ~5–7 MB. **Conclusão: no Lion, compilar
  sem `PSX_ENABLE_BLOCK_CYCLES`** (o tempo dos timers passa a ser grosseiro, por bloco ou por quadro) e com `-Os`.
  **Medido com clang PPC** (`psxlion/build_game.sh`, headers freestanding em `psxlion/shim/`, sem
  `PSX_ENABLE_BLOCK_CYCLES`): os 189 arquivos (188 shards + dispatch) compilam sem erro; `-Os` = 5,11 MB de
  código + 0,67 MB de dados (tabelas de despacho) = **5,8 MB**; `-O2` = 5,82 MB (praticamente igual).
- **Núcleo `psxlion/` linkando** (`psxlion/build.sh MK4/work/mk4 MK4/work/mk4/psxrecomp <saída>`): jogo + OpenBIOS
  recompilados + `psxlion/rt/` + `gte.cpp` do framework (compilado como está, com shims de `std::`) linkam com
  `ld.lld` **sem símbolos indefinidos**. Imagem: 5,46 MB de código + 0,70 MB de dados + 2,1 MB de bss (a RAM do PS1)
  ≈ 8,3 MB. Ainda não executa: nunca foi rodada no emulador.
  - `rt/core.c`: RAM 2 MB / scratchpad / ROM do BIOS com acesso little-endian (o Tuffy é big-endian), `CPUState`,
    `psx_syscall` (Enter/ExitCriticalSection direto; o resto entra no vetor de exceção do BIOS), `psx_dispatch*`
    encaminhando ao `OpenBIOS_psx_dispatch*`, variáveis de estado e falhas.
  - `rt/hw.c`: MMIO, ainda vazio (leituras 0). `rt/libc.c`, `rt/stubs.c`: mem*/str*, divisão de 64 bits, PGXP e
    widescreen desligados.
  - **Stubs que precisam virar código de verdade antes de o BIOS bootar:** entrega de interrupções
    (`psx_check_interrupts*`, que no PC usa `setjmp`/fibers), o interpretador de RAM suja (`dirty_ram_dispatch`; o
    OpenBIOS grava código no kernel em tempo de execução), `psx_kernel_bless_dispatchable`, `psx_rfe_escape_check`,
    e todo o hardware (GPU, SPU, CD, DMA, timers, pads). Alternativa para encurtar: modo `bios_hle` (pula o boot).
- **Boot do BIOS no emulador Lion (feito):** `psxlion/build.sh` gera `psx.lion` (ELF→`.lion` por `mklion.py`, imagem em
  `0x240000`, log em `0x1F0000`); `build/lion psx.lion --headless --frames N --dump-ram log.bin,0x1F0000,N` lê o log.
  O OpenBIOS recompilado roda no Tuffy: inicializa o controlador de memória, imprime o banner pelo TTY
  (`uC-sdk - booting`), configura o SPU e chega ao **primeiro acesso à GPU: `GP1 reset` (`0x1F801814 = 0`)**, depois de
  ~250 M de ciclos emulados. Para chegar lá foi preciso o interpretador de RAM suja: o BIOS escreve em tempo de execução
  os vetores A0/B0/C0 (`0xA0/0xB0/0xC0`), que não estão na tabela estática.
  - `rt/interp_deps.c` liga o `dirty_ram_interp.c` do framework (compilado sem alterações) ao Lion: overlays, PGXP,
    widescreen e depurador viram stubs. O rastreio de páginas sujas (4 KB; escritas na janela do kernel
    `0..0xFFFF` e `dirty_ram_mark_executable_range`) está em `rt/core.c`. Custo de memória: o interpretador traz
    ~10,6 MB de bss (uma tabela de 8 MB, `g_dirty_ram_pc_table`) — a imagem inteira passa a ter 19,5 MB de 24; reduzir.
  - `rt/irq.c`: entrega de interrupções **sem `setjmp`/fibras**. O handler do kernel roda num `psx_dispatch` aninhado; o
    `RFE` (`psx_rfe_escape_check`) e o `RestoreState` desfazem os quadros aninhados pelo protocolo de "call bail" do
    próprio despacho (`g_psx_call_bail` + `cpu->pc`). VBLANK vem do contador de quadros do VI do Lion
    (`0x1F030010`), lido a cada 1024 bordas de bloco. **Não suportado ainda (para com log):** handler que retoma
    outro contexto (`ChangeThread`) — pediria uma pilha de máquina por thread do jogo.
  - Com `PSX_EXTRA_CFLAGS=-DPSX_GPU_PASSTHROUGH` a GPU finge estar sempre pronta: o BIOS segue e manda
    `GP1 reset`, modo de vídeo, e os `E3/E4/E5` (área de desenho).
  - **Depois da GPU:** o BIOS espera o **DMA do SPU** (`CHCR = 0x01000201`, canal 4): `rt/hw.c` ganhou DMA (DPCR/DICR; os
    7 canais completam na hora; o OTC, canal 6, escreve a tabela de ordenação) e `SPUSTAT` espelhando `SPUCNT`.
  - **CD-ROM** (`rt/cd.c`): controlador completo o bastante para o BIOS: bancos de registradores, FIFOs, Init/Getstat/
    Setloc/SeekL/ReadN/Pause/GetID/GetTN/GetTD/Setmode/Test, INT1–5 com atraso, FIFO de dados, DMA do canal 3. **Os
    setores vêm do disco do MK4 cru** (`MK4.BIN`, 2352 B/setor) dentro de uma ISO do Lion, lidos pelo CD do Lion em blocos
    de 16 setores (`rt/lioncd.h`). Verificado: o BIOS lê o volume ISO9660 (`01 CD001`) e o diretório raiz certos.
  - **Disco em duas etapas** (`psxlion/mkiso.sh`): a ROM do Lion só carrega um `BOOT.LION` de até ~6 MB (3000 setores)
    e a imagem tem 6,7 MB. `BOOT.LION` agora é o *estágio 0* (`rt/stage0.c`, 2 KB) que lê `PSX.IMG` (`mkimg.py`,
    segmentos alinhados a 2048 B) direto para os endereços finais por DMA e salta para ele; o `crt0` zera o bss.
  - **Contadores de raiz** (`0x1F801100..2F`) calculados do time base do Tuffy (25 MHz → 33,8688 MHz). O BIOS espera por
    eles (e por `GPUSTAT`) em laços de VSync. `GPUSTAT`: bits de pronto + bit 13 (campo) e bit 31 em fases opostas,
    seguindo o contador de quadros do VI (modo 3 de `PSX_GPUSTAT_MODE`).
  - **Marco: o BIOS liga as interrupções** (`SR = 0x40000401`, aos ~630 quadros do Lion, ~10 s de tempo PS1). Antes
    disso ele espera ~5 s de relógio PS1 depois do segundo `Pause` — o **runtime de referência no PC faz a mesma espera**
    (165 M de ciclos entre o `Pause` e o `Init` seguinte) — e a sequência de comandos do CD é idêntica à dele
    (`0A 0A 13 1A 0E 02 06 09 02 06 09`, depois `0A 02 15 0E 06 09 …`).
  - **O jogo é alcançado.** Depois das interrupções ligadas o BIOS lê `SYSTEM.CNF`, carrega o `SLUS_006.05` por DMA do CD e despacha
    para `0x80010008`. Para isso: `-DPSX_HAS_GAME_DISPATCH` no despacho do BIOS e no interpretador (sem ele o código do jogo
    não era roteado), "jogo iniciou" = primeiro despacho à entrada do EXE (`fntrace_record`), e texto nativo só vale
    depois disso e enquanto nenhum DMA do CD sobrescreveu a página (`dirty_ram_text_native_ok*` em `rt/core.c`).
  - **Troca de contexto na interrupção (feita):** quando o handler do kernel retoma outra thread, `rt/abandon.S` descarta os
    quadros da thread deixada e reentra no despacho no novo EPC (`lion_abandon`, em `rt/irq.c`).
- Pontos ainda abertos: sobre `FAT.OVL`, não sei se o MK4 carrega código em overlays; isso só se vê rodando.

## Estado atual (20/10/2026): MK4 e GTA jogáveis, luta do MK4 com imagem correta

Os dois jogos rodam do boot até a partida, com controle. MK4 usa a OpenBIOS; GTA (e MML) a `SCPH1001` recompilada
(`PSX_BIOS=SCPH1001 BIOS=…/SCPH1001.BIN`).

- **MK4:** logos, FMV de abertura (MDEC 24 bits, imagem correta), título, menu, seleção de lutador, torre e a luta completa
  (cenário, lutadores, sombras, HUD) a ~18–20 quadros por segundo (medido pelas trocas de área de exibição,
  `gpu: flips per 256 frames` no log; o jogo no PS1 roda a 30).
- **GTA:** logos (FMV e 3D), menu, seleção de jogador, mapa e o jogo em Liberty City com HUD, textos de missão e som
  (~14 quadros/s no jogo, 60 nos menus).

Correções desta etapa (todas em `psxlion/rt/`):

- **Polígonos Gouraud (`gpu.c`, `exec_poly`):** os comandos 0x30–0x3F liam as palavras deslocadas de uma (cor da palavra do
  xy, xy da do uv…): vértices aleatórios. Eram os polígonos gigantes e os lutadores invisíveis da luta (~550 dos ~740
  polígonos de um quadro; os menus quase não usam Gouraud).
- **Upload CPU→VRAM (`gpu.c`):** a última palavra de dados sempre descartava a metade alta como enchimento, que só existe com
  contagem ímpar de pixels: todo upload par perdia o último pixel. A última entrada de cada CLUT ficava 0x0000 (transparente):
  os buracos na lava e os traços nas paredes. O mesmo na leitura VRAM→CPU.
- **Referência de quadro (`gpuref.py`, novo):** com `-DPSX_GPU_TRACE_FRAME=N` a gravação também copia a VRAM para
  `0x1500000`; `gpuref.py gt.bin vram.bin out.png [flips] [x,y…]` redesenha o quadro como a GPU do PS1 e diz que comando
  pintou cada pixel pedido. Igual à imagem do Spike = o erro está nos dados; diferente = na tradução para o Spike.

- **Controle (`sio.c`):** o byte recebido e o /ACK (IRQ7) chegam com o tempo do hardware (≈1088 e ≈1258 ciclos do PS1 depois
  da escrita, medidos no relógio do time base). Respondendo na hora, o `readPad` da OpenBIOS e o libpad perdiam o ACK e
  davam o controle como desconectado.
- **MDEC por DMA (`hw.c`):** os canais 0 e 1 só andam quando o MDEC pede (DREQ). O libpress arma a saída antes da entrada;
  com DMA instantâneo cada fatia da imagem chegava uma transferência atrasada.
- **Texturas no Spike (`gpu_spike.c`):** a volta SGRAM→`g_vram` copia só os pixels que o Spike mudou (o RGB565 não tem o bit
  15, que em texturas de 4/8 bits é dado de índice: as listras verticais do GTA); a conversão de texturas é preguiçosa, por
  blocos de 16×16 texels que a primitiva usa; a CLUT vale pelo conteúdo (checksum), não pela versão da página.
- **CD (`cd.c`):** a imagem é lida em blocos de 64 setores com leitura antecipada assíncrona. Cada READ do drive do Lion
  paga ≥80 ms de busca, e a leitura sob demanda bloqueava o Tuffy durante o streaming (FMV, música XA).
- **SPU (`spu.c`, novo):** as 24 vozes do SPU tocam nas 24 vozes do mixer do Tyke. ADPCM decodificado na hora do key-on
  para um cache de PCM16 (1,5 MB, validado por página de 1 KB da RAM de som), loops, ADSR por quadro, ENDX, transferências
  por FIFO e DMA 4. Faltam reverb, ruído, modulação de pitch, IRQ do SPU e o áudio do CD (XA e CD-DA: precisa de um
  programa próprio para o Tyke, com um fluxo estéreo além das 24 vozes).
- **Desempenho do cache de texturas (`gpu_spike.c`, `libc.c`):** com a geometria ainda quebrada, a luta do MK4 passou de
  ~5 para ~10,5 quadros/s (0x15 → 0x2D trocas por 256 quadros do VI). O custo era a conversão de texels (~1,9 milhão por quadro, ~30 instruções cada), não a
  busca. Mudanças: conversão sem janela de textura por palavra da VRAM com a CLUT convertida uma vez (`convert_fast`); busca
  por hash e substituição por relógio (segunda chance); evicção esperando só o token `OP_SYNC` emitido quando o slot deixou de
  ser ligado, não o Spike inteiro; versões da VRAM e "desenhado na SGRAM" por faixa de 16 linhas, e o bloco só sincroniza as
  faixas que lê; 22 slots de 128×128 a mais na SGRAM livre depois do quadro de exibição; `memset`/`memcpy` por palavra (o GTE
  do psxrecomp copia o estado inteiro a cada operação). Com a correção dos polígonos Gouraud as falhas caíram para quase zero (o
  lixo de geometria estourava o cache) e a luta foi a ~18–20 quadros/s (0x4F–0x54); estatísticas `tex:` no log.
- Log circular (`hw.c` + `logcat.py`), `lion --dump-every N`, `LION_ARGS` no `dev.sh`, gravação dos comandos da GPU de um
  quadro (`-DPSX_GPU_TRACE_FRAME=N`, decodificada por `gputrace.py`).

Como reproduzir (o boot até o menu leva ~6000 quadros, ~8 min no PC):

```bash
LION_ARGS="--hold 0:0 --hold 6100:1000 --hold 6106:0 --dump-every 300" psxlion/dev.sh MK4/work/mk4 MK4/work 9000
```
