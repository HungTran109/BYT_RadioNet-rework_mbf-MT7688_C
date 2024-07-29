export STAGING_DIR=/mnt/e/openwrt-sdk-gcc-11-2-0/staging_dir/
printenv STAGING_DIR
export OPEN_WRT_GCC_DIR=${STAGING_DIR}/toolchain-mipsel_24kc_gcc-11.2.0_musl/bin/
printenv OPEN_WRT_GCC_DIR
cd src/samples
make
