# Atari Lion A2 — Referência de hardware do emulador

Este documento define o que o emulador implementa: mapa de memória, registradores,
formato de comandos do Spike, QVE, Tyke, CD-ROM e formato de imagem `.lion`.
A especificação de alto nível está em [Atari-Lion-A2-Project-Apex.md](../Atari-Lion-A2-Project-Apex.md).

Todos os registradores são de 32 bits, big-endian, alinhados a 4 bytes.

## Clocks e temporização

| Domínio  | Clock    | Observação                                   |
| -------- | -------- | -------------------------------------------- |
| Tuffy    | 500 MHz  | 1 ciclo por instrução + latências e caches (abaixo) |
| Quacker  | 100 MHz  | 1 ciclo por instrução + latências e caches (abaixo) |
| QVE      | 100 MHz  | 8 ciclos por vértice                          |
| Bus      | 100 MHz  | Time base / decrementer do Tuffy = bus / 4   |
| Spike    | 90 MHz   | 2 pixels por ciclo (contabilizado)           |
| Tyke     | 40 MHz   | DSP programável; saída estéreo a 44,1 kHz    |
| Vídeo    | 60 Hz    | 525 linhas (480p/480i) ou 262 linhas (240p)  |
| CD-ROM   | 12×      | 1,8 MB/s; busca de 80 ms + até 150 ms         |

### Modelo de ciclos

| CPU     | Despacho                                   | Cache                                   | Penalidades (ciclos do núcleo)                          |
| ------- | ------------------------------------------ | --------------------------------------- | ------------------------------------------------------- |
| Tuffy   | 2 instruções/ciclo + desvios dobrados      | L1 I/D 32 KB 8 vias, L2 1 MB 2 vias (linhas de 32 B) | acerto no L2 +10, SDRAM +60, sem cache/MMIO +30, store perde metade, desvio mal previsto +4 |
| Quacker | 2 instruções/ciclo                         | I/D 8 KB 2 vias (linhas de 32 B)        | SDRAM +10, MMIO +2, SRAM local sem penalidade, desvio tomado fora do TIC +1 |

- **Despacho duplo:** uma instrução sai no mesmo ciclo que a anterior quando as duas vão para
  unidades diferentes (ou para as duas unidades inteiras), a segunda não lê nem sobrescreve um
  registrador escrito pela primeira, e nenhuma delas é serializante (SPRs, MSR, sync, rfi,
  registradores de controle). Unidades do Tuffy: IU1, IU2 (multiplicação/divisão), LSU, FPU,
  VALU, VPERM, SRU. Unidades do Quacker: 2 inteiras, campos de bits, multiplicador
  (inteiro, FP e `pmul`), somador FP, divisor, load/store, soma de pixels, empacotamento de pixels.
- **Previsão de desvio do Tuffy:** tabela de histórico de 512 entradas com contadores de 2 bits.
  Desvios previstos corretamente são dobrados (não-tomado: 0 ciclo; tomado: encerra o grupo de busca, 1 ciclo).
- **TIC do Quacker:** 32 entradas; um desvio tomado que não está no TIC custa +1, exceto as
  formas `.n`, cujo delay slot esconde a bolha.
- **Disputa pela SDRAM:** um árbitro serializa as transferências de Tuffy, Quacker, DMA do CD e
  leituras externas do Tyke (linha de 32 B = 8 ciclos de barramento, acesso simples = 3, setor de
  CD = 256). Cada mestre usa o próprio relógio, contado desde a energização; a espera entra
  na penalidade do acesso. O Spike usa a SGRAM, em barramento próprio.

Latências extras: Tuffy `mullw/mulhw` +3, `mulli` +2, `divw/divwu` +18, estimativas e somas
AltiVec +2/+3; Quacker `mul` +2, `div` +37, `fdiv` +12 (simples) / +22 (duplo), `fsqrt` +25.
O FPU e o AltiVec são pipelined (sem custo extra nas operações comuns). As caches modelam só
o tempo: os dados ficam sempre na memória do barramento. `HID0[ICE/DCE]` e `L2CR[L2E]`
ligam as caches do Tuffy; `HID0[ICFI/DCFI]` e `L2CR[L2I]` as invalidam.

## Mapa de memória (físico, igual para Tuffy e Quacker)

| Início        | Fim           | Tamanho | Conteúdo                                 |
| ------------- | ------------- | ------- | ---------------------------------------- |
| `0x0000_0000` | `0x017F_FFFF` | 24 MB   | SDRAM principal                          |
| `0x1000_0000` | `0x107F_FFFF` | 8 MB    | SGRAM (texturas, framebuffer, Z)         |
| `0x1F00_0000` | `0x1F00_00FF` |         | Controle do Quacker / Command FIFO       |
| `0x1F00_1000` | `0x1F00_10FF` |         | QVE (Quacker Vector Engine)              |
| `0x1F01_0000` | `0x1F01_00FF` |         | Spike                                    |
| `0x1F02_0000` | `0x1F02_FFFF` | 64 KB   | Tyke (RAM de dados, controle, RAM de programa) |
| `0x1F03_0000` | `0x1F03_00FF` |         | VI (interface de vídeo)                  |
| `0x1F04_0000` | `0x1F04_00FF` |         | Controlador de interrupções (PIC)        |
| `0x1F05_0000` | `0x1F05_00FF` |         | Controles (4 portas)                     |
| `0x1F06_0000` | `0x1F06_00FF` |         | Drive de CD-ROM                          |
| `0x1F10_0000` | `0x1F10_3FFF` | 16 KB   | SRAM local do Quacker                    |
| `0xFFF0_0000` | `0xFFFF_FFFF` | 1 MB    | Boot ROM (reset em `0xFFF0_0100`)        |

## Quacker — controle (`0x1F00_0000`)

| Offset | Nome        | Acesso | Descrição                                                         |
| ------ | ----------- | ------ | ----------------------------------------------------------------- |
| `0x00` | CTRL        | RW     | bit 0 RUN (1 = executa); escrever 0 mantém em reset              |
| `0x04` | START_PC    | RW     | PC usado quando RUN passa de 0 para 1                             |
| `0x08` | FIFO        | W/R    | Tuffy escreve (push). Quacker lê (pop); vazio = Quacker aguarda   |
| `0x0C` | FIFO_LEVEL  | R      | Palavras na Command FIFO (capacidade 1024)                        |
| `0x10` | MAILBOX     | RW     | Valor livre Quacker → Tuffy                                       |
| `0x14` | IRQ         | W      | Quacker escreve qualquer valor: levanta PIC bit 2 no Tuffy        |

## QVE — Quacker Vector Engine (`0x1F00_1000`)

Floats IEEE 754 de 32 bits. Os vetores são 4 floats consecutivos.

| Offset  | Nome        | Descrição                                                   |
| ------- | ----------- | ----------------------------------------------------------- |
| `0x000` | MATRIX[16]  | Matriz 4×4, linha principal (`out = M × v`)                 |
| `0x040` | VIEWPORT[4] | escala X, escala Y, offset X, offset Y                      |
| `0x050` | LIGHT[4]    | direção de luz (x, y, z, ambiente)                          |
| `0x060` | SRC         | Endereço de origem                                          |
| `0x064` | DST         | Endereço de destino                                         |
| `0x068` | COUNT       | Número de vetores                                           |
| `0x06C` | SRC_STRIDE  | Bytes entre vetores de origem (0 = 16)                      |
| `0x070` | DST_STRIDE  | Bytes entre vetores de destino (0 = 16)                     |
| `0x074` | OP          | Escrita inicia a operação                                   |
| `0x078` | STATUS      | bit 0 BUSY                                                  |

Operações (`OP`):

| Valor | Nome         | Saída por vetor                                               |
| ----- | ------------ | ------------------------------------------------------------- |
| 1     | XFORM        | `M × v` (x, y, z, w)                                          |
| 2     | XFORM_PROJ   | `sx, sy, z/w, 1/w` em coordenadas de tela                     |
| 3     | LIGHT        | `max(n·L, 0) + ambiente` em x; y, z, w copiados               |

Custo: 8 ciclos do Quacker por vetor (2 MACs FP32 por ciclo).

## Spike (`0x1F01_0000`)

| Offset | Nome     | Acesso | Descrição                                  |
| ------ | -------- | ------ | ------------------------------------------ |
| `0x00` | CMD      | W      | Push de uma palavra de comando             |
| `0x04` | STATUS   | R      | bit 0 BUSY                                 |
| `0x08` | SYNC     | R      | Último token de SYNC executado             |
| `0x0C` | PIXELS   | R      | Contador de pixels escritos                |

Cada comando começa com um cabeçalho `(opcode << 24) | número_de_palavras_de_dados`.

| Opcode | Nome        | Dados                                                                    |
| ------ | ----------- | ------------------------------------------------------------------------ |
| `0x00` | NOP         | —                                                                        |
| `0x01` | SET_TARGET  | cor, stride (bytes), largura, altura, endereço Z (stride = largura × 4) |
| `0x02` | CLEAR       | flags (bit0 cor, bit1 Z), cor RGB565, valor Z (24 bits)                 |
| `0x03` | SET_MODE    | flags de modo (ver abaixo)                                               |
| `0x04` | SET_TEXTURE | endereço, log2 largura, log2 altura, [número de níveis de mipmap]        |
| `0x05` | TRI         | 3 vértices × 7 palavras: x, y, z, 1/w, ARGB8888, u, v                   |
| `0x06` | SYNC        | token → registrador SYNC, levanta PIC bit 1                              |

Flags de `SET_MODE`: bit 0 Z test (LESS), bit 1 Z write, bit 2 Gouraud, bit 3 textura,
bit 4 bilinear, bit 5 perspectiva, bit 6 alpha blend, bit 7 culling de faces traseiras,
bit 8 mipmapping, bit 9 trilinear (mistura os dois níveis mais próximos), bit 10 blend
aditivo (`destino + fonte × alfa`, saturado; para brilhos e texto).

Mipmaps: os níveis ficam em sequência a partir do endereço da textura, cada um com metade
da largura e da altura do anterior (mínimo 1×1). O nível é escolhido por pixel a partir da
área da textura coberta por um passo de pixel em X e em Y.

Coordenadas de tela com Y para baixo. Faces da frente são as que aparecem em sentido
anti-horário na tela (a convenção do OpenGL depois do viewport).

Formato de framebuffer e textura: RGB565. Z: 24 bits em palavras de 32 bits (S8 nos 8 bits altos).

## VI — vídeo (`0x1F03_0000`)

| Offset | Nome    | Descrição                                                      |
| ------ | ------- | -------------------------------------------------------------- |
| `0x00` | MODE    | 0 desligado, 1 240p (320×240), 2 480i (640×480), 3 480p        |
| `0x04` | FB_ADDR | Endereço do framebuffer RGB565                                 |
| `0x08` | STRIDE  | Bytes por linha                                                |
| `0x0C` | LINE    | Linha atual (somente leitura)                                  |
| `0x10` | FRAME   | Contador de frames (somente leitura)                           |

O VBlank começa na linha 480 (ou 240 em 240p) e levanta PIC bit 0.

## PIC — interrupções (`0x1F04_0000`)

| Offset | Nome    | Descrição                                       |
| ------ | ------- | ----------------------------------------------- |
| `0x00` | PENDING | Leitura: pendentes. Escrita: 1 limpa o bit      |
| `0x04` | MASK    | 1 = habilitada                                  |
| `0x08` | STRAPS  | Somente leitura: jumpers da placa. Bit 0 = unidade de desenvolvimento (boot rápido) |

Fontes: bit 0 VBLANK, bit 1 SPIKE, bit 2 QUACKER, bit 3 TYKE, bit 4 PAD, bit 5 CD.
`PENDING & MASK ≠ 0` aciona a exceção externa (`0x500`) do Tuffy quando `MSR[EE] = 1`.

## Controles (`0x1F05_0000`)

Porta *n* em `0x10 × n`:

| Offset | Nome      | Descrição                                                    |
| ------ | --------- | ------------------------------------------------------------ |
| `0x00` | BUTTONS   | Máscara de botões                                            |
| `0x04` | STICKS    | `LX << 24 \| LY << 16 \| RX << 8 \| RY` (0x80 = centro)        |
| `0x08` | CONNECTED | 1 se conectado                                               |

Botões: 0 Up, 1 Down, 2 Left, 3 Right, 4 A, 5 B, 6 C, 7 X, 8 Y, 9 Z, 10 L, 11 R, 12 Start, 13 Select.

## Tyke — DSP de áudio (`0x1F02_0000`)

O Tyke é um DSP de 40 MHz com RAM de dados e de programa próprias. Ele lê amostras da SDRAM
pelo barramento e entrega pares estéreo ao conversor a 44,1 kHz. A Boot ROM carrega nele um
firmware de mixagem de 24 canais; um jogo pode substituí-lo por outro programa.

| Offset (no Tyke)  | Conteúdo                                                        |
| ----------------- | --------------------------------------------------------------- |
| `0x0000`–`0x7FFF` | RAM de dados: 8192 palavras de 32 bits, compartilhada com o host |
| `0x8000`          | CTRL: bit 0 RUN (0→1 reinicia o DSP no endereço 0)              |
| `0x8004`          | PC (somente leitura)                                            |
| `0x8008`          | Contador de amostras emitidas (somente leitura)                 |
| `0xC000`–`0xFFFF` | RAM de programa: 4096 instruções de 32 bits                     |

### Conjunto de instruções

16 registradores de 32 bits (`r0` vale sempre 0; `r15` guarda o retorno de `CALL`) e dois
acumuladores de 64 bits (`a0`, `a1`). Endereços de programa e de dados são em palavras.
Formato: `[31:26] op`, `[25:22] rd`, `[21:18] ra`, `[17:14] rb`, `[13:0] imm14` (com sinal);
`MOVI` usa `[17:0]` e `BR`/`CALL` usam `[25:0]` como deslocamento em palavras.

| Op     | Instrução             | Efeito                                                      |
| ------ | --------------------- | ----------------------------------------------------------- |
| `0x00` | NOP                   |                                                             |
| `0x01` | MOVI rd, imm18        | rd = imm (com sinal)                                        |
| `0x02` | MOVHI rd, imm16       | rd = (rd & 0xFFFF) \| imm << 16                              |
| `0x03`–`0x0B` | ADD SUB AND OR XOR SHL SHR SAR MUL rd, ra, rb | operações de 32 bits (MUL: produto baixo com sinal) |
| `0x0C`–`0x10` | ADDI SHLI SHRI SARI ANDI rd, ra, imm | ANDI usa imm sem sinal                      |
| `0x11` | LD rd, imm(ra)        | rd = RAM de dados[ra + imm]                                 |
| `0x12` | ST rd, imm(ra)        | RAM de dados[ra + imm] = rd                                 |
| `0x13` | LDXH rd, ra, rb       | rd = amostra de 16 bits com sinal em ra + 2·rb (barramento)  |
| `0x14` | LDX rd, ra, imm       | rd = palavra de 32 bits em ra + imm (barramento)             |
| `0x15` | CLR a                 | acumulador = 0                                              |
| `0x16` | MAC a, ra, rb         | a += ra × rb (com sinal)                                    |
| `0x17` | MACS ra, rb           | a0 += ra × rb[31:16], a1 += ra × rb[15:0] (estéreo)          |
| `0x18` | MVA rd, a, n          | rd = saturação de 32 bits de (a >> n)                        |
| `0x19` | SAT16 rd, ra          | rd = ra limitado a −32768…32767                             |
| `0x1A` | BR desl26             | desvio relativo                                             |
| `0x1B` | CALL desl26           | r15 = retorno; desvio                                       |
| `0x1C` | RET                   | pc = r15                                                    |
| `0x1D`–`0x22` | BEQ BNE BLT BGE BLTU BGEU ra, rb, desl14 | desvios condicionais                     |
| `0x23` | OUT porta, ra         | porta 0: esquerda; 1: direita (emite o par); 2: IRQ no PIC  |
| `0x24` | IN rd, porta          | porta 0: contador de amostras                               |
| `0x25` | WAIT                  | espera o próximo período de 44,1 kHz (≈907 ciclos)          |
| `0x26` | HALT                  | para o DSP                                                  |

Cada instrução leva 1 ciclo; `LDX`/`LDXH` levam +2 ciclos, mais a espera pelo barramento.

### Firmware de mixagem (padrão)

O firmware lê seus registradores na RAM de dados. Para o host, eles ficam nos mesmos
endereços da interface anterior:

| Endereço do host       | Nome       | Descrição                                        |
| ---------------------- | ---------- | ------------------------------------------------ |
| `+0x000`               | MASTER_VOL | 0–0x100 (o firmware inicia com 0x100)            |
| `+0x100 + 0x20 × ch`   | START      | Endereço das amostras PCM16 big-endian           |
| `+0x104 + 0x20 × ch`   | LENGTH     | Número de amostras                               |
| `+0x108 + 0x20 × ch`   | LOOP_START | Amostra de retorno do loop                       |
| `+0x10C + 0x20 × ch`   | PITCH      | Passo 16.16 por amostra de saída (0x10000 = 44,1 kHz) |
| `+0x110 + 0x20 × ch`   | VOLUME     | `L << 16 \| R`, 0–0x100 cada                      |
| `+0x114 + 0x20 × ch`   | CTRL       | bit 0 KEY_ON, bit 1 LOOP, bit 8 confirmação do firmware (toda escrita do host a limpa, então escrever KEY_ON sempre reinicia a amostra) |
| `+0x118 + 0x20 × ch`   | POSITION   | Amostra atual (escrita pelo firmware)            |

Com os 24 canais ativos, o firmware ocupa cerca de 82% do DSP; a folga cobre as esperas pelo
barramento quando o Tuffy disputa a SDRAM.


## CD-ROM (`0x1F06_0000`)

| Offset | Nome     | Acesso | Descrição                                               |
| ------ | -------- | ------ | ------------------------------------------------------- |
| `0x00` | STATUS   | R      | bit 0 disco presente, 1 ocupado, 2 erro, 3 concluído    |
| `0x04` | COMMAND  | W      | 1 READ, 2 STOP, 3 ACK (limpa concluído/erro)            |
| `0x08` | LBA      | RW     | Primeiro setor (2048 bytes)                             |
| `0x0C` | COUNT    | RW     | Número de setores                                       |
| `0x10` | DMA_ADDR | RW     | Destino na memória                                      |
| `0x14` | CAPACITY | R      | Setores no disco                                        |
| `0x18` | DONE     | R      | Setores já transferidos                                 |

`READ` busca o setor (80 ms + até 150 ms, proporcional à distância num disco de 74 minutos)
e transfere a 12× (≈1,1 ms por setor), copiando cada setor por DMA assim que chega. Ao
terminar, ativa `DONE` e o PIC bit 5. Pedidos fora do disco ou sem disco ativam `ERROR`.
Imagens de disco são arquivos ISO9660 com setores de 2048 bytes.

## Formato `.lion`

Cabeçalho big-endian:

| Offset | Campo         |
| ------ | ------------- |
| `0x00` | `"LION"`      |
| `0x04` | versão (1)    |
| `0x08` | entry do Tuffy |
| `0x0C` | nº de segmentos |
| `0x10` | segmentos: `{endereço de carga, tamanho, offset no arquivo}` × N |

## Boot ROM e dashboard

A ROM é código PowerPC de verdade (gerado por `src/bootrom.cpp`, com fonte, malhas, samples e
tabelas de animação de `src/rom/assets.cpp`), executado a partir do reset em `0xFFF0_0100`
com `MSR[IP] = 1`:

1. carrega o firmware de mixagem na RAM de programa do Tyke, liga o DSP e copia a fonte para
   a SGRAM;
2. **intro** (4 s): o emblema do Lion (três barras dentro de um anel com uma juba de raios)
   entra girando sobre um brilho vermelho, com flash e o jingle de abertura: rugido, acorde
   de sinos em Lá maior e pad. Start ou A pulam para o fim;
3. com um disco no drive, dá boot nele; sem disco, abre o **dashboard**;
4. o loader lê o descritor de volume (setor 16) e o diretório raiz do ISO9660, procura
   `BOOT.LION;1`, confere o cabeçalho, copia os segmentos, silencia o Tyke, mascara as
   interrupções e entra no jogo em `entry` com `MSR = ME | FP | VEC` e `r1` no topo da SDRAM.

O dashboard roda a 60 fps em 480p com buffer duplo:
- **Cena:** céu em degradê, grade de chão rolando em direção à câmera (blend aditivo) e o
  emblema girando no cabeçalho.
- **Menu:** três cards com ícones 3D (DISC, AUDIO e SYSTEM). O card escolhido cresce,
  balança e ganha um brilho pulsante.
- **Desenho:** tudo sai da Spike por uma FIFO que o Tuffy preenche. O QVE projeta as malhas,
  com sombreamento plano por face calculado no Tuffy. Entre frames o Tuffy dorme
  (`MSR[POW]`) e acorda com as interrupções de VBlank e da Spike.

| Item   | A / Start                                                                 |
| ------ | ------------------------------------------------------------------------- |
| DISC   | dá boot no disco (fade e tela LOADING com o rótulo do volume)            |
| AUDIO  | teste de som: toca o jingle e mostra as 24 vozes do Tyke                  |
| SYSTEM | painel com os chips, a memória e a versão da ROM                          |

← e → escolhem; B fecha os painéis. Os efeitos de navegação usam as vozes 21–23 do mixer.
O drive é acompanhado sem bloquear: ao inserir um disco, o dashboard lê o rótulo do volume
(`READING DISC...`, depois `PLAY DISC` e o rótulo). Se o disco não tem `BOOT.LION` válido,
a ROM volta ao dashboard com `THIS DISC CANNOT BE PLAYED`. Uma exceção inesperada mostra uma
tela vermelha.

Estado em SDRAM (`0x00A0_0000`, ver `bootrom.h`): cena, estado do disco, item escolhido,
painel aberto, rótulo do volume.

Com `PIC.STRAPS` bit 0 (unidade de desenvolvimento, opção `--fast-boot`), não há intro: com
disco, o boot é imediato; sem disco, o dashboard abre direto.

**Boot de desenvolvimento** (imagens `.lion` sem disco): o emulador copia os segmentos direto
na memória e carrega o firmware do Tyke, como um kit de desenvolvimento. O Tuffy começa em `entry` com
`MSR = ME | FP | VEC` (FPU e AltiVec habilitados, interrupções desligadas, vetores de
exceção em `0x0000_0xxx`) e `r1` apontando para o topo da SDRAM.
O Quacker fica em reset até o programa escrever `START_PC` e `CTRL.RUN`.

## Tuffy — MMU

- 4 pares IBAT e 4 pares DBAT (SPRs 528–543), 16 registradores de segmento e tabela de
  páginas com hash descrita por `SDR1`, como no MPC7400. Os bits R e C das PTEs são atualizados.
- `MSR[IR]`/`MSR[DR]` ligam a tradução. Falhas geram ISI (`0x400`) ou DSI (`0x300`) com
  `DAR`/`DSISR` (sem tradução, proteção, store) preenchidos.
- Uma TLB em software (1024 entradas por tipo de acesso) é esvaziada por `tlbie`, `tlbia`,
  `mtsr(in)`, escrita em `SDR1`/BATs e mudança de `MSR[PR]`.
- Segmentos de E/S direta (`T = 1`) geram DSI.

## Quacker — extensões do MC88110

As codificações seguem a tabela de opcodes do GNU binutils (`m88k-opcode.h`, entradas
`_MC88110`); o teste `m88k_encodings_match_binutils` confere cerca de 90 delas.

- **Registradores estendidos `x0–x31`** (80 bits; `x0` vale sempre +0,0). Loads e stores com
  `ld/st xD` (4 bytes), `.d` (8) e `.x` (16 bytes: sinal e expoente nos 16 bits altos da
  palavra 0, mantissa de 64 bits nas palavras 1–2). Operações de FP nos formatos x usam
  TD[10:9] T1[8:7] T2[6:5]; `fcmp`/`fcmpu`, `int`/`nint`/`trnc`, `fsqrt`, `flt` e `mov`
  entre bancos seguem os campos da tabela do binutils.
- **`muls`**, **`mulu.d`** e **`divu.d`** (resultado ou dividendo de 64 bits num par de registradores).
- **SFU2 gráfica** (opcode `0x22`), conforme a seção 5 e as páginas de instruções da seção 10
  do *MC88110 User's Manual* (1991). Os operandos de 64 bits são pares par/ímpar de registradores:
  - `padd`/`psub` `.b/.h/(32)`: soma/subtração por campo, módulo 2ᵗ;
    `padds`/`psubs` `.u/.us/.s`: saturação sem sinal, mista (S1 sem sinal, S2 com sinal,
    resultado sem sinal) ou com sinal (S[8:7] = 01, 10, 11). Confere com a tabela 5-2 do manual.
  - `pcmp`: compara sem sinal os dois campos de 32 bits; bits 4–7 combinam ≥/< das duas
    metades e bits 8–11 são os complementos.
  - `pmul`: par de 64 bits × registrador de 32 bits, 64 bits baixos (como nas figuras da seção 5;
    a página de referência da seção 10 descreve os operandos ao contrário).
  - `ppack.r.t`: pega os (t·r)/64 bits altos de cada campo de t bits de S2, concatena num campo
    de r bits e o encaixa abaixo de S1 deslocado r bits (R[10:7] = r/4).
  - `punpk.n/.b/.h`: expande os campos de S1 para o dobro da largura, alinhados à direita.
  - `prot`: rotação do par para a esquerda em múltiplos de 4 bits (imediato em R[10:7], ou
    bits 5–2 de S2).
  - Campos inválidos, pares ímpares ou `PSR[4]` (SFU2 desabilitada) geram a exceção 116.
- **Vetores de exceção** conforme a tabela 7-1 do manual: 5 opcode não implementado,
  6 violação de privilégio, 7 limites (`tbnd`), 8 divisão por zero, 9 overflow inteiro,
  114 SFU1, 116 SFU2; vetores de trap 0–127 em modo usuário geram violação de privilégio.

## Estado da emulação

- **Tuffy:** ISA PowerPC 32-bit completa (inteiros, FPU, AltiVec do MPC7400), exceções,
  MSR[POW], MMU com BATs e tabela de páginas, despacho duplo, previsão de desvio, caches L1/L2.
  Como extensão da Atari, executa `fsqrt`/`fsqrts`; a opção `--strict-7400` as torna ilegais,
  como no 7400 comercial.
- **Quacker:** ISA 88100/88110 com delay slots, registradores x, SFU1 e SFU2 conforme o
  manual, despacho duplo, TIC, caches.
- **Spike:** rasterizador com Z24, Gouraud, textura bilinear com perspectiva, mipmapping e
  filtragem trilinear, blend e culling. No emulador, triângulos grandes são divididos em faixas
  de linhas entre várias threads, com o mesmo resultado de uma thread só.
- **Tyke:** DSP programável com firmware de mixagem de 24 canais. O relógio do DSP é absoluto:
  esperas pelo barramento são compensadas nas fatias seguintes. No árbitro da SDRAM o Tyke tem
  prioridade de tempo real: espera no máximo a transferência em andamento e passa à frente da fila.
- **CD-ROM:** drive 12× com busca, DMA e interrupção.
- **Boot ROM:** intro, dashboard e loader em código PowerPC.
- **Barramento:** disputa pela SDRAM entre os mestres, com prioridade de tempo real para o Tyke.
