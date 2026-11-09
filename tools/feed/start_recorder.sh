#!/usr/bin/env bash
cd "$HOME/meridian" || exit 1
pgrep -f tools/feed/run_forever.sh >/dev/null && exit 0
exec bash tools/feed/run_forever.sh >> data/live/runner.log 2>&1
