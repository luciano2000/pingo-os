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

SSH: pela rede cabeada (`root`/`pingo`; a tela mostra o endereço). Com o
Wi-Fi ligado, o SSH tem de passar a aceitar só chave.

## Feito

- [x] Boot, rede, partição ROMS automática
- [x] Vídeo (vc4 KMS) em 1280×800, som pela Picade
- [x] FBNeo: Metal Slug a 59,8 fps (5% do quadro em média)
- [x] Botão de ligar desliga com segurança (S30pingo-power)

## Próximos

- [ ] pingo-retro: Picade/teclado como controle (gpio-keys), ESC = menu,
      KEY_POWER salva e desliga
- [ ] Galeria: Quicksand em pílula, fundo com o screenshot do jogo
      escurecido, vídeo à direita, fabricante/ano/jogadores, logo do
      Retrogamer no canto (`board/arcade/marca/`)
- [ ] Filtro "Clássicos": sem homebrew/demos/testes, hacks e bootlegs,
      mahjong/quiz/cassino, versões PCB repetidas, BIOS, clones; "Mostrar
      todos" e "Esconder este jogo" no menu
- [ ] Mídia: libretro-thumbnails (capas, telas) e ScreenScraper (vídeos,
      conta do usuário no cartão) pelo Wi-Fi
- [ ] Modo atração (vídeos aleatórios; "insira uma ficha" com a moeda)
- [ ] RetroAchievements (o ra.c do Pingo); login por teclado ou arquivo
- [ ] Wi-Fi, hora (NTP), tela de boot com o logo vertical
- [ ] Depois: micro arcade vertical na Pi Zero W (tela SPI 3,2" 320×240)
