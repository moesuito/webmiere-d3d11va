# Community-fork installer

This folder builds the unsigned Inno Setup package used by the community fork. It intentionally uses the standard Inno Setup UI and does not include KawaiiEngine's proprietary installer artwork or icons.

Requirements:

- Visual Studio 2022 Build Tools with MSVC v143
- Adobe Premiere Pro 26 C++ SDK in the path configured by `WebMiere.vcxproj`
- Matching FFmpeg SDK at the repository sibling path `..\ffmpeg`
- Inno Setup 6
- KawaiiEngine FFmpeg 8.1.2 corresponding-source bundle at the default path used by the build script, or supplied through `-FfmpegSourceBundle`

Build:

```powershell
.\installer\build_installer.ps1
```

The installer, corresponding FFmpeg/dav1d source archive, and SHA-256 manifest are written to `dist\`. The installer is unsigned; avoiding SmartScreen warnings cannot be guaranteed without Authenticode signing and reputation.
