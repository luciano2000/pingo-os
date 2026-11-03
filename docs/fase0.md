# Fase 0: prova de conceito (RG35XX-H)

Objetivo: provar que kernel mainline + Buildroot sobem no RG35XX-H com tela, botões, som, bateria, Wi-Fi e suspend, e rodar um core libretro com um frontend mínimo.

## Decisões tomadas

| Tema | Decisão | Motivo |
|---|---|---|
| Buildroot | 2026.08.1 (hash conferido com o `.sign` oficial) | Versão estável mais recente |
| Kernel | Linux 7.2 + patches do ROCKNIX | Mesma versão que o ROCKNIX usa no H700 |
| U-Boot | 2026.01 + patches e defconfig LPDDR4 do ROCKNIX | O ROCKNIX mantém um defconfig com os parâmetros de DRAM do H700 |
| TF-A | 2.12.0 (`sun50i_h616`) | Versão em que o patch de suspend do ROCKNIX se aplica |
| Toolchain | Bootlin aarch64 glibc estável | Economiza ~20 min de build |
| Init | BusyBox + eudev | Boot rápido; eudev para hotplug de cartão e controles |
| Raiz | squashfs zstd, somente leitura, `root=PARTUUID=50494e47-02` | Já é o modelo final da Fase 1 |
| Build | Dentro do Docker (Debian trixie) | O GCC 16 do host quebra pacotes antigos do Buildroot |
| GPU | Fica para a etapa 2 da Fase 0 | O Panfrost no Mesa atual exige LLVM/Clang, que pesa horas e ~10 GB |

## O que veio do ROCKNIX

Importado por `scripts/sync-rocknix.sh` no commit registrado em `external/board/h700/ROCKNIX_COMMIT`:

- 35 patches de kernel: painel via `panel-mipi-dpi-spi`, PWM do backlight, HDMI, OPPs de CPU/GPU, driver de controle, vibração, LEDs RGB, OTG, segundo cartão SD sempre alimentado e vários ajustes de suspend (codec, display, pads do LCD).
- Device trees das variantes de painel (ex.: `rg35xx-h-rev6-panel`) e dos outros aparelhos H700.
- Sequências de inicialização dos painéis (`.panel`), embutidas no kernel.
- Config de kernel completa do H700 (`linux-rocknix.config`), com nossos ajustes em `linux-pingo.fragment`.
- Patch do TF-A para **suspend real** (PSCI SYSTEM_SUSPEND com um stub em SRAM). Já aplicado, mas inativo até compilarmos o stub.

## Achados que mudam a especificação

1. **Suspend profundo existe.** O ROCKNIX implementou PSCI SYSTEM_SUSPEND no H700: o TF-A copia um pequeno programa para a SRAM, que coloca a DRAM em self-refresh. Isso reduz bastante o risco da seção 7. Para ativar: empacotar o [h700-suspend-stub](https://github.com/MagnominusGit/h700-suspend-stub) e compilar o TF-A com `SUNXI_SYSTEM_SUSPEND=1`.
2. **Existem revisões de hardware.** Há unidades com DRAM LPDDR3 e com outro painel ("rev6"). O ROCKNIX gera uma imagem por tipo de DRAM e coloca todos os DTBs juntos. Por enquanto geramos só LPDDR4 e o `extlinux.conf` aponta para o DTB padrão; trocar para o `-rev6-panel` é editar uma linha no PC.
3. **Uma imagem só para RG35XX-H e RG40XX é viável.** Mesmo U-Boot (LPDDR4), DTBs diferentes. Falta um jeito de escolher o DTB sozinho no boot (o ROCKNIX tem um patch `rocknix-dt-id` para isso).

## Layout da imagem

| Região | Conteúdo |
|---|---|
| 8 KB | SPL + U-Boot (`u-boot-sunxi-with-spl.bin`) |
| p1, 16 MB, 64 MB | `PINGOBOOT` (FAT32): `Image`, `dtbs/`, `extlinux/extlinux.conf`, `pingo-wifi.txt` |
| p2, 512 MB | Sistema (squashfs) |
| resto | `ROMS` (exFAT), criada no primeiro boot na Fase 1 |

## Como testar no aparelho

1. Grave a imagem no cartão.
2. Para testar o Wi-Fi: no PC, abra `pingo-wifi.txt` na partição `PINGOBOOT` e preencha `SSID=` e `SENHA=`.
3. Ligue o aparelho. Deve aparecer o log do kernel na tela e, no fim, o login.
4. Pela rede: `ssh root@pingo` (ou o IP), senha `pingo`.
5. Rode `pingo-hwtest` e anote o resultado de cada item.
6. Suspend: `pingo-suspend-test 600` com o carregador desconectado.

Console serial: UART a 115200 baud nos pads da placa (`ttyS0`). Ajuda muito se o boot travar antes da tela.

## Checklist

- [ ] Build completo da imagem
- [ ] U-Boot sobe (LED / serial)
- [ ] Kernel sobe, log na tela
- [ ] Botões e analógicos (`evtest`)
- [ ] Áudio (`speaker-test`)
- [ ] Bateria (`pingo-hwtest bateria`)
- [ ] Wi-Fi (`pingo-hwtest wifi`)
- [ ] Segundo cartão SD
- [ ] Suspend s2idle: consumo medido
- [ ] Suspend deep (stub SRAM): consumo medido
- [ ] Etapa 2: Mesa/Panfrost + SDL2 KMSDRM + `kmscube`
- [ ] Etapa 2: frontend mínimo rodando o gambatte
