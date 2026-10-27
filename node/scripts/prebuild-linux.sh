#!/bin/sh
# Builds the portable linux-x64 prebuilds into prebuilds/linux-x64/:
#   *.glibc.node - built on manylinux_2_28 (glibc 2.28, GCC 14) so it loads
#                  on any glibc >= 2.25 distro (Debian 10+, Ubuntu 18.04+,
#                  RHEL 8+). Building on the host instead would bake in the
#                  host's newer glibc symbol versions.
#   *.musl.node  - built on node:22-alpine, for Alpine.
# libstdc++/libgcc are linked statically (binding.gyp), so neither binary
# depends on the target's C++ runtime. Needs Docker. Run from node/.
set -eu

cd "$(dirname "$0")/.."
node scripts/vendor.js

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar --exclude=node_modules --exclude=build --exclude=prebuilds -cf - . | tar -xf - -C "$work"
uid=$(id -u)
gid=$(id -g)

docker run --rm -v "$work:/pkg" -w /pkg quay.io/pypa/manylinux_2_28_x86_64 sh -c "
  set -e
  curl -fsSL https://nodejs.org/dist/v22.11.0/node-v22.11.0-linux-x64.tar.xz | tar -xJ -C /opt
  export PATH=/opt/node-v22.11.0-linux-x64/bin:/opt/python/cp312-cp312/bin:\$PATH
  npm install --ignore-scripts --no-audit --no-fund
  npx prebuildify --napi --strip --tag-libc
  chown -R $uid:$gid /pkg"

docker run --rm -v "$work:/pkg" -w /pkg node:22-alpine sh -c "
  set -e
  apk add --no-cache python3 make g++
  npx prebuildify --napi --strip --tag-libc
  chown -R $uid:$gid /pkg"

mkdir -p prebuilds/linux-x64
cp "$work"/prebuilds/linux-x64/*.node prebuilds/linux-x64/
ls -l prebuilds/linux-x64
