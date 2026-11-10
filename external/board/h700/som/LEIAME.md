# Som ao ligar

O som que toca ao ligar o aparelho é um latido de cachorro, de
**scottishperson**, do Pixabay (arquivo
`scottishperson-sound-effect-dog-barking-and-growling-03-367318.mp3`), sob a
[licença do Pixabay](https://pixabay.com/service/license-summary/). Ele vai
dentro da imagem, mas **não fica neste repositório**: a licença não permite
redistribuir o arquivo sozinho.

Para o build incluir o som, ponha o WAV em `.cache/som/ligar.wav` (na raiz do
projeto; a pasta `.cache` não vai para o Git). Sem ele, a imagem sai sem som.

Formato: WAV PCM 16 bits, 48 kHz, até 3 s, pico perto de -1 dB, sem silêncio
no começo e com fade-out no fim. O original (5,9 s) foi cortado logo depois do
primeiro latido (2,3 s) com:

    ffmpeg -i scottishperson-sound-effect-dog-barking-and-growling-03-367318.mp3 \
      -af "atrim=0.18:2.5,asetpts=PTS-STARTPTS,afade=t=in:d=0.005,afade=t=out:st=2.0:d=0.32,volume=-1dB,aresample=48000:resampler=soxr" \
      -ac 2 -c:a pcm_s16le ligar.wav
