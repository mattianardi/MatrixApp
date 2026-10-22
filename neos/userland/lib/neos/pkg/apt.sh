#!/bin/sh
# pkg install apt:<name...>   pkg remove apt:<name>   pkg update apt: (refresh the index)  - Debian/Ubuntu archives over HTTP, resolved by apt.py
. /lib/neos/pkg/common.sh
sub=$1; shift
PY=/opt/python3/bin/python3
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
if [ ! -x $PY ]; then
  say "python3 is not installed yet: installing it from the package cache first"
  neosctl pkg.update >/dev/null 2>&1
  neosctl pkg.install python3 >/dev/null || die "python3 is not available (pkg update; pkg list)"
fi
exec $PY /lib/neos/pkg/apt.py "$sub" "$@"
