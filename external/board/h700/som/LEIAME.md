# Som ao ligar

O som que toca ao ligar o aparelho é "Magic Spell", de **Universfield**, do Pixabay
(arquivo `universfield-magic-spell-278824.mp3`), sob a
[licença do Pixabay](https://pixabay.com/service/license-summary/). Ele vai
dentro da imagem, mas **não fica neste repositório**: a licença não permite
redistribuir o arquivo sozinho.

Para o build incluir o som, ponha o WAV em `.cache/som/ligar.wav` (na raiz do
projeto; a pasta `.cache` não vai para o Git). Sem ele, a imagem sai sem som.

Formato: WAV PCM 16 bits, 48 kHz, até 3 s, pico perto de -1 dB, sem silêncio
no começo e com fade-out no fim. O original (`universfield-magic-spell-278824.mp3`,
5,9 s) foi cortado em 2,5 s com:

    ffmpeg -i universfield-magic-spell-278824.mp3 \
      -af "atrim=0:2.5,afade=t=in:d=0.005,afade=t=out:st=1.7:d=0.8,volume=2.5dB" \
      -ar 48000 -ac 2 -c:a pcm_s16le ligar.wav
