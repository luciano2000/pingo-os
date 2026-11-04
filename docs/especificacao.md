# Pingo OS: especificação

> Distro Linux minimalista para handhelds retrô.
> O nome vem de Pingo, o cachorro de infância do autor: curto, com cara de mascote, e representa algo pequeno.
> Documento-base para iniciar o desenvolvimento no Claude Code.
> Mockup das telas: https://claude.ai/artifact/Vz7H1VQirbdDkg7MoUSr6c

---

## 1. Visão

Uma distro Linux própria e mínima para consoles portáteis, distribuída como **uma imagem `.img`**. O usuário grava a imagem no cartão SD, liga o aparelho e tudo já funciona: imagem, som, botões, Wi-Fi, interface e emuladores.

A experiência de uso segue a filosofia do **MinUI**: minimalista, sem distrações e direta ao ponto. O emulador nunca "sai da interface" nem expõe os menus do RetroArch.

### Problema que resolve
O MinUI depende do firmware original de cada aparelho. Quando o cartão corrompe, é preciso achar o firmware original, gravá-lo, instalá-lo, copiar os arquivos do MinUI e subir tudo de novo. Aqui, a recuperação é gravar a `.img` e pronto.

### Princípios
- Simples de instalar e de recuperar: uma imagem, um passo.
- Interface mínima: sem boxart, sem temas, sem sons de navegação.
- Robusta contra corrupção do cartão.
- Escopo enxuto: só sistemas que rodam com perfeição.

---

## 2. Aparelhos-alvo

| Fase | Aparelho | SoC | Observação |
|---|---|---|---|
| 1 | **Anbernic RG35XX-H** | Allwinner H700 | Alvo inicial. Dois slots de SD, Wi-Fi. |
| 2 | Anbernic RG40XX (H/V) | Allwinner H700 | Mesmo SoC, idealmente a mesma imagem. |
| 3 | TrimUI Brick | Allwinner A133P | Suporte no kernel mainline mais fraco; provavelmente exige o kernel do fabricante. Um slot de SD. |
| — | RG35XX original | Actions ATM7039S | **Fora do escopo.** SoC fechado, preso ao kernel 3.10 do fabricante, sem Wi-Fi. Continua com o MinUI. |

---

## 3. Arquitetura do sistema

### Base
- **Buildroot** gera a imagem completa a partir do código-fonte. Não usar Debian, Ubuntu ou Arch.
- **Kernel mainline** com os patches e device trees que o **ROCKNIX** mantém para o H700 (tela, áudio, botões, Wi-Fi, bateria).
- GPU Mali-G31 com o driver aberto **Panfrost** (Mesa), via KMS/DRM.
- Sem X11 e sem Wayland: o frontend desenha direto via **SDL2 + KMS/DRM + GLES**.
- Init simples (BusyBox init ou similar). Boot rápido é requisito.

### Referências de mercado
- **Knulli** (fork do Batocera) usa Buildroot. É o modelo mais próximo deste projeto.
- **ROCKNIX** (fork do JELOS) usa o sistema de build do LibreELEC. É a fonte do kernel e dos device trees do H700.
- **muOS** aproveita o kernel e a base do firmware original.
- **MinUI** é a referência de interface e de frontend (launcher + minarch).

### Layout do cartão SD

| Partição | Conteúdo | Formato | Modo |
|---|---|---|---|
| 1 | Boot (U-Boot, kernel, device tree) | FAT32 | — |
| 2 | Sistema | **squashfs** | **somente leitura** |
| 3 | `ROMS`: jogos, BIOS, saves, configurações | **exFAT** | leitura e escrita, acessível pelo PC |

O sistema somente leitura é a principal defesa contra corrupção: ele nunca é escrito, então não estraga.

### Primeiro boot
1. A `.img` vem pequena, só com as partições 1 e 2.
2. No primeiro boot, uma tela simples mostra "Preparando cartão..." e pede para não desligar.
3. Um script detecta o espaço livre, cria a partição 3 no restante do cartão (`sfdisk`), formata em exFAT com o rótulo `ROMS` e cria a estrutura de pastas.
4. Em todo boot, rodar `fsck.exfat` na partição 3 antes de montar.

### Estrutura de pastas na partição ROMS
```
/Roms/
  GB/  GBC/  GBA/  NES/  SNES/  VB/  POKEMINI/
  MS/  GG/  MD/  SEGACD/  32X/
  PCE/  PCECD/  NGP/  WS/  MSX/
  A2600/  A7800/  LYNX/
  PS/
  ARCADE/
/Bios/
/Saves/      ← SRAM (sempre no SD1)
/States/     ← save states (sempre no SD1)
/Ports/      ← PortMaster
/Config/     ← configurações do sistema e de cada console
```
Os nomes finais das pastas ainda podem mudar, mas a estrutura deve ser idêntica no SD1 e no SD2.

---

## 4. Sistemas e emuladores

**Regra de escopo: só o que roda com perfeição no H700. Até PS1, mais arcade.**

| Sistema | Core libretro |
|---|---|
| Game Boy / Game Boy Color | gambatte |
| Game Boy Advance | mGBA |
| NES | FCEUmm (alternativa: Nestopia) |
| SNES | Snes9x |
| Virtual Boy | Beetle VB |
| Pokémon mini | PokeMini |
| Master System / Game Gear / Mega Drive / Sega CD | Genesis Plus GX |
| 32X | PicoDrive |
| PC Engine / PC Engine CD | Beetle PCE Fast |
| Neo Geo Pocket / Color | Beetle NeoPop |
| WonderSwan / Color | Beetle WonderSwan |
| MSX | blueMSX |
| Atari 2600 / 7800 / Lynx | Stella / ProSystem / Handy |
| PlayStation | PCSX ReARMed |
| Arcade | **FBNeo** (principal); MAME 2003-Plus só como complemento |

### Fora do escopo (decisão do projeto)
Nintendo DS, N64, Dreamcast, PSP e Saturn. Rodam mal ou só parcialmente nesse hardware.

### Arcade
- **FBNeo como core único**, com uma única versão de romset documentada. Versões de romset incompatíveis são a principal causa de "jogo não abre".
- **Nomes legíveis:** o launcher lê o arquivo DAT do FBNeo e mostra "Street Fighter II" em vez de `sf2.zip`.

### PlayStation
- Formatos aceitos: `.chd` (preferido), `.cue/.bin` e `.m3u` para jogos com vários discos.
- Jogos com vários discos aparecem como **um único item** na lista, com a indicação "N discos".

### PortMaster
- Integrar o PortMaster (ports nativos para Linux).
- O sistema precisa oferecer: bash, SDL2, GLES funcionando (Panfrost), `gptokeyb` para mapear botões e suporte aos runtimes em squashfs.
- Usar a integração do ROCKNIX e do Knulli como referência.
- Os ports rodam fora do frontend. Padronizar pelo menos um **atalho global de saída** (ex.: MENU + START) e o comportamento de salvar ao sair.

---

## 5. Frontend (launcher + emulação)

### Abordagem
Escrever um frontend próprio, inspirado no **MinUI** (launcher + `minarch`):
- Carrega os cores **libretro diretamente**, sem RetroArch.
- Desenha o próprio menu mínimo dentro do jogo.
- O usuário nunca vê menus de RetroArch nem de outro emulador.

**Ação inicial:** avaliar o código do MinUI (github.com/shauninman/MinUI) e **verificar a licença** antes de reaproveitar ou fazer fork de qualquer parte. Hoje o projeto aparece como "Legacy" no README, ou seja, sem desenvolvimento ativo.

### Tecnologia
- C (ou C++), SDL2, SDL2_ttf, KMS/DRM, GLES.
- Fonte renderizada como vetor, para escalar entre 640x480 (Anbernic) e 1024x768 (Brick).

---

## 6. Regras de origem dos jogos (SD1 / SD2)

Vale só para aparelhos com dois slots (RG35XX-H, RG40XX). Nos aparelhos com um slot, a opção fica oculta.

- **A origem é escolhida por console**, no MENU da lista de jogos: item **"Origem: SD1 / SD2"**.
- Ao mudar a origem, o launcher faz na hora o scan da pasta correspondente no outro cartão.
- A escolha é **salva por console** em `/Config` no SD1 e vale depois de reiniciar. O padrão de fábrica é SD1.
- **Não há indicação de SD2 na tela inicial nem na lista de jogos.** A informação fica só no MENU da lista.
- Se o SD2 não estiver presente e algum console apontar para ele: mostrar "SD2 não encontrado" e oferecer voltar para o SD1. **Nunca trocar sozinho.**
- **Saves e save states ficam sempre no SD1**, qualquer que seja a origem do jogo.
- **Recentes e favoritos** são salvos como `console + caminho relativo à pasta do console` (ex.: `MD/1 US - A-F/Jogo.md`), nunca como caminho absoluto. Assim funcionam com subpastas e nos dois cartões.
- Inserir ou remover o SD2 dispara um novo scan. Usar cache das listas (uma por pasta) para o boot e a navegação continuarem rápidos.
- A estrutura de subpastas de cada console vale igual nos dois cartões.
- Remover o cartão com um jogo rodando: pausar e avisar.

Caso de uso típico: um cartão de 128 GB com todos os consoles e arcade (SD1) e outro só com ISOs de PS1 e PC Engine CD (SD2).

---

## 7. Energia: suspend, resume e desligamento

### Toque no Power (suspender)
1. O frontend pausa o jogo.
2. Grava um save state e a SRAM no SD1, mais um marcador "retomar este jogo".
3. `sync`.
4. Desliga o Wi-Fi e silencia o áudio (evita o estalo).
5. Suspende na RAM (`echo mem > /sys/power/state`).

### Novo toque no Power (retomar)
O jogo volta instantaneamente da RAM, exatamente de onde parou.

### Suspensão prolongada
Quando a bateria chega a ~10 %, ou depois de no máximo 24 horas suspenso, o aparelho grava tudo e desliga de vez. (Decidido na Fase 1: o deep suspend medido na Fase 0 gasta ~2 %/h, então 10 a 15 minutos jogaria fora essa vantagem.) Como o save state já está gravado, ao ligar de novo ele volta automaticamente para o jogo (auto-resume).

### Bateria acabou durante a suspensão
No boot seguinte, o launcher encontra o marcador e carrega o save state sozinho.

### Segurar o Power por 2 segundos
Grava o save state e desliga. Nunca existe desligamento "sujo".

### Risco técnico (resolvido na Fase 0)
O deep suspend funciona com o stub de SRAM do ROCKNIX no TF-A: ~2 %/h no RG35XX-H. O Wi-Fi precisa ser desligado por `rfkill` antes de dormir (ver `docs/fase0.md`).

---

## 8. Interface

Mockup: https://claude.ai/artifact/Vz7H1VQirbdDkg7MoUSr6c

### Visual
| Elemento | Valor |
|---|---|
| Fundo | `#000000` |
| Texto principal | `#E8E8E8` (branco suave; branco puro cansa a vista em telas IPS pequenas) |
| Texto secundário | `#8C8C8C` (contagem de jogos, dicas de botões, status) |
| Seleção | barra `#E8E8E8` com texto `#000000` e cantos arredondados (10 px) |
| Cor de destaque | no máximo uma, só para alertas (bateria baixa, "SD2 não encontrado") |
| Fonte | Inter (livre), pesos 400/500/600 |
| Resolução de referência | 640x480; itens de lista com 44 px de altura e texto de 23 px |

### Telas

**1. Início**
- Barra superior: relógio à esquerda; Wi-Fi, ícone de bateria e porcentagem à direita.
- Lista: Recentes, consoles com jogos (com a contagem à direita) e Ports.
- Consoles com pasta vazia ficam ocultos.
- Rodapé: "A abrir · MENU opções".

**2. Lista de jogos**
- Funciona como um navegador de pastas, como o MinUI: cada console pode ter **subpastas em qualquer nível** (ex.: `Roms/MD/1 US - A-F/Jogo.md`). A abre a pasta, B volta um nível e a seleção fica na pasta de onde se saiu.
- Ordem alfabética natural ("2" antes de "10"), pastas e jogos misturados. Prefixos numéricos ("1 US", "2 Europe") servem para controlar a ordem.
- Pastas sem nenhum jogo (em qualquer nível abaixo) ficam ocultas.
- Barra superior: nome do console ou da pasta atual e "N itens" daquele nível.
- Nomes limpos: sem extensão e sem tags como "(USA) [!]". No arcade, nome vindo do DAT.
- Jogos com vários discos: um item, com "N discos".
- Rodapé: posição ("4 / 48") à esquerda; "A jogar · B voltar" à direita.
- MENU nesta tela: opções do console, incluindo **"Origem: SD1 / SD2"**.

**3. Menu dentro do jogo**
- Abre com o botão **MENU**, que age **ao soltar** (como no MinUI): segurar MENU e usar outro botão (ex.: Vol± para o brilho) não abre o menu. O MENU não sai do jogo; sair é a opção "Sair".
- Barra superior: nome do jogo e disco atual.
- Itens: **Continuar, Salvar, Carregar, Opções, Sair**.
- **Opções** (remapeamento de botões, filtro de tela, escala, opções do core): ao salvar, o usuário escolhe se vale **só para este jogo** ou **para todos os jogos do console**. A configuração do jogo, quando existe, tem prioridade sobre a do console.
- À direita: prévia do save (captura da tela no momento salvo), número do slot e data/hora.
- **4 slots**, trocados com ◀ ▶.
- Rodapé: "◀ ▶ slot · A confirmar · B voltar".

### Transições
- Só entre menus (entrar e sair de um console). **Nenhuma ao abrir ou fechar jogos.**
- Deslizamento lateral de 120 a 180 ms, com desaceleração no final (ease-out).
- Entrar desliza para a esquerda; voltar desliza para a direita.
- **Nunca bloqueia comandos:** apertar um botão durante a animação termina a animação na hora e executa o comando.
- A tela de destino já está pronta (vinda do cache) antes da animação começar.
- Opção nas configurações para desligar as transições.

### Fora da interface (de propósito)
Boxart, temas, sons de navegação, animação ao abrir jogos e excesso de configurações.

---

## 8.0 Volume e brilho

- **Vol+ / Vol−**: volume em 20 passos (um controle para alto-falante e fone).
- **MENU (segurado) + Vol+ / Vol−**: brilho em 10 passos, com um mínimo acima de zero (a tela nunca apaga de vez).
- Sobreposição: barra vertical arredondada no lado direito, que enche de baixo para cima, com o ícone embaixo: alto-falante no volume, sol no brilho. Some ~1,5 s depois do último toque.
- Funciona no launcher e dentro do jogo (desenhada por cima da imagem do emulador).
- Os valores ficam salvos no cartão (`/Config`) e voltam no próximo boot.

## 8.1 Filtros de tela

Presets simples por console, escolhidos em **Menu do jogo → Opções → Tela**. O usuário escolhe um nome; nunca vê parâmetros de shader.

| Console | Presets | Implementação |
|---|---|---|
| Game Boy | **Original** (cores cruas), **DMG** (verde da tela original) | Paleta interna do core gambatte (custo zero) |
| Game Boy Color | **Original**, **GBC** (cores lavadas da tela real) | Correção de cor do gambatte ou shader de cor GBC |
| Game Boy Advance | **Original**, **GBA** (tela sem luz, escura/lavada), **SP 101** (backlit, cores vivas) | Correção de cor do mGBA ou shaders de cor (`gba-color`, `sp101-color`) |

Camada opcional, independente da cor:
- **Grade LCD** (efeito de pixel da tela original). Só faz sentido com escala inteira: GB e GBC em 3x (480x432) ficam ótimos; o GBA em 2x (480x320) fica pequeno, e em tela cheia a escala não é inteira e a grade fica irregular.
- **Interframe blending / ghosting** no GB e no GBA. Vários jogos dependem do "rastro" da tela original para simular transparência (piscar de sprites).

Regras:
- Implementar como shaders GLES no próprio frontend. No Mali-G31, a 640x480, o custo é desprezível.
- Usar como base os shaders do repositório `libretro/glsl-shaders`, conferindo a licença de cada um.
- A escolha é salva **por console**. Por jogo, só se for pedido no futuro.
- Padrão de fábrica: GB → DMG; GBC e GBA → Original.
- Escala: opção "Inteira / Ajustada" ao lado dos filtros (ver 8.2).

## 8.2 Proporção da imagem

**A proporção original é sempre preservada. Não existe opção de esticar a imagem.** O espaço que sobra fica com bordas pretas.

Duas opções de escala, ambas com a proporção original:
- **Ajustada (padrão):** a maior imagem que cabe na tela.
- **Inteira:** só múltiplos exatos (2x, 3x...). A imagem fica menor, mas os pixels saem perfeitamente uniformes. É a melhor opção para a grade LCD.

Exemplos em tela 640x480 (4:3):

| Console | Resolução nativa | Ajustada | Inteira |
|---|---|---|---|
| Game Boy / GBC (10:9) | 160x144 | 533x480, bordas laterais | 3x = 480x432 |
| GBA (3:2, levemente wide) | 240x160 | 640x427, bordas pretas em cima e embaixo | 2x = 480x320 |
| SNES / Mega Drive / PS1 (4:3) | variável | tela cheia | depende da resolução do jogo |

- **Arcade:** usar a proporção de cada jogo informada pelo FBNeo. Jogos verticais (shmups) aparecem em pé, com bordas laterais.
- **PS1:** respeitar a proporção 4:3 da TV mesmo quando a resolução interna do jogo muda (ex.: 256, 320, 512 ou 640 de largura).
- Quando a proporção dos pixels não for quadrada (Mega Drive, SNES, PS1), corrigir para a proporção da TV original, sem distorcer.

---

## 9. Wi-Fi (usos previstos)
- Conectar a redes pelo menu de configurações.
- Futuro: sincronizar saves (ex.: Syncthing) e baixar ports pelo PortMaster.
- Desligar o Wi-Fi antes de suspender e religar ao retomar.

---

## 10. Roteiro

### Fase 0: prova de conceito (RG35XX-H)
1. Ambiente Buildroot com toolchain ARM64.
2. Kernel mainline + device tree do ROCKNIX: boot com imagem na tela.
3. Validar áudio, botões e analógicos (evdev), bateria e Wi-Fi.
4. **Testar o suspend (deep vs s2idle) e medir o consumo.**
5. Rodar um core libretro (ex.: gambatte) com um frontend mínimo em SDL2/KMS.

### Fase 1: sistema utilizável (RG35XX-H)
1. Layout de partições, squashfs somente leitura e expansão no primeiro boot.
2. Launcher: Início, lista de jogos e cache.
3. Frontend libretro com o menu dentro do jogo, save states e slots.
4. Todos os cores da seção 4.
5. Arcade com nomes vindos do DAT.
6. Suspend, resume, auto-resume e desligamento seguro.
7. Lógica de origem SD1/SD2.

### Fase 2: polimento
1. Transições.
2. PortMaster.
3. Configurações de Wi-Fi.
4. Suporte ao RG40XX.

### Fase 3: expansão
1. TrimUI Brick (avaliar kernel do fabricante vs mainline).
2. Sincronização de saves.

---

## 11. Decisões em aberto
- Atalho global de saída para os ports.
- Se uma única `.img` atende o RG35XX-H e o RG40XX, ou se é preciso uma imagem por aparelho.

Decididas na Fase 1 (ver `docs/fase1.md`): frontend escrito do zero (o MinUI não tem licença); pastas com os códigos curtos da seção 3; saves com os nomes do RetroArch (`.srm`); desligamento de emergência com 10 % de bateria ou depois de 24 h de suspensão; interface em português com textos em arquivo separado.
