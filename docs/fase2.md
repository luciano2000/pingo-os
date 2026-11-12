# Fase 2: polimento

## Wi-Fi

- **Tela:** MENU no Início → **Wi-Fi**. Liga/desliga (lembrado entre boots), estado ("Conectado a REDE · IP"), lista das redes por perto com o sinal em 4 barras e as marcas "conectada", "salva" e "aberta". A conecta, X procura de novo, Y esquece uma rede salva, B volta.
- **Senha:** teclado na tela (minúsculas, MAIÚSC com Y, símbolos com SELECT, X espaço, B apaga, START confirma, MENU cancela). Mínimo de 8 caracteres. Redes WEP são recusadas.
- **Backend:** `/usr/sbin/pingo-wifi` (`start|stop|on|off|status|scan|connect|forget`) sobre o `wpa_supplicant` pelo socket de controle (`wpa_cli`), sem NetworkManager. O launcher passa a senha pela entrada padrão do script, nunca pela linha de comando.
- **Nada da senha em texto:** o script converte com `wpa_passphrase` e grava só a chave (psk de 64 dígitos) em `/mnt/sd1/.system/wifi/wpa_supplicant.conf`; o nome da rede vai em hexadecimal. A partição de boot (que qualquer PC abre) não guarda nada de Wi-Fi.
- **Conexão que falha** (senha errada, rede fora do alcance): depois de ~20 s a rede nova é descartada e as salvas voltam a valer, sem perder a conexão anterior.
- **`pingo-wifi.txt`** (Fase 0): saiu da imagem. Se alguém puser um na `PINGOBOOT` pelo PC, o boot importa a rede (já como chave) e apaga o arquivo.
- `S41pingo-wifi` só chama `pingo-wifi start` em segundo plano (inclui o religamento do driver do RTL8821CS e o acerto do relógio por NTP).

### Checklist

- [x] Backend no aparelho: importa o `pingo-wifi.txt` e apaga, conecta, lista redes, desliga/liga, senha errada volta para a rede salva
- [x] Tela: ligar/desligar (corrigido o travamento ao religar), estado e lista de redes
- [x] Conectar numa rede nova (roteador do celular) com o teclado na tela

## Data, hora e fuso

- **Tela:** MENU no Início → **Data e hora**: data e hora atuais em destaque, **Fuso horário** (◀ ▶, 19 fusos com nome em português e a diferença do UTC calculada na hora, já com horário de verão), **Acertar pela internet** (padrão ligado).
- Com o acerto pela internet desligado aparecem **Data** e **Hora**: A abre o editor (◀ ▶ escolhem o campo, ▲ ▼ mudam, A grava, B cancela). A hora vai para o relógio do sistema (`clock_settime`) e para o RTC (`hwclock -w -u`), que segue contando com o aparelho desligado.
- O relógio do sistema e o RTC ficam em UTC. O fuso é um nome da base de fusos (glibc + tzdata) gravado em `Config/fuso.txt`; o `pingo-shell` exporta `TZ` antes de abrir o launcher e cada jogo. Sem escolha vale o `/etc/localtime` da imagem (São Paulo).
- `Config/hora-manual` (criado ao desligar o acerto pela internet) faz o `pingo-wifi` não rodar o `ntpd`; religar o acerto roda o `ntpd` na hora.

### Checklist

- [x] Trocar o fuso: o relógio da barra muda na hora e vale nos jogos ("Salvo hoje, HH:MM")
- [x] Hora manual: data e hora gravadas continuam certas depois de desligar e ligar
- [x] Voltar para "pela internet": a hora volta a ficar certa

## Idiomas e Desligar

- Textos em `common/textos_pt.h` (lista X-macro): cada `S_NOME` é um ponteiro, em português por padrão. `textos.c` carrega `<código>.txt` (`CHAVE=Texto`, a chave sem o `S_`) de `/usr/share/pingo/idiomas` ou de `Idiomas/` no SD1 (vale mais). Linhas com `%d`/`%s`/`%u` diferentes do original são ignoradas (formatos de data, com `%H`, podem mudar). Escolha em `Config/idioma.txt`; launcher e `pingo-retro` leem ao abrir, e o launcher troca na hora.
- Inglês completo (`frontend/idiomas/en.txt`). Guia para a comunidade: `docs/traducao.md`.
- MENU no Início: Wi-Fi, Data e hora, **Idioma** (◀ ▶), Preparar o SD2 (quando há cartão novo) e **Desligar** (último, com confirmação).

### Checklist

- [x] Trocar para English no menu: Início, listas, menu do jogo, Wi-Fi, Data e hora
- [x] Voltar para Português
- [x] Desligar pelo menu

## RetroAchievements

- **Biblioteca:** rcheevos 12.5.0 (MIT, oficial do RetroAchievements), pacote `external/package/rcheevos` compilado como `librcheevos.a` (sem RAIntegration). HTTPS pelo libcurl com mbed TLS e os certificados do `ca-certificates`.
- **Conta:** MENU no Início → **RetroAchievements** → Entrar (usuário e senha no teclado na tela). O launcher roda `pingo-retro --ra-login <usuário>` com a senha pela entrada padrão; o servidor devolve um token e só ele fica no cartão (`.system/retroachievements.txt`: usuário, token, hardcore). "Sair da conta" apaga o arquivo. Modo hardcore liga/desliga ali.
- **No jogo** (`frontend/ra.c`): com conta, o `pingo-retro` entra com o token e identifica o jogo pelo hash (rc_client); a memória vem do `SET_MEMORY_MAPS` do core ou do `retro_get_memory_data` (rc_libretro). A internet roda numa thread (libcurl); as respostas voltam ao rc_client no laço principal, então o jogo nunca espera a rede. Sem conexão, os desbloqueios ficam pendentes e vão quando ela volta.
- **Avisos** em cima da tela: conquista desbloqueada (pontos e nome), jogo completo, desafios (placares), sem conexão / conectado de novo, jogo não reconhecido. Ao abrir: "Jogo: X de Y conquistas".
- **Imagens das conquistas:** quando o jogo é reconhecido, as imagens (PNG 64×64, `badge_url`) de todas as conquistas são baixadas em segundo plano para `.system/cache/ra/<badge>.png` (o mesmo jogo não baixa de novo). PNG lido com o `stb_image` (domínio público/MIT, `frontend/include/stb_image.h`, em `common/imagem.c`).
- **Notificação:** caixa escura com borda branca, imagem da conquista à esquerda, "Conquista desbloqueada · N pontos" e o nome em destaque; desliza de cima, fica 5 s. Avisos sem imagem usam a mesma caixa.
- **Menu do jogo → Conquistas**: em ordem de pontos (mais fáceis primeiro), imagem pequena (escurecida nas bloqueadas), ✓ e pontos; descrição da selecionada numa caixa de diálogo embaixo (fonte do nome, até 3 linhas); ◀ ▶ passam página.
- **Hardcore:** Carregar (save state) e o retorno automático ao jogo ficam desligados. As opções de core proibidas pelo rcheevos para aquele core e console (`rc_libretro_get_disallowed_settings_for_system`: região PAL, camadas desligadas no Snes9x, trapaças no Genesis Plus GX...) voltam ao primeiro valor permitido e são puladas no menu de opções; core não permitido para o console desliga o hardcore na sessão, com aviso. Por isso o RetroAchievements começa antes de carregar o jogo.
- **User agent:** `PingoOS/<versão> (Linux) <Core>/<versão do core> rcheevos/12.5`. O "Warning: Unknown Emulator" (conquista falsa com id ≥ 101000001) é o servidor avisando que o Pingo OS ainda não está aprovado para o hardcore: aparece como aviso e fica fora da lista. Pedido de aprovação: `docs/retroachievements-pedido.md`.
- **CHD** (`frontend/chd_hash.c`): o leitor de CD do rcheevos só lê `.cue`/`.bin`/`.iso`; um cdreader próprio abre os `.chd` com o libchdr (pacote `libchdr`, BSD-3, estático) e repassa o resto ao leitor padrão. Setores no LBA do disco (faixa 1 no 0), pregap no arquivo só com PGTYPE `V`, faixas arredondadas a 4 quadros de 2448 bytes, áudio com os bytes trocados. Conferido no PC com 25 jogos de PC Engine CD (hash do `.chd` igual ao do `.cue`/`.bin` extraído) e no aparelho com Crash Bandicoot (PS), Sonic CD (Sega CD) e Lords of Thunder (PC Engine CD).

- **Sem internet (2026-11-11):** ao sair do jogo, o `ra_stop` esperava a fila inteira; com as imagens de um jogo grande ainda baixando (FF IX: 104 conquistas) e a internet falhando, a saída travava por vários segundos. Agora as imagens que faltam são descartadas (ficam para a próxima vez) e o resto tem até 6 s. O login que falha por falta de internet (logo depois de acordar, o Wi-Fi ainda está voltando) é repetido a cada 5 s, até 12 vezes, e o jogo é carregado quando ele dá certo; conta ou token inválido avisa na hora. Testado tirando a rota padrão durante o login.

### Checklist

- [x] HTTPS com o servidor (login com usuário inexistente devolve "senha errada", nada gravado)
- [x] Entrar com uma conta de verdade pela tela
- [x] Jogo de cartucho reconhecido (Super Mario World: "3 de 89 conquistas")
- [x] Conquista desbloqueada na tela (Mario Kart)
- [x] Notificação com a imagem da conquista; imagens na lista (bloqueadas em preto e branco)
- [x] Lista de conquistas no menu do jogo (ordem por pontos, caixa de descrição)
- [ ] Hardcore: Carregar bloqueado

## Transições entre listas

- Entrar numa pasta/console (A): a tela atual sai pela esquerda e a nova entra pela direita; voltar (B): o contrário. 180 ms, desacelerando no fim (cúbica). Botões apertados no meio ficam na fila e valem logo depois (pular por "evento esperando" não serve: o soltar do próprio A já está na fila e cancelava a animação no primeiro quadro).
- As duas telas são desenhadas em texturas (`SDL_TEXTUREACCESS_TARGET`, 640×480) com o `draw_scene()` (o desenho sem exibir) e deslizam juntas; o vsync dita o ritmo.
- Entre o launcher e o jogo não há transição (decisão: só atrasaria a entrada).

### Checklist

- [x] Entrar e voltar deslizando, sem tremer nem piscar
- [x] Navegação rápida (A/B seguidos) não fica presa na animação

## PortMaster

- **No sistema:** bash, Python 3 (ssl, ctypes, zlib, bz2, xz, xml, unicodedata), SDL2_image (PNG/JPEG), SDL2_mixer, OpenAL (LÖVE) e o `curl` (com `CURL_CA_BUNDLE`, o mbed TLS não acha os certificados sozinho). No BusyBox: `pgrep`/`pkill` (o PortMaster cai sem eles), `timeout`, `stat`, `taskset` com lista de núcleos (`-c 0,1,2,3`, o `$TASKSET` dos ports) e o `tar` com gz/xz/bz2 (os runtimes). O kernel já tinha uinput, loop, squashfs, overlay e FUSE. A raiz cresceu de 73 para 86 MB.
- **`/roms/ports` → `/run/pingo/ports` → pasta Ports do cartão escolhido** (MENU → PortMaster → Instalar no: SD1/SD2; `Config/ports.txt`). É onde o PortMaster e os ports procuram tudo. No SD2, o `pingo-portmaster` libera a escrita enquanto o PortMaster ou o port roda e volta a só leitura no fim; sem o SD2, aviso. Trocar não move o que já está instalado.
- **Tabela de controles:** só a linha do H700 Gamepad no `gamecontrollerdb.txt` do PortMaster (a original vai para `gamecontrollerdb.full.txt`; refeita a cada abertura). Num sistema que o PortMaster não conhece, os ports punham a tabela inteira (~1.800 controles) numa variável de ambiente, acima do limite de 128 KB do Linux: nada do port rodava ("Argument list too long"). Rótulos como no aparelho: A à direita, B embaixo, X em cima, Y à esquerda. Sistema desconhecido para ele = plataforma genérica, com o nome do `/etc/os-release` ("Pingo OS").
- **MENU no Início → PortMaster:** "Instalar o PortMaster" enquanto ele não está instalado, depois "Abrir o PortMaster". Fica fora da lista de jogos (decisão: não sujar os consoles com o instalador).
- **Instalar:** `pingo-portmaster install` baixa o `PortMaster.zip` oficial mais recente do GitHub, confere o md5 e extrai em `Ports/PortMaster/`. Depois ele se atualiza sozinho.
- **Ports no Início** (depois dos consoles, só quando há algum port): os `Ports/*.sh` em ordem alfabética (as pastas de dados dos ports não aparecem). A abre: o launcher grava `port` em `/run/pingo/next` e o `pingo-shell` roda `pingo-portmaster run <script>`, com `HOME` e `XDG_*` em `.system/home` no SD1 (a raiz é somente leitura). Ao sair do port, volta ao launcher.
- **Direcional vazio no `.gptk`:** muitos ports deixam o direcional e o analógico esquerdo vazios (`up = \"`), porque nos sistemas onde foram feitos o próprio sistema entrega o direcional como setas. O `pingo-portmaster` gera o `mod_Pingo OS.txt` (que todo port carrega), passando o gptokeyb pelo `pingo-gptokeyb`: numa cópia do `.gptk`, só essas linhas vazias viram setas.
- **Sair de um port:** o atalho é o do próprio port (gptokeyb do PortMaster, normalmente SELECT + START).

### Checklist

- [x] Instalar o PortMaster pelo menu (reconhece "Pingo OS", Anbernic RG35XX H, 640x480, Panfrost)
- [x] Abrir o PortMaster (interface dele aparece e responde aos botões)
- [x] Instalar ports gratuitos e jogar: Cave Story (nxengine-evo) e Ticoban com o direcional e Celeste Classic
- [ ] Sair do port e voltar ao launcher

## Analógicos nos jogos

- **O que faltava:** na Fase 0 os analógicos foram conferidos só no Linux (`evtest`); o `pingo-retro` lia apenas botões e direcional, e nenhum jogo recebia os sticks. Passou despercebido porque quase tudo testado era digital.
- **`pingo-retro`:** lê `ABS_X/Y/RX/RY` do "H700 Gamepad" (faixa pelo `EVIOCGABS`, -1800..1800 nos dois aparelhos), converte para -32767..32767 com zona morta de ~8 % e responde `RETRO_DEVICE_ANALOG` na porta 1. O driver já desinverte os eixos de cada aparelho pelo device tree.
- **PlayStation:** a porta 1 vira DualShock (`retro_set_controller_port_device`). No console de verdade o modo analógico só liga pelo botão Analog (Crash Bandicoot 2) ou pelo próprio jogo (Gran Turismo). Patch no PCSX ReARMed (`0001-dualshock-sempre-analogico.patch`): opção `pcsx_rearmed_dualshock_analog`, que mantém o modo analógico; no menu do jogo, Opções → **Analógico** (padrão Ligado no Pingo). Ligada, o atalho L1+R1+Select deixa de alternar o modo. Jogo antigo que não aceite o controle: desligar e "Salvar para este jogo".

### Checklist

- [x] Crash Bandicoot 2 e Gran Turismo com analógico (RG40XX-H)
- [ ] Analógico Desligado no menu: o stick para de responder
- [x] RG35XX-H (v0.3.3-beta)

## Vibração

- **Hardware:** um motor no PWM (PWM3 no RG40XX-H, PWM2 no RG35XX-H), exposto pelo rocknix-joypad como `FF_RUMBLE` (ff-memless) no "H700 Gamepad"; `rumble_enable` no sysfs já vem ligado.
- **`pingo-retro`:** responde `RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE`; uma vez por quadro, se a força mudou, atualiza um único efeito (`EVIOCSFF`, duração 0 = até parar) e liga ou desliga com `EV_FF`. O aparelho tem um motor só: o driver usa o forte, ou o fraco se o forte for zero. Para ao abrir o menu, ao suspender e ao sair (o kernel também apaga o efeito se o processo morrer). O controle passa a ser aberto para escrita.
- **Menu:** Opções → **Vibração** (Ligada/Desligada), só quando o core pede a interface e o aparelho tem motor; salva como `pingo.vibracao` por jogo ou console.
- PCSX ReARMed: vibração do DualShock (`pcsx_rearmed_vibration`, ligada por padrão). Testado no RG40XX-H com Metal Gear Solid.
- **Travamento (resolvido, 2026-11-12):** depois de alguns minutos de Metal Gear, o aparelho inteiro congelava (nem o power respondia), sem mensagem no kernel. Um teste que muda a força a cada 16 ms travava no primeiro comando. Causa no rocknix-joypad: o `rumble_play_effect` pegava um mutex, mas o ff-memless o chama com o `event_lock` (spinlock, IRQs desligadas) ou de um timer; quando a leitura do controle segurava o mutex, ele dormia em contexto atômico. Patch `0001-rumble-sem-mutex-em-contexto-atomico.patch`: só grava o nível e agenda o `play_work`. Depois: 18.635 mudanças em 5 minutos sem travar. (Descarregar o módulo com `rmmod` corrompe a memória: para testar o driver, gravar a imagem.)

### Checklist

- [x] Metal Gear Solid vibra (RG40XX-H)
- [ ] O motor para ao abrir o menu; Vibração Desligada no menu
- [ ] RG35XX-H

## RG40XX-H

- Mesmo H700, memória e tela 640x480 do RG35XX-H. O device tree dele (do ROCKNIX) já era compilado; as diferenças estão nele: analógico direito invertido (`invert-absrx/ry`), vibração por PWM, luzes RGB dos analógicos (`led-rgb`, UART5) e o painel `anbernic,rg40xx-panel`. O driver de controle é o mesmo (rocknix-joypad).
- **Um build, uma imagem por aparelho:** kernel, U-Boot e sistema são idênticos; o `post-image.sh` gera `pingo-os-<aparelho>.img` para cada `board/h700/<aparelho>/extlinux.conf` (só a linha `fdt` muda). Unidades com o painel "v2": trocar a linha `fdt` pelo `sun50i-h700-anbernic-rg40xx-h-v2-panel.dtb`.
- **Decisão: uma imagem por aparelho, sempre** (sem detecção de tela, processador etc. no boot: cada imagem fica enxuta, só com o que o aparelho dela usa).

### Checklist

- [ ] Boot com imagem na tela (ou trocar para o device tree do painel v2)
- [ ] Botões, analógicos (o direito no sentido certo), volume, power
- [ ] Assistente do primeiro boot, jogos, som, suspender
- [ ] Wi-Fi, SD2
- [ ] Luzes RGB (o que fazer com elas: apagar, cor fixa?)

## Luzes (RG40XX-H/V)

- **Hardware:** um microcontrolador na `/dev/ttyS2` (UART5, 115200 8N1). Pacote: modo (1), brilho (0-255), as cores RGB dos LEDs (o ROCKNIX manda 16), soma de verificação (soma dos bytes & 0xFF); ~50 bytes, ~4 ms. `rgb:kbd_backlight` (gpio) só liga a energia. Referência: `quirks/devices/Anbernic RG40XX V/bin/analog_sticks_ledcontrol` do ROCKNIX.
- **App:** MENU no Início → Luzes (só nos aparelhos com LEDs). Modos: Desligado, Cor fixa (presets com nome + matiz livre, a mesma cor nos dois analógicos ou uma para cada), Respirar, Arco-íris, Bateria (verde/amarelo/laranja/vermelho), Ambilight. Brilho 0-100 %. Apaga com a tela e na suspensão.
- **Ambilight:** o `pingo-retro` reduz o quadro a ~16x12, ~15 vezes por segundo, e calcula a cor de cada metade (esquerda → analógico esquerdo, direita → direito). Cor predominante, não média: peso maior para as cores saturadas; tela escura/cinza = luz fraca e neutra. O serviço `pingo-leds` suaviza as transições, roda os efeitos e fala com a serial. Fora do jogo, a cor fixa.
- **Etapas:** (1) protocolo + teste no aparelho (quantos LEDs por analógico, ordem, lados); (2) cor fixa, presets, brilho, tela; (3) efeitos e Bateria; (4) Ambilight.
- **Travamento do microcontrolador (resolvido, 2026-11-09):** com a Ambilight, as luzes congelavam (ou apagavam) no começo do jogo e só voltavam desligando o aparelho; nem cortar a energia pelo `rgb:kbd_backlight` destravava. O chip responde `XXH_002` a cada pacote aceito, o que permitiu ver o momento exato: o serviço mandava rajadas (um pacote a cada ~5 ms), porque cada cor do jogo acordava o laço e disparava um envio. Parado no menu, o chip aguenta as rajadas; com o jogo rodando, não. Agora: relógio fixo, no máximo um pacote a cada 40 ms; a energia das luzes nunca é desligada (apagar = cor preta); 4 pacotes sem resposta e só vai um pacote de teste a cada meio segundo. Resumo em `/run/pingo/leds.state`.
