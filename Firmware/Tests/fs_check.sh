#!/bin/sh
# Judge the USB volume with real FAT tools instead of the test suite's own host
# model. Needs a C compiler, mtools and dosfstools (apt install mtools dosfstools).
#
#   ./fs_check.sh        from Firmware/Tests
#
# Exits non-zero on the first thing that does not behave.
set -eu

cd "$(dirname "$0")"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export MTOOLS_SKIP_CHECK=1

cc -std=c11 -O1 -g -Wall -Wextra -I../App/Inc -o "$WORK/vol_image" \
    ../App/Src/*.c host_platform.c vol_image.c

IMG="$WORK/dev.img"
fail() { echo "FAIL: $*"; exit 1; }
step() { echo; echo "== $*"; }

step "dump the volume and let fsck.fat judge it"
"$WORK/vol_image" dump "$IMG"
fsck.fat -n "$IMG" || fail "fsck.fat rejects the freshly generated volume"
mdir -i "$IMG" ::

step "read the files the way a PC does"
mcopy -i "$IMG" ::ATTEND.CSV "$WORK/attend.csv"
mcopy -i "$IMG" ::SETTINGS.CSV "$WORK/settings.csv"
mcopy -i "$IMG" ::STATUS.TXT "$WORK/status.txt"
head -c 200 "$WORK/attend.csv"; echo ...
grep -q '^DATE,TIME,CARD_ID ' "$WORK/attend.csv" || fail "ATTEND.CSV header"
grep -q '^2026-10-06,09:30:00,0000001000' "$WORK/attend.csv" || fail "ATTEND.CSV first row"
# Only date, time and card number: three columns, nothing else.
[ "$(tail -n +2 "$WORK/attend.csv" | tr -d '\r' | awk -F, 'NF != 3 { bad++ } END { print bad + 0 }')" -eq 0 ] || fail "every row must have exactly three columns"
[ "$(wc -l < "$WORK/attend.csv")" -eq 61 ] || fail "ATTEND.CSV should have a header and 60 rows (the lecture marker is not a row)"
# Every row is exactly 32 bytes, so the file size is a multiple of 32.
[ $(( $(wc -c < "$WORK/attend.csv") % 32 )) -eq 0 ] || fail "row width"
grep -q '4294967' "$WORK/attend.csv" && fail "a marker id leaked into ATTEND.CSV"
grep -q '^#MODULE,EN2090' "$WORK/settings.csv" || fail "SETTINGS.CSV shows the lecture in progress"
grep -q '^#LECTURE,Lecture 1' "$WORK/settings.csv" || fail "SETTINGS.CSV shows the lecture in progress"
grep -q '^#DEVICE,0012648430' "$WORK/settings.csv" || fail "SETTINGS.CSV shows the device id"
cat "$WORK/status.txt"

step "the LECTURES folder: one CSV per lecture, long names"
mdir -i "$IMG" ::LECTURES
mcopy -i "$IMG" "::LECTURES/L001_2026-10-06_09-48.csv" "$WORK/l001.csv" || fail "lecture 1's file by its long name"
mcopy -i "$IMG" "::LECTURES/L000.CSV" "$WORK/l000.csv" || fail "the taps before it, by the 8.3 alias"
grep -q '^DATE,TIME,CARD_ID ' "$WORK/l001.csv" || fail "lecture file header"
[ "$(wc -l < "$WORK/l001.csv")" -eq 31 ] || fail "lecture 1 should have a header and 30 rows"
[ "$(wc -l < "$WORK/l000.csv")" -eq 31 ] || fail "L000 should have a header and the 30 taps before the lecture"
mcopy -i "$IMG" ::LASTCARD.TXT "$WORK/lastcard.txt"
grep -q '^Taps    : 0' "$WORK/lastcard.txt" || fail "LASTCARD.TXT starts at no taps"

step "an untouched image changes nothing"
"$WORK/vol_image" apply "$IMG" | tee "$WORK/out.txt"
grep -q 'outcome=0' "$WORK/out.txt" || fail "an untouched image must not apply anything"

step "copy a new SETTINGS.CSV over the old one, as a user would"
cat > "$WORK/new.csv" <<EOF
#TIME,2030-05-06 07:08:09
#MODULE,MA1010
#LECTURE,"Calculus, part 2"
#DEVICE,0x2A
EOF
mcopy -o -i "$IMG" "$WORK/new.csv" ::SETTINGS.CSV
fsck.fat -n "$IMG" || fail "fsck.fat rejects the volume after the copy"
"$WORK/vol_image" apply "$IMG" | tee "$WORK/out.txt"
grep -q 'outcome=1' "$WORK/out.txt" || fail "the new settings were not applied"
grep -q 'time_set=1' "$WORK/out.txt" || fail "the clock was not set"
grep -q 'device_set=1' "$WORK/out.txt" || fail "the device id was not changed"
grep -q 'session=1' "$WORK/out.txt" || fail "a lecture should have started"
grep -q '^module=MA1010$' "$WORK/out.txt" || fail "module"
grep -q '^lecture=Calculus  part 2$' "$WORK/out.txt" || fail "lecture (the comma becomes a space)"
grep -q '^device id now 42$' "$WORK/out.txt" || fail "device id 0x2A is 42"
grep -q '^clock 2030-05-06 07:08:09$' "$WORK/out.txt" || fail "the clock"

step "an empty SETTINGS.CSV is refused"
"$WORK/vol_image" dump "$WORK/bad.img" >/dev/null
: > "$WORK/empty.csv"
mcopy -o -i "$WORK/bad.img" "$WORK/empty.csv" ::SETTINGS.CSV
"$WORK/vol_image" apply "$WORK/bad.img" | tee "$WORK/out.txt"
grep -q 'outcome=2' "$WORK/out.txt" || fail "a zero-length file must be refused"
grep -q '^device id now 12648430$' "$WORK/out.txt" || fail "a refusal must change nothing"

step "an old student list under another name is none of the device's business"
"$WORK/vol_image" dump "$WORK/alt.img" >/dev/null
printf 'CARD_ID,NAME\r\n1000,Alice\r\n' > "$WORK/students.csv"
mcopy -i "$WORK/alt.img" "$WORK/students.csv" ::students.csv
fsck.fat -n "$WORK/alt.img" || fail "fsck.fat rejects the volume with an extra file"
"$WORK/vol_image" apply "$WORK/alt.img" | tee "$WORK/out.txt"
grep -q 'outcome=0' "$WORK/out.txt" || fail "an unrelated file must not apply anything"

step "a host that clears the read-only flag and overwrites ATTEND.CSV harms nothing"
"$WORK/vol_image" dump "$WORK/ro.img" >/dev/null
mattrib -i "$WORK/ro.img" -r ::ATTEND.CSV </dev/null
mcopy -o -i "$WORK/ro.img" "$WORK/new.csv" ::ATTEND.CSV </dev/null
"$WORK/vol_image" apply "$WORK/ro.img" | tee "$WORK/out.txt"
grep -q 'outcome=0' "$WORK/out.txt" || fail "overwriting ATTEND.CSV must not apply anything"

step "a near-full log is still a valid FAT12 volume"
"$WORK/vol_image" dump "$WORK/big.img" big
fsck.fat -n "$WORK/big.img" || fail "fsck.fat rejects the large volume"
mcopy -i "$WORK/big.img" ::ATTEND.CSV "$WORK/big.csv"
[ "$(wc -l < "$WORK/big.csv")" -eq 13901 ] || fail "big ATTEND.CSV should have 13900 rows and a header"
echo "big ATTEND.CSV: $(wc -c < "$WORK/big.csv") bytes"

echo
echo "fs_check: all good"
