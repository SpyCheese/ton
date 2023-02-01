#!/bin/bash
pushd .
# ANDROID_TOOLCHAIN
# ANDROID_ABI
# ANDROID_PLATFORM
# ANDROID_STL
# ANDROID_PIE
# ANDROID_CPP_FEATURES
# ANDROID_ALLOW_UNDEFINED_SYMBOLS
# ANDROID_ARM_MODE
# ANDROID_ARM_NEON
# ANDROID_DISABLE_FORMAT_STRING_CHECKS
# ANDROID_CCACHE

export SECP256K1_INCLUDE_DIR=$(pwd)/third_party/secp256k1/include
export SECP256K1_LIBRARY=$(pwd)/third_party/secp256k1/.libs/libsecp256k1.a

if [ $ARCH == "arm" ]
then
  ABI="armeabi-v7a"
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-armv7-a/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-armv7-a/lib/libsodium.a
elif [ $ARCH == "x86" ]
then
  ABI=$ARCH
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-i686/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-i686/lib/libsodium.a
elif [ $ARCH == "x86_64" ]
then
  ABI=$ARCH
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-westmere/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-westmere/lib/libsodium.a
elif [ $ARCH == "arm64" ]
then
  ABI="arm64-v8a"
  export SODIUM_INCLUDE_DIR=$(pwd)/third_party/libsodium/libsodium-android-armv8-a/include
  export SODIUM_LIBRARY_RELEASE=$(pwd)/third_party/libsodium/libsodium-android-armv8-a/lib/libsodium.a
fi


ARCH=$ABI
echo $ABI

mkdir -p build-$ARCH
cd build-$ARCH

echo cmake .. -GNinja -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DANDROID_ABI=${ABI} -DOPENSSL_ROOT_DIR=${OPENSSL_DIR}/${ARCH} -DTON_ARCH="" -DTON_ONLY_TONLIB=ON \
      -DSECP256K1_INCLUDE_DIR=${SECP256K1_INCLUDE_DIR} -DSECP256K1_LIBRARY=${SECP256K1_LIBRARY} \
      -DSODIUM_INCLUDE_DIR=${SODIUM_INCLUDE_DIR} -DSODIUM_LIBRARY_RELEASE=${SODIUM_LIBRARY_RELEASE}

ls -lart ${SODIUM_LIBRARY_RELEASE}

cmake .. -GNinja -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake \
 -DCMAKE_BUILD_TYPE=Release -DANDROID_ABI=${ABI} -DOPENSSL_ROOT_DIR=${OPENSSL_DIR}/${ARCH} -DTON_ARCH="" -DTON_ONLY_TONLIB=ON \
 -DSECP256K1_INCLUDE_DIR=${SECP256K1_INCLUDE_DIR} -DSECP256K1_LIBRARY=${SECP256K1_LIBRARY} \
 -DSODIUM_INCLUDE_DIR=${SODIUM_INCLUDE_DIR} -DSODIUM_LIBRARY_RELEASE=${SODIUM_LIBRARY_RELEASE} || exit 1
ninja native-lib || exit 1
popd

mkdir -p libs/$ARCH/
cp build-$ARCH/libnative-lib.so* libs/$ARCH/


