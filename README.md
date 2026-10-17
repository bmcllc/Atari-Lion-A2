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
| Boot ROM | Código PowerPC: carrega o firmware do Tyke e dá boot em `BOOT.LION` do disco |
| Barramento | Disputa pela SDRAM entre Tuffy, Quacker, DMA do CD e Tyke          |
| VI/PIC/Controles | 240p/480i/480p, interrupções, 4 portas analógico + digital   |

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
- o `lion` aceita imagens `.lion` e discos ISO9660; sem argumento, liga o console com o drive vazio
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
  bootrom.cpp    Boot ROM (montada em código PowerPC)
  dispatch.h    modelo de despacho duplo e previsão de desvio
tools/          montadores PPC/88k/Tyke, gravador de ISO9660, demo e mkdemo
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
