#!/bin/sh

rm -rf libsodium

git clone https://github.com/jedisct1/libsodium.git

cd libsodium
./autogen.sh -s

./dist-build/android-x86.sh
cp -R libsodium-android-i686 ..

./dist-build/android-x86_64.sh
cp -R libsodium-android-westmere ..

./dist-build/android-armv7-a.sh
cp -R libsodium-android-armv7-a ..

./dist-build/android-armv8-a.sh
cp -R libsodium-android-armv8-a+crypto ..

#./dist-build/android-aar.sh

