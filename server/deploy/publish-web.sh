#!/bin/sh
# publish-web.sh -- put a browser build on the site.
#
#   server/deploy/publish-web.sh <build dir> [ssh host] [web root]
#   server/deploy/publish-web.sh build-web side /var/www/umk3
#
# The build dir holds the game page (umk3.html), its .js and .wasm, and the
# page's helpers (ipa.js, ipa_store.js). None of it is game data: the page
# reads each player's own .ipa in their browser.
#
# Each build goes into its own folder named by the hash of its files, and the
# site's root page points at the newest one. Cloudflare in front of the site
# keeps .js for four hours whatever the server says, so a build that reused
# the same URLs would meet last build's script in players' caches; a new
# folder is a new URL. The root page is HTML, which is not held, and it keeps
# the query string, so a ?room= link still lands in its room.
set -eu

BUILD=${1:?usage: publish-web.sh <build dir> [ssh host] [web root]}
HOST=${2:-side}
ROOT=${3:-/var/www/umk3}
FILES="umk3.html umk3.js umk3.wasm ipa.js ipa_store.js"

cd "$BUILD"
for f in $FILES; do
    [ -f "$f" ] || { echo "missing $BUILD/$f" >&2; exit 1; }
done
HASH=$(cat $FILES | sha256sum | cut -c1-12)

ssh "$HOST" "mkdir -p '$ROOT/g/$HASH'"
scp -q $FILES "$HOST:$ROOT/g/$HASH/"

# The root page last, so it never points at a folder still being copied.
cat > /tmp/umk3-index.$$ <<EOF
<!doctype html>
<meta charset="utf-8">
<title>UMK3 Online</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<script>location.replace("/g/$HASH/umk3.html" + location.search + location.hash);</script>
<noscript><a href="/g/$HASH/umk3.html">Play</a></noscript>
EOF
scp -q /tmp/umk3-index.$$ "$HOST:$ROOT/index.html.new"
rm -f /tmp/umk3-index.$$
ssh "$HOST" "mv '$ROOT/index.html.new' '$ROOT/index.html'"
echo "published $HASH"
