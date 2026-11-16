#!/bin/bash
# Release guard (tools/android/README.md): a tagged release must not ship an install text that still says the phone
# test is "in testing". Dry runs (any ref that is not a tag) are allowed.
#
# usage: tools/android/check_release_text.sh <install text> <git ref>
#   e.g. tools/android/check_release_text.sh tools/android/ANDROID-INSTALL.txt "$GITHUB_REF"
# Exit 0 when the text may ship from this ref, 1 when a tag build would ship "in testing", 2 on a usage error.
set -euo pipefail

if [ $# -ne 2 ] || [ -z "$2" ]; then
    echo "usage: $0 <install text> <git ref, e.g. refs/tags/v0.9.0 or refs/heads/master>" >&2
    exit 2
fi
TEXT=$1
REF=$2
if [ ! -f "$TEXT" ]; then
    echo "error: missing $TEXT" >&2
    exit 2
fi

# Line ends become spaces first, so "in" at the end of one line and "testing" at the start of the next still match.
if tr '\r\n' '  ' < "$TEXT" | grep -qiwE 'in[[:space:]]+testing'; then
    lines=$(grep -niE 'testing' "$TEXT" || true)
else
    lines=
fi
if [ -z "$lines" ]; then
    echo "check_release_text: ok ($TEXT has no 'in testing'; ref $REF)"
    exit 0
fi
case "$REF" in
    refs/tags/*)
        echo "FAIL: $TEXT still says 'in testing' and $REF is a tag (a release). Replace it with the real test" \
             "result (tools/android/TEST-GUIDE.md, 'Before the first Android release'):"
        sed 's/^/    /' <<< "$lines"
        exit 1
        ;;
    *)
        echo "check_release_text: ok for a dry run ($REF is not a tag); a tag build fails while $TEXT says:"
        sed 's/^/    /' <<< "$lines"
        ;;
esac
