#!/bin/sh
export PATH=$PATH:$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin
rm -rf secp256k1
git clone https://github.com/libbitcoin/secp256k1.git
cd secp256k1
./autogen.sh
#./configure --enable-module-recovery
#./configure --enable-module-recovery --with-asm=arm --enable-experimental
./configure --enable-module-recovery --enable-experimental --with-asm=arm --host=arm-linux-androideabi CC=armv7a-linux-androideabi21-clang CFLAGS="-mthumb -march=armv7-a" CCASFLAGS="-Wa,-mthumb -Wa,-march=armv7-a"
#./configure --enable-module-recovery --enable-experimental --with-asm=arm --host=arm-linux-androideabi CC=armv8a-linux-androideabi21-clang CFLAGS="-mthumb -march=armv8-a" CCASFLAGS="-Wa,-mthumb -Wa,-march=armv8-a"
#./configure --enable-module-recovery --host=x86_64-linux-androideabi CC=x86_64-linux-android21-clang
#./configure --enable-module-recovery --host=i686-linux-androideabi CC=i686-linux-android21-clang
make
#make install
