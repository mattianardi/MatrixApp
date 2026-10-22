#!/bin/sh
# pkg install deb:<file|url>[#sha256=<hex>]     pkg remove deb:<name>
. /lib/neos/pkg/common.sh
sub=$1; spec=$2
case "$spec" in *'#sha256='*) sha=${spec#*#sha256=}; spec=${spec%%#*} ;; *) sha= ;; esac
case $sub in
  install)
    [ -n "$spec" ] || die "usage: pkg install deb:<file|url>"
    tmp=/tmp/neos-pkg-$$.deb
    fetch "$spec" $tmp
    check_sha $tmp "$sha"
    ctl=$(dpkg-deb -f $tmp) || { rm -f $tmp; die "not a Debian package (BusyBox reads gzip/xz members; zstd-compressed .deb files are not supported)"; }
    name=$(echo "$ctl" | sed -n 's/^Package: *//p')
    ver=$(echo "$ctl" | sed -n 's/^Version: *//p')
    dep=$(echo "$ctl" | sed -n 's/^Depends: *//p')
    dest=/opt/$name
    rm -rf $dest; mkdir -p $dest
    dpkg-deb -x $tmp $dest || { rm -f $tmp; die "cannot unpack the package"; }
    rm -f $tmp
    # programs: a wrapper per executable so the package's own libraries are found (LD_LIBRARY_PATH) and /opt/bin stays the one PATH entry
    mkdir -p /opt/bin
    for bd in $dest/usr/bin $dest/bin $dest/usr/sbin $dest/sbin; do
      [ -d $bd ] || continue
      for f in $bd/*; do
        [ -f "$f" ] && [ -x "$f" ] || continue
        b=$(basename "$f")
        printf '#!/bin/sh\nexport LD_LIBRARY_PATH=%s/usr/lib/x86_64-linux-gnu:%s/lib/x86_64-linux-gnu:%s/usr/lib:$LD_LIBRARY_PATH\nexec %s "$@"\n' $dest $dest $dest "$f" > /opt/bin/$b
        chmod +x /opt/bin/$b
      done
    done
    neosctl pkg.record "$name" "$ver" "deb" >/dev/null || die "could not register $name"
    say "installed $name $ver into $dest"
    [ -n "$dep" ] && say "depends on: $dep (not resolved: use apt:$name to install with dependencies)"
    ;;
  remove)
    name=$spec
    [ -d /opt/$name ] || die "$name is not installed"
    for f in /opt/bin/*; do [ -f "$f" ] && grep -q "/opt/$name/" "$f" 2>/dev/null && rm -f "$f"; done
    rm -rf /opt/$name /var/pkg/$name
    say "removed $name"
    ;;
  *) die "usage: pkg install|remove deb:..." ;;
esac
