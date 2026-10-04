#!/usr/bin/env bash
# Runs in the build container, before the build (see the Dockerfile):
#
#   tree-sync.sh SRC TREE [SEED]
#
# TREE is this checkout's build tree, a cache mount: TREE/src holds its own
# copy of the sources, which the build reads instead of SRC, and TREE/out the
# build itself. This brings TREE/src level with SRC by content. A changed or new
# file is copied in and stamped with the current time; a deleted one is
# removed; an identical one is not touched at all. So ninja -- and CMake's moc,
# which also goes by timestamps -- rebuild exactly what changed in content
# since this tree's last build, whatever the timestamps in SRC say. Those come
# from the checkout: git sets them when it writes a file, so a fresh worktree,
# or a branch switched back to older code, would otherwise look either all new
# or not new enough.
#
# A TREE never built yet starts as a copy of SEED, the main checkout's tree,
# when that one is complete (TREE/.complete, written after a build succeeds and
# removed while one runs). Most of it is then already built, and the sync below
# says what is not.
set -euo pipefail
src=$1 tree=$2 seed=${3:-}

rm -f "$tree/.complete"

# Trees from before this layout (the build directly in TREE): rebuilt from scratch.
if [ -e "$tree/CMakeCache.txt" ]; then
	echo "tree-sync: old layout in $tree, starting over"
	find "$tree" -mindepth 1 -delete
fi

if [ ! -e "$tree/out/CMakeCache.txt" ] && [ -n "$seed" ] && [ -e "$seed/.complete" ]; then
	echo "tree-sync: $tree starts as a copy of $seed"
	find "$tree" -mindepth 1 -delete
	cp -a "$seed/src" "$seed/out" "$tree/"
	# Rebuilt while it was copied: not a tree to start from after all.
	if [ ! -e "$seed/.complete" ]; then
		echo "tree-sync: $seed changed while it was copied, building from scratch"
		find "$tree" -mindepth 1 -delete
	fi
fi

if [ ! -d "$tree/src" ]; then
	cp -a "$src" "$tree/src"
	exit 0
fi

# What differs, by content. diff names each changed file, and each file or
# directory that only one side has.
n=0
while IFS= read -r line; do
	case "$line" in
	"Files $src/"*" and $tree/src/"*" differ")
		rel=${line#"Files $src/"}
		rel=${rel%%" and $tree/src/"*}
		cp -a "$src/$rel" "$tree/src/$rel"
		touch "$tree/src/$rel"
		;;
	"Only in $src"*)
		rest=${line#"Only in $src"}
		dir=${rest%%": "*}
		name=${rest#*": "}
		mkdir -p "$tree/src$dir"
		cp -a "$src$dir/$name" "$tree/src$dir/"
		find "$tree/src$dir/$name" -exec touch {} +
		;;
	"Only in $tree/src"*)
		rest=${line#"Only in $tree/src"}
		dir=${rest%%": "*}
		name=${rest#*": "}
		rm -rf "${tree:?}/src$dir/$name"
		;;
	*)
		# A file that became a directory, a changed symlink, ...: the plain way.
		echo "tree-sync: $line -- copying the sources whole"
		rm -rf "${tree:?}/src"
		cp -a "$src" "$tree/src"
		find "$tree/src" -exec touch {} +
		exit 0
		;;
	esac
	n=$((n + 1))
done < <(diff -rq --no-dereference "$src" "$tree/src" || true)
echo "tree-sync: $n change(s) since this tree's last build"
