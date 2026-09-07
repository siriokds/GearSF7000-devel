#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: $0 VERSION RELEASE_TAG ARCHIVE DOWNLOAD_URL OUTPUT" >&2
    exit 2
fi

version=$1
release_tag=$2
archive=$3
download_url=$4
output=$5

case "$version" in
    ''|*[!0-9A-Za-z._-]*) echo "invalid version: $version" >&2; exit 2 ;;
esac
case "$release_tag" in
    ''|*[!0-9A-Za-z._-]*) echo "invalid release tag: $release_tag" >&2; exit 2 ;;
esac

if [ ! -f "$archive" ]; then
    echo "archive not found: $archive" >&2
    exit 1
fi

length=$(stat -f %z "$archive" 2>/dev/null || wc -c < "$archive" | tr -d ' ')

{
    printf '%s\n' '<?xml version="1.0" encoding="utf-8"?>'
    printf '%s\n' '<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">'
    printf '%s\n' '  <channel>'
    printf '%s\n' '    <title>GearSC3000</title>'
    printf '%s\n' '    <item>'
    printf '      <title>GearSC3000 %s</title>\n' "$version"
    printf '%s\n' '      <sparkle:minimumSystemVersion>11.0</sparkle:minimumSystemVersion>'
    printf '%s\n' '      <enclosure'
    printf '        url="%s"\n' "$download_url"
    printf '        sparkle:version="%s"\n' "$version"
    printf '        sparkle:shortVersionString="%s"\n' "$version"
    printf '        length="%s"\n' "$length"
    printf '%s\n' '        type="application/octet-stream" />'
    printf '%s\n' '    </item>'
    printf '%s\n' '  </channel>'
    printf '%s\n' '</rss>'
} > "$output"
