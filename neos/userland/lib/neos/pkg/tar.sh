#!/bin/sh
# pkg install tar:<file|url>[#sha256=<hex>]     pkg remove tar:<name>
. /lib/neos/pkg/common.sh
sub=$1; spec=$2
case "$spec" in *'#sha256='*) sha=${spec#*#sha256=}; spec=${spec%%#*} ;; *) sha= ;; esac
case $sub in
  install)
    [ -n "$spec" ] || die "usage: pkg install tar:<file|url>"
    file=$(basename "$spec")
    name=${file%%.tar*}; name=${name%.tgz}; name=${name%.zip}
    ver=$(echo "$name" | sed -n 's/^.*[-_]\([0-9][0-9A-Za-z.]*\)$/\1/p'); [ -n "$ver" ] || ver=0
    name=$(echo "$name" | sed 's/[-_][0-9][0-9A-Za-z.]*$//')
    tmp=/tmp/neos-pkg-$$.tar
    fetch "$spec" $tmp
    check_sha $tmp "$sha"
    dest=/opt/$name
    rm -rf $dest; mkdir -p $dest
    case "$file" in
      *.zip) unzip -q -o $tmp -d $dest || die "cannot unpack" ;;
      *) case "$file" in *.gz|*.tgz) z=z;; *.bz2|*.tbz2) z=j;; *.xz|*.txz) z=J;; *) z=;; esac
         tar -x${z}f $tmp -C $dest || { rm -f $tmp; die "cannot unpack"; } ;;
    esac
    rm -f $tmp
    # a single top-level directory is the usual layout: hoist it
    n=$(ls $dest | wc -l)
    if [ "$n" = 1 ] && [ -d $dest/$(ls $dest) ]; then top=$dest/$(ls $dest); mv $top/* $dest/ 2>/dev/null; mv $top/.[!.]* $dest/ 2>/dev/null; rmdir $top 2>/dev/null; fi
    link_bins $dest/bin
    neosctl pkg.record "$name" "$ver" "tar" >/dev/null || die "could not register $name"
    say "installed $name $ver into $dest"
    ;;
  remove)
    name=$spec
    [ -d /opt/$name ] || die "$name is not installed"
    for f in /opt/bin/*; do [ -L "$f" ] && case "$(readlink "$f")" in /opt/$name/*) rm -f "$f";; esac; done
    rm -rf /opt/$name /var/pkg/$name
    say "removed $name"
    ;;
  *) die "usage: pkg install|remove tar:..." ;;
esac
