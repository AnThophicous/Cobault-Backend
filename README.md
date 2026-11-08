# Cobault-Backend

Baixador de mídia escrito em C. O programa `cobault` recebe uma ou mais URLs e
grava cada arquivo no disco. Aceita arquivos HTTP(S), playlists HLS (com ou sem
criptografia AES-128) e vídeos do YouTube, obtidos pela API interna do player.

O Cobault pretende ser o backend de download de um reprodutor de música para
Linux. A interface não está neste repositório.

## Escopo

O Cobault não pretende competir com o yt-dlp. O yt-dlp suporta centenas de
sites, usa cookies e login, baixa legendas, grava metadados e faz
pós-processamento. O Cobault cobre um conjunto menor de origens (HTTP direto,
HLS e alguns formatos do YouTube) e se concentra em conexões paralelas,
retomada e uso baixo de CPU e memória. Para cobertura ampla, use o yt-dlp.

As comparações desta página medem o custo de cada abordagem em cenários
definidos. Elas não avaliam as ferramentas como um todo.

## Requisitos

- Linux com gcc ou clang (C11)
- libcurl
- OpenSSL (libcrypto)
- pkg-config e make
- ffmpeg, opcional: usado só para juntar vídeo e áudio de formatos separados

Testado com libcurl 8.22.0 e OpenSSL 3.6.5. No Debian e no Ubuntu:

```sh
sudo apt install build-essential pkg-config libcurl4-openssl-dev libssl-dev
```

## Compilação

```sh
make               # gera build/cobault
make test          # testes unitários, sem rede
make check         # testes unitários e de integração com servidor local
sudo make install  # instala em /usr/local/bin/cobault
```

`make check` usa `python3`, `curl` e o binário `openssl`.

## Uso

```sh
cobault https://exemplo.com/arquivo.mp4
cobault -F https://exemplo.com/playlist.m3u8
cobault -f 18 -N 8 -s 1M "https://www.youtube.com/watch?v=VIDEO_ID"
cobault -f ba -o "musicas/%(title)s [%(id)s].%(ext)s" "https://www.youtube.com/watch?v=VIDEO_ID"
```

Sem `-F`, o seletor (`-f`) escolhe o formato e o arquivo é gravado com o nome
definido por `-o`. Os diretórios do modelo são criados quando não existem. O
caminho de cada arquivo final sai na saída padrão, uma linha por arquivo, o que
facilita o uso em scripts.

Em um terminal, uma barra mostra o progresso, a velocidade média e o tempo
restante. Ao receber SIGINT (Ctrl+C), os blocos em andamento são descartados e o
estado dos blocos concluídos fica em `NOME.part.state`. Rodar o mesmo comando de
novo continua de onde parou. O arquivo recebe o nome definitivo só quando está
completo.

Quando o seletor pede vídeo e áudio separados (`A+B`) e o ffmpeg está
disponível, os dois são baixados e juntados com `ffmpeg -c copy`. A saída é
`.mp4` quando vídeo e áudio são mp4 e m4a, e `.mkv` nos outros casos.

Sem ffmpeg, ou com `--no-merge`, cada trilha vira um arquivo. Se o modelo de nome
der o mesmo nome às duas, a segunda recebe o id do formato antes da extensão.
Por exemplo, vídeo e áudio em webm com o modelo padrão geram
`NOME [ID].webm` e `NOME [ID].f249.webm`.

## Opções

```
-F, --list-formats      lista os formatos disponíveis e sai
-f, --format SEL        seletor de formato (padrão: bv+ba/b)
-o, --output MODELO     nome de saída (padrão: "%(title)s [%(id)s].%(ext)s")
                        campos: %(id)s %(title)s %(ext)s %(extractor)s
                                %(format_id)s %(height)s
-N, --jobs N            conexões paralelas por download (padrão: 8, máximo: 64)
-s, --chunk-size TAM    bloco de Range, de 64k a 64M (padrão: 1M)
-R, --retries N         tentativas extras por requisição (padrão: 5, máximo: 20)
    --client NOME       cliente do YouTube: auto, visionos, ios ou android (padrão: auto)
    --ffmpeg CAMINHO    ffmpeg usado na mesclagem (padrão: o do PATH)
    --no-merge          mantém vídeo e áudio em arquivos separados
-q, --quiet             mostra só erros
-v, --verbose           mostra mensagens de depuração
-V, --version           mostra a versão
-h, --help              mostra a ajuda
```

Códigos de saída: `0` sucesso; `1` falha em pelo menos uma URL; `2` uso incorreto,
inclusive seletor com sintaxe inválida; `130` interrompido por SIGINT ou SIGTERM.

## Seletor de formatos (`-f`)

| Forma | Significado |
|---|---|
| `best`, `b` | melhor formato; formatos muxados (vídeo e áudio no mesmo arquivo) vêm antes |
| `worst`, `w` | pior formato, com a mesma preferência por muxados |
| `bv`, `bestvideo` | melhor formato só de vídeo; `bv*` também aceita muxados |
| `ba`, `bestaudio` | melhor formato só de áudio; `ba*` também aceita muxados |
| `wv`, `wa` | pior formato só de vídeo e pior só de áudio; `wv*` e `wa*` aceitam muxados |
| `ID` | formato pelo id exato, como `18` ou `hls-2` |
| `[campo op valor]` | filtro aplicado à forma anterior. Campos: `height`, `width`, `fps`, `tbr`, `filesize`, `ext`, `format_id`. Operadores: `<`, `<=`, `>`, `>=`, `=`, `!=` |
| `A+B` | vídeo de A com áudio de B, juntados com ffmpeg |
| `A/B` | alternativa: a primeira que casar vence |

Padrão: `bv+ba/b`. O seletor procura o melhor vídeo e o melhor áudio para juntar;
se não houver par, usa o melhor formato. Exemplos:

```sh
cobault -f "bv[height<=720]+ba/b[height<=720]" URL
cobault -f "b[format_id=18]" URL
```

Na seleção implícita, formatos maiores que o limite que o cliente libera são
ignorados. Hoje só o cliente IOS do YouTube define esse limite, e a coluna NOTA
de `-F` mostra `limite do cliente` nesses formatos. Antes de baixar, o Cobault
testa a cauda de cada formato escolhido, pedindo os últimos 16 KiB. Se a cauda
não for entregue, o formato é marcado como indisponível e a seleção é refeita.
Pedidos pelo id exato não passam pela verificação de tamanho, mas a cauda
continua sendo testada.

## Eficiência

Cada mecanismo abaixo fica no módulo que o implementa.

- `src/download/direct.c`: cada bloco de Range é gravado com `pwrite` no offset
  correto do `.part`, direto do callback de escrita do libcurl
  (`src/net/http.c`). Não há buffer com o arquivo inteiro nem etapa de
  remontagem. O `.part` é pré-alocado com `ftruncate`.
- `src/download/direct.c`: o arquivo é dividido em blocos de `-s` bytes, baixados
  por até `-N` conexões. Um bloco com erro é refeito sozinho, sem reiniciar o
  download.
- `src/download/direct.c`: a primeira requisição pede `Range: bytes=0-0`. Uma
  resposta 206 informa o tamanho total sem baixar um bloco inteiro antes do
  paralelismo. Se o servidor responder 200, ignorando o Range, o corpo inteiro é
  gravado nessa mesma requisição.
- `src/download/direct.c`: depois de cada bloco, o bitmap de blocos concluídos é
  regravado em arquivo temporário seguido de `rename` (`src/download/common.c`).
  Ctrl+C perde apenas os blocos em andamento.
- `src/net/http.c`: um `CURLSH` é compartilhado por todos os workers. Conexões
  TCP e TLS, cache de DNS, sessões TLS e a lista de sufixos públicos são
  reaproveitados entre eles. Cada worker tem um handle próprio, com keep-alive, e
  usa HTTP/2 quando o servidor aceita.
- `src/net/http.c`: uma conexão com menos de 1 byte/s durante 30 s é abortada e
  entra no retry. Não há limite de tempo total, então arquivos grandes não são
  cortados.
- `src/download/hls.c`: os segmentos são baixados em paralelo e decifrados em
  memória. A thread que chama `cb_download_hls` grava os segmentos na ordem. Um
  segmento que esteja mais de `max(4, 4 × N)` posições à frente do escritor
  espera, o que limita a memória em uso. As chaves AES-128 são baixadas uma vez
  por URL.
- `src/net/pool.c`: a fila é FIFO. Tarefas de índice menor começam antes das
  maiores. Sem essa ordem, workers ocupados com segmentos distantes poderiam
  impedir o início do segmento que o escritor espera.
- `src/core/arena.c`: metadados de mídia, nós JSON e listas de playlist ficam em
  blocos de arena e são liberados de uma vez com `cb_arena_free()`. A arena não
  zera a memória, então os nós são inicializados explicitamente.
- `src/core/json.c`: o parser lê direto do buffer da resposta. Cada string é
  decodificada uma vez, dentro da arena.
- `src/core/progress.c`: o contador de bytes é atômico, e a barra é redesenhada a
  cada 250 ms em uma thread própria. O caminho de escrita dos downloads não usa
  lock por pedaço recebido. A velocidade exibida é uma média móvel exponencial.
- `src/download/mux.c`: `ffmpeg -c copy` roda com `fork` e `execvp`, sem shell.
  Vídeo e áudio são copiados sem recodificação.

## Medições

As medições foram feitas em um Intel Celeron N4100 (4 núcleos, 1,10 GHz), com o
desktop em uso e outros programas abertos, o que faz o tempo variar entre
rodadas. Cada ferramenta rodou 5 vezes, e a tabela traz a mediana. Foram medidos
o tempo de parede, o tempo de CPU (usuário mais sistema) e o pico de memória
residente (RSS). Os arquivos baixados foram comparados byte a byte com a
referência.

Versões: yt-dlp 2026.10.26, aria2c 1.37.0, ffmpeg 6.1.6.

Estas medições comparam o custo de cada abordagem em cenários definidos. O yt-dlp
faz muito mais que transferir arquivos, e um teste de transferência não mede essas
funções. O Cobault não pretende competir com ele.

### Cenário local

Arquivo de 64 MiB servido em localhost por `tests/cbserve.py --kbps 2048`, que
limita cada conexão a 2 MiB/s. O cenário imita CDNs que limitam a taxa por
conexão. Se o servidor limitar a banda total do cliente, o paralelismo deixa de
ajudar, como mostra a seção do YouTube.

Comandos, com `DIR` sendo a pasta de destino e `URL` o endereço do servidor:

```sh
cobault -q -N 8 -s 1M -o DIR/file.bin URL
aria2c -x8 -s8 -k1M --allow-overwrite=true --console-log-level=error -d DIR -o file.bin URL
yt-dlp -q --no-warnings --no-part -o DIR/file.bin URL
yt-dlp --downloader aria2c --downloader-args "aria2c:-x8 -s8 -k1M" -q --no-warnings --no-part -o DIR/file.bin URL
ffmpeg -y -loglevel error -nostdin -f s16le -i URL -c copy -f s16le DIR/file.bin
```

Os alvos com `-N 1` usam o mesmo comando do Cobault, trocando `-N 8` por `-N 1`.
A entrada e a saída do ffmpeg usam o formato `s16le`, que copia os bytes sem
reempacotar o conteúdo. O yt-dlp foi chamado pelo módulo Python, com os mesmos
argumentos da linha de comando.

| Ferramenta | Tempo | CPU | RSS máximo | Conexões |
|---|---:|---:|---:|---:|
| Cobault `-N 8` | 3,97 s | 0,19 s | 14,1 MB | 8, blocos de Range |
| aria2c `-x8 -s8 -k1M` | 4,04 s | 0,22 s | 24,5 MB | 8 |
| yt-dlp + aria2c `-x8 -s8 -k1M` | 5,92 s | 2,07 s | 51,6 MB | 8 |
| ffmpeg `-c copy` | 32,09 s | 1,56 s | 30,1 MB | 1 |
| Cobault `-N 1` | 32,13 s | 0,24 s | 14,1 MB | 1 |
| yt-dlp padrão | 33,91 s | 2,09 s | 57,7 MB | 1 |

Com 8 conexões, o Cobault e o aria2c ficam praticamente empatados. A diferença de
0,07 s está dentro da variação entre rodadas: o Cobault variou de 3,88 s a 4,34 s,
e o aria2c, de 4,04 s a 4,59 s. O Cobault usou cerca de 40% menos memória que o
aria2c.

Com uma conexão, o Cobault e o ffmpeg levam o mesmo tempo, porque os dois baixam
em sequência. Nesse caso, o Cobault gastou 0,24 s de CPU, contra 1,56 s do
ffmpeg.

O yt-dlp, com o downloader padrão, baixa em uma conexão e leva 33,9 s. Com o
aria2c como downloader externo, o tempo cai para 5,9 s, mas o consumo de CPU e de
memória continua acima do Cobault e do aria2c usados sozinhos.

### Áudio do YouTube

Vídeo `aqz-KE-bpKQ` (634 s), formato 251 (Opus, 10,2 MB), 5 execuções por
ferramenta. As duas saídas são idênticas à referência do yt-dlp.

```sh
cobault -q -f 251 -o DIR/file.bin URL
yt-dlp -q --no-warnings --no-part -f 251 -o DIR/file.bin URL
```

| Ferramenta | Tempo | CPU | RSS máximo |
|---|---:|---:|---:|
| Cobault `-f 251` | 5,32 s | 0,67 s | 17,6 MB |
| yt-dlp `-f 251` | 6,35 s | 1,84 s | 59,7 MB |

A primeira execução do yt-dlp levou 11,6 s. As outras quatro ficaram entre 5,9 s e
6,4 s, e a mediana é 6,35 s. O Cobault variou de 5,15 s a 5,51 s. Os tempos
incluem a extração (consultas à API do player) e dependem da velocidade que o
YouTube entrega no momento. A diferença maior está em CPU e memória.

## YouTube

A extração usa a API interna do player com três clientes: VISIONOS, IOS e
ANDROID. No modo `--client auto` (padrão), os três são consultados nessa ordem e
os formatos são somados. Se um itag já veio de um cliente anterior, o cliente
seguinte não o adiciona de novo. Itags repetidos dentro do mesmo cliente, como as
variantes de idioma ou de DRC, recebem sufixo: `140-2`, `251-2`.

- VISIONOS: formatos adaptativos (vídeo e áudio) com URL direta. Nas medições não
  houve corte de faixa: um vídeo de 150 MB baixou inteiro. O cliente exige
  `visitorData`. Sem ele, a resposta é `LOGIN_REQUIRED`. O Cobault repete o pedido
  uma vez com o valor que a própria resposta recusada traz.
- IOS: os mesmos formatos, com faixa inicial limitada: cerca de 13 MiB em vídeo e
  cerca de 1 MiB em áudio. Pedidos além desse limite voltam com 403. A cauda de
  cada formato escolhido é testada antes do download, e um formato que não fecha é
  trocado pela próxima opção.
- ANDROID: apenas o formato muxado 18, geralmente 360p. Um vídeo de 10 minutos
  baixou inteiro, com 28,5 MB.

Só áudio: `-f ba`, ou o id de um formato de áudio como `-f 251`, baixa somente a
trilha de áudio. A extração ainda consulta a API do player, mas não há download de
vídeo, ffmpeg nem mesclagem, porque o seletor devolve um único formato.

Vídeo e áudio separados (`-f bv+ba`) são baixados em dois arquivos temporários e
juntados com `ffmpeg -c copy`. Sem ffmpeg, ficam em dois arquivos, nomeados como
descrito na seção de uso.

Não implementado: decifragem de `signatureCipher`, que exige executar o JavaScript
do player, e os PO tokens. Formatos que dependem disso são pulados. Com `-v`, a
saída informa quantos foram pulados. Em vídeos longos e em altas resoluções, pode
não haver formato completo disponível sem essas partes.

Nas medições, observou-se um teto de cerca de 2,7 MB/s por IP, independentemente do
número de conexões: com `-N 1` e com `-N 8`, o tempo foi o mesmo. Nesse caso, o
paralelismo não acelera o download.

## Testes

- `make test` roda `tests/test_core.c`, com 86 verificações sobre a arena, o parser
  JSON (escapes e pares substitutos), URLs relativas, tamanhos, modelos de nome,
  playlists m3u8 (master e mídia) e o seletor de formatos.
- `make check` roda `tests/integration.sh` contra `tests/cbserve.py`, um servidor
  local com Range, limite por conexão, respostas 503 injetadas e modo sem Range.
  São 10 verificações, entre elas: download paralelo, HLS sem criptografia e com
  AES-128, fallback sem Range, retentativas, SIGINT com retomada e URL inacessível.

## Estrutura do código

```
src/
  main.c               interface de linha de comando e fluxo por URL
  core/
    arena.c, json.c    memória por arena e parser JSON
    util.c             logs, URLs, tamanhos, modelos de nome, backoff
    progress.c         barra de progresso com velocidade suavizada
  net/
    http.c             libcurl: CURLSH, handle por worker, retentativas
    pool.c             pool de threads com fila FIFO
  media/
    media.c            modelo de mídia e dono da arena
    format.c           seletor de formatos e tabela de -F
  extract/
    extract.c          despacho entre YouTube, HLS e arquivo direto
    youtube.c          clientes VISIONOS, IOS e ANDROID
    m3u8.c             parser de playlists HLS
  download/
    direct.c           Range paralelo, retomada e teste de cauda
    hls.c              segmentos em paralelo, escrita ordenada e AES-128
    mux.c              ffmpeg -c copy via fork e execvp
    common.c           escrita atômica de arquivos de estado
tests/
  test_core.c          testes unitários
  integration.sh       testes de integração
  cbserve.py           servidor HTTP local para os testes
```

## Limitações

- Sem decifragem de `signatureCipher` e sem PO tokens (YouTube).
- Sem transmissão ao vivo. Playlists em andamento baixam só os segmentos
  disponíveis no momento, com aviso no log.
- HLS com criptografia que não seja AES-128 (por exemplo, SAMPLE-AES) é recusado
  com mensagem de erro.
- Sem legendas, sem gravação de metadados no arquivo, sem cookies e sem login.
- A única etapa de pós-processamento é a mesclagem com `ffmpeg -c copy`.

## Aviso de uso

O Cobault baixa conteúdo de terceiros. Use-o apenas com conteúdo que você tem
direito de baixar ou que esteja licenciado para esse uso. Os Termos de Serviço do
YouTube proíbem baixar conteúdo, salvo quando a própria plataforma exibe um link de
download para ele ou quando há permissão prévia por escrito da plataforma e, se for
o caso, do titular dos direitos. A responsabilidade pelo uso da ferramenta é de
quem a executa.

## Licença

MIT. O texto completo está em `LICENSE`. Créditos e origem das informações de
referência estão em `NOTICE.md`.
