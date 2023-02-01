#!/bin/sh

rm -rf libsodium

export ANDROID_NDK_ROOT=../../../../android-ndk-r25b
export NDK_PLATFORM="android-24"
export OPENSSL_DIR=../crypto

wget https://download.libsodium.org/libsodium/releases/libsodium-1.0.18.tar.gz
tar -xvf libsodium-1.0.18.tar.gz

cd libsodium-1.0.18
#./autogen.sh -s

./dist-build/android-x86.sh
cp -R libsodium-android-i686 ..

./dist-build/android-x86_64.sh
cp -R libsodium-android-westmere ..

./dist-build/android-armv7-a.sh
cp -R libsodium-android-armv7-a ..

./dist-build/android-armv8-a.sh
cp -R libsodium-android-armv8-a ..

#./dist-build/android-aar.sh

