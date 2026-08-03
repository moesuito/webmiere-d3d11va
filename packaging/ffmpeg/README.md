# FFmpeg 8.1.2 D3D11VA runtime

The community fork uses the unmodified FFmpeg source at commit `38b88335f99e76ed89ff3c93f877fdefce736c13` and dav1d at commit `42b2b24fb8819f1ed3643aa9cf2a62f03868e3aa`.

Build the shared x64 runtime with MSVC from an MSYS2 shell using the options in `configure.args.txt`. `PKG_CONFIG_PATH` must point to a static MSVC dav1d 1.5.1 installation, and `nasm` must be available.

No FFmpeg or dav1d source patches are applied. The resulting runtime is LGPLv3-or-later and consists of exactly five DLLs: avcodec, avformat, avutil, swresample, and swscale.
