#!/bin/sh

# 947
# size in control mode should change after refresh-client -C, and -x and -y
# should work without -d for control clients

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

TMP=$(mktemp)
OUT=$(mktemp)
trap "rm -f $TMP $OUT" 0 1 15

$TMUX -f/dev/null new -d || exit 1
sleep 1
cat <<EOF|$TMUX -C a >$TMP
ls -F':#{window_width} #{window_height}'
refresh -C 100,50
EOF
grep ^: $TMP >$OUT
$TMUX ls -F':#{window_width} #{window_height}' >>$OUT
printf ":80 24\n:100 50\n"|cmp -s $OUT - || exit 1
$TMUX kill-server 2>/dev/null

$TMUX -f/dev/null new -d || exit 1
sleep 1
cat <<EOF|$TMUX -f/dev/null -C a >$TMP
ls -F':#{window_width} #{window_height}'
refresh -C 80,24
EOF
grep ^: $TMP >$OUT
$TMUX ls -F':#{window_width} #{window_height}' >>$OUT
printf ":80 24\n:80 24\n"|cmp -s $OUT - || exit 1
$TMUX kill-server 2>/dev/null

cat <<EOF|$TMUX -f/dev/null -C new -x 100 -y 50 >$TMP
ls -F':#{window_width} #{window_height}'
refresh -C 80,24
EOF
grep ^: $TMP >$OUT
$TMUX ls -F':#{window_width} #{window_height}' >>$OUT
printf ":100 50\n:80 24\n"|cmp -s $OUT - || exit 1
$TMUX kill-server 2>/dev/null

exit 0
