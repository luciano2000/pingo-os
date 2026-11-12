# Pingo Arcade (Raspberry Pi 3 B+, bartop)

Projeto irmão do Pingo OS: só arcade (FBNeo), num bartop com Picade X HAT
e tela de 10" (HannStar HSD101PWW1, 1280×800, placa PCB800099 por HDMI).
Mesmo repositório, mesmo frontend (pingo-retro), configuração própria:
`scripts/build.sh arcade` → `output/arcade/images/pingo-arcade-rpi3.img`.

## Hardware

- **Pi 3 B+**: kernel 6.12 da Raspberry Pi Foundation (`raspberrypi/linux`,
  o commit do `raspberrypi3_64_defconfig` do Buildroot), `bcm2711_defconfig`
  + `board/arcade/linux.fragment`. Firmware de boot da Broadcom
  (`bootcode.bin`, `start.elf`).
- **Picade X HAT**: overlay próprio (`board/arcade/pingo-picade.dts`; o da
  Pimoroni não declara licença). Botões viram teclas (as do MAME), ⏻ manda
  KEY_POWER e o GPIO 4 corta a energia no fim do desligamento. Som: PCM5102A
  no I2S (o nó tem que se chamar `pcm5102a-codec`).
- **Tela**: a placa não sinaliza hotplug; `video=HDMI-A-1:D` no
  `cmdline.txt` força a saída. O EDID anuncia 1280×800.
- **Energia**: fonte 12 V 5 A → placa da tela + conversor 12→5 V USB →
  micro-USB da Picade.

| Botão do gabinete | Borne da Picade | Tecla | Função |
|---|---|---|---|
| 6 de ação | 1-6 | Ctrl, Alt, espaço, Shift, Z, X | soco/chute (SF), A B C D (Neo Geo) |
| preto 1 | 1UP | 1 | start |
| preto 2 | ESC | Esc | menu |
| pequeno da frente | ¼ | 5 | ficha |
| azul de trás | ⏻ | KEY_POWER | liga/desliga |

## Cartão

p1 PINGOBOOT (FAT, 4 MiB), p2 sistema (squashfs, 68 MiB), p3 ROMS (exFAT),
criada e formatada sozinha no primeiro boot. Pastas: `Roms/` (os .zip do
FBNeo, com `neogeo.zip`), `Bios/fbneo/samples/`, `Midia/`, `Saves/`,
`States/`, `Config/`.

Atualizar sem perder os jogos: `sudo scripts/gravar-sistema.sh <img> /dev/sdX`
(lê os endereços da imagem e confere o cartão antes de gravar).

SSH: só por chave (o dropbear roda com `-s`; a senha `pingo` é pública).
As chaves autorizadas ficam no cartão, em `.system/ssh/authorized_keys`
(`/root/.ssh` aponta para lá). A chave de desenvolvimento fica no PC, em
`.cache/ssh/pingo-arcade` (fora do Git). O endereço aparece no menu da
galeria (Pingo Arcade <versão> · rede <ip>).

## Feito

- [x] Boot, rede, partição ROMS automática
- [x] Vídeo (vc4 KMS) em 1280×800, som pela Picade
- [x] FBNeo: Metal Slug a 59,8 fps (5% do quadro em média)
- [x] Botão de ligar desliga com segurança
- [x] pingo-retro: Picade/teclado como controle; menu do jogo nítido em
      1280×800 (fontes na resolução real); dicas com as cores do painel
- [x] Galeria (`pingo-arcade`): Quicksand em pílula, logo do Retrogamer,
      painel do jogo, fundo desfocado (quando houver `Midia/<zip>/snap.png`)
- [x] Listas: Clássicos, Todos, Favoritos, Mais jogados e gêneros do FBNeo
      (lidos dos drivers: `scripts/fbneo-drivers.py`)
- [x] Controle 2 (USB): reconhecido a quente, mapa por modelo
      (`Config/controles/<vid>-<pid>.cfg`), assistente na galeria,
      remapeamento por jogo no menu do jogo (Controle 1 / Controle 2)
- [x] "Carregando…" entre a galeria e o jogo (fb0 de 16 bits, console
      desligado)
- [x] Wi-Fi com teclado na tela (pingo-wifi), hora pela internet (ntpd)

Painel (fios na Picade): verde 1, azul 2, lilás 3, vermelho 4, amarelo 5,
laranja 6; preto de cima = ESC (menu), preto de baixo = 1UP, branco da
frente = ficha, azul de trás = ⏻. Na galeria e nos menus: amarelo
confirma, laranja volta, lilás troca a lista, vermelho favorita.
`gabinete=Nanorama` em `Config/arcade.cfg` dá o nome do controle 1.

## Próximos

- [ ] Conferir a imagem gravada em 12/11 (boot direto na galeria, Wi-Fi
      salvo conectando sozinho, hora certa) e medir o tempo de boot
- [ ] Perfil de botões do Neo Geo (A B C D = 1 2 3 4, o arco do MVS) e
      camada `neogeo.cfg`; "Botões" na galeria
- [ ] Mídia: libretro-thumbnails (capas, telas) e ScreenScraper (vídeos,
      conta do usuário no cartão)
- [ ] Vídeo no painel da galeria; modo atração ("insira uma ficha")
- [ ] Título do jogo no menu do jogo com o nome completo (hoje "mslug")
- [ ] RetroAchievements (o ra.c do Pingo); login pelo teclado na tela
- [ ] Tela de boot com o logo vertical
- [ ] Pequeno glitch de cor ao sair do jogo (quadro do jogo na troca)
- [ ] Depois: micro arcade vertical na Pi Zero W (tela SPI 3,2" 320×240,
      clone com ILI9341 + XPT2046)
