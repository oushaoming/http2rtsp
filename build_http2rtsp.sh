
/home/build/lede/sdk-17.01.7/staging_dir/toolchain-mips_24kc_gcc-5.4.0_musl-1.1.16/bin/mips-openwrt-linux-musl-gcc -Wall -Os -s -static -o http2rtsp-mips-static-gcc5.4 http2rtsp.c
/home/build/lede/sdk-17.01.7/staging_dir/toolchain-mips_24kc_gcc-5.4.0_musl-1.1.16/bin/mips-openwrt-linux-musl-gcc -Wall -Os -s -o http2rtsp-mips-dymatic-gcc5.4 http2rtsp.c

/home/build/lede/sdk-23.05.5/staging_dir/toolchain-mipsel_24kc_gcc-12.3.0_musl/bin/mipsel-openwrt-linux-gcc -Wall -Os -s -static -o http2rtsp-mipsel-static-gcc12.3 http2rtsp.c
/home/build/lede/sdk-23.05.5/staging_dir/toolchain-mipsel_24kc_gcc-12.3.0_musl/bin/mipsel-openwrt-linux-gcc -Wall -Os -s -o http2rtsp-mipsel-dymatic-gcc12.3 http2rtsp.c

/opt/rt-n56u/toolchain-mipsel/toolchain-3.4.x/bin/mipsel-linux-uclibc-gcc -Wall -Os -s -static -o http2rtsp-mipsel-static-gcc7.4 http2rtsp.c
/opt/rt-n56u/toolchain-mipsel/toolchain-3.4.x/bin/mipsel-linux-uclibc-gcc -Wall -Os -s -o http2rtsp-mipsel-dymatic-gcc7.4 http2rtsp.c

curl -F "file=@/home/build/lede/gogit/http2rtsp/http2rtsp-mips-dymatic-gcc5.4" http://192.168.5.120/chfs/upload
curl -F "file=@/home/build/lede/gogit/http2rtsp/http2rtsp-mipsel-dymatic-gcc12.3" http://192.168.5.120/chfs/upload


zip ./http2rtsp-v1.1.zip ./http2rtsp-mip*
curl -F "file=@/home/build/lede/gogit/http2rtsp/http2rtsp-v1.1.zip" http://192.168.5.120/chfs/upload


#!/bin/sh /etc/rc.common
# /usr/sbin/http2rtsp

START=98

USE_PROCD=1
NAME=http2rtsp
PROG=http2rtsp

start_service() {
    procd_open_instance "$NAME"
    procd_set_param command "$PROG" -p 8090
    procd_set_param respawn
    procd_set_param stdout 1
    procd_set_param stderr 1
    procd_close_instance
}

stop_service() {
    :
}

reload_service() {
    restart
}