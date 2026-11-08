#!/bin/sh
# Testes de integração do Cobault contra um servidor HTTP local.
#   sh tests/integration.sh /caminho/para/cobault
set -eu

BIN="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cobault-it.XXXXXX")
WWW="$WORK/www"
OUT="$WORK/out"
mkdir -p "$WWW/hls" "$OUT"

PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

FAILS=0
PASSES=0
pass() { PASSES=$((PASSES + 1)); echo "  ok   - $1"; }
fail() { FAILS=$((FAILS + 1)); echo "  FALHA - $1"; }

free_port() {
    python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

start_server() {  # $1 = porta, resto = opções do servidor
    port="$1"; shift
    python3 -I "$HERE/cbserve.py" "$WWW" "$port" "$@" >"$WORK/server-$port.log" 2>&1 &
    PIDS="$PIDS $!"
    i=0
    while ! curl -s -o /dev/null "http://127.0.0.1:$port/" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -gt 100 ] && { echo "servidor não subiu na porta $port"; exit 1; }
        sleep 0.05
    done
}

echo "== preparando dados"
dd if=/dev/urandom of="$WWW/file.bin" bs=1M count=8 2>/dev/null
dd if=/dev/urandom of="$WWW/resume.bin" bs=1M count=24 2>/dev/null

# Segmentos em claro (512 KiB cada) e versões cifradas com AES-128-CBC.
KEY_HEX=$(python3 -I -c 'import os; print(os.urandom(16).hex())')
printf '%s' "$KEY_HEX" | python3 -I -c 'import sys,binascii; sys.stdout.buffer.write(binascii.unhexlify(sys.stdin.read()))' > "$WWW/hls/key.bin"
N=6
i=0
while [ "$i" -lt "$N" ]; do
    dd if=/dev/urandom of="$WWW/hls/seg$i.ts" bs=512K count=1 2>/dev/null
    IV=$(printf '%032x' "$i")
    openssl enc -aes-128-cbc -K "$KEY_HEX" -iv "$IV" \
        -in "$WWW/hls/seg$i.ts" -out "$WWW/hls/enc$i.ts"
    i=$((i + 1))
done

{
    echo "#EXTM3U"; echo "#EXT-X-VERSION:3"; echo "#EXT-X-TARGETDURATION:4"
    echo "#EXT-X-MEDIA-SEQUENCE:0"
    i=0; while [ "$i" -lt "$N" ]; do echo "#EXTINF:4.0,"; echo "seg$i.ts"; i=$((i + 1)); done
    echo "#EXT-X-ENDLIST"
} > "$WWW/hls/plain.m3u8"

{
    echo "#EXTM3U"; echo "#EXT-X-VERSION:3"; echo "#EXT-X-TARGETDURATION:4"
    echo "#EXT-X-MEDIA-SEQUENCE:0"
    echo "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\""
    i=0; while [ "$i" -lt "$N" ]; do echo "#EXTINF:4.0,"; echo "enc$i.ts"; i=$((i + 1)); done
    echo "#EXT-X-ENDLIST"
} > "$WWW/hls/enc.m3u8"

{
    echo "#EXTM3U"
    echo "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=640x360,CODECS=\"avc1.42001e,mp4a.40.2\""
    echo "plain.m3u8"
    echo "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,CODECS=\"avc1.64001f,mp4a.40.2\""
    echo "enc.m3u8"
} > "$WWW/hls/master.m3u8"

# Referência do HLS: concatenação dos segmentos em claro.
i=0; : > "$WORK/hls-expected.ts"
while [ "$i" -lt "$N" ]; do cat "$WWW/hls/seg$i.ts" >> "$WORK/hls-expected.ts"; i=$((i + 1)); done

echo "== servidor normal (Range, sem limite)"
PORT=$(free_port)
start_server "$PORT"
BASE="http://127.0.0.1:$PORT"

echo "== download direto paralelo"
if "$BIN" -q -N 8 -s 512k -o "$OUT/direct.bin" "$BASE/file.bin" >"$WORK/d.out" 2>&1 \
   && cmp -s "$WWW/file.bin" "$OUT/direct.bin"; then
    pass "8 MiB em blocos de 512 KiB idênticos ao original"
else
    fail "download direto paralelo"; cat "$WORK/d.out" || true
fi

echo "== listagem de formatos (HLS master)"
if "$BIN" -F "$BASE/hls/master.m3u8" | grep -q "hls-1" && \
   "$BIN" -F "$BASE/hls/master.m3u8" | grep -q "hls-0"; then
    pass "-F lista as variantes HLS"
else
    fail "-F não listou as variantes"
fi

echo "== HLS sem criptografia"
if "$BIN" -q -N 4 -f hls-0 -o "$OUT/plain.ts" "$BASE/hls/master.m3u8" >"$WORK/h.out" 2>&1 \
   && cmp -s "$WORK/hls-expected.ts" "$OUT/plain.ts"; then
    pass "segmentos gravados em ordem"
else
    fail "HLS em claro"; cat "$WORK/h.out" || true
fi

echo "== HLS com AES-128"
if "$BIN" -q -N 4 -f hls-1 -o "$OUT/enc.ts" "$BASE/hls/master.m3u8" >"$WORK/e.out" 2>&1 \
   && cmp -s "$WORK/hls-expected.ts" "$OUT/enc.ts"; then
    pass "decifragem AES-128 correta"
else
    fail "HLS AES-128"; cat "$WORK/e.out" || true
fi

echo "== tentativa repetida (ignora o arquivo já existente)"
if "$BIN" -q -o "$OUT/direct.bin" "$BASE/file.bin" >/dev/null 2>&1; then
    pass "arquivo existente é pulado sem erro"
else
    fail "pular arquivo existente"
fi

echo "== servidor sem Range (200 com corpo inteiro)"
PORT2=$(free_port)
start_server "$PORT2" --no-range
if "$BIN" -q -o "$OUT/norange.bin" "http://127.0.0.1:$PORT2/file.bin" >/dev/null 2>&1 \
   && cmp -s "$WWW/file.bin" "$OUT/norange.bin"; then
    pass "fallback de fluxo único funciona"
else
    fail "fallback sem Range"
fi

echo "== servidor instável (503 a cada 4 requisições)"
PORT3=$(free_port)
start_server "$PORT3" --fail-every 4
if "$BIN" -q -R 5 -N 4 -s 256k -o "$OUT/flaky.bin" "http://127.0.0.1:$PORT3/file.bin" >/dev/null 2>&1 \
   && cmp -s "$WWW/file.bin" "$OUT/flaky.bin"; then
    pass "retentativas com backoff recuperam os 503"
else
    fail "retentativas"
fi

echo "== interrupção (SIGINT) e retomada"
PORT4=$(free_port)
start_server "$PORT4" --kbps 1024
"$BIN" -N 4 -s 512k -o "$OUT/resume.bin" "http://127.0.0.1:$PORT4/resume.bin" \
    >"$WORK/r1.out" 2>&1 &
RPID=$!
sleep 2
kill -INT "$RPID" 2>/dev/null || true
RC=0
wait "$RPID" || RC=$?
if [ "$RC" -eq 130 ] && [ -f "$OUT/resume.bin.part" ] && [ -f "$OUT/resume.bin.part.state" ] \
   && [ ! -f "$OUT/resume.bin" ]; then
    pass "SIGINT sai com 130 e deixa .part + .state"
else
    fail "interrupção (rc=$RC)"; cat "$WORK/r1.out" || true
fi

if "$BIN" -q -N 8 -s 512k -o "$OUT/resume.bin" "http://127.0.0.1:$PORT4/resume.bin" >"$WORK/r2.out" 2>&1 \
   && cmp -s "$WWW/resume.bin" "$OUT/resume.bin" && [ ! -f "$OUT/resume.bin.part.state" ]; then
    pass "retomada conclui com conteúdo idêntico e limpa o estado"
else
    fail "retomada"; cat "$WORK/r2.out" || true
fi

echo "== erro esperado"
if "$BIN" -q "http://127.0.0.1:9/nada.bin" >/dev/null 2>&1; then
    fail "URL inacessível deveria falhar"
else
    pass "URL inacessível termina com erro"
fi

echo
echo "$PASSES ok, $FAILS falha(s)"
[ "$FAILS" -eq 0 ]
