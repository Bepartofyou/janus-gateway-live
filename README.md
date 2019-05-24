# Janus-gateway live

[![janus-gateway-live compliant](https://img.shields.io/badge/rtmp%20live-janus--gateway-brightgreen.svg)](https://github.com/Bepartofyou/janus-gateway-live)

> Make janus-gateway more useful

 This repository may be used in the scenarios: ?Your client rtmp living is not stable because of the not stable network; ?You may use janus-gateway's plugins, and want a standard rtmp/http-flv/hls for sharing.

This repository support now:

* [x] webrtc(opus + h264) ------(push)------> rtmp (only h264)
* [ ] webrtc(opus + h264) ------(push)------> rtmp (aac + h264)
* [ ] webrtc(opus + h264) <------(pull)------- rtmp (aac + h264)
* [ ] webrtc(opus + h264) ---(multi mix + push)---> rtmp (aac + h264)


## Table of Contents

- [Janus-gateway live](#janus-gateway-live)
  - [Table of Contents](#table-of-contents)
  - [Background](#background)
  - [Install](#install)
    - [Preparation](#preparation)
    - [Patch](#patch)
    - [RTMP server or CDN](#rtmp-server-or-cdn)
  - [Usage](#usage)
    - [RTMP pubslish](#rtmp-pubslish)
  - [Maintainers](#maintainers)
  - [Contributing](#contributing)
  - [License](#license)

## Background

Janus-gateway is a wonderful webrtc gateway with many media scenarios, such as sip,videoroom,record and so on. Plugin record has record/playback api, so we can use the api for rtmp publish/play like record's record/playback.

> Proposal: Don't make rtp2rtmp as a plugin, because any plugin may use the function just like record


The goals for this repository are:

1. **An example**. May help someone for project using.
2. **Purpose** Make more thing happen using janus.


## Install

### Preparation
  
First you should make sure that you can install the original [janus-gateway](https://github.com/meetecho/janus-gateway) successfully with `--enable-post-processing`. Because if `enable-post-processing` works, representing your ENV has `Opus` and `FFmpeg`, which will be used later.

```sh
# My environment is macOS High Sierra(version 10.13.6)
$ git clone https://github.com/meetecho/janus-gateway.git
$ git checkout a7d7991a

# make some janus-gateway prebuild for janus-gateway README.md

$ ./autogen.sh
$ ./configure --prefix=/your/path/janus PKG_CONFIG_PATH=/usr/local/opt/openssl/lib/pkgconfig  --enable-post-processing
$ make -j8 && make install && make configs

# Then you can use janus-gateway's record plugin for record and play testing.
```

### Patch

Patch the `janus-gateway-live.patch`, plugin `recordplay` can publish video to **RTMP server or CDN**.

```sh
$ cp live.h live.c janus-gateway-live.patch   /your/janus/source/path
$ cd /your/janus/source/path
$ patch -p1 < janus-gateway-live.patch
$ ./configure --prefix=/your/path/janus PKG_CONFIG_PATH=/usr/local/opt/openssl/lib/pkgconfig  --enable-post-processing
$ make -j8 && make install && make configs
```

### RTMP server or CDN

In the `janus-gateway-live.patch`, I use local RTMP server for video publish. You can modify the RTMP server yourself in `conf/janus.plugin.recordplay.jcfg.sample.in` with **CDN**. If you just want to use local RTMP server for testing, here it is:

```sh
$ git clone https://github.com/nginx/nginx.git
$ git clone https://github.com/arut/nginx-rtmp-module.git
$ git clone https://github.com/openssl/openssl.git
$ cd nginx && ./auto/configure --prefix=/path/nginx --with-debug --add-module=/path/nginx-rtmp-module --with-openssl=/path/openssl
$ make -j8 && make install
$ cp nginx.conf /path/nginx/conf && cp /path/nginx && ./sbin/nginx -c conf/nginx.conf
```

## Usage

Use python SimpleHTTPServer for html web server:

```sh
$ cd /install/janus/path/share/janus/demos && nohup python -m SimpleHTTPServer 8888 &
$ /path/janus -C /path/janus.jcfg -p /path/janus.pid -L /path/janus.log -l -R -b -D -d 7 -e -B 50
```

### RTMP pubslish

Open the [demo](http://localhost:8888/)`http://localhost:8888/` , Record start! <br/> Then you can play the rtmp url with `ffplay rtmp://localhost:1935/qixi/recording-id`


## Maintainers

[@Bepartofyou](https://github.com/RichardLitt).

## Contributing

Feel free to dive in! [Open an issue](https://github.com/Bepartofyou/janus-gateway-live/issues/new) or submit PRs.

Standard Readme follows the [Contributor Covenant](http://contributor-covenant.org/version/1/3/0/) Code of Conduct.


## License

[MIT](LICENSE) Bepartofyou (七曦)
