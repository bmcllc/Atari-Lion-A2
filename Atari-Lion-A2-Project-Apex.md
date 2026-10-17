# ATARI LION — Project Apex (1999)

**Hardware Revision A2 • Tuffy + Quacker • Sexta geração**

A revisão A2 transforma o Atari Lion em um console com dois processadores personalizados pela Atari, preservando o Spike, o Tyke, a memória e todos os demais componentes da arquitetura anterior.

A mudança é estratégica: substituir o PowerPC 604e e o Motorola MC88110 comerciais por derivados próprios, com menos lógica desnecessária para jogos, comunicação otimizada e maior capacidade de processamento especializado.

| | CPU principal | Processador geométrico |
| --- | --- | --- |
| Nome | **ATARI TUFFY** | **ATARI QUACKER** |
| Base | Custom PowerPC 7400 (G4) | Custom Motorola MC88110 |
| Clock | **500 MHz** | **100 MHz** |

> Frequências propostas para o silício personalizado, ainda não validadas por simulação física.

A escolha do 7400 faz sentido historicamente: a Motorola já divulgava variantes embarcadas do 7400 em 1998, com foco em menor custo e consumo. *(Fonte citada: cpushack.com)*

Um Tuffy de 500 MHz em 1999, porém, é uma meta **agressiva**: o 7400 comercial teve dificuldade para chegar a 500 MHz e essa faixa só ficou disponível em volume por volta de 2000. Por isso o Tuffy usa um processo mais fino que os demais chips (0,20 µm) e prevê versões de engenharia em frequências inferiores.

### Nomenclatura

Os chips do Jaguar se chamavam Tom, Jerry e Butch, personagens de *Tom & Jerry*. O Lion mantém a tradição: **Spike** e **Tyke** (os buldogues), **Tuffy** (o ratinho) e **Quacker** (o patinho).

---

## 1. Especificações definitivas do Lion A2

| Componente      | Atari Lion A2 — lançamento 1999           |
| --------------- | ----------------------------------------- |
| CPU principal   | Atari Tuffy, PPC 7400 (G4) custom @ 500 MHz |
| Geometria       | Atari Quacker, MC88110 custom @ 100 MHz   |
| GPU             | Spike ASIC @ 90 MHz                       |
| Rasterização    | 2 pipelines; 180 Mpixels/s teóricos       |
| Gráficos        | Bilinear, mipmap, perspectiva, Z24/S8     |
| Áudio           | Tyke DSP @ 40 MHz; 24 canais              |
| RAM principal   | 24 MB SDRAM unificada                     |
| RAM de texturas | 8 MB SGRAM dedicada                       |
| Memória total   | 32 MB (24 + 8)                            |
| Barramento      | 64 bits @ 100 MHz (800 MB/s teóricos), protocolo MPX |
| Cache da CPU    | 1 MB L2 backside @ 250 MHz, de série      |
| Mídia           | CD-ROM 12×                                |
| Vídeo           | 240p, 480i e 480p                         |
| Controles       | 4 portas nativas, analógico + digital     |

---

## 2. Atari Tuffy — Custom PowerPC 7400 (G4)

*Codinome: Tuffy • CPU de propósito geral • Arquitetura PowerPC 32-bit*

O Tuffy não é apenas um 7400 com clock diferente. É uma variante G4 otimizada para consoles, mantendo compatibilidade total com o conjunto de instruções do 7400, incluindo o AltiVec.

| Característica         | Tuffy                                          |
| ---------------------- | ---------------------------------------------- |
| Núcleo                 | PowerPC 7400 modificado                        |
| Frequência             | 500 MHz (multiplicador 5× sobre o bus de 100 MHz) |
| Processo de fabricação | CMOS 200 nm (0,20 µm)                          |
| Cache L1               | 32 KB instruções + 32 KB dados, 8 vias         |
| Cache L2               | 1 MB backside, SRAM dedicada @ 250 MHz, de série |
| Interface externa      | MPX (60x estendido), adaptada ao Lion Fabric   |
| FPU                    | IEEE 754, 32/64 bits                           |
| AltiVec                | Sim, 128 bits (4 × FP32 por instrução)         |
| DMA                    | Acesso a filas do controlador Lion             |
| Endianness             | Big-endian nativo                              |
| ISA                    | PowerPC 7400 completo (PowerPC 32-bit + AltiVec) |

### Personalizações do Tuffy

- **Tuffy Fast Dispatch** — comunicação por registradores e filas de comandos com Quacker, Spike e Tyke. Permite iniciar operações sem que o processador principal fique aguardando sua conclusão.
- **Tuffy Streaming Assist** — prefetch e gerenciamento otimizado de buffers para CD-ROM, descompressão por software e carregamento de recursos.
- **Tuffy Interrupt Controller** — priorização de interrupções para sincronização vertical, entrada de controles, áudio e DMA. O controlador externo trata os eventos antes de interromper a CPU.
- **Tuffy Cache Control** — rotinas padronizadas de limpeza e invalidação do cache em transferências compartilhadas, preservando a compatibilidade PowerPC.

### Tuffy × 604e

O núcleo do 7400 descende do 750 (G3), que por MHz já igualava ou superava o 604e na maioria das cargas inteiras, e o 7400 trouxe uma FPU mais forte que a do G3. Com 500 MHz contra 200 MHz, o Tuffy fica bem acima de 2× a CPU anterior em código escalar, e muito mais em código vetorizado com AltiVec.

### Subsistema de memória do Tuffy

Um núcleo de 500 MHz sobre um barramento de 100 MHz espera 5 ciclos de CPU por ciclo de bus, e a SDRAM de 800 MB/s é dividida com o Quacker, o Tyke e o DMA do CD. Sem cache grande, o Tuffy passaria boa parte do tempo parado. A A2 resolve isso **sem mudar a RAM nem o barramento**:

- **L1 completo de 32 + 32 KB**, como no 7400 original.
- **1 MB de L2 backside de série**, em SRAM dedicada a 250 MHz (metade do clock do núcleo). O L2 fica fora do barramento compartilhado, então os acertos de cache não consomem os 800 MB/s.
- **Protocolo MPX** no lugar do 60x: transações pipelinadas e fora de ordem, que aproveitam melhor o mesmo bus de 100 MHz.
- **Prefetch por streams do AltiVec** (`dst`), usado pelo Tuffy Streaming Assist para trazer dados antes do uso.
- **Arbitragem com prioridade**: Spike e Tyke têm prioridade de tempo real; o Tuffy recebe a banda restante em rajadas.

Os 800 MB/s continuam sendo o limite teórico da SDRAM; o L2 apenas faz com que o Tuffy precise dela com bem menos frequência.

---

## 3. Atari Quacker — Custom Motorola MC88110

*Codinome: Quacker • Processador geométrico • Arquitetura Motorola 88000*

Aqui está a principal inovação do Lion.

O MC88110 original já possuía uma unidade especializada em instruções gráficas (parte da SFU2), com operações de manipulação de pixels e aritmética saturada. Portanto, o Quacker parte de uma base que efetivamente tinha recursos gráficos em hardware. *(Fonte citada: praxibetel.org)*

| Característica    | Quacker                                |
| ----------------- | -------------------------------------- |
| Núcleo            | MC88110 customizado                    |
| Frequência        | 100 MHz                                |
| Processo          | CMOS 250 nm                            |
| ISA               | Motorola 88000 + extensão própria      |
| Cache L1          | 8 KB instruções + 8 KB dados           |
| Memória local     | 16 KB SRAM para trabalho geométrico    |
| FPU               | Compatível com MC88110                 |
| Unidade adicional | Quacker Vector Engine (QVE)            |
| Integração        | Command FIFO + DMA                     |
| Função            | Transformação, iluminação e geometria  |

### Quacker Vector Engine (QVE)

Unidade nova, controlada pelo MC88110, sem substituir a FPU original.

```
┌──────────────────────────────────────────┐
│ MC88110 @ 100 MHz                        │
│ Execução de código Motorola 88000        │
│  ┌────────────────────────────────────┐  │
│  │ Quacker Vector Engine              │  │
│  │  ┌──────────────┐ ┌──────────────┐ │  │
│  │  │ MAC FP32 A   │ │ MAC FP32 B   │ │  │
│  │  │ Multiply+Add │ │ Multiply+Add │ │  │
│  │  └──────────────┘ └──────────────┘ │  │
│  │  2 operações FP32 MAC por ciclo    │  │
│  └────────────────────────────────────┘  │
└──────────────────────────────────────────┘
                    │
                    ▼
 Triângulos e atributos enviados ao Spike por DMA
```

Com duas unidades capazes de completar uma multiplicação e soma por ciclo, o QVE tem um pico de **400 MFLOPS** (2 MAC × 2 FLOPs × 100 MHz), contando multiplicação e adição separadamente. É uma meta teórica da unidade vetorial, não o desempenho sustentado do Quacker inteiro.

Para referência: uma transformação de vértice por matriz 4×4 custa 16 MACs, ou seja, 8 ciclos no QVE. No pico, isso daria ~12,5 milhões de vértices transformados por segundo, antes de iluminação, clipping e gargalos de memória.

O QVE acelera matrizes 4×4, transformações de vértices, cálculos de iluminação, interpolação, animação esquelética e operações com vetores de três ou quatro componentes.

A lógica vetorial é acessada por comandos e registradores específicos, preservando a execução de programas MC88110 convencionais. A divisão de trabalho é clara: o QVE cuida da geometria por vértice, e o AltiVec do Tuffy fica livre para física, animação, áudio procedural e descompressão.

---

## 4. Comunicação Tuffy + Quacker + Spike

A chave é executar lógica, geometria e renderização simultaneamente, sem que um processador tenha de esperar constantemente pelo outro.

```
Tuffy — 500 MHz
  IA, física, gameplay, comandos de cena
        │  Command FIFO / DMA
        ▼
Quacker — 100 MHz
  T&L, matrizes, clipping e vértices
        │  Vertex FIFO / DMA
        ▼
Spike — 90 MHz
  Rasterização, texturas, Z-buffer
        │
        ▼
Framebuffer / Saída de vídeo
```

Pipeline lógico. Os buffers compartilhados exigem sincronização explícita; o trabalho dos estágios pode ser sobreposto entre frames.

A arquitetura continua usando os mesmos 24 MB de SDRAM, 8 MB de SGRAM e barramento de 64 bits a 100 MHz da revisão anterior. As FIFOs e a SRAM local diminuem parte do tráfego externo, mas não aumentam os 800 MB/s teóricos da SDRAM.

---

## 5. Como baratear a fabricação em 1999?

Estratégia de licenciamento e fabricação dedicada, em parceria com a Motorola e uma foundry.

A tecnologia de 250 nm é coerente com esse cenário para o Quacker: a TSMC já fabricava lógica CMOS de 0,25 µm em volume em 1998. *(Fonte citada: investor.tsmc.com)* O Tuffy, por causa dos 500 MHz, vai para 0,20 µm, o processo mais caro do sistema.

| Decisão                     | Benefício pretendido                                   |
| --------------------------- | ------------------------------------------------------ |
| Tuffy baseado no 7400       | Núcleo menor e menor dissipação que a família 604e     |
| L2 de 1 MB de série         | Evita aumentar o barramento ou trocar a RAM            |
| Quacker derivado do 88110   | Reaproveitar a arquitetura e as ferramentas M88k       |
| Processo de 250 nm (Quacker) e 200 nm (Tuffy) | Maior densidade; 200 nm só onde o clock exige |
| ISA preservada              | Reduzir mudanças em compiladores                       |
| DMA compartilhado           | Evitar lógica de transferência redundante              |
| Fabricação em grande volume | Amortizar máscaras e custos de desenvolvimento         |

Condição importante: personalizar dois processadores exige acesso ao projeto dos núcleos e licenciamento apropriado. Não é simplesmente modificar chips comerciais acabados. O Quacker, especialmente, demanda uma migração substancial de tecnologia e validação de temporização, já que a Motorola havia descontinuado a linha 88000 em favor do PowerPC.

O custo por console pode cair em relação a duas CPUs comerciais, mas o investimento inicial é elevado. A economia depende dos volumes de produção, rendimentos de fabricação e royalties negociados.

---

## 6. Resultado da revisão

| Aspecto                   | Lion anterior                      | Lion A2                            |
| ------------------------- | ---------------------------------- | ---------------------------------- |
| CPU                       | 604e 200 MHz                       | Tuffy G4 500 MHz                   |
| Geometria                 | MC88110 50 MHz                     | Quacker 100 MHz + QVE              |
| Compatibilidade PPC       | PowerPC 604e                       | PowerPC 7400 completo, com AltiVec |
| Cache L2 da CPU           | —                                  | 1 MB backside de série             |
| Unidade vetorial dedicada | Não                                | QVE (400 MFLOPS) + AltiVec no Tuffy |
| Controle de produção      | CPUs comerciais                    | Núcleos personalizados licenciados |
| GPU / Áudio               | Spike / Tyke                       | Sem alterações                     |
| Memória                   | 24 MB SDRAM + 8 MB SGRAM           | Sem alterações                     |
| Mídia                     | CD-ROM 12×                         | Sem alterações                     |

Tuffy a 500 MHz e Quacker a 100 MHz são as metas de lançamento, com versões de engenharia em frequências inferiores caso a validação do silício exija. Os clocks do Spike e do Tyke não mudam.

Assim, o Atari Lion preserva a proposta da Atari de utilizar múltiplos processadores especializados, mas com uma organização muito mais adequada ao desenvolvimento de jogos 3D de sexta geração.

O principal ganho é a combinação do Tuffy como CPU de controle com o Quacker como processador programável de geometria, deixando o Spike inteiramente dedicado à rasterização. Isso torna o Lion tecnicamente mais interessante sem exigir alterações no restante do hardware.
