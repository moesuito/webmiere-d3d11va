# Third-Party Notices

This file summarizes factual third-party licensing and distribution information for WebMiere. It is not legal advice, and the applicable upstream license terms remain controlling.

## WebMiere Licensing Scope

- Community-fork publisher: moesuito
- Upstream project: KawaiiEngine/WebMiere
- Copyright: Copyright (c) 2026 KawaiiEngine (Sashimiso)
- Source files under `src/`: Mozilla Public License 2.0
- Installed source license text: `assets\licenses\WebMiere-MPL-2.0.txt`
- Corresponding source for this fork release: https://github.com/moesuito/webmiere-d3d11va/releases/
- Community-fork binaries and the installer do not limit the rights granted under the MPL-2.0 to the source files under `src/`.
- The installer, artwork, characters, logos, branding, and other materials are not licensed under the MPL unless expressly stated.

## Adobe Premiere Pro C++ SDK

- Adobe Premiere Pro 26.0 C++ SDK is a build-time dependency.
- Adobe SDK headers, samples, documentation, libraries, and binaries are not vendored or redistributed by this repository.
- Developers must obtain the SDK separately under Adobe's terms.

## FFmpeg

This fork dynamically links to its packaged shared FFmpeg build.

- FFmpeg version: 8.1.2
- FFmpeg commit: `38b88335f99e76ed89ff3c93f877fdefce736c13`
- Build identity: `8.1.2-kawaiiengine-webmiere-d3d11va`
- License reported by the runtime: `LGPL version 3 or later`
- Original source bundle: `KawaiiEngine/WebMiere-FFmpeg` release `ffmpeg-webmiere-8.1.2-4`
- Linkage: dynamic/shared DLL linkage
- Local FFmpeg source changes: none
- Installed license texts: `assets\licenses\FFmpeg-COPYING.LGPLv3.txt` and `assets\licenses\FFmpeg-COPYING.GPLv3.txt`

Runtime contains exactly:

```text
avcodec-62.dll
avformat-62.dll
avutil-60.dll
swscale-9.dll
swresample-6.dll
```

The exact FFmpeg and dav1d source archives, D3D11VA build configuration, and license records are included at `assets\licenses\source\WebMiere-D3D11VA-FFmpeg-8.1.2-Corresponding-Source.zip`.

The normal hardware path uses FFmpeg D3D11VA through the installed AMD, Intel, or NVIDIA driver. AV1 software fallback uses libdav1d; VP9 software fallback uses the FFmpeg native decoder.

## dav1d

- Version: 1.5.1
- Commit: `42b2b24fb8819f1ed3643aa9cf2a62f03868e3aa`
- License: BSD 2-Clause
- Linkage: the static dav1d library is linked into FFmpeg `avcodec-62.dll`.
- No separate `dav1d.dll` or `libdav1d.dll` is shipped.
- Installed license text: `assets\licenses\dav1d-COPYING.BSD-2-Clause.txt`, copied from the exact pinned upstream `COPYING`.
- The exact corresponding dav1d source archive and build records are included in `assets\licenses\source\WebMiere-D3D11VA-FFmpeg-8.1.2-Corresponding-Source.zip`.

## Direct3D 11

- D3D11VA, the D3D11 Video Processor, and the compute-shader path use Windows and GPU-driver interfaces supplied by Microsoft and the installed hardware vendor.
- No CUDA, NPP, NVDEC SDK, AMF, Media Foundation, DXVA2, D3D12VA, or vendor runtime DLL is bundled by this fork.

## Microsoft Runtime

- WebMiere uses the MSVC `/MD` runtime model.
- Microsoft Visual C++ 2015-2022 Redistributable x64 may be required.
- The installer may bundle `VC_redist.x64.exe` as a prerequisite payload and run it during installation.
- `VC_redist.x64.exe` is not installed into the final WebMiere plugin directory.
- Redistribution must follow Microsoft's applicable terms.
- Installed acquisition note: `assets\licenses\Microsoft-Visual-Cpp-Redistributable.txt`

## Installer

- The community-fork setup executable is built with Inno Setup; see the Inno Setup project for its license terms.

## Installer and Brand Assets

- Mina and Miere are original WebMiere characters belonging to
  KawaiiEngine (Sashimiso).
- WebMiere logos, icons, installer artwork, promotional images,
  and character assets are proprietary and all rights are reserved.
- Their use is governed by `ARTWORK_POLICY.md`.
- Non-commercial fan art is welcome under that policy.
- The community-fork installer uses the standard Inno Setup presentation and does not bundle the proprietary WebMiere installer artwork or icons.
