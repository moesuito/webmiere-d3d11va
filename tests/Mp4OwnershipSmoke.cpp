// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "WebMiere.h"
#include "Demuxer.h"
#include "VideoDecoder.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <windows.h>


static bool ProbeExpected(const wchar_t *path, bool expected, WebMiereCodec codec)
{
	MediaProbeInfo probe = {};
	std::string error;
	const bool accepted = ProbeMedia(
		reinterpret_cast<const prUTF16Char*>(path), &probe, &error);
	if (accepted != expected ||
		(accepted && (probe.container != WEBMIERE_CONTAINER_MP4 || probe.videoCodec != codec)))
	{
		std::fwprintf(stderr, L"unexpected ownership for %ls: accepted=%d error=%hs\n",
			path, accepted ? 1 : 0, error.c_str());
		return false;
	}
	return true;
}


int wmain(int argc, wchar_t **argv)
{
	if (argc != 5)
	{
		std::fwprintf(stderr,
			L"usage: mp4_ownership_smoke.exe <WebMiere.prm> <AV1.mp4> <H264.mp4> <HEVC.mp4>\n");
		return 2;
	}

	HMODULE module = LoadLibraryW(argv[1]);
	if (module == nullptr)
	{
		std::fwprintf(stderr, L"LoadLibraryW failed: %lu\n", GetLastError());
		return 1;
	}

	auto importEntry = reinterpret_cast<decltype(&xImportEntry)>(
		GetProcAddress(module, "xImportEntry"));
	if (importEntry == nullptr)
	{
		std::fprintf(stderr, "xImportEntry not exported\n");
		FreeLibrary(module);
		return 1;
	}

	imStdParms standard = {};
	standard.imInterfaceVer = IMPORTMOD_VERSION_6;
	imImportInfoRec importer = {};
	const prMALError initResult = importEntry(imInit, &standard, &importer, nullptr);
	if (initResult != imIsCacheable ||
		importer.priority != 1000)
	{
		std::fprintf(stderr, "importer priority contract failed: result=%d priority=%d\n",
			static_cast<int>(initResult), static_cast<int>(importer.priority));
		FreeLibrary(module);
		return 1;
	}

	const char *extensions[] = { "webm", "mkv", "mp4" };
	const csSDK_int32 filetypes[] = {
		WEBMIERE_FILETYPE_WEBM, WEBMIERE_FILETYPE_MKV, WEBMIERE_FILETYPE_MP4
	};
	for (csSDK_size_t index = 0; index < 3; index++)
	{
		imIndFormatRec record = {};
		if (importEntry(imGetIndFormat, nullptr, reinterpret_cast<void*>(index), &record) != malNoError ||
			record.filetype != filetypes[index] ||
			std::strcmp(record.PlatformExtension, extensions[index]) != 0)
		{
			std::fprintf(stderr, "format registration failed at index %llu\n",
				static_cast<unsigned long long>(index));
			FreeLibrary(module);
			return 1;
		}
	}
	imIndFormatRec exhausted = {};
	if (importEntry(imGetIndFormat, nullptr, reinterpret_cast<void*>(3), &exhausted) != imBadFormatIndex)
	{
		std::fprintf(stderr, "format registration did not stop at index 3\n");
		FreeLibrary(module);
		return 1;
	}

	const bool ownershipOk =
		ProbeExpected(argv[2], true, WEBMIERE_CODEC_AV1) &&
		ProbeExpected(argv[3], false, WEBMIERE_CODEC_OTHER) &&
		ProbeExpected(argv[4], false, WEBMIERE_CODEC_OTHER);
	if (!ownershipOk)
	{
		FreeLibrary(module);
		return 1;
	}

	VideoDecoder decoder;
	if (!decoder.Open(reinterpret_cast<const prUTF16Char*>(argv[2])))
	{
		std::fprintf(stderr, "AV1 MP4 decoder open failed\n");
		FreeLibrary(module);
		return 1;
	}
	const int outputWidth = decoder.Width() / 2;
	const int outputHeight = decoder.Height() / 2;
	std::vector<uint8_t> pixels(
		static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight) * 4);
	if (outputWidth <= 0 || outputHeight <= 0 ||
		!decoder.DecodeFrameToBGRA(0, pixels.data(), outputWidth * 4, outputWidth, outputHeight))
	{
		std::fprintf(stderr, "AV1 MP4 frame decode failed\n");
		FreeLibrary(module);
		return 1;
	}
	FreeLibrary(module);

	std::printf("registered webm/mkv/mp4; AV1 decoded at half size; H.264/HEVC rejected\n");
	return 0;
}
