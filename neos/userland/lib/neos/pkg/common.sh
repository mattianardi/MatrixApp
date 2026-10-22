# Shared helpers of the NEOS package backends (sourced by pip.sh, deb.sh, tar.sh, apt.sh).
# Every backend registers what it installs with `neosctl pkg.record` - the same capability-gated, audited
# tool an AI agent would use - so `pkg installed` shows one list whatever the origin, and NeoFS keeps it across reboots.
say() { echo "pkg: $*"; }
die() { echo "pkg: $*" >&2; exit 1; }
link_bins() {                    # link_bins <dir>... : make every executable in these directories reachable through /opt/bin (which PATH contains)
  mkdir -p /opt/bin
  for d in "$@"; do
    [ -d "$d" ] || continue
    for f in "$d"/*; do [ -f "$f" ] && [ -x "$f" ] && ln -sf "$f" "/opt/bin/$(basename "$f")"; done
  done
}
fetch() {                        # fetch <url|path> <dest>
  case "$1" in
    http://*) wget -q -O "$2" "$1" || die "cannot download $1" ;;
    https://*) wget -q -O "$2" "$1" || die "cannot download $1 (HTTPS: certificates are NOT verified by BusyBox's TLS; pin a checksum with sha256=)" ;;
    *) cp "$1" "$2" || die "cannot read $1" ;;
  esac
}
check_sha() {                    # check_sha <file> <expected|"">
  [ -z "$2" ] && return 0
  got=$(sha256sum "$1" | cut -d' ' -f1)
  [ "$got" = "$2" ] || die "SHA-256 mismatch for $1 (expected $2, got $got)"
  say "sha256 verified"
}
