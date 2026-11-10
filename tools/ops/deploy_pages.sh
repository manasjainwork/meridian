#!/usr/bin/env bash
# Build web-ui and publish the result to the gh-pages branch (a deploy branch only, force-pushed each time).
set -euo pipefail
cd "$(dirname "$0")/../.."
REMOTE="$(git remote get-url origin)"
( cd web-ui && npm run build )
TMP="$(mktemp -d)"
cp -r web-ui/dist/. "$TMP/"
touch "$TMP/.nojekyll"
cd "$TMP"
git init -q -b gh-pages
git add -A
git -c user.name="$(git -C - config user.name 2>/dev/null || echo deploy)" -c user.email="deploy@localhost" commit -q -m "deploy web-ui"
git remote add origin "$REMOTE"
git push -q -f origin gh-pages
echo "pushed gh-pages"
