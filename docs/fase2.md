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
- **Limites:** jogos em `.chd` (PS, Sega CD, PC Engine CD) não são identificados ainda (o leitor de CD do rcheevos só lê `.cue`/`.bin`/`.iso`); falta o libchdr. Sem imagens das conquistas (só texto).

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
- [x] Instalar ports gratuitos e jogar: Cave Story (nxengine-evo) e Ticoban com o direcional; Celeste Classic precisa do `taskset -c` (próxima imagem)
- [ ] Sair do port e voltar ao launcher
