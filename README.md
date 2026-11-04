# Pingo OS by Retrogamer Brasil

Uma distro Linux mínima para consoles portáteis retrô. Grave a `.img` no cartão SD, ligue o aparelho e jogue.

A interface segue a filosofia do MinUI: direta ao ponto, sem boxart, temas ou menus de emulador. O sistema roda em uma partição somente leitura, então um cartão corrompido se resolve gravando a imagem de novo.

> O nome vem do Pingo, o cachorro de infância do autor: curto, com cara de mascote, e representa algo pequeno.

Um projeto do Retrogamer Brasil.

Gostou do projeto? Ajude a manter o desenvolvimento:

<a href="https://www.buymeacoffee.com/wiawfsakny"><img src="https://img.buymeacoffee.com/button-api/?text=Buy%20me%20a%20coffee&emoji=%E2%98%95&slug=wiawfsakny&button_colour=FFDD00&font_colour=000000&font_family=Cookie&outline_colour=000000&coffee_colour=ffffff" alt="Buy me a coffee" height="42"></a>

## Aparelhos

| Aparelho | SoC | Estado |
|---|---|---|
| Anbernic RG35XX-H | Allwinner H700 | **Fase 0** (prova de conceito) |
| Anbernic RG40XX-H / V | Allwinner H700 | Fase 2 (mesmo SoC) |
| TrimUI Brick | Allwinner A133P | Fase 3 |

## Como compilar

Requisitos: Linux, Docker e uns 30 GB livres. O build roda dentro de um contêiner Debian, então o toolchain do seu sistema não interfere.

```sh
scripts/build.sh            # RG35XX-H, imagem completa
```

A imagem sai em `output/rg35xxh/images/pingo-os-rg35xxh.img` (e `.img.xz`). Grave com o balenaEtcher, o Raspberry Pi Imager ou:

```sh
sudo dd if=output/rg35xxh/images/pingo-os-rg35xxh.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Outros alvos do Buildroot: `scripts/build.sh rg35xxh menuconfig`, `scripts/build.sh rg35xxh linux-rebuild all`.

## Estrutura

```
docs/                  especificação, mockup da interface, notas de cada fase, BIOS (bios.md)
docker/                ambiente de build
frontend/              pingo-retro, o frontend libretro (Fase 0: mínimo)
scripts/build.sh       build dentro do Docker
scripts/sync-rocknix.sh  importa patches, device trees e firmwares do ROCKNIX
external/              árvore BR2_EXTERNAL do Buildroot
  configs/             um defconfig por aparelho
  board/h700/          kernel, U-Boot, TF-A, imagem e overlay da raiz (família H700)
  package/             pacotes próprios
```

## Créditos

- [ROCKNIX](https://github.com/ROCKNIX/distribution): kernel, patches e device trees do H700 (GPL-2.0). O commit importado fica em `external/board/h700/ROCKNIX_COMMIT`.
- [rocknix-joypad](https://github.com/hackbar/rocknix-joypad): driver dos controles.
- [Buildroot](https://buildroot.org), [U-Boot](https://u-boot.org), [Trusted Firmware-A](https://www.trustedfirmware.org).
- [MinUI](https://github.com/shauninman/MinUI): referência de interface.
- [libretro](https://www.libretro.com): API dos cores (`libretro.h`, MIT) e o core [gambatte](https://github.com/libretro/gambatte-libretro).

## Licença

GPL-2.0. Componentes de terceiros mantêm as próprias licenças.

**Cores com licença não comercial.** Snes9x, Genesis Plus GX, PicoDrive, FBNeo e blueMSX só podem ser distribuídos sem fins lucrativos. A imagem do Pingo OS é gratuita e pode incluí-los; **vender** a imagem, cartões ou aparelhos com esses cores não é permitido pelas licenças deles.
