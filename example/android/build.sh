#!/bin/bash

pushd .

export SECP256K1_INCLUDE_DIR=$(pwd)/third_party/secp256k1/include

if [ $ARCH == "arm" ]
then
  ABI="armeabi-v7a"
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-armv7-a/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-armv7-a/lib/libsodium.a
  export SECP256K1_LIBRARY=$(pwd)/third_party/secp256k1/armv7/libsecp256k1.a
elif [ $ARCH == "x86" ]
then
  ABI=$ARCH
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-i686/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-i686/lib/libsodium.a
  export SECP256K1_LIBRARY=$(pwd)/third_party/secp256k1/i686/libsecp256k1.a
elif [ $ARCH == "x86_64" ]
then
  ABI=$ARCH
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-westmere/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-westmere/lib/libsodium.a
  export SECP256K1_LIBRARY=$(pwd)/third_party/secp256k1/x86-64/libsecp256k1.a
elif [ $ARCH == "arm64" ]
then
  ABI="arm64-v8a"
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-armv8-a/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-armv8-a/lib/libsodium.a
  export SECP256K1_LIBRARY=$(pwd)/third_party/secp256k1/armv8/libsecp256k1.a
fi


ARCH=$ABI
echo $ABI

mkdir -p build-$ARCH
cd build-$ARCH

echo cmake .. -GNinja -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DANDROID_ABI=${ABI} -DOPENSSL_ROOT_DIR=${OPENSSL_DIR}/${ARCH} -DTON_ARCH="" -DTON_ONLY_TONLIB=ON \
      -DSECP256K1_INCLUDE_DIR=${SECP256K1_INCLUDE_DIR} -DSECP256K1_LIBRARY=${SECP256K1_LIBRARY} \
      -DSODIUM_INCLUDE_DIR=${SODIUM_INCLUDE_DIR} -DSODIUM_LIBRARY_RELEASE=${SODIUM_LIBRARY_RELEASE}

cmake .. -GNinja -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake \
 -DCMAKE_BUILD_TYPE=Release -DANDROID_ABI=${ABI} -DOPENSSL_ROOT_DIR=${OPENSSL_DIR}/${ARCH} -DTON_ARCH="" -DTON_ONLY_TONLIB=ON \
 -DSECP256K1_INCLUDE_DIR=${SECP256K1_INCLUDE_DIR} -DSECP256K1_LIBRARY=${SECP256K1_LIBRARY} \
 -DSODIUM_INCLUDE_DIR=${SODIUM_INCLUDE_DIR} -DSODIUM_LIBRARY_RELEASE=${SODIUM_LIBRARY_RELEASE} || exit 1

ninja native-lib || exit 1
popd

$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip build-$ARCH/libnative-lib.so

mkdir -p libs/$ARCH/
cp build-$ARCH/libnative-lib.so* libs/$ARCH/

