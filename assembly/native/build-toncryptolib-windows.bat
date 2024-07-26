REM execute this script inside elevated (Run as Administrator) console "x64 Native Tools Command Prompt for VS 2019"

echo off

echo Installing chocolatey windows package manager...
@"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "iex ((New-Object System.Net.WebClient).DownloadString('https://chocolatey.org/install.ps1'))" && SET "PATH=%PATH%;%ALLUSERSPROFILE%\chocolatey\bin"
choco -?
IF %errorlevel% NEQ 0 (
  echo Can't install chocolatey
  exit /b %errorlevel%
)

choco feature enable -n allowEmptyChecksums

echo Installing pkgconfiglite...
choco install -y pkgconfiglite
IF %errorlevel% NEQ 0 (
  echo Can't install pkgconfiglite
  exit /b %errorlevel%
)

echo Installing ninja...
choco install -y ninja
IF %errorlevel% NEQ 0 (
  echo Can't install ninja
  exit /b %errorlevel%
)

if not exist "zlib" (
git clone https://github.com/madler/zlib.git
cd zlib
git checkout v1.3.1
cd contrib\vstudio\vc14
msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:platform=x64 -p:PlatformToolset=v142

IF %errorlevel% NEQ 0 (
  echo Can't install zlib
  exit /b %errorlevel%
)
cd ..\..\..\..
) else (
echo Using zlib...
)

if not exist "lz4" (
git clone https://github.com/lz4/lz4.git
cd lz4
git checkout v1.9.4
cd build\VS2017\liblz4
msbuild liblz4.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v142

IF %errorlevel% NEQ 0 (
  echo Can't install lz4
  exit /b %errorlevel%
)
cd ..\..\..\..
) else (
echo Using lz4...
)

if not exist "secp256k1" (
git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
git checkout v0.3.2
cmake -G "Visual Studio 16 2019" -A x64 -S . -B build -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DBUILD_SHARED_LIBS=OFF
IF %errorlevel% NEQ 0 (
  echo Can't configure secp256k1
  exit /b %errorlevel%
)
cmake --build build --config Release
IF %errorlevel% NEQ 0 (
  echo Can't install secp256k1
  exit /b %errorlevel%
)
cd ..
) else (
echo Using secp256k1...
)


curl --retry 5 --retry-delay 10 -Lo libsodium-1.0.18-stable-msvc.zip https://download.libsodium.org/libsodium/releases/libsodium-1.0.18-stable-msvc.zip
IF %errorlevel% NEQ 0 (
  echo Can't download libsodium
  exit /b %errorlevel%
)
unzip libsodium-1.0.18-stable-msvc.zip
) else (
echo Using libsodium...
)

if not exist "openssl-3.1.4" (
curl  -Lo openssl-3.1.4.zip https://github.com/neodiX42/precompiled-openssl-win64/raw/main/openssl-3.1.4.zip
IF %errorlevel% NEQ 0 (
  echo Can't download OpenSSL
  exit /b %errorlevel%
)
unzip -q openssl-3.1.4.zip
) else (
echo Using openssl...
)

if not exist "libmicrohttpd-0.9.77-w32-bin" (
curl  -Lo libmicrohttpd-0.9.77-w32-bin.zip https://github.com/neodiX42/precompiled-openssl-win64/raw/main/libmicrohttpd-0.9.77-w32-bin.zip
IF %errorlevel% NEQ 0 (
  echo Can't download libmicrohttpd
  exit /b %errorlevel%
)
unzip -q libmicrohttpd-0.9.77-w32-bin.zip
) else (
echo Using libmicrohttpd...
)

if not exist "readline-5.0-1-lib" (
curl  -Lo readline-5.0-1-lib.zip https://github.com/neodiX42/precompiled-openssl-win64/raw/main/readline-5.0-1-lib.zip
IF %errorlevel% NEQ 0 (
  echo Can't download readline
  exit /b %errorlevel%
)
unzip -q -d readline-5.0-1-lib readline-5.0-1-lib.zip
) else (
echo Using readline...
)


set root=%cd%
echo %root%
set SODIUM_DIR=%root%\libsodium

mkdir build
cd build
cmake -GNinja  -DCMAKE_BUILD_TYPE=Release ^
-DTON_USE_ABSEIL=OFF ^
-DPORTABLE=1 ^
-DSODIUM_USE_STATIC_LIBS=1 ^
-DSECP256K1_FOUND=1 ^
-DSECP256K1_INCLUDE_DIR=%root%\secp256k1\include ^
-DSECP256K1_LIBRARY=%root%\secp256k1\build\src\Release\libsecp256k1.lib ^
-DLZ4_FOUND=1 ^
-DLZ4_INCLUDE_DIRS=%root%\lz4\lib ^
-DLZ4_LIBRARIES=%root%\lz4\build\VS2017\liblz4\bin\x64_Release\liblz4_static.lib ^
-DMHD_FOUND=1 ^
-DMHD_LIBRARY=%root%\libmicrohttpd-0.9.77-w32-bin\x86_64\VS2019\Release-static\libmicrohttpd.lib ^
-DMHD_INCLUDE_DIR=%root%\libmicrohttpd-0.9.77-w32-bin\x86_64\VS2019\Release-static ^
-DZLIB_FOUND=1 ^
-DZLIB_INCLUDE_DIR=%root%\zlib ^
-DZLIB_LIBRARIES=%root%\zlib\contrib\vstudio\vc14\x64\ZlibStatReleaseWithoutAsm\zlibstat.lib ^
-DOPENSSL_FOUND=1 ^
-DOPENSSL_INCLUDE_DIR=%root%\openssl-3.1.4\x64\include ^
-DOPENSSL_CRYPTO_LIBRARY=%root%\openssl-3.1.4\x64\lib\libcrypto_static.lib ^
-DREADLINE_INCLUDE_DIR=%root%\readline-5.0-1-lib\include ^
-DREADLINE_LIBRARY=%root%\readline-5.0-1-lib\lib\readline.lib ^
-DCMAKE_CXX_FLAGS="/DTD_WINDOWS=1 /EHsc /bigobj" ..
IF %errorlevel% NEQ 0 (
  echo Can't configure TON
  exit /b %errorlevel%
)

ninja ton_crypto_lib
IF %errorlevel% NEQ 0 (
  echo Can't compile TON
  exit /b %errorlevel%
)

echo Creating artifacts...
cd ..
mkdir artifacts
mkdir artifacts\include

copy build\crypto\ton_crypto_lib.dll artifacts\
copy build\crypto\ton_crypto_lib.lib artifacts\
mkdir artifacts\include\ton
mkdir artifacts\include\crypto
mkdir artifacts\include\crypto\block
copy crypto\block\block-auto.h artifacts\include\crypto\block
copy crypto\block\block-parse.h artifacts\include\crypto\block
copy crypto\block\block.h artifacts\include\crypto\block
xcopy /s /k /h /i crypto\common\*.h* artifacts\include\crypto\common
xcopy /s /k /h /i crypto\tl\*.h* artifacts\include\crypto\tl
xcopy /s /k /h /i crypto\vm\*.h* artifacts\include\crypto\vm
cd artifacts\include\crypto\vm
del atom.h cp0.h dispatch.h fmt.hpp Hasher.h log.h memo.h opctable.h utils.h vm.h bls.h *ops.h
rmdir /s /q db
cd ..\..\..\..
copy ton\ton-types.h artifacts\include\ton
xcopy /s /k /h /i common\*.h* artifacts\include\common
xcopy /s /k /h /i tdutils\*.h* artifacts\include\tdutils
copy build\tdutils\td\utils\config.h artifacts\include\tdutils\td\utils\