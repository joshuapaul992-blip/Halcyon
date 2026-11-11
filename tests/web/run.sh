#!/bin/sh
# Host tests of the web library: URLs, the trust anchors made by
# tools/mktrust.py, inflate (against Python's zlib/gzip and corrupt
# streams), and HTTP / HTTPS against local test servers (tests/web/server.py)
# with certificates from a test CA made here with openssl: good, expired,
# not yet valid, wrong host, self-signed, untrusted chain, an intermediate
# CA, an EC key, a TLS-1.3-only server; truncated answers, huge headers, bad
# chunked encoding, redirect loops, timeouts, size limits, cancellation.
#   tests/web/run.sh            (needs gcc, python3, openssl)
set -e
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
OUT=$ROOT/build/hostweb
make -s -C tests/web OUT=$OUT >/dev/null
T=$(mktemp -d "${TMPDIR:-/tmp}/webtest.XXXXXX")
SRV=
cleanup() { [ -n "$SRV" ] && kill $SRV 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM

fails=0
checks=0
pass() { checks=$((checks + 1)); }
fail() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL: $*"; }

echo "== unit tests"
$OUT/urltest || fails=$((fails + 1))
$OUT/tatest userland/third_party/cacert/cacert.pem || fails=$((fails + 1))
python3 -I tests/web/gen_inflate.py "$T/inf" >/dev/null
$OUT/inflatetest "$T/inf" || fails=$((fails + 1))
if [ -x $OUT/imagetest ]; then
    python3 -I tests/web/gen_images.py "$T/img" >/dev/null
    $OUT/imagetest "$T/img" || fails=$((fails + 1))
    $OUT/htmltest rootfs/Documents/Web/*.htm || fails=$((fails + 1))
fi

echo "== test certificates"
P=$T/pki
mkdir -p $P
O="openssl"
cat > $P/leaf.ext <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF
cat > $P/other.ext <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:other.example,DNS:www.other.example
EOF
cat > $P/ca.ext <<EOF
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
EOF
q() { "$@" >/dev/null 2>&1 || { echo "command failed: $*"; exit 1; }; }
q $O req -x509 -newkey rsa:2048 -nodes -keyout $P/ca.key -out $P/ca.pem -days 3650 -subj "/CN=Halcyon Test CA" \
    -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign
q $O req -x509 -newkey rsa:2048 -nodes -keyout $P/ca2.key -out $P/ca2.pem -days 3650 -subj "/CN=Untrusted Test CA" \
    -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign
leaf() {  # name signer-cert signer-key ext keytype [extra x509 options]
    n=$1; ca=$2; cak=$3; ext=$4; kt=$5; shift 5
    if [ "$kt" = ec ]; then q $O req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout $P/$n.key -out $P/$n.csr -subj "/CN=localhost"
    else q $O req -newkey rsa:2048 -nodes -keyout $P/$n.key -out $P/$n.csr -subj "/CN=localhost"; fi
    q $O x509 -req -in $P/$n.csr -CA $ca -CAkey $cak -CAcreateserial -out $P/$n.pem -extfile $ext "$@"
}
leaf good $P/ca.pem $P/ca.key $P/leaf.ext rsa -days 365
leaf ecgood $P/ca.pem $P/ca.key $P/leaf.ext ec -days 365
# certificates with given dates go through "openssl ca" (x509 -req has no start date)
mkdir -p $P/db
: > $P/db/index.txt
echo 1000 > $P/db/serial
cat > $P/ca.cnf <<EOF
[ca]
default_ca = d
[d]
database = $P/db/index.txt
serial = $P/db/serial
new_certs_dir = $P/db
default_md = sha256
policy = p
unique_subject = no
[p]
commonName = supplied
EOF
dated() {  # name start end
    q $O req -newkey rsa:2048 -nodes -keyout $P/$1.key -out $P/$1.csr -subj "/CN=localhost"
    q $O ca -batch -config $P/ca.cnf -cert $P/ca.pem -keyfile $P/ca.key -in $P/$1.csr -out $P/$1.pem \
        -extfile $P/leaf.ext -startdate $2 -enddate $3 -notext
}
dated expired 20200101000000Z 20210101000000Z
dated notyet 20400101000000Z 20410101000000Z
leaf wronghost $P/ca.pem $P/ca.key $P/other.ext rsa -days 365
leaf untrusted $P/ca2.pem $P/ca2.key $P/leaf.ext rsa -days 365
q $O req -x509 -newkey rsa:2048 -nodes -keyout $P/self.key -out $P/self.pem -days 365 -subj "/CN=localhost" \
    -addext subjectAltName=DNS:localhost
# root -> intermediate -> leaf, the server sends leaf + intermediate
q $O req -newkey rsa:2048 -nodes -keyout $P/inter.key -out $P/inter.csr -subj "/CN=Halcyon Test Intermediate"
q $O x509 -req -in $P/inter.csr -CA $P/ca.pem -CAkey $P/ca.key -CAcreateserial -out $P/inter.pem -extfile $P/ca.ext -days 365
leaf chained $P/inter.pem $P/inter.key $P/leaf.ext rsa -days 365
cat $P/chained.pem $P/inter.pem > $P/chain.pem
# an intermediate that is not a CA signs the leaf
q $O req -newkey rsa:2048 -nodes -keyout $P/notca.key -out $P/notca.csr -subj "/CN=Not A CA"
q $O x509 -req -in $P/notca.csr -CA $P/ca.pem -CAkey $P/ca.key -CAcreateserial -out $P/notca.pem -extfile $P/leaf.ext -days 365
leaf bynotca $P/notca.pem $P/notca.key $P/leaf.ext rsa -days 365
cat $P/bynotca.pem $P/notca.pem > $P/bynotca-chain.pem
echo "made with $($O version)"

echo "== servers"
base=$(( 20000 + $$ % 20000 ))
PL=$base
PG=$((base + 1)); PE=$((base + 2)); PN=$((base + 3)); PW=$((base + 4)); PS=$((base + 5))
PU=$((base + 6)); PC=$((base + 7)); PEC=$((base + 8)); P13=$((base + 9)); PNC=$((base + 10))
python3 -I tests/web/server.py $T/ready $PL $PG:$P/good.pem:$P/good.key $PE:$P/expired.pem:$P/expired.key \
    $PN:$P/notyet.pem:$P/notyet.key $PW:$P/wronghost.pem:$P/wronghost.key $PS:$P/self.pem:$P/self.key \
    $PU:$P/untrusted.pem:$P/untrusted.key $PC:$P/chain.pem:$P/chained.key $PEC:$P/ecgood.pem:$P/ecgood.key \
    $P13:$P/good.pem:$P/good.key:tls13 $PNC:$P/bynotca-chain.pem:$P/bynotca.key &
SRV=$!
for i in $(seq 50); do [ -f $T/ready ] && break; sleep 0.1; done
[ -f $T/ready ] || { echo "server did not start"; exit 1; }

G="$OUT/hostget"
H=http://127.0.0.1:$PL
# expect "pattern" hostget-args...
expect() {
    pat=$1; shift
    res=$($G "$@" 2>&1 | grep -a RESULT | tail -1)
    if echo "$res" | grep -q -- "$pat"; then pass; else fail "$* -> $res (wanted $pat)"; fi
}

echo "== HTTP"
expect "err=OK status=200 len=82 .*ctype=text/html charset=utf-8" $H/hello
expect "err=OK status=200" --method HEAD $H/hello
expect "len=0 " --method HEAD $H/hello
$G --out $T/ua.txt $H/ua >/dev/null; grep -q "ua=Halcyon/1.0 host=127.0.0.1:$PL" $T/ua.txt && pass || fail "User-Agent / Host: $(cat $T/ua.txt)"
expect "err=OK status=200 len=8200 " $H/gzip
expect "err=OK status=500" --no-gzip $H/gzip
expect "err=OK status=200 len=4100 " $H/deflate
expect "err=OK status=200 len=4100 " $H/rawdeflate
expect "err=DECODE" $H/badgzip
expect "err=TOOBIG" $H/gzipbomb
expect "err=OK status=200 len=6015 " $H/chunked
expect "err=OK status=200 len=2460 " $H/chunkedgzip
expect "err=PROTOCOL.*chunk" $H/badchunk1
expect "err=PROTOCOL.*chunk" $H/badchunk2
expect "err=PROTOCOL.*chunk" $H/badchunk3
expect "err=TRUNCATED" $H/badchunk4
expect "err=TRUNCATED" $H/truncated
expect "err=OK status=200 len=7500 " $H/close
expect "err=OK status=200 len=10 " $H/http10
expect "err=HEADERS" $H/hugeheader
expect "err=HEADERS" $H/manyheaders
expect "err=OK status=200 len=2 .*charset=latin-1" $H/folded
expect "err=PROTOCOL.*Content-Length" $H/badlength
expect "err=PROTOCOL.*Content-Length" $H/notalength
expect "err=PROTOCOL.*status line" $H/garbage
expect "err=CLOSED" $H/nothing
expect "err=OK status=200 len=4 " $H/continue
expect "err=OK status=200 len=7 redirects=5" $H/redirect/5
expect "err=REDIRECTS" $H/redirect/11
expect "err=REDIRECTS" $H/loop
expect "err=OK status=302 len=5 redirects=0" --no-redirect $H/redirect/1
expect "err=OK status=200 len=82 redirects=1" $H/abs-redirect
expect "err=OK status=404" $H/missing
expect "err=OK status=200 len=3145728 " --max 4000000 $H/big
expect "err=TOOBIG" --max 1000000 $H/big
expect "err=TOOBIG" --stream --max 1000000 $H/big
expect "err=OK status=200 len=3145728 " --stream $H/big
expect "err=OK.*charset=iso-8859-1" $H/latin1
$G --method POST --data "a=1&b=x+y" --out $T/post.txt $H/post >/dev/null
grep -q "method=POST type=application/x-www-form-urlencoded body=a=1&b=x+y" $T/post.txt && pass || fail "POST: $(cat $T/post.txt)"
$G --method POST --data "q=1" --out $T/303.txt $H/redirect303 >/dev/null; grep -q "method=GET" $T/303.txt && pass || fail "303 after POST: $(cat $T/303.txt)"
expect "err=TIMEOUT" --timeout 1000 $H/slow
expect "err=TIMEOUT" --timeout 1500 $H/slowbody
expect "err=CANCELED" --job 300 $H/slow
expect "err=OK status=200 len=82" --job 5000 $H/hello
# keep-alive and cookies over several requests in one session
expect "err=OK status=200 len=82" --repeat 5 $H/hello
$G --out $T/ck.txt $H/getcookie >/dev/null; grep -q "cookie=None" $T/ck.txt && pass || fail "cookie without set: $(cat $T/ck.txt)"
$G --out $T/ck.txt $H/setcookie $H/getcookie >/dev/null
grep -q "sid=abc123" $T/ck.txt && grep -q "theme=blue" $T/ck.txt && ! grep -q "gone\|evil" $T/ck.txt && pass || fail "cookies: $(cat $T/ck.txt)"
expect "err=CONNECT.*refused" http://127.0.0.1:1/
expect "err=DNS" http://no-such-host.invalid/
expect "err=URL" "http://bad host/"
expect "err=SCHEME" ftp://example.com/

echo "== HTTPS"
CA="--ca $P/ca.pem"
expect "err=OK status=200 len=82 .*tls=yes" $CA https://localhost:$PG/hello
expect "err=OK status=200 len=8200 .*tls=yes" $CA https://localhost:$PG/gzip
expect "err=OK status=200 len=6015 " $CA https://localhost:$PG/chunked
expect "err=OK status=200 len=82" $CA --repeat 4 https://localhost:$PG/hello
expect "err=OK status=200 len=82" $CA https://localhost:$PC/hello
expect "err=OK status=200 len=82" $CA https://localhost:$PEC/hello
expect "err=CERT.*expired on 2021-01-01" $CA https://localhost:$PE/hello
expect "err=CERT.*not valid before 2040-01-01" $CA https://localhost:$PN/hello
expect "err=CERT.*not for localhost: it is for other.example, www.other.example" $CA https://localhost:$PW/hello
expect "err=CERT.*self-signed" $CA https://localhost:$PS/hello
expect "err=CERT.*Untrusted Test CA.*not a trusted" $CA https://localhost:$PU/hello
expect "err=CERT.*not a certificate authority" $CA https://localhost:$PNC/hello
expect "err=CERT" https://localhost:$PG/hello
expect "err=CERT.*not for 127.0.0.1" $CA https://127.0.0.1:$PG/hello
expect "err=TLS.*TLS 1.3" $CA https://localhost:$P13/hello
expect "err=TLS\|err=PROTOCOL\|err=CLOSED" https://127.0.0.1:$PL/hello
expect "err=TRUNCATED" $CA https://localhost:$PG/truncated
expect "err=OK status=200 len=7500 " $CA https://localhost:$PG/close
expect "err=TIMEOUT" $CA --timeout 1000 https://localhost:$PG/slow
# the clock: a certificate made today, checked 2 hours before it was made
NOW=$(date +%s)
WEB_TEST_TIME=$((NOW - 7200)) expect "err=CERT.*not valid before" $CA https://localhost:$PG/hello
# ... with an unknown time zone (as on Halcyon: +-14 hours) it is accepted
WEB_TEST_TIME=$((NOW - 7200)) WEB_TEST_UNCERTAIN=50400 expect "err=OK status=200" $CA https://localhost:$PG/hello
# ... but a certificate that expired years ago is not
WEB_TEST_UNCERTAIN=50400 expect "err=CERT.*expired on 2021" $CA https://localhost:$PE/hello
WEB_TEST_UNCERTAIN=50400 expect "err=CERT.*not for localhost" $CA https://localhost:$PW/hello
# a year from now, the good certificate has expired
WEB_TEST_TIME=$((NOW + 400 * 86400)) expect "err=CERT.*expired on" $CA https://localhost:$PG/hello

echo "web: $checks checks, $fails failures"
[ $fails -eq 0 ]
