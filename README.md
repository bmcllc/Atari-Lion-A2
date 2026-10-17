# Atari Lion A2 — emulador

Emulador de baixo nível do console fictício **Atari Lion A2 (Project Apex, 1999)**.
Especificação: [Atari-Lion-A2-Project-Apex.md](Atari-Lion-A2-Project-Apex.md) •
Referência de hardware do emulador: [docs/hardware.md](docs/hardware.md).

| Chip    | Emulação                                                              |
| ------- | --------------------------------------------------------------------- |
| Tuffy   | PowerPC 7400 @ 500 MHz: inteiros, FPU, AltiVec completo, MMU (BAT + tabela de páginas), caches L1/L2, despacho duplo, previsão de desvio |
| Quacker | MC88110 @ 100 MHz: delay slots, registradores x0–x31, SFU1 e SFU2 conforme o manual, caches, despacho duplo, TIC |
| QVE     | Quacker Vector Engine: transformação, projeção e iluminação de vetores |
| Spike   | Rasterizador: Z24, Gouraud, textura com perspectiva, bilinear, mipmap e trilinear |
| Tyke    | DSP de 40 MHz com conjunto de instruções próprio; firmware de mixagem de 24 canais |
| CD-ROM  | Drive 12× com busca, DMA e interrupção                                |
| Boot ROM | Código PowerPC: intro, dashboard 3D com player de CD, temas sonoros, ajustes do Tyke e visualizadores, boot de `BOOT.LION` |
| Barramento | Disputa pela SDRAM entre Tuffy, Quacker, DMA do CD e Tyke          |
| VI/PIC/Controles | 240p/480i/480p, interrupções, 4 portas analógico + digital   |

![Intro: as garras rasgam a placa](docs/img/intro_tear.png) ![Intro: o Lion A2 coroado](docs/img/intro.png)
![Dashboard](docs/img/dashboard.png) ![Player de CD](docs/img/music.png)

## Dashboard

Ao ligar, o console toca a intro:
1. o leão ruge;
2. três garradas rasgam uma placa "JAGUAR";
3. surge o emblema do Lion A2, e uma coroa cai sobre a juba.

Depois abre o dashboard, no estilo dos consoles de sexta geração com a cara do Lion: grade
vermelha rolando até o horizonte e cinco cards 3D.

- **PLAY:** dá boot no jogo do disco.
- **MUSIC:** player de CD para faixas `.WAV` do disco, com espectro e osciloscópio.
- **THEME:** escolhe o tema sonoro do dashboard. Os dois primeiros, LION A2 THEME (o padrão) e
  LION A2 THEME II, são as músicas de `Assets/`. Os outros são sequenciados pela ROM: PRIDE,
  NEON SAVANNA e NIGHT PLAINS. Há também SILENCE.
- **TYKE:** volume, graves, agudos e eco do DSP de áudio, com os medidores das 24 vozes e os
  visualizadores.
- **SYSTEM:** informações do hardware.

Tudo roda na própria Boot ROM, em PowerPC, desenhando pela Spike e tocando pelo Tyke.

Controles e opções:
- ← → escolhem, ↑ ↓ navegam nos painéis, A (tecla Z) ou Enter confirmam, B (tecla X) volta;
- X (tecla A) para a música;
- arraste um ISO para a janela para inserir o disco; F9 ejeta;
- `--fast-boot` simula uma unidade de desenvolvimento: sem intro, o disco dá boot na hora.

O rugido, as garras e os dois temas gravados vêm de `Assets/`. Depois de trocar esses arquivos,
gere de novo a versão que vai para a ROM (requer ffmpeg):

```bash
cmake --build build --target lionasset
```

```bash
tools/convert_assets.sh
```

Para montar um disco de música:

```bash
./build/mkdemo --music build/musica.iso faixa1.wav faixa2.wav
```

Sem arquivos, o comando grava três faixas de demonstração. Os `.wav` precisam ser PCM de
16 bits, mono ou estéreo, de 8 a 48 kHz.

Detalhes em [docs/hardware.md](docs/hardware.md#boot-rom-e-dashboard).

## Compilar

Requisitos: CMake ≥ 3.20, compilador C++23 (GCC 14 ou Clang 17+), SDL2.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

## Rodar a demo

`mkdemo` monta a demo "Apex Cube" (Tuffy → Quacker/QVE → Spike). Como disco ISO, ela
carrega uma textura com mipmaps do CD e desenha o cubo texturizado:

```bash
./build/mkdemo build/apex_cube.iso
```

```bash
./build/lion build/apex_cube.iso
```

Com a extensão `.lion`, `mkdemo` gera uma imagem sem disco, e o cubo sai só com Gouraud.

Na demo, ← e → mudam a velocidade de rotação e Z (botão A) para o cubo.

Opções úteis:

- `--headless --frames N`: roda sem janela e mostra estatísticas
- `--dump frame.ppm`: salva o último frame (implica `--headless`)
- o `lion` aceita imagens `.lion` e discos ISO9660; sem argumento, liga o console no dashboard
- `--fast-boot`: pula a intro (unidade de desenvolvimento)
- `--strict-7400`: `fsqrt`/`fsqrts` viram instruções ilegais, como no 7400 comercial
- `--trace-tuffy`, `--trace-quacker`: rastreia cada instrução
- `--verbose`, `--scale N`

Teclado (porta 1): setas, Z/X/C = A/B/C, A/S/D = X/Y/Z, Q/W = L/R,
Enter = Start, Shift direito = Select. Controles USB ocupam as portas 1–4.

## Testes

```bash
./build/lion_tests
```

## Organização

```
src/            núcleo (biblioteca lioncore) e frontend SDL2 (main.cpp)
  tuffy/        CPU PowerPC 7400 (inteiros, FPU, AltiVec, MMU, temporização)
  quacker/      CPU MC88110, controle/Command FIFO e QVE
  cdrom.*       drive de CD-ROM e leitura de ISO9660
  cache.h       modelo de cache usado pelas duas CPUs
  spike/        processador de comandos e rasterizador
  tyke/         DSP Tyke e firmware de mixagem
  bootrom.cpp   Boot ROM: intro, dashboard e loader (montados em código PowerPC)
  rom/          intro, dashboard, player de CD e visualizadores (PowerPC gerado em C++),
                e os dados: fonte, textura, malhas 3D, samples, jingle, temas, animações
  dispatch.h    modelo de despacho duplo e previsão de desvio
tools/          montadores PPC/88k/Tyke, gravador de ISO9660, demo, discos de música, mkdemo,
                conversor de áudio para a ROM (lionasset, convert_assets.sh, IMA ADPCM)
Assets/         áudio original (intro e temas) e Assets/rom/, a versão convertida para a ROM
tests/          testes unitários e de sistema
docs/           referência de hardware
```

## Observações

- A SFU2 do Quacker segue o *MC88110 User's Manual* (Motorola, 1991). Para `pmul`, o manual
  descreve os operandos de jeito diferente na seção 5 e na seção 10; o emulador segue a seção 5.
- O modelo de ciclos é aproximado: despacho duplo em ordem, sem modelar renomeação, execução
  fora de ordem nem filas de reserva.
- Na disputa pelo barramento, cada mestre usa o próprio relógio; como o escalonador os executa
  em fatias de 1 µs, a ordem entre mestres dentro de uma fatia é aproximada.

## Port parcial do motor Unreal

`tools/unrport/` carrega fases do Unreal 1 (`.unr`, pacote v40+) no Lion: `unrport` lê os
polígonos dos brushes do BSP e `client.c` (C freestanding, clang para PowerPC) os transforma na FPU
do Tuffy e os desenha pela Spike, com Z-buffer e Gouraud. Texturas vêm de `.utx` (paleta de 256 cores e mipmaps, convertidas para RGB565; UVs calculados com
`TextureU/V` do polígono). Ainda não há atores, lightmaps nem UnrealScript; `--gain` compensa a falta de luz.

```bash
cmake --build build --target unrport
```

```bash
tools/unrport/build.sh Engine/Maps/Entry.unr build/unreal.iso \
  --wall Source/Unreal/Textures/MenuGr.utx:smlmet --floor Source/Unreal/Textures/MenuGr.utx:rmetal \
  --ceiling Source/Unreal/Textures/MenuGr.utx:smlmet --gain 6
```
