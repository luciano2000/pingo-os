# Fase 1: sistema utilizável (RG35XX-H)

## Decisões (2026-11-04)

| Tema | Decisão | Motivo |
|---|---|---|
| Frontend | Escrito do zero, a partir do `pingo-retro` da Fase 0 | O MinUI não tem licença (todos os direitos reservados) e está arquivado; só aproveitamos ideias de UX |
| Arquitetura | Dois programas: launcher e `pingo-retro`; o launcher abre o jogo como outro processo | Core que trava ou vaza memória não derruba o launcher |
| Suspensão prolongada | Fica suspenso até a bateria chegar a 10 % (desligamento de emergência) ou por no máximo 24 h; aí salva e desliga | O deep suspend gasta ~2 %/h (Fase 0); 10-15 min jogaria fora essa vantagem |
| Pastas | Códigos curtos no cartão (`GB`, `SNES`, `MD`...); o launcher mostra o nome completo | Fácil de digitar no PC, sem acentos nem espaços |
| Saves | Nomes do RetroArch (`Jogo.srm`); save states em formato próprio | Saves do ROCKNIX/RetroArch funcionam sem renomear |
| Idioma | Português, com os textos num arquivo separado; depois, arquivos de idioma lidos em tempo de execução | A comunidade traduz sem compilar nada |

## Fatias

1. **Cartão:** partição `ROMS` (exFAT) no primeiro boot, `fsck` em todo boot, pastas, dados do sistema persistentes, imagem sem o Clang.
2. **Launcher:** Início, lista de jogos como navegador de pastas (subpastas em qualquer nível, como o MinUI), cache por pasta, fonte Inter, barra com relógio, Wi-Fi e bateria. Boot direto no launcher.
3. **Jogo completo:** menu dentro do jogo (MENU ao soltar), 4 slots com prévia, SRAM, escala Ajustada/Inteira, sincronização pelo áudio, opções por jogo ou por console.
   Volume e brilho (Vol± e MENU+Vol±, com sobreposição) entram junto com o launcher e valem também no jogo.
4. **Energia:** Power suspende/retoma, auto-resume, segurar 2 s salva e desliga.
5. **Cores:** todos da seção 4 da especificação, BIOS, PS1 (`.chd`, `.m3u`), filtros de tela.
6. **Arcade:** FBNeo com nomes vindos do DAT.
7. **SD1/SD2:** origem por console.

## Fatia 1: cartão

`/etc/init.d/S06pingo-storage`:

- Acha o disco do sistema pelo `root=PARTUUID=` do cmdline.
- Sem partição 3 e com pelo menos 1 GiB livre: mostra "Preparando cartão..." na tela, cria a partição 3 (tipo `07`) no espaço restante com `sfdisk --append`, registra no kernel com `partx -a` (o disco está em uso, não dá para reler a tabela inteira) e formata em exFAT com o rótulo `ROMS`.
- **Regravar a `.img` não apaga os jogos:** o `dd` troca a tabela de partições, mas a partição 3 é recriada no mesmo lugar; se já existir um exFAT `ROMS` ali, ele é reaproveitado sem formatar.
- Em todo boot: `fsck.exfat -p` e montagem em `/mnt/sd1` (`noatime,errors=remount-ro`).
- Uma partição 3 que não seja exFAT nunca é tocada.

Estrutura criada:

```
Roms/<GB GBC GBA NES SNES VB POKEMINI MS GG MD SEGACD 32X PCE PCECD NGP WS MSX A2600 A7800 LYNX PS ARCADE>
Bios/  Saves/  States/  Ports/  Config/
.system/   dropbear (chaves SSH), cache (shaders do Mesa), logs
```

Outros ajustes:

- Chaves SSH persistentes: `/var/run/dropbear` aponta para `.system/dropbear`, então o PC não reclama mais de chave trocada a cada boot.
- `XDG_CACHE_HOME=/mnt/sd1/.system/cache` (`/etc/profile.d/pingo.sh`): o Mesa guarda os shaders compilados.
- O `post-build.sh` remove o Clang, o `libLLVMSPIRVLib` e os shaders de OpenCL (~145 MB): o Mesa só precisa deles no build.

### Checklist

- [x] Imagem sem o Clang (131 MB, 57 MB em `.xz`); `kmscube` e `pingo-retro` a 60 fps; cache de shaders na `ROMS`
- [x] Primeiro boot cria e formata a `ROMS` (118,5 GB no cartão de 119 GB). Primeira versão criava a partição no buraco antes do boot; corrigido com início explícito
- [x] A `ROMS` aparece no PC (Linux) com as pastas; Windows ainda sem teste
- [x] Regravar a imagem preserva a `ROMS` e o conteúdo (ROM, cache e chave SSH sobreviveram)
- [x] Chaves SSH iguais entre boots
- [x] Desligamento sujo (power segurado): arquivos íntegros (md5 igual) e `fsck.exfat` limpo no boot seguinte

Nota para a fatia 3: o frontend deve gravar SRAM e save states com `fsync` logo em seguida; o cache de escrita do kernel leva alguns segundos para descer ao cartão.

## Formatos de jogos

| Sistema | Formatos | Observação |
|---|---|---|
| Cartuchos (GB, GBA, NES, SNES, MD...) | ROM solta ou `.zip` | O `pingo-retro` descompacta o `.zip` na memória (fatia 3) |
| Arcade (FBNeo) | `.zip` (romset) | Nunca extrair: o romset é o próprio `.zip` |
| PlayStation (PCSX ReARMed) | `.chd` (recomendado), `.cue`+`.bin`, `.m3u`, `.pbp` | Vários discos: um item na lista via `.m3u` |
| PC Engine CD (Beetle PCE Fast) | `.chd` (recomendado), `.cue`+`.bin`/`.wav`/`.ogg`, `.ccd` | |
| `.zip` de CD, `.7z` | Não suportados por enquanto | Para CD, o compactado é o `.chd` (`chdman` no PC) |

## Tela de boot

- `board/h700/splash/pingo.png` (640x480) vira `/usr/share/pingo/splash.raw` no build (`scripts/png2fb.py`, XRGB8888) e o `S00pingo-splash` copia para o `/dev/fb0` no começo do boot.
- Kernel silencioso (`loglevel=3 logo.nologo vt.global_cursor_default=0`); `/dev/console` passa a ser a serial (`ttyS0`), então as mensagens do init e o login não escrevem por cima do logo. Diagnóstico: `pingo-log.txt`, `dmesg`, SSH e serial USB.

## Fatia 2: launcher

Código em `frontend/launcher/` (`pingo-launcher`) e `frontend/common/osd.c` (volume e brilho, usado também pelo `pingo-retro`). Pacote `pingo-frontend`; fonte Inter 4.1 (`font-inter`, OFL).

- Telas do mockup: Início (relógio, Wi-Fi, bateria; Recentes e consoles com contagem) e listas como navegador de pastas.
- Biblioteca: varre `Roms/<console>` com subpastas; oculta pastas sem jogos e arquivos ocultos; nome limpo (sem extensão, `(...)` e `[...]`); ordem natural. Cache em `.system/cache/launcher.cache`, conferido em segundo plano a cada abertura.
- Navegação: ↑↓ (segurar repete), ←→ página, L1/R1 letra inicial anterior/próxima, A abre/joga, B volta. Power suspende.
- Volume (Vol±, 20 passos) e brilho (MENU segurado + Vol±, 10 passos) com a barra vertical à direita; salvos em `Config/sistema.cfg`.
- Abrir um jogo: o launcher grava `/run/pingo/next` e sai; o `pingo-shell` roda o `pingo-retro` e reabre o launcher no mesmo lugar (`/run/pingo/launcher.state`). Recentes em `Config/recentes.txt` (console + caminho relativo).
- O launcher desenha só quando algo muda (bateria). Desenha dois quadros ao abrir: o KMS/DRM do SDL nem sempre mostra o primeiro, e a tela ficava preta ao voltar do jogo.
- Boot: `S90pingo-ui` inicia o `pingo-shell`; o Wi-Fi (`S41`) e o log de boot (`S99`) passaram para segundo plano.

### Checklist

- [x] Início, listas e subpastas como no MinUI (pastas vazias ocultas)
- [x] B volta para a pasta de onde se saiu; rolagem ao segurar; L1/R1 por letra
- [x] Abrir jogo, voltar para a lista, Recentes
- [x] Power suspende e volta para a mesma tela
- [x] Volume e brilho com sobreposição, no launcher e no jogo; valores persistem
- [x] Boot direto no launcher pela imagem (~7 s do power até a lista)
- [x] Power: toque suspende ao soltar (launcher e jogo); segurar 2 s mostra "Desligando…" e desliga, sem estalo
- [x] Transições sem o logo do boot: "Carregando…" ao abrir o jogo, preto ao voltar (a última tela é copiada para o `/dev/fb0`, que é o que aparece entre um programa e outro)
- [x] Assistente do primeiro boot num cartão "novo": B desliga e volta a perguntar no boot seguinte; A prepara, mostra "Pronto!" e abre o launcher

### Botão power (AXP717)

- O chip manda "apertou" e "soltou" corretamente (medido: segurar 3,5 s gera os dois eventos, 3,51 s de diferença).
- Segurando por mais tempo (> 3,5 s), o próprio chip corta a energia, sem avisar o sistema: desligamento sujo. Por isso o "segurar 2 s" do Pingo desliga antes.
- Bug corrigido: o instante do aperto era guardado com o bit mais baixo ligado para distinguir de "solto"; metade das vezes ficava 1 ms no futuro, a conta sem sinal dava um número enorme e o toque virava "segurou 2 s".
- Log da interface no cartão: `.system/logs/ui.log` (eventos do power, jogos abertos, fps).

## Fatia 3: jogo completo

`frontend/pingo-retro.c` reescrito; a camada de desenho (`common/ui.c`) e os textos (`common/strings_pt.h`) agora são compartilhados com o launcher.

- **Menu do jogo** (MENU ao soltar), como no mockup: Continuar, Salvar, Carregar, Opções, Sair, com o jogo escurecido atrás; prévia do slot à direita, 4 slots (◀ ▶), "Salvo hoje, HH:MM". Salvar volta ao jogo, como no MinUI.
- **Save states**: `States/<console>/<jogo>.st0..st3` + prévia `.bmp`; o slot inicial é o salvo mais recentemente.
- **SRAM**: `Saves/<console>/<jogo>.srm` (nome do RetroArch); grava sozinha quando muda (a cada 5 s), ao abrir o menu, antes de suspender e ao sair. Tudo com arquivo temporário + `fsync` + `rename`.
- **Som** a 48 kHz com reamostragem e controle dinâmico de taxa (±0,5 %): o Game Boy (59,73 Hz) roda um quadro por vsync (60 Hz) sem picotar; consoles longe de 60 Hz (PAL) acumulam tempo.
- **Escala** Ajustada (padrão) ou Inteira, sempre com a proporção do core.
- **`.zip`** de cartucho aberto na memória (`libzip`, maior arquivo do `.zip`); cores com `need_fullpath` (FBNeo) recebem o `.zip` como está.
- **Opções**: Escala + lista curada por core (`frontend/cores/<core>.opts`, "chave=Nome em português"); "Salvar para este jogo" (`Config/<console>/<jogo>.cfg`) ou "para o console" (`Config/<console>/console.cfg`), o do jogo tem prioridade. Só as opções mostradas são gravadas.
- O launcher passa o código do console ao `pingo-retro` (3ª linha de `/run/pingo/next`).

### Checklist

- [x] Som limpo (sem picotes) no Game Boy
- [x] Menu do jogo, salvar/carregar nos 4 slots com prévia
- [x] Escala Ajustada/Inteira e opções do Gambatte (só as úteis)
- [x] `.zip` de Game Boy
- [x] Remapeamento de botões (Opções → Botões): nomes das funções vindos do core (FBNeo: "Weak Punch"...), A + botão do aparelho, troca quando o botão já estava em uso, restaurar padrão; salvo por jogo ou por console (`pingo.bind.<função>=<botão>`, só o que muda do padrão). Falta testar no aparelho nas Opções (fica para depois dos cores, fatia 5)

## Fatia 4: energia

- **Toque no power no jogo:** grava a SRAM, um save state automático (`States/<console>/<jogo>.auto`) e a marca `.system/resume`; suspende. Acordou pelo botão: o jogo continua da RAM e a marca é apagada.
- **Suspensão prolongada** (`pingo-suspend`): a luz da tela apaga antes de dormir; a cada 1 h o RTC acorda o aparelho com a tela apagada, confere a bateria e volta a dormir. Com 10 % (sem carregador) ou 24 h suspenso, sai com código 10 e o launcher/jogo desliga (a marca fica).
- Quem acordou: o alarme do RTC ainda armado = botão; zerado = relógio.
- **Segurar o power 2 s no jogo:** salva para retomar e desliga.
- **Bateria no jogo:** aviso "Bateria fraca" com 10 %; com 3 %, salva para retomar e desliga.
- **Boot:** na primeira abertura, o launcher acha a marca e abre o jogo com `resume` ("Carregando…"); o `pingo-retro` carrega o `.auto` e apaga a marca (um só retorno, para não entrar em laço se travar).
- **Sair pelo menu** apaga a marca: não retoma.
- Teste rápido: `/run/pingo/suspend-test` com `CHECK_EVERY=60` e `MAX_SUSPEND=120` (some no reboot).

### Checklist

- [x] Toque no power no jogo: suspende e volta
- [x] Segurar no jogo: desliga e, no boot, volta para o jogo no mesmo ponto
- [x] Suspensão prolongada (teste curto): desliga sozinho e retoma no boot
- [x] Sair pelo menu: o boot seguinte abre o launcher normal

## Fatia 5: cores

16 cores libretro na imagem (`/usr/lib/libretro/`), ~45 MB no total; imagem de 139 MB (64 MB em `.xz`).

- Modelo comum em `external/external.mk` (`pingo-libretro-core`): cada pacote `libretro-*` só define commit, hash, Makefile, subpasta e nome do `.so`. Compila com `platform=unix` e só `CC/CXX/AR` na linha de comando.
- mGBA usa CMake (`-DBUILD_LIBRETRO=ON`, sem interface nem dependências).
- PicoDrive vem por git com submódulos (o `.tar.gz` do GitHub não traz `libpicofe`, `cyclone`...).
- PCSX ReARMed com recompilador dinâmico ARM64 (`DYNAREC=ari64`).
- Atari 2600: `stella2014` (mais leve que o Stella atual).
- BIOS: `docs/bios.md`.

| Console | Core |
|---|---|
| GB/GBC | gambatte |
| GBA | mgba |
| NES/FDS | fceumm |
| SNES | snes9x |
| Virtual Boy | mednafen_vb |
| Pokémon mini | pokemini |
| MS/GG/MD/Sega CD | genesis_plus_gx |
| 32X | picodrive |
| PC Engine / CD | mednafen_pce_fast |
| Neo Geo Pocket | mednafen_ngp |
| WonderSwan | mednafen_wswan |
| MSX | bluemsx |
| Atari 2600/7800/Lynx | stella2014 / prosystem / handy |
| PlayStation | pcsx_rearmed |

### Pendente nesta fatia

- [x] Testar um jogo de cada console no aparelho: todos abrem; corrigidos FDS e NGP (cores com `need_fullpath` recebiam o `.zip`; agora o jogo é extraído para `/run/pingo/rom`, a não ser que o core aceite `.zip`) e `.m3u` com barra no começo das linhas
- [x] FDS: pasta própria e "Virar o disquete" no menu (o FCEUmm vira o lado pelos botões R e L; o menu aperta R, L, R)
- [x] Ordem dos consoles por número na pasta (`01 - NES`), como no MinUI
- [x] PS1 com vários discos: `.m3u` como um item ("N discos"), agrupamento automático de "(Disc N)" sem `.m3u`, troca de disco no menu e "Disco N" na barra; abertura da BIOS ligada
- [ ] Listas curadas de opções dos cores novos (por enquanto só a Escala)
- [ ] Filtros de tela (seção 8.1)
- [x] Remapeamento de botões (Opções → Botões): nomes das funções vindos do core (FBNeo: "Weak Punch"...), A + botão do aparelho, troca quando o botão já estava em uso, restaurar padrão; salvo por jogo ou por console (`pingo.bind.<função>=<botão>`, só o que muda do padrão). Falta testar no aparelho
