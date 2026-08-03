
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#define __STDC_CONSTANT_MACROS

#include "VideoDecoder.h"
#include "WebMiereLimits.h"
#include "WebMiereColorPolicy.h"

#include <windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <cstring>
#include <limits>
#include <new>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

using Microsoft::WRL::ComPtr;

static std::mutex gD3D11ComputeExecutionMutex;

struct Vp9oD3D11State
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Device3> device3;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11VideoDevice> videoDevice;
	ComPtr<ID3D11VideoContext> videoContext;
	ComPtr<ID3D11VideoContext1> videoContext1;

	ComPtr<ID3D11VideoProcessorEnumerator> bgraEnumerator;
	ComPtr<ID3D11VideoProcessor> bgraProcessor;
	ComPtr<ID3D11Texture2D> bgraTexture;
	ComPtr<ID3D11VideoProcessorOutputView> bgraOutputView;
	ComPtr<ID3D11Texture2D> bgraStaging;
	int bgraSrcW = 0;
	int bgraSrcH = 0;
	int bgraDstW = 0;
	int bgraDstH = 0;

	ComPtr<ID3D11ComputeShader> yuvShader;
	ComPtr<ID3D11Buffer> yuvConstants;
	ComPtr<ID3D11SamplerState> yuvSampler;
	ComPtr<ID3D11Texture2D> nv12Texture;
	ComPtr<ID3D11ShaderResourceView1> nv12YView;
	ComPtr<ID3D11ShaderResourceView1> nv12UVView;
	ComPtr<ID3D11Texture2D> yTexture;
	ComPtr<ID3D11Texture2D> uTexture;
	ComPtr<ID3D11Texture2D> vTexture;
	ComPtr<ID3D11UnorderedAccessView> yUav;
	ComPtr<ID3D11UnorderedAccessView> uUav;
	ComPtr<ID3D11UnorderedAccessView> vUav;
	ComPtr<ID3D11Texture2D> yStaging;
	ComPtr<ID3D11Texture2D> uStaging;
	ComPtr<ID3D11Texture2D> vStaging;
	int yuvSrcW = 0;
	int yuvSrcH = 0;
	int yuvDstW = 0;
	int yuvDstH = 0;
};

namespace {
struct AVBufferRefDeleter
{
	void operator()(AVBufferRef *p) const noexcept
	{
		if (p)
		{
			av_buffer_unref(&p);
		}
	}
};
using AVBufferRefGuard = std::unique_ptr<AVBufferRef, AVBufferRefDeleter>;

class D3D11DeviceLockGuard
{
public:
	explicit D3D11DeviceLockGuard(AVD3D11VADeviceContext *ctx) noexcept
		: mContext(ctx)
	{
		if (mContext != nullptr && mContext->lock != nullptr)
		{
			mContext->lock(mContext->lock_ctx);
		}
	}

	~D3D11DeviceLockGuard() noexcept
	{
		if (mContext != nullptr && mContext->unlock != nullptr)
		{
			mContext->unlock(mContext->lock_ctx);
		}
	}

private:
	AVD3D11VADeviceContext *mContext;
};

constexpr char kVp9oNv12ScaleShader[] =
	"cbuffer Dimensions : register(b0) {"
	" uint Width; uint Height; uint ChromaWidth; uint ChromaHeight;"
	"};"
	"Texture2D<float> InputY : register(t0);"
	"Texture2D<float2> InputUV : register(t1);"
	"SamplerState LinearClamp : register(s0);"
	"RWTexture2D<float> OutputY : register(u0);"
	"RWTexture2D<float> OutputU : register(u1);"
	"RWTexture2D<float> OutputV : register(u2);"
	"[numthreads(16, 16, 1)]"
	"void main(uint3 id : SV_DispatchThreadID) {"
	" if (id.x < Width && id.y < Height) {"
	"  float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);"
	"  OutputY[id.xy] = InputY.SampleLevel(LinearClamp, uv, 0);"
	" }"
	" if (id.x < ChromaWidth && id.y < ChromaHeight) {"
	"  float2 uvPos = (float2(id.xy) + 0.5) / float2(ChromaWidth, ChromaHeight);"
	"  float2 uv = InputUV.SampleLevel(LinearClamp, uvPos, 0);"
	"  OutputU[id.xy] = uv.x;"
	"  OutputV[id.xy] = uv.y;"
	" }"
	"}";
}


static std::string Utf16ToUtf8(const prUTF16Char *src)
{
	if (src == nullptr)
	{
		return std::string();
	}
	const wchar_t *w = reinterpret_cast<const wchar_t*>(src);
	int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 1)
	{
		return std::string();
	}
	std::string out(static_cast<size_t>(needed), '\0');
	int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], needed, nullptr, nullptr);
	if (written <= 1)
	{
		return std::string();
	}
	out.resize(static_cast<size_t>(written - 1));
	return out;
}


extern "C" {
static enum AVPixelFormat Vp9oGetFormat(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
	VideoDecoder *self = static_cast<VideoDecoder*>(ctx->opaque);

	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
	{
		if (*p == AV_PIX_FMT_D3D11)
		{
			if (self)
			{
				self->OnHwFormatAccepted();
			}
			return AV_PIX_FMT_D3D11;
		}
	}

	if (self)
	{
		self->OnHwFormatRejected();
	}
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
	{
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
		if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
		{
			return *p;
		}
	}
	return fmts[0];
}
}


static bool IsHwPixFmt(int fmt)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fmt));
	return (d != nullptr) && ((d->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0);
}


static bool CodecSupportsD3D11(const AVCodec *codec)
{
	if (codec == nullptr)
	{
		return false;
	}
	for (int i = 0; ; i++)
	{
		const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
		if (config == nullptr)
		{
			return false;
		}
		if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
			(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
		{
			return true;
		}
	}
}


static void CopyPlaneRespectingRowBytes(uint8_t *dst, int dstRowBytes,
										const uint8_t *src, int srcRowBytes,
										int widthBytes, int height)
{
	for (int y = 0; y < height; y++)
	{
		uint8_t       *d = dst + static_cast<ptrdiff_t>(y) * dstRowBytes;
		const uint8_t *s = src + static_cast<ptrdiff_t>(y) * srcRowBytes;
		std::memcpy(d, s, static_cast<size_t>(widthBytes));
	}
}


static bool RowBytesCanHold(int rowBytes, int widthBytes)
{
	return widthBytes > 0 && (rowBytes >= widthBytes || rowBytes <= -widthBytes);
}

static bool ValidateFramePlanesForDescriptor(const AVFrame *f, const AVPixFmtDescriptor *desc)
{
	if (f == nullptr || desc == nullptr)
	{
		return false;
	}

	bool usedPlanes[AV_NUM_DATA_POINTERS] = {};
	for (int i = 0; i < desc->nb_components; i++)
	{
		const int plane = desc->comp[i].plane;
		if (plane < 0 || plane >= AV_NUM_DATA_POINTERS)
		{
			return false;
		}
		usedPlanes[plane] = true;
	}

	for (int plane = 0; plane < AV_NUM_DATA_POINTERS; plane++)
	{
		if (usedPlanes[plane] && (f->data[plane] == nullptr || f->linesize[plane] == 0))
		{
			return false;
		}
	}
	return true;
}

static bool ValidateDecodedFrameForView(const AVFrame *f)
{
	if (f == nullptr ||
		!Vp9oIsValidVideoSize(f->width, f->height) ||
		f->format < 0)
	{
		return false;
	}

	if (f->format == AV_PIX_FMT_D3D11)
	{
		return f->hw_frames_ctx != nullptr && f->data[0] != nullptr;
	}

	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
	if (desc == nullptr)
	{
		return false;
	}
	if ((desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0)
	{
		return f->data[0] != nullptr;
	}
	return ValidateFramePlanesForDescriptor(f, desc);
}

static bool ValidateYuv420PFrame(const AVFrame *f)
{
	return f != nullptr &&
		   f->format == AV_PIX_FMT_YUV420P &&
		   Vp9oIsValidYuv420Size(f->width, f->height) &&
		   f->data[0] != nullptr &&
		   f->data[1] != nullptr &&
		   f->data[2] != nullptr &&
		   RowBytesCanHold(f->linesize[0], f->width) &&
		   RowBytesCanHold(f->linesize[1], f->width / 2) &&
		   RowBytesCanHold(f->linesize[2], f->width / 2);
}

static bool ValidateNV12Frame(const AVFrame *f)
{
	return f != nullptr &&
		   f->format == AV_PIX_FMT_NV12 &&
		   Vp9oIsValidYuv420Size(f->width, f->height) &&
		   f->data[0] != nullptr &&
		   f->data[1] != nullptr &&
		   RowBytesCanHold(f->linesize[0], f->width) &&
		   RowBytesCanHold(f->linesize[1], f->width);
}

static bool ValidateD3D11NV12Frame(const AVFrame *f,
								   AVHWFramesContext **outFramesCtx,
								   AVD3D11VADeviceContext **outD3D11Dev,
								   ID3D11Texture2D **outTexture,
								   UINT *outArraySlice)
{
	if (outFramesCtx != nullptr)
	{
		*outFramesCtx = nullptr;
	}
	if (outD3D11Dev != nullptr)
	{
		*outD3D11Dev = nullptr;
	}
	if (outTexture != nullptr)
	{
		*outTexture = nullptr;
	}
	if (outArraySlice != nullptr)
	{
		*outArraySlice = 0;
	}

	if (f == nullptr ||
		f->format != AV_PIX_FMT_D3D11 ||
		!Vp9oIsValidYuv420Size(f->width, f->height) ||
		f->data[0] == nullptr ||
		f->hw_frames_ctx == nullptr)
	{
		return false;
	}

	AVHWFramesContext *fctx = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
	if (fctx == nullptr ||
		fctx->sw_format != AV_PIX_FMT_NV12 ||
		fctx->device_ctx == nullptr ||
		fctx->device_ctx->hwctx == nullptr)
	{
		return false;
	}

	AVD3D11VADeviceContext *d3ddev =
		static_cast<AVD3D11VADeviceContext*>(fctx->device_ctx->hwctx);
	ID3D11Texture2D *texture = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
	if (d3ddev == nullptr || d3ddev->device == nullptr ||
		d3ddev->device_context == nullptr || d3ddev->video_device == nullptr ||
		d3ddev->video_context == nullptr || texture == nullptr)
	{
		return false;
	}
	const intptr_t rawIndex = reinterpret_cast<intptr_t>(f->data[1]);
	if (rawIndex < 0 || rawIndex > static_cast<intptr_t>(std::numeric_limits<UINT>::max()))
	{
		return false;
	}
	D3D11_TEXTURE2D_DESC textureDesc = {};
	texture->GetDesc(&textureDesc);
	const UINT arraySlice = static_cast<UINT>(rawIndex);
	if (textureDesc.Format != DXGI_FORMAT_NV12 ||
		arraySlice >= textureDesc.ArraySize ||
		static_cast<UINT>(f->width) > textureDesc.Width ||
		static_cast<UINT>(f->height) > textureDesc.Height)
	{
		return false;
	}

	if (outFramesCtx != nullptr)
	{
		*outFramesCtx = fctx;
	}
	if (outD3D11Dev != nullptr)
	{
		*outD3D11Dev = d3ddev;
	}
	if (outTexture != nullptr)
	{
		*outTexture = texture;
	}
	if (outArraySlice != nullptr)
	{
		*outArraySlice = arraySlice;
	}
	return true;
}


VideoDecoder::VideoDecoder()
	: mFmt(nullptr)
	, mCodecCtx(nullptr)
	, mFrame(nullptr)
	, mReceiveFrame(nullptr)
	, mSwFrame(nullptr)
	, mPacket(nullptr)
	, mPendingPacket(nullptr)
	, mHasPendingPacket(false)
	, mSws(nullptr)
	, mVideoStream(-1)
	, mWidth(0)
	, mHeight(0)
	, mFrameRateNum(60)
	, mFrameRateDen(1)
	, mTimeBaseNum(1)
	, mTimeBaseDen(1000)
	, mVideoStartTime(0)
	, mCurrentFrame(-1)
	, mHasFrame(false)
	, mEofDrained(false)
	, mLastReadError(0)
	, mFlushSent(false)
	, mShortSeekThreshold(120)
	, mUseHw(false)
	, mHwActive(false)
	, mDisableHw(false)
	, mFallbackReason(nullptr)
	, mSwsSrcFmt(-1)
	, mSwsSrcW(0)
	, mSwsSrcH(0)
	, mSwsDstW(0)
	, mSwsDstH(0)
	, mSwsFlags(SWS_BILINEAR)
	, mSwsYuv(nullptr)
	, mSwsYuvSrcFmt(-1)
	, mSwsYuvSrcW(0)
	, mSwsYuvSrcH(0)
	, mSwsYuvDstW(0)
	, mSwsYuvDstH(0)
	, mD3D11State(nullptr)
	, mOpened(false)
{
}

VideoDecoder::~VideoDecoder()
{
	Close();
}


void VideoDecoder::Close()
{
	ReleaseD3D11Resources();

	if (mSws)
	{
		sws_freeContext(mSws);
		mSws = nullptr;
	}
	if (mSwsYuv)
	{
		sws_freeContext(mSwsYuv);
		mSwsYuv = nullptr;
	}
	if (mPacket)
	{
		av_packet_free(&mPacket);
	}
	if (mPendingPacket)
	{
		av_packet_free(&mPendingPacket);
	}
	mHasPendingPacket = false;
	if (mSwFrame)
	{
		av_frame_free(&mSwFrame);
	}
	if (mReceiveFrame)
	{
		av_frame_free(&mReceiveFrame);
	}
	if (mFrame)
	{
		av_frame_free(&mFrame);
	}
	if (mCodecCtx)
	{
		avcodec_free_context(&mCodecCtx);
	}
	if (mFmt)
	{
		avformat_close_input(&mFmt);
	}

	mVideoStream			= -1;
	mCurrentFrame			= -1;
	mHasFrame				= false;
	mEofDrained				= false;
	mLastReadError			= 0;
	mFlushSent				= false;
	mUseHw					= false;
	mHwActive				= false;
	mFallbackReason			= nullptr;
	mSwsSrcFmt				= -1;
	mSwsSrcW = mSwsSrcH = mSwsDstW = mSwsDstH = 0;
	mSwsYuvSrcFmt			= -1;
	mSwsYuvSrcW = mSwsYuvSrcH = mSwsYuvDstW = mSwsYuvDstH = 0;
	mOpened					= false;
}


bool VideoDecoder::Open(const prUTF16Char *path)
{
	mPathUtf8 = Utf16ToUtf8(path);
	wchar_t forceCpu[8] = {};
	if (GetEnvironmentVariableW(L"WEBMIERE_FORCE_CPU_DECODE", forceCpu, static_cast<DWORD>(sizeof(forceCpu) / sizeof(forceCpu[0]))) > 0 &&
		forceCpu[0] == L'1')
	{
		mDisableHw = true;
	}
	return OpenInternal(!mDisableHw);
}


bool VideoDecoder::ReopenCpu()
{
	mDisableHw = true;
		return OpenInternal(false);
}


bool VideoDecoder::OpenInternal(bool allowHw)
{
	Close();

	if (mPathUtf8.empty())
	{
		return false;
	}

	if (avformat_open_input(&mFmt, mPathUtf8.c_str(), nullptr, nullptr) < 0)
	{
		Close();
		return false;
	}
	if (avformat_find_stream_info(mFmt, nullptr) < 0)
	{
		Close();
		return false;
	}

	const AVCodec *defaultDec = nullptr;
	mVideoStream = av_find_best_stream(mFmt, AVMEDIA_TYPE_VIDEO, -1, -1, &defaultDec, 0);
	if (mVideoStream < 0 || defaultDec == nullptr)
	{
		Close();
		return false;
	}

	AVStream *st = mFmt->streams[mVideoStream];
	const bool isVp9 = (st->codecpar->codec_id == AV_CODEC_ID_VP9);
	const bool isAv1 = (st->codecpar->codec_id == AV_CODEC_ID_AV1);
	const bool isSupportedVideo = isVp9 || isAv1;
	if (!isSupportedVideo)
	{
		Close();
		return false;
	}
	{
		const AVCodecParameters *par = st->codecpar;
		if (!Vp9oIsValidVideoSize(par->width, par->height))
		{
			Close();
			return false;
		}
		const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
		if (isSupportedVideo &&
			(par->format == AV_PIX_FMT_NONE || pd == nullptr ||
			 (pd->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0 ||
			 (pd->flags & AV_PIX_FMT_FLAG_RGB) != 0 ||
			 (pd->flags & AV_PIX_FMT_FLAG_ALPHA) != 0 ||
			 pd->comp[0].depth != 8 ||
			 pd->log2_chroma_w != 1 || pd->log2_chroma_h != 1))
		{
			Close();
			return false;
		}
		if (isSupportedVideo && !Vp9oIsStrictBt709(par))
		{
			Close();
			return false;
		}
	}

	mWidth  = st->codecpar->width;
	mHeight = st->codecpar->height;

	AVRational fr = st->avg_frame_rate;
	if (fr.num <= 0 || fr.den <= 0)
	{
		fr = st->r_frame_rate;
	}
	if (fr.num <= 0 || fr.den <= 0)
	{
		fr.num = 60;
		fr.den = 1;
	}
	mFrameRateNum = fr.num;
	mFrameRateDen = fr.den;
	mTimeBaseNum  = st->time_base.num;
	mTimeBaseDen  = st->time_base.den;
	const int64_t rawStartTime = (st->start_time == AV_NOPTS_VALUE) ? 0 : st->start_time;
	mVideoStartTime = rawStartTime;


	AVBufferRefGuard hwGuard;
	const AVCodec *hardwareDec = defaultDec;
	if (isAv1)
	{
		hardwareDec = avcodec_find_decoder_by_name("av1");
	}
	bool wantHw = allowHw && !mDisableHw && CodecSupportsD3D11(hardwareDec);
	if (wantHw)
	{
		AVBufferRef *hwRaw = nullptr;
		int hr = av_hwdevice_ctx_create(&hwRaw, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
		if (hr < 0)
		{
			wantHw = false;
		}
		else
		{
			hwGuard.reset(hwRaw);
					}
	}


	auto tryOpenCodec = [&](bool useHw) -> bool
	{
		const AVCodec *attemptDec = useHw ? hardwareDec : defaultDec;
		if (isAv1)
		{
			attemptDec = useHw ? hardwareDec : avcodec_find_decoder_by_name("libdav1d");
			if (!useHw && attemptDec == nullptr)
			{
				attemptDec = avcodec_find_decoder_by_name("libaom-av1");
			}
			if (!useHw && attemptDec == nullptr)
			{
				attemptDec = defaultDec;
			}
			if (attemptDec == nullptr)
			{
				return false;
			}
		}
		if (mCodecCtx)
		{
			avcodec_free_context(&mCodecCtx);
		}
		mCodecCtx = avcodec_alloc_context3(attemptDec);
		if (mCodecCtx == nullptr)
		{
			return false;
		}
		if (avcodec_parameters_to_context(mCodecCtx, st->codecpar) < 0)
		{
			return false;
		}
		mCodecCtx->thread_count = 0;
		mCodecCtx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

		if (useHw)
		{
			AVBufferRef *hwRef = av_buffer_ref(hwGuard.get());
			if (hwRef == nullptr)
			{
				mUseHw = false;
				return false;
			}
			mCodecCtx->hw_device_ctx = hwRef;
			mCodecCtx->opaque        = this;
			mCodecCtx->get_format    = Vp9oGetFormat;
			mUseHw = true;
		}
		else
		{
			mUseHw = false;
		}

		return avcodec_open2(mCodecCtx, attemptDec, nullptr) == 0;
	};

	bool opened = false;
	if (wantHw && hwGuard)
	{
		opened = tryOpenCodec(true);
	}
	if (!opened)
	{
		opened = tryOpenCodec(false);
	}
	if (!opened)
	{
		Close();
		return false;
	}


	mFrame         = av_frame_alloc();
	mReceiveFrame  = av_frame_alloc();
	mSwFrame       = av_frame_alloc();
	mPacket        = av_packet_alloc();
	mPendingPacket = av_packet_alloc();
	mHasPendingPacket = false;
	if (mFrame == nullptr || mReceiveFrame == nullptr || mSwFrame == nullptr ||
		mPacket == nullptr || mPendingPacket == nullptr)
	{
		Close();
		return false;
	}

	mCurrentFrame			= -1;
	mHasFrame				= false;
	mEofDrained				= false;
	mLastReadError			= 0;
	mFlushSent				= false;
	mOpened					= true;

	return true;
}


void VideoDecoder::OnHwFormatAccepted()
{
	mHwActive = true;
}

void VideoDecoder::OnHwFormatRejected()
{
	mHwActive = false;
}


int64_t VideoDecoder::FrameIndexOf(const AVFrame *f) const
{
	int64_t pts = f->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
	{
		pts = f->pts;
	}
	if (pts == AV_NOPTS_VALUE)
	{
		return mCurrentFrame + 1;
	}
	pts -= mVideoStartTime;
	if (pts < 0)
	{
		pts = 0;
	}
	const AVRational timeBase = { mTimeBaseNum, mTimeBaseDen };
	const AVRational frameBase = { mFrameRateDen, mFrameRateNum };

	return av_rescale_q_rnd(
		pts,
		timeBase,
		frameBase,
		AV_ROUND_NEAR_INF);
}


bool VideoDecoder::SeekBeforeFrame(int64_t targetFrame)
{
	if (targetFrame < 0)
	{
		targetFrame = 0;
	}

	AVRational tb     = { mTimeBaseNum, mTimeBaseDen };
	AVRational invFps = { mFrameRateDen, mFrameRateNum };

	int64_t		backoff		= 0;
	const int	kMaxAttempts = 6;

	for (int attempt = 0; attempt < kMaxAttempts; attempt++)
	{
		int64_t seekFrame = targetFrame - backoff;
		if (seekFrame < 0)
		{
			seekFrame = 0;
		}

		int64_t ts = av_rescale_q(seekFrame, invFps, tb) + mVideoStartTime;

		int ret = av_seek_frame(mFmt, mVideoStream, ts, AVSEEK_FLAG_BACKWARD);
		if (ret < 0)
		{
						return false;
		}

		avcodec_flush_buffers(mCodecCtx);
		if (mHasPendingPacket)
		{
			av_packet_unref(mPendingPacket);
			mHasPendingPacket = false;
		}
		mHasFrame		= false;
		mCurrentFrame	= -1;
		mEofDrained		= false;
		mLastReadError = 0;
		mFlushSent		= false;

		if (!DecodeNext())
		{
			if (seekFrame == 0)
			{
								return false;
			}
		}
		else
		{
			if (mCurrentFrame <= targetFrame)
			{
								return true;
			}
					}

		if (seekFrame == 0)
		{
			return mHasFrame;
		}
		backoff = (backoff == 0) ? 30 : (backoff * 2);
	}

		return mHasFrame;
}


bool VideoDecoder::DecodeNext()
{
	for (;;)
	{
		int ret = avcodec_receive_frame(mCodecCtx, mReceiveFrame);
		if (ret == 0)
		{
			const int64_t decodedFrame = FrameIndexOf(mReceiveFrame);

			av_frame_unref(mFrame);
			av_frame_move_ref(mFrame, mReceiveFrame);

			mCurrentFrame = decodedFrame;
			mHasFrame = true;
			return true;
		}
		if (ret == AVERROR_EOF)
		{
			mEofDrained = true;
			return false;
		}
		if (ret != AVERROR(EAGAIN))
		{
			if (mUseHw)
			{
				mFallbackReason = "hw-decode-receive";
			}
			return false;
		}

		if (mHasPendingPacket)
		{
			int sret = avcodec_send_packet(mCodecCtx, mPendingPacket);
			if (sret != AVERROR(EAGAIN))
			{
				av_packet_unref(mPendingPacket);
				mHasPendingPacket = false;
				if (sret < 0)
				{
					if (mUseHw)
					{
						mFallbackReason = "hw-decode-send";
					}
					return false;
				}
			}
			continue;
		}

		if (mFlushSent)
		{
			mEofDrained = true;
			return false;
		}

		int rd = av_read_frame(mFmt, mPacket);
		if (rd == AVERROR_EOF)
		{
			const int sret = avcodec_send_packet(mCodecCtx, nullptr);
			if (sret == 0 || sret == AVERROR_EOF)
			{
				mFlushSent = true;
			}
			else if (sret != AVERROR(EAGAIN))
			{
				if (mUseHw)
				{
					mFallbackReason = "hw-decode-flush";
				}
				return false;
			}
			continue;
		}
		if (rd < 0)
		{
			mLastReadError = rd;
			return false;
		}
		if (mPacket->stream_index == mVideoStream)
		{
			int sret = avcodec_send_packet(mCodecCtx, mPacket);
			if (sret == AVERROR(EAGAIN))
			{
				av_packet_move_ref(mPendingPacket, mPacket);
				mHasPendingPacket = true;
				continue;
			}
			if (sret < 0)
			{
				av_packet_unref(mPacket);
				if (mUseHw)
				{
					mFallbackReason = "hw-decode-send";
				}
				return false;
			}
		}
		av_packet_unref(mPacket);
	}
}


bool VideoDecoder::DecodeFrameToBGRA(int64_t targetFrame,
									 uint8_t *dstBGRA, int dstRowBytes,
									 int dstWidth, int dstHeight)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLastReadError = 0;

	if (!mOpened || dstBGRA == nullptr)
	{
		return false;
	}
	if (targetFrame < 0)
	{
		targetFrame = 0;
	}

	if (DecodeToTargetAndConvert(targetFrame, dstBGRA, dstRowBytes, dstWidth, dstHeight))
	{
		return true;
	}


	if (mUseHw && mFallbackReason != nullptr)
	{
				if (ReopenCpu())
		{
			return DecodeToTargetAndConvert(targetFrame, dstBGRA, dstRowBytes, dstWidth, dstHeight);
		}
			}
	return false;
}


bool VideoDecoder::DecodeToTargetAndConvert(int64_t targetFrame, uint8_t *dst, int rowBytes, int w, int h)
{
	Vp9oDecodedFrameView view;
	if (!DecodeFrameToSurface(targetFrame, &view))
	{
		return false;
	}
	return ConvertSurfaceToBGRA(view, dst, rowBytes, w, h);
}


bool VideoDecoder::DecodeFrameToSurface(int64_t targetFrame, Vp9oDecodedFrameView *outView)
{
	mFallbackReason = nullptr;

	if (outView == nullptr)
	{
		return false;
	}

	if (!(mHasFrame && mCurrentFrame == targetFrame))
	{


		const bool needSeek =
			(!mHasFrame) ||
			(targetFrame < mCurrentFrame) ||
			(targetFrame - mCurrentFrame > mShortSeekThreshold);

		if (needSeek)
		{
						if (!SeekBeforeFrame(targetFrame))
			{
								return false;
			}
		}

		while (!mHasFrame || mCurrentFrame < targetFrame)
		{
			if (!DecodeNext())
			{
								break;
			}
		}


		if (!mHasFrame)
		{
			return false;
		}

		if (mCurrentFrame < targetFrame)
		{
			const bool holdLastFrame =
				mEofDrained &&
				targetFrame == mCurrentFrame + 1;

			if (!holdLastFrame)
			{
				return false;
			}
		}
	}


	if (!ValidateDecodedFrameForView(mFrame))
	{
				return false;
	}


	outView->frame         = mFrame;
	outView->width         = mFrame->width;
	outView->height        = mFrame->height;
	outView->avPixelFormat = mFrame->format;
	outView->isD3D11       = (mFrame->format == AV_PIX_FMT_D3D11);
	for (int i = 0; i < 4; i++)
	{
		outView->data[i]     = mFrame->data[i];
		outView->linesize[i] = mFrame->linesize[i];
	}
	return true;
}


bool VideoDecoder::ConvertSurfaceToBGRA(const Vp9oDecodedFrameView &view, uint8_t *dst, int dstRowBytes, int dstW, int dstH)
{

	const AVFrame *src = view.frame;
	if (src == nullptr)
	{
		return false;
	}

	if (view.isD3D11)
	{
		if (mDisableHw)
		{
			mFallbackReason = "d3d11-disabled";
			return false;
		}


		if (src->data[0] == nullptr || src->hw_frames_ctx == nullptr)
		{
						return false;
		}


		if (ConvertToBGRA_D3D11(src, dst, dstRowBytes, dstW, dstH))
		{
			return true;
		}


		av_frame_unref(mSwFrame);
		int tr = av_hwframe_transfer_data(mSwFrame, src, 0);
		if (tr < 0)
		{
						mFallbackReason = "hwdownload";
			return false;
		}
				src = mSwFrame;
		if (!ValidateDecodedFrameForView(src) || IsHwPixFmt(src->format))
		{
			mFallbackReason = "hwdownload-frame";
			return false;
		}
	}
	else if (IsHwPixFmt(src->format))
	{

				mFallbackReason = "unexpected-hwfmt";
		return false;
	}

	return ConvertToBGRA(src, dst, dstRowBytes, dstW, dstH);
}


bool VideoDecoder::DecodeFrameToYUV420P(int64_t targetFrame,
									   uint8_t *dstY, int dstYRowBytes,
									   uint8_t *dstU, int dstURowBytes,
									   uint8_t *dstV, int dstVRowBytes,
									   int dstWidth, int dstHeight)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLastReadError = 0;

	if (!mOpened || dstY == nullptr || dstU == nullptr || dstV == nullptr)
	{
		return false;
	}
	if (targetFrame < 0)
	{
		targetFrame = 0;
	}

	Vp9oDecodedFrameView view;
	const bool decoded = DecodeFrameToSurface(targetFrame, &view);
	if (decoded &&
		ConvertSurfaceToYUV420P(view, dstY, dstYRowBytes, dstU, dstURowBytes,
								dstV, dstVRowBytes, dstWidth, dstHeight))
	{
		return true;
	}

	if (mUseHw && mFallbackReason != nullptr && ReopenCpu())
	{
		if (DecodeFrameToSurface(targetFrame, &view))
		{
			return ConvertSurfaceToYUV420P(view, dstY, dstYRowBytes, dstU, dstURowBytes,
										   dstV, dstVRowBytes, dstWidth, dstHeight);
		}
	}
	return false;
}


bool VideoDecoder::ConvertSurfaceToYUV420P(const Vp9oDecodedFrameView &view,
										   uint8_t *dstY, int dstYRowBytes,
										   uint8_t *dstU, int dstURowBytes,
										   uint8_t *dstV, int dstVRowBytes,
										   int dstW, int dstH)
{
	const AVFrame *src = view.frame;
	if (src == nullptr || dstY == nullptr || dstU == nullptr || dstV == nullptr)
	{
		return false;
	}

	if (!Vp9oIsValidYuv420Size(dstW, dstH))
	{
				return false;
	}

	if (!RowBytesCanHold(dstYRowBytes, dstW) ||
		!RowBytesCanHold(dstURowBytes, dstW / 2) ||
		!RowBytesCanHold(dstVRowBytes, dstW / 2))
	{
		return false;
	}


	if (view.isD3D11)
	{
		if (ConvertD3D11NV12ToYUV420P(view, dstY, dstYRowBytes, dstU, dstURowBytes,
									 dstV, dstVRowBytes, dstW, dstH))
		{
			return true;
		}
	}


	if (view.isD3D11)
	{
		if (mDisableHw)
		{
			mFallbackReason = "d3d11-disabled";
			return false;
		}
		av_frame_unref(mSwFrame);
		int tr = av_hwframe_transfer_data(mSwFrame, src, 0);
		if (tr < 0)
		{
			mFallbackReason = "hwdownload";
						return false;
		}
				src = mSwFrame;
		if (!ValidateDecodedFrameForView(src) || IsHwPixFmt(src->format))
		{
			mFallbackReason = "hwdownload-frame";
			return false;
		}
	}
	else if (IsHwPixFmt(src->format))
	{
		mFallbackReason = "unexpected-hwfmt";
				return false;
	}


	if (src->color_range == AVCOL_RANGE_JPEG)
	{
				return false;
	}

	const int srcFmt = src->format;
	const int srcW   = src->width;
	const int srcH   = src->height;
	if (!Vp9oIsValidVideoSize(srcW, srcH))
	{
		return false;
	}


	const bool isYuv420p = (srcFmt == AV_PIX_FMT_YUV420P);
	const bool isNv12    = (srcFmt == AV_PIX_FMT_NV12);
	if (!isYuv420p && !isNv12)
	{
				return false;
	}
	if (isYuv420p && !ValidateYuv420PFrame(src))
	{
		return false;
	}
	if (isNv12 && !ValidateNV12Frame(src))
	{
		return false;
	}

	const int cW = dstW / 2;
	const int cH = dstH / 2;


	if (isYuv420p && srcW == dstW && srcH == dstH)
	{
		CopyPlaneRespectingRowBytes(dstY, dstYRowBytes, src->data[0], src->linesize[0], dstW, dstH);
		CopyPlaneRespectingRowBytes(dstU, dstURowBytes, src->data[1], src->linesize[1], cW, cH);
		CopyPlaneRespectingRowBytes(dstV, dstVRowBytes, src->data[2], src->linesize[2], cW, cH);
				return true;
	}


	if (mSwsYuv == nullptr ||
		srcFmt != mSwsYuvSrcFmt || srcW != mSwsYuvSrcW || srcH != mSwsYuvSrcH ||
		dstW != mSwsYuvDstW || dstH != mSwsYuvDstH)
	{
		mSwsYuv = sws_getCachedContext(mSwsYuv, srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
									   dstW, dstH, AV_PIX_FMT_YUV420P,
									   mSwsFlags, nullptr, nullptr, nullptr);
		if (mSwsYuv == nullptr)
		{
			return false;
		}
		mSwsYuvSrcFmt = srcFmt;
		mSwsYuvSrcW   = srcW;
		mSwsYuvSrcH   = srcH;
		mSwsYuvDstW   = dstW;
		mSwsYuvDstH   = dstH;
			}


	uint8_t *dstData[4]     = { dstY, dstU, dstV, nullptr };
	int      dstLinesize[4] = { dstYRowBytes, dstURowBytes, dstVRowBytes, 0 };

	int scaledH = sws_scale(mSwsYuv, src->data, src->linesize, 0, srcH, dstData, dstLinesize);
	if (!(scaledH == dstH))
	{
		return false;
	}
		return true;
}


bool VideoDecoder::ConvertToBGRA(const AVFrame *f, uint8_t *dst, int dstRowBytes, int dstW, int dstH)
{
	if (f == nullptr || dst == nullptr || !Vp9oIsValidVideoSize(dstW, dstH))
	{
		return false;
	}
	if (!ValidateDecodedFrameForView(f) || IsHwPixFmt(f->format))
	{
		return false;
	}
	if (dstW > std::numeric_limits<int>::max() / 4)
	{
		return false;
	}
	const int dstWidthBytes = dstW * 4;

	if (!RowBytesCanHold(dstRowBytes, dstWidthBytes))
	{
		return false;
	}

	const int srcFmt = f->format;
	const int srcW   = f->width;
	const int srcH   = f->height;

	if (mSws == nullptr ||
		srcFmt != mSwsSrcFmt || srcW != mSwsSrcW || srcH != mSwsSrcH ||
		dstW != mSwsDstW || dstH != mSwsDstH)
	{

		mSws = sws_getCachedContext(mSws, srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
									dstW, dstH, AV_PIX_FMT_BGRA,
									mSwsFlags, nullptr, nullptr, nullptr);
		if (mSws == nullptr)
		{
			mFallbackReason = "sws_context";
			return false;
		}

		const int   srcRange = (f->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
		const int  *coef709  = sws_getCoefficients(SWS_CS_ITU709);
		sws_setColorspaceDetails(mSws, coef709, srcRange, coef709, 1, 0, 1 << 16, 1 << 16);

		mSwsSrcFmt = srcFmt;
		mSwsSrcW   = srcW;
		mSwsSrcH   = srcH;
		mSwsDstW   = dstW;
		mSwsDstH   = dstH;

			}


	uint8_t *dstData[4]     = { dst + static_cast<ptrdiff_t>(dstH - 1) * dstRowBytes, nullptr, nullptr, nullptr };
	int      dstLinesize[4] = { -dstRowBytes, 0, 0, 0 };

	int scaledH = sws_scale(mSws, f->data, f->linesize, 0, srcH, dstData, dstLinesize);
	if (!(scaledH == dstH))
	{
		mFallbackReason = "sws_scale";
		return false;
	}

	return true;
}




static void ResetBgraResources(Vp9oD3D11State *state)
{
	if (state == nullptr)
	{
		return;
	}
	state->bgraEnumerator.Reset();
	state->bgraProcessor.Reset();
	state->bgraTexture.Reset();
	state->bgraOutputView.Reset();
	state->bgraStaging.Reset();
	state->bgraSrcW = state->bgraSrcH = state->bgraDstW = state->bgraDstH = 0;
}


static void ResetYuvResources(Vp9oD3D11State *state)
{
	if (state == nullptr)
	{
		return;
	}
	state->yuvShader.Reset();
	state->yuvConstants.Reset();
	state->yuvSampler.Reset();
	state->nv12Texture.Reset();
	state->nv12YView.Reset();
	state->nv12UVView.Reset();
	state->yTexture.Reset();
	state->uTexture.Reset();
	state->vTexture.Reset();
	state->yUav.Reset();
	state->uUav.Reset();
	state->vUav.Reset();
	state->yStaging.Reset();
	state->uStaging.Reset();
	state->vStaging.Reset();
	state->yuvSrcW = state->yuvSrcH = state->yuvDstW = state->yuvDstH = 0;
}


static bool InitializeD3D11State(
	Vp9oD3D11State **statePtr,
	AVD3D11VADeviceContext *deviceContext)
{
	if (statePtr == nullptr || deviceContext == nullptr ||
		deviceContext->device == nullptr || deviceContext->device_context == nullptr ||
		deviceContext->video_device == nullptr || deviceContext->video_context == nullptr)
	{
		return false;
	}

	Vp9oD3D11State *state = *statePtr;
	if (state != nullptr && state->device.Get() == deviceContext->device)
	{
		return true;
	}

	delete state;
	state = new (std::nothrow) Vp9oD3D11State();
	if (state == nullptr)
	{
		*statePtr = nullptr;
		return false;
	}
	state->device = deviceContext->device;
	state->context = deviceContext->device_context;
	state->videoDevice = deviceContext->video_device;
	state->videoContext = deviceContext->video_context;
	if (FAILED(state->device.As(&state->device3)))
	{
		delete state;
		*statePtr = nullptr;
		return false;
	}
	state->videoContext.As(&state->videoContext1);
	*statePtr = state;
	return true;
}


static bool EnsureBgraResources(
	Vp9oD3D11State *state,
	int srcW,
	int srcH,
	int dstW,
	int dstH)
{
	if (state == nullptr || state->device == nullptr || state->videoDevice == nullptr ||
		!Vp9oIsValidVideoSize(srcW, srcH) || !Vp9oIsValidVideoSize(dstW, dstH))
	{
		return false;
	}
	if (state->bgraEnumerator != nullptr &&
		state->bgraSrcW == srcW && state->bgraSrcH == srcH &&
		state->bgraDstW == dstW && state->bgraDstH == dstH)
	{
		return true;
	}

	ResetBgraResources(state);
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
	contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	contentDesc.InputFrameRate.Numerator = 60;
	contentDesc.InputFrameRate.Denominator = 1;
	contentDesc.InputWidth = static_cast<UINT>(srcW);
	contentDesc.InputHeight = static_cast<UINT>(srcH);
	contentDesc.OutputFrameRate.Numerator = 60;
	contentDesc.OutputFrameRate.Denominator = 1;
	contentDesc.OutputWidth = static_cast<UINT>(dstW);
	contentDesc.OutputHeight = static_cast<UINT>(dstH);
	contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
	if (FAILED(state->videoDevice->CreateVideoProcessorEnumerator(
			&contentDesc, &state->bgraEnumerator)))
	{
		return false;
	}

	UINT inputSupport = 0;
	UINT outputSupport = 0;
	if (FAILED(state->bgraEnumerator->CheckVideoProcessorFormat(
			DXGI_FORMAT_NV12, &inputSupport)) ||
		FAILED(state->bgraEnumerator->CheckVideoProcessorFormat(
			DXGI_FORMAT_B8G8R8A8_UNORM, &outputSupport)) ||
		(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
		(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0 ||
		FAILED(state->videoDevice->CreateVideoProcessor(
			state->bgraEnumerator.Get(), 0, &state->bgraProcessor)))
	{
		ResetBgraResources(state);
		return false;
	}

	D3D11_TEXTURE2D_DESC outputDesc = {};
	outputDesc.Width = static_cast<UINT>(dstW);
	outputDesc.Height = static_cast<UINT>(dstH);
	outputDesc.MipLevels = 1;
	outputDesc.ArraySize = 1;
	outputDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	outputDesc.SampleDesc.Count = 1;
	outputDesc.Usage = D3D11_USAGE_DEFAULT;
	outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (FAILED(state->device->CreateTexture2D(
			&outputDesc, nullptr, &state->bgraTexture)))
	{
		ResetBgraResources(state);
		return false;
	}

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
	outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	if (FAILED(state->videoDevice->CreateVideoProcessorOutputView(
			state->bgraTexture.Get(), state->bgraEnumerator.Get(),
			&outputViewDesc, &state->bgraOutputView)))
	{
		ResetBgraResources(state);
		return false;
	}

	outputDesc.Usage = D3D11_USAGE_STAGING;
	outputDesc.BindFlags = 0;
	outputDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(state->device->CreateTexture2D(
			&outputDesc, nullptr, &state->bgraStaging)))
	{
		ResetBgraResources(state);
		return false;
	}
	state->bgraSrcW = srcW;
	state->bgraSrcH = srcH;
	state->bgraDstW = dstW;
	state->bgraDstH = dstH;
	return true;
}


static bool CreatePlaneResources(
	Vp9oD3D11State *state,
	UINT width,
	UINT height,
	ComPtr<ID3D11Texture2D> *texture,
	ComPtr<ID3D11UnorderedAccessView> *uav,
	ComPtr<ID3D11Texture2D> *staging)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(state->device->CreateTexture2D(
			&desc, nullptr, texture->ReleaseAndGetAddressOf())) ||
		FAILED(state->device->CreateUnorderedAccessView(
			texture->Get(), nullptr, uav->ReleaseAndGetAddressOf())))
	{
		return false;
	}
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	return SUCCEEDED(state->device->CreateTexture2D(
		&desc, nullptr, staging->ReleaseAndGetAddressOf()));
}


static bool EnsureYuvResources(
	Vp9oD3D11State *state,
	int srcW,
	int srcH,
	int dstW,
	int dstH)
{
	if (state == nullptr || state->device == nullptr || state->device3 == nullptr ||
		!Vp9oIsValidYuv420Size(srcW, srcH) ||
		!Vp9oIsValidYuv420Size(dstW, dstH))
	{
		return false;
	}
	if (state->nv12Texture != nullptr &&
		state->yuvSrcW == srcW && state->yuvSrcH == srcH &&
		state->yuvDstW == dstW && state->yuvDstH == dstH)
	{
		return true;
	}

	ResetYuvResources(state);
	ComPtr<ID3DBlob> shaderBlob;
	ComPtr<ID3DBlob> shaderErrors;
	if (FAILED(D3DCompile(
			kVp9oNv12ScaleShader, sizeof(kVp9oNv12ScaleShader) - 1,
			"WebMiereNV12Scale", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &shaderErrors)) ||
		shaderBlob == nullptr ||
		FAILED(state->device->CreateComputeShader(
			shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
			nullptr, &state->yuvShader)))
	{
		ResetYuvResources(state);
		return false;
	}

	D3D11_BUFFER_DESC constantDesc = {};
	constantDesc.ByteWidth = 16;
	constantDesc.Usage = D3D11_USAGE_DEFAULT;
	constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(state->device->CreateBuffer(
			&constantDesc, nullptr, &state->yuvConstants)))
	{
		ResetYuvResources(state);
		return false;
	}
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(state->device->CreateSamplerState(
			&samplerDesc, &state->yuvSampler)))
	{
		ResetYuvResources(state);
		return false;
	}

	D3D11_TEXTURE2D_DESC nv12Desc = {};
	nv12Desc.Width = static_cast<UINT>(srcW);
	nv12Desc.Height = static_cast<UINT>(srcH);
	nv12Desc.MipLevels = 1;
	nv12Desc.ArraySize = 1;
	nv12Desc.Format = DXGI_FORMAT_NV12;
	nv12Desc.SampleDesc.Count = 1;
	nv12Desc.Usage = D3D11_USAGE_DEFAULT;
	nv12Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(state->device->CreateTexture2D(
			&nv12Desc, nullptr, &state->nv12Texture)))
	{
		ResetYuvResources(state);
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC1 yViewDesc = {};
	yViewDesc.Format = DXGI_FORMAT_R8_UNORM;
	yViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	yViewDesc.Texture2D.MipLevels = 1;
	yViewDesc.Texture2D.PlaneSlice = 0;
	if (FAILED(state->device3->CreateShaderResourceView1(
			state->nv12Texture.Get(), &yViewDesc, &state->nv12YView)))
	{
		ResetYuvResources(state);
		return false;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC1 uvViewDesc = yViewDesc;
	uvViewDesc.Format = DXGI_FORMAT_R8G8_UNORM;
	uvViewDesc.Texture2D.PlaneSlice = 1;
	if (FAILED(state->device3->CreateShaderResourceView1(
			state->nv12Texture.Get(), &uvViewDesc, &state->nv12UVView)))
	{
		ResetYuvResources(state);
		return false;
	}

	const UINT chromaW = static_cast<UINT>(dstW / 2);
	const UINT chromaH = static_cast<UINT>(dstH / 2);
	if (!CreatePlaneResources(state, static_cast<UINT>(dstW),
			static_cast<UINT>(dstH), &state->yTexture, &state->yUav,
			&state->yStaging) ||
		!CreatePlaneResources(state, chromaW, chromaH, &state->uTexture,
			&state->uUav, &state->uStaging) ||
		!CreatePlaneResources(state, chromaW, chromaH, &state->vTexture,
			&state->vUav, &state->vStaging))
	{
		ResetYuvResources(state);
		return false;
	}
	state->yuvSrcW = srcW;
	state->yuvSrcH = srcH;
	state->yuvDstW = dstW;
	state->yuvDstH = dstH;
	return true;
}


static bool CopyMappedPlane(
	ID3D11DeviceContext *context,
	ID3D11Texture2D *staging,
	uint8_t *dst,
	int dstRowBytes,
	int width,
	int height)
{
	if (context == nullptr || staging == nullptr || dst == nullptr ||
		!RowBytesCanHold(dstRowBytes, width))
	{
		return false;
	}
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		return false;
	}
	CopyPlaneRespectingRowBytes(dst, dstRowBytes,
		static_cast<const uint8_t*>(mapped.pData),
		static_cast<int>(mapped.RowPitch), width, height);
	context->Unmap(staging, 0);
	return true;
}


bool VideoDecoder::ConvertToBGRA_D3D11(
	const AVFrame *f,
	uint8_t *dst,
	int dstRowBytes,
	int dstW,
	int dstH)
{
	mFallbackReason = "d3d11-bgra";
	if (f == nullptr || dst == nullptr ||
		!Vp9oIsValidVideoSize(dstW, dstH) ||
		dstW > std::numeric_limits<int>::max() / 4 ||
		!RowBytesCanHold(dstRowBytes, dstW * 4))
	{
		return false;
	}

	AVHWFramesContext *framesContext = nullptr;
	AVD3D11VADeviceContext *deviceContext = nullptr;
	ID3D11Texture2D *sourceTexture = nullptr;
	UINT arraySlice = 0;
	if (!ValidateD3D11NV12Frame(f, &framesContext, &deviceContext,
			&sourceTexture, &arraySlice))
	{
		return false;
	}
	(void)framesContext;

	D3D11DeviceLockGuard lock(deviceContext);
	if (!InitializeD3D11State(&mD3D11State, deviceContext) ||
		!EnsureBgraResources(mD3D11State, f->width, f->height, dstW, dstH))
	{
		return false;
	}

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
	inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	inputViewDesc.Texture2D.ArraySlice = arraySlice;
	ComPtr<ID3D11VideoProcessorInputView> inputView;
	if (FAILED(mD3D11State->videoDevice->CreateVideoProcessorInputView(
			sourceTexture, mD3D11State->bgraEnumerator.Get(),
			&inputViewDesc, &inputView)))
	{
		return false;
	}

	const RECT sourceRect = { 0, 0, f->width, f->height };
	const RECT outputRect = { 0, 0, dstW, dstH };
	mD3D11State->videoContext->VideoProcessorSetStreamSourceRect(
		mD3D11State->bgraProcessor.Get(), 0, TRUE, &sourceRect);
	mD3D11State->videoContext->VideoProcessorSetStreamDestRect(
		mD3D11State->bgraProcessor.Get(), 0, TRUE, &outputRect);
	mD3D11State->videoContext->VideoProcessorSetOutputTargetRect(
		mD3D11State->bgraProcessor.Get(), TRUE, &outputRect);
	mD3D11State->videoContext->VideoProcessorSetOutputAlphaFillMode(
		mD3D11State->bgraProcessor.Get(),
		D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);

	const bool fullRange = f->color_range == AVCOL_RANGE_JPEG;
	if (mD3D11State->videoContext1 != nullptr)
	{
		mD3D11State->videoContext1->VideoProcessorSetStreamColorSpace1(
			mD3D11State->bgraProcessor.Get(), 0,
			fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
				: DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
		mD3D11State->videoContext1->VideoProcessorSetOutputColorSpace1(
			mD3D11State->bgraProcessor.Get(),
			DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	}
	else
	{
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor = {};
		inputColor.Usage = 1;
		inputColor.YCbCr_Matrix = 1;
		inputColor.Nominal_Range = fullRange
			? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
			: D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor = {};
		outputColor.Usage = 1;
		mD3D11State->videoContext->VideoProcessorSetStreamColorSpace(
			mD3D11State->bgraProcessor.Get(), 0, &inputColor);
		mD3D11State->videoContext->VideoProcessorSetOutputColorSpace(
			mD3D11State->bgraProcessor.Get(), &outputColor);
	}

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = inputView.Get();
	if (FAILED(mD3D11State->videoContext->VideoProcessorBlt(
			mD3D11State->bgraProcessor.Get(),
			mD3D11State->bgraOutputView.Get(), 0, 1, &stream)))
	{
		return false;
	}
	mD3D11State->context->CopyResource(
		mD3D11State->bgraStaging.Get(), mD3D11State->bgraTexture.Get());

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(mD3D11State->context->Map(
			mD3D11State->bgraStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
	{
		return false;
	}
	uint8_t *flippedDst = dst + static_cast<ptrdiff_t>(dstH - 1) * dstRowBytes;
	CopyPlaneRespectingRowBytes(flippedDst, -dstRowBytes,
		static_cast<const uint8_t*>(mapped.pData),
		static_cast<int>(mapped.RowPitch), dstW * 4, dstH);
	mD3D11State->context->Unmap(mD3D11State->bgraStaging.Get(), 0);
	mFallbackReason = nullptr;
	return true;
}


bool VideoDecoder::ConvertD3D11NV12ToYUV420P(
	const Vp9oDecodedFrameView &view,
	uint8_t *dstY,
	int dstYRowBytes,
	uint8_t *dstU,
	int dstURowBytes,
	uint8_t *dstV,
	int dstVRowBytes,
	int dstW,
	int dstH)
{
	mFallbackReason = "d3d11-yuv420p";
	const AVFrame *f = view.frame;
	if (f == nullptr || dstY == nullptr || dstU == nullptr || dstV == nullptr ||
		!Vp9oIsValidYuv420Size(dstW, dstH) ||
		f->color_range == AVCOL_RANGE_JPEG ||
		!RowBytesCanHold(dstYRowBytes, dstW) ||
		!RowBytesCanHold(dstURowBytes, dstW / 2) ||
		!RowBytesCanHold(dstVRowBytes, dstW / 2))
	{
		return false;
	}

	AVHWFramesContext *framesContext = nullptr;
	AVD3D11VADeviceContext *deviceContext = nullptr;
	ID3D11Texture2D *sourceTexture = nullptr;
	UINT arraySlice = 0;
	if (!ValidateD3D11NV12Frame(f, &framesContext, &deviceContext,
			&sourceTexture, &arraySlice))
	{
		return false;
	}
	(void)framesContext;

	D3D11DeviceLockGuard lock(deviceContext);
	std::lock_guard<std::mutex> computeLock(gD3D11ComputeExecutionMutex);
	if (!InitializeD3D11State(&mD3D11State, deviceContext) ||
		!EnsureYuvResources(mD3D11State, f->width, f->height, dstW, dstH))
	{
		return false;
	}

	D3D11_TEXTURE2D_DESC sourceDesc = {};
	sourceTexture->GetDesc(&sourceDesc);
	const UINT sourceSubresource = D3D11CalcSubresource(
		0, arraySlice, sourceDesc.MipLevels);
	D3D11_BOX sourceBox = {};
	sourceBox.right = static_cast<UINT>(f->width);
	sourceBox.bottom = static_cast<UINT>(f->height);
	sourceBox.back = 1;
	mD3D11State->context->CopySubresourceRegion(
		mD3D11State->nv12Texture.Get(), 0, 0, 0, 0,
		sourceTexture, sourceSubresource, &sourceBox);

	struct ShaderDimensions
	{
		UINT width;
		UINT height;
		UINT chromaWidth;
		UINT chromaHeight;
	};
	const ShaderDimensions dimensions = {
		static_cast<UINT>(dstW), static_cast<UINT>(dstH),
		static_cast<UINT>(dstW / 2), static_cast<UINT>(dstH / 2)
	};
	mD3D11State->context->UpdateSubresource(
		mD3D11State->yuvConstants.Get(), 0, nullptr, &dimensions, 0, 0);

	ID3D11ShaderResourceView *shaderViews[2] = {
		mD3D11State->nv12YView.Get(), mD3D11State->nv12UVView.Get()
	};
	ID3D11UnorderedAccessView *outputViews[3] = {
		mD3D11State->yUav.Get(), mD3D11State->uUav.Get(),
		mD3D11State->vUav.Get()
	};
	ID3D11Buffer *constantBuffers[1] = { mD3D11State->yuvConstants.Get() };
	ID3D11SamplerState *samplers[1] = { mD3D11State->yuvSampler.Get() };
	mD3D11State->context->CSSetShader(mD3D11State->yuvShader.Get(), nullptr, 0);
	mD3D11State->context->CSSetShaderResources(0, 2, shaderViews);
	mD3D11State->context->CSSetUnorderedAccessViews(0, 3, outputViews, nullptr);
	mD3D11State->context->CSSetConstantBuffers(0, 1, constantBuffers);
	mD3D11State->context->CSSetSamplers(0, 1, samplers);
	mD3D11State->context->Dispatch(
		(static_cast<UINT>(dstW) + 15) / 16,
		(static_cast<UINT>(dstH) + 15) / 16, 1);

	ID3D11ShaderResourceView *nullShaderViews[2] = {};
	ID3D11UnorderedAccessView *nullOutputViews[3] = {};
	ID3D11Buffer *nullConstantBuffers[1] = {};
	ID3D11SamplerState *nullSamplers[1] = {};
	mD3D11State->context->CSSetShader(nullptr, nullptr, 0);
	mD3D11State->context->CSSetShaderResources(0, 2, nullShaderViews);
	mD3D11State->context->CSSetUnorderedAccessViews(0, 3, nullOutputViews, nullptr);
	mD3D11State->context->CSSetConstantBuffers(0, 1, nullConstantBuffers);
	mD3D11State->context->CSSetSamplers(0, 1, nullSamplers);

	mD3D11State->context->CopyResource(
		mD3D11State->yStaging.Get(), mD3D11State->yTexture.Get());
	mD3D11State->context->CopyResource(
		mD3D11State->uStaging.Get(), mD3D11State->uTexture.Get());
	mD3D11State->context->CopyResource(
		mD3D11State->vStaging.Get(), mD3D11State->vTexture.Get());
	if (!CopyMappedPlane(mD3D11State->context.Get(),
			mD3D11State->yStaging.Get(), dstY, dstYRowBytes, dstW, dstH) ||
		!CopyMappedPlane(mD3D11State->context.Get(),
			mD3D11State->uStaging.Get(), dstU, dstURowBytes, dstW / 2, dstH / 2) ||
		!CopyMappedPlane(mD3D11State->context.Get(),
			mD3D11State->vStaging.Get(), dstV, dstVRowBytes, dstW / 2, dstH / 2))
	{
		return false;
	}
	mFallbackReason = nullptr;
	return true;
}


void VideoDecoder::ReleaseD3D11Resources() noexcept
{
	delete mD3D11State;
	mD3D11State = nullptr;
}
