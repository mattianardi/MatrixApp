#!/bin/sh
# pkg install pip:<requirement...>     pkg remove pip:<name>      (python3 comes from the offline cache on first use)
. /lib/neos/pkg/common.sh
sub=$1; shift; spec="$*"
PY=/opt/python3/bin/python3
if [ ! -x $PY ]; then
  say "python3 is not installed yet: installing it from the package cache first"
  neosctl pkg.update >/dev/null 2>&1
  neosctl pkg.install python3 >/dev/null || die "python3 is not available (pkg update; pkg list)"
  [ -x $PY ] || die "python3 installation failed"
fi
sync_records() {
  $PY -m pip list --format=freeze 2>/dev/null | while IFS='=' read -r n _ v; do
    [ -n "$n" ] && neosctl pkg.record "pip:$n" "$v" "pip" >/dev/null
  done
  link_bins /opt/python3/bin
}
case $sub in
  install)
    [ -n "$spec" ] || die "usage: pkg install pip:<package>"
    $PY -m pip install --no-warn-script-location --disable-pip-version-check $spec || exit 1
    sync_records
    ;;
  remove)
    $PY -m pip uninstall -y $spec || exit 1
    for n in $spec; do rm -f "/var/pkg/pip:$n"; done
    ;;
  *) die "usage: pkg install|remove pip:..." ;;
esac
