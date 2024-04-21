#/bin/bash

nix-build --version
test $? -eq 0 || { echo "Nix is not installed!"; exit 1; }

cp assembly/nix/linux-x86-64* .
cp assembly/nix/microhttpd.nix .
cp assembly/nix/openssl.nix .
export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

nix-build linux-x86-64-toncryptolib.nix
mkdir -p artifacts/include/
cp ./result/lib/libton_crypto_lib.so artifacts/libton_crypto_lib.so
cp -r ./result/include/crypto/ ./result/include/common/ ./result/include/tdutils/ artifacts/include/
