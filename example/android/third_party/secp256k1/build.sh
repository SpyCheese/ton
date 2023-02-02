#!/bin/sh
rm -rf secp256k1
git clone https://github.com/libbitcoin/secp256k1.git
cd secp256k1
./autogen.sh
./configure --enable-module-recovery
#./configure --enable-module-recovery --with-asm=arm --enable-experimental
make
make install
