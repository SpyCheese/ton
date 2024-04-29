#/bin/bash

nix-build --version
test $? -eq 0 || { echo "Nix is not installed!"; exit 1; }

cp assembly/nix/macos-* .
export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

nix-build macos-toncryptolib.nix

nix-build macos-toncryptolib.nix
mkdir -p artifacts/include/
cp ./result/lib/libton_crypto_lib.dylib artifacts/libton_crypto_lib.dylib
cp -r ./result/include/crypto/ artifacts/include/crypto
cp -r ./result/include/common/ artifacts/include/common
cp -r ./result/include/ton/ artifacts/include/ton
cp -r ./result/include/tdutils/ artifacts/include/tdutils
