// Copyright (c) 2026 KawaiiEngine (Sashimiso)
// SPDX-License-Identifier: MPL-2.0

#include "../src/AudioDecoder.h"
#include "../src/Demuxer.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static double Energy(const std::vector<std::vector<float>> &audio, int64_t samples)
{
	double energy = 0.0;
	for (const std::vector<float> &channel : audio)
	{
		for (int64_t i = 0; i < samples; ++i)
		{
			energy += std::abs(channel[static_cast<size_t>(i)]);
		}
	}
	return energy;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2 || argc > 3)
	{
		std::wcerr << L"usage: audio_smoke.exe <media.mkv|media.webm|media.mp4> [audio-stream-ordinal]\n";
		return 2;
	}

	MediaProbeInfo probe = {};
	std::string probeError;
	if (!ProbeMedia(reinterpret_cast<const prUTF16Char*>(argv[1]), &probe, &probeError))
	{
		std::cerr << "probe failed: " << probeError << "\n";
		return 1;
	}
	const long streamOrdinal = (argc == 3) ? std::wcstol(argv[2], nullptr, 10) : 0;
	if (streamOrdinal < 0 || streamOrdinal >= probe.audioStreamCount)
	{
		std::cerr << "requested audio stream is not available\n";
		return 1;
	}

	const MediaProbeInfo::AudioStreamProbeInfo &stream = probe.audioStreams[streamOrdinal];
	AudioDecoder decoder;
	if (!decoder.Open(reinterpret_cast<const prUTF16Char*>(argv[1]), stream.ffmpegStreamIndex))
	{
		std::cerr << "decoder open failed\n";
		return 1;
	}
	if (decoder.SampleRate() != stream.sampleRate || decoder.Channels() != stream.channels)
	{
		std::cerr << "decoder output does not match probed audio metadata\n";
		return 1;
	}

	constexpr int64_t kReadSize = 2048;
	std::vector<std::vector<float>> audio(
		static_cast<size_t>(decoder.Channels()),
		std::vector<float>(static_cast<size_t>(kReadSize)));
	float *planes[2] = { nullptr, nullptr };
	for (int channel = 0; channel < decoder.Channels(); ++channel)
	{
		planes[channel] = audio[static_cast<size_t>(channel)].data();
	}

	const int64_t firstRead = decoder.ReadSamples(0, kReadSize, planes, decoder.Channels());
	const double firstEnergy = Energy(audio, firstRead);
	if (firstRead != kReadSize || decoder.HadReadError() || firstEnergy <= 0.0)
	{
		std::cerr << "initial decode failed: samples=" << firstRead
			<< " error=" << decoder.LastReadError()
			<< " energy=" << firstEnergy << "\n";
		return 1;
	}

	const int64_t seekSample = decoder.SampleRate();
	const int64_t seekRead = decoder.ReadSamples(seekSample, kReadSize, planes, decoder.Channels());
	const double seekEnergy = Energy(audio, seekRead);
	if (seekRead != kReadSize || decoder.HadReadError() || seekEnergy <= 0.0)
	{
		std::cerr << "non-sequential decode failed: samples=" << seekRead
			<< " error=" << decoder.LastReadError()
			<< " energy=" << seekEnergy << "\n";
		return 1;
	}

	std::cout << "audio_smoke_ok=1\n"
		<< "audio_stream=" << streamOrdinal << "\n"
		<< "codec=" << static_cast<int>(stream.codec) << "\n"
		<< "sample_rate=" << decoder.SampleRate() << "\n"
		<< "channels=" << decoder.Channels() << "\n"
		<< "initial_samples=" << firstRead << "\n"
		<< "seek_sample=" << seekSample << "\n"
		<< "seek_samples=" << seekRead << "\n";
	return 0;
}
