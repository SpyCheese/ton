#!/bin/sh

git clone https://github.com/libbitcoin/secp256k1.git
cd secp256k1
./autogen.sh
./configure
make
