#include "TEncoderInstance.h"
#include "SoyH264.h"
#include "MagicEnum/include/magic_enum.hpp"
#include "json11.hpp"
#include "PopH264.h"

#if defined(TARGET_OSX) && defined(TARGET_ARCH_INTEL64)
#define OSX_X264_SUPPORT
#endif




//	gr: disabled on ios atm as there's no x64 build for simulator. See if we can change this at compile time (but can't do the same with linking!)
#if defined(TARGET_ANDROID) || defined(TARGET_IOS) || !defined(OSX_X264_SUPPORT)
//	no x264
#else
#define ENABLE_X264
#endif

#if defined(TARGET_IOS) || defined(TARGET_OSX)
#define ENABLE_AVF
#endif

#if defined(TARGET_WINDOWS)
#define ENABLE_MEDIAFOUNDATION
#endif

#if defined(ENABLE_X264)
#if defined(_MSC_VER)	//	different warning syntax, but we don't wanna do this globally :/
//#define warning message
#pragma message("X264 encoder enabled")
#else
#pragma warning("X264 encoder enabled")
#endif
#include "X264Encoder.h"
#endif

#if defined(ENABLE_AVF)
#include "AvfEncoder.h"
#endif

#if defined(ENABLE_MEDIAFOUNDATION)
#include "MediaFoundationEncoder.h"
#endif

//	ENABLE_NVIDIA should be defined by makefile/project now
#if defined(ENABLE_NVIDIA)
#pragma warning("Nvidia encoder enabled")
#include "NvidiaEncoder.h"
#endif

PopH264::TEncoderInstance::TEncoderInstance(const std::string& OptionsJsonString)
{
	//	get options
	std::string ParseError;
	auto Options = json11::Json::parse(OptionsJsonString,ParseError);
	if ( ParseError.length() )
	{
		ParseError = "Error parsing encoder options json: " + ParseError;
		throw Soy::AssertException(ParseError);
	}
	
	auto OnOutputPacket = [this](TPacket& Packet)
	{
		this->OnNewPacket(Packet);
	};
	
	auto EncoderName = Options[POPH264_ENCODER_KEY_ENCODERNAME].string_value();

#if defined(ENABLE_AVF)
	if ( EncoderName.empty() || EncoderName == Avf::TEncoder::Name )
	{
		Avf::TEncoderParams Params(Options);
		mEncoder.reset( new Avf::TEncoder(Params,OnOutputPacket) );
		return;
	}
#endif
	
#if defined(ENABLE_MEDIAFOUNDATION)
	if (EncoderName.empty() || EncoderName == MediaFoundation::TEncoder::Name)
	{
		try
		{
			MediaFoundation::TEncoderParams Params(Options);
			mEncoder.reset(new MediaFoundation::TEncoder(Params, OnOutputPacket));
			return;
		}
		catch (std::exception& e)
		{
			std::Debug << e.what() << std::endl;
		}
	}
#endif
	
	
#if defined(ENABLE_NVIDIA)
	if ( EncoderName.empty() || EncoderName == Nvidia::TEncoder::Name )
	{
		Nvidia::TEncoderParams Params(Options);
		mEncoder.reset( new Nvidia::TEncoder(Params,OnOutputPacket) );
		return;
	}
#endif
	
	
#if defined(ENABLE_X264)
	if ( EncoderName.empty() || EncoderName == X264::TEncoder::Name )
	{
		X264::TEncoderParams Params(Options);
		mEncoder.reset( new X264::TEncoder(Params,OnOutputPacket) );
		return;
	}
#endif
	
	//	success!
	if ( mEncoder )
		return;
	
	std::stringstream Error;
	Error << "No encoder supported (requested \"" << EncoderName << "\") empty=" << EncoderName.empty();
	throw Soy::AssertException(Error);
}

void PopH264::TEncoderInstance::EndOfStream()
{
	mEncoder->FinishEncoding();
}

static SoyPixels DummyPixels;
SoyPixelsImpl& GetDummyPixels(SoyPixelsMeta Meta)
{
	DummyPixels.Init(Meta);
	return DummyPixels;
}

void PopH264::TEncoderInstance::PushFrame(const std::string& Meta,const uint8_t* LumaData,const uint8_t* ChromaUData,const uint8_t* ChromaVData)
{
	std::string ParseError;
	auto Json = json11::Json::parse( Meta, ParseError );
	//	these return 0 if missing
	auto Width = Json[POPH264_ENCODEFRAME_KEY_WIDTH].int_value();
	auto Height = Json[POPH264_ENCODEFRAME_KEY_HEIGHT].int_value();
	auto LumaSize = Json[POPH264_ENCODEFRAME_KEY_LUMASIZE].int_value();
	auto ChromaUSize = Json[POPH264_ENCODEFRAME_KEY_CHROMAUSIZE].int_value();
	auto ChromaVSize = Json[POPH264_ENCODEFRAME_KEY_CHROMAVSIZE].int_value();
	auto Keyframe = Json[POPH264_ENCODEFRAME_KEY_KEYFRAME].bool_value();
	auto FormatName = Json[POPH264_ENCODEFRAME_KEY_FORMAT].string_value();

	//	check for data/size mismatch
	if ( LumaData && LumaSize==0 )
		throw Soy::AssertException("Luma pointer but zero LumaSize");
	if ( !LumaData && LumaSize!=0 )
		throw Soy::AssertException("Luma null but LumaSize nonzero");
	
	if ( ChromaUData && ChromaUSize==0 )
		throw Soy::AssertException("ChromaU pointer but zero ChromaUSize");
	if ( !ChromaUData && ChromaUSize!=0 )
		throw Soy::AssertException("ChromaU null but ChromaUSize nonzero");
	
	if ( ChromaVData && ChromaVSize==0 )
		throw Soy::AssertException("ChromaV pointer but zero ChromaVSize");
	if ( !ChromaVData && ChromaVSize!=0 )
		throw Soy::AssertException("ChromaV null but ChromaVSize nonzero");
	
	//	check for special case of 1 plane
	if (LumaData && !ChromaUData && !ChromaVData)
	{
		auto PixelFormat = SoyPixelsFormat::Greyscale;
		//	only one plane
		if (!FormatName.empty())
		{
			//	todo: check for fourccs
			auto PixelFormatMaybe = magic_enum::enum_cast<SoyPixelsFormat::Type>(FormatName);
			if (PixelFormatMaybe.has_value())
			{
				PixelFormat = *PixelFormatMaybe;
			}
			else
			{
				//	gr: magic_enum currenty isnt working
				PixelFormat = SoyPixelsFormat::ToType(FormatName);
				if (PixelFormat == SoyPixelsFormat::Invalid)
					throw Soy::AssertException(std::string("Unrecognised pixel format ") + FormatName);
			}
		}

		SoyPixelsRemote LumaPixels(const_cast<uint8_t*>(LumaData), Width, Height, LumaSize, PixelFormat );
		
		#if defined(TARGET_IOS)
		{
			//	gr: ios seems to encode this pure grey, so pad with fake buffers;
			//		osx seems to encode greyscale without fuss?
			//	gr: that Encode() isn't implemented, turn into yuv
			if ( PixelFormat == SoyPixelsFormat::Greyscale )
			{
				SoyPixels YuvPixels(LumaPixels);
				YuvPixels.SetFormat( SoyPixelsFormat::Yuv_8_88 );
				mEncoder->Encode( YuvPixels, Meta, Keyframe);
				return;
				/*
				//	gr: ios seems to encode this pure grey, so pad with fake buffers;
				#if defined(TARGET_IOS)
				{
					SoyPixelsMeta ChromaPlaneMeta( Width/2, Height/2, SoyPixelsFormat::Greyscale );
					auto& ChromaXPlane = GetDummyPixels(ChromaPlaneMeta);
					mEncoder->Encode( LumaPixels, ChromaXPlane, ChromaXPlane, Meta, Keyframe );
				}
				#else
				{
					mEncoder->Encode( LumaPixels, Meta, Keyframe);
				}
				#endif
				*/
			}
		}
		#endif

		mEncoder->Encode( LumaPixels, Meta, Keyframe);
		return;
	}

	if ( LumaData && ChromaUData && ChromaVData )
	{
		SoyPixelsMeta YuvMeta(Width, Height, SoyPixelsFormat::Yuv_8_8_8);

		BufferArray<SoyPixelsMeta, 3> YuvMetas;
		YuvMeta.GetPlanes(GetArrayBridge(YuvMetas));
		auto WidthChroma = YuvMetas[1].GetWidth();
		auto HeightChroma = YuvMetas[1].GetHeight();

		//	look out for striped data and make a single pixel buffer
		{
			auto* StripedLumaData = LumaData;
			auto* StripedChromaUData = LumaData + YuvMetas[0].GetDataSize();
			auto* StripedChromaVData = StripedChromaUData + YuvMetas[1].GetDataSize();
			if ( StripedLumaData==LumaData && StripedChromaUData==ChromaUData && StripedChromaVData==ChromaVData )
			{
				auto TotalSize = LumaSize + ChromaUSize + ChromaVSize;
				SoyPixelsRemote PixelsStriped( const_cast<uint8_t*>(LumaData), TotalSize, YuvMeta );
				mEncoder->Encode( PixelsStriped, Meta, Keyframe );
				return;
			}
		}
		
		//	yuv_8_8_8
		SoyPixelsRemote PixelsY( const_cast<uint8_t*>(LumaData), Width, Height, LumaSize, SoyPixelsFormat::Luma );
		SoyPixelsRemote PixelsU( const_cast<uint8_t*>(ChromaUData), WidthChroma, HeightChroma, ChromaUSize, SoyPixelsFormat::ChromaU_8 );
		SoyPixelsRemote PixelsV( const_cast<uint8_t*>(ChromaVData), WidthChroma, HeightChroma, ChromaVSize, SoyPixelsFormat::ChromaV_8 );
		
		mEncoder->Encode( PixelsY, PixelsU, PixelsV, Meta, Keyframe );
		return;
	}
	
	std::stringstream Error;
	Error << "Unable to get a YUV combination from YUV sizes [" << LumaSize << "," << ChromaUSize << "," << ChromaVSize << "]";
	throw Soy::AssertException(Error);
}


void PopH264::TEncoderInstance::PeekPacket(json11::Json::object& Meta)
{
	if (mPackets.IsEmpty())
		return;

	size_t DataSize = 0;
	std::string InputMeta;
	{
		std::lock_guard<std::mutex> Lock(mPacketsLock);
		auto& Packet0 = mPackets[0];
		DataSize = Packet0.mData ? Packet0.mData->GetDataSize() : 0;
		InputMeta = Packet0.mInputMeta;
	}

	//	write meta
	Meta["DataSize"] = static_cast<int>(DataSize);
	if (!InputMeta.empty())
	{
		using namespace json11;
		//	we're expecting json, so make it an object
		std::string ParseError;
	auto MetaObject = Json::parse(InputMeta, ParseError);

		//	this shouldn't come up, as we've already parsed it on input, but just in case
		if (!ParseError.empty())
		{
			Meta["Meta"] = std::string("PopH264 Unexpected parse error ") + ParseError;
		}
		else
		{
			Meta["Meta"] = MetaObject;
		}
	}
}

size_t PopH264::TEncoderInstance::PeekNextFrameSize()
{
	{
		std::lock_guard<std::mutex> Lock(mPacketsLock);
		if ( !mPackets.IsEmpty() )
		{
			auto& Packet0 = mPackets[0];
			return Packet0.mData->GetSize();
		}
	}
	
	return 0;
}

void PopH264::TEncoderInstance::PopPacket(ArrayBridge<uint8_t>&& Data)
{
	std::shared_ptr<Array<uint8_t>>	PacketData;
	{
		std::lock_guard<std::mutex> Lock(mPacketsLock);
		if ( !this->mPackets.GetSize() )
			throw Soy::AssertException("PopH264::TEncoderInstance::PopPacket no packets queued");
		
		auto Packet = mPackets.PopAt(0);
		PacketData = Packet.mData;
	}
	Data.Copy(*PacketData);
}

void PopH264::TEncoderInstance::OnNewPacket(TPacket& Packet)
{
	static bool Debug = false;
	if ( Debug )
	{
		try
		{
			auto H264PacketType = H264::GetPacketType(GetArrayBridge(*Packet.mData));
			std::Debug << __PRETTY_FUNCTION__ << "(" << magic_enum::enum_name(H264PacketType) << ")" << std::endl;
		}
		catch (std::exception& e)
		{
			std::Debug << __PRETTY_FUNCTION__ << " Error getting Nalu packet type; " << e.what() << std::endl;
		}
	}
	
	{
		std::lock_guard<std::mutex> Lock(mPacketsLock);
		mPackets.PushBack(Packet);
	}
	
	if ( mOnNewPacket )
		mOnNewPacket();
}

void PopH264::TEncoderInstance::AddOnNewFrameCallback(std::function<void()> Callback)
{
	//	does this need to be threadsafe?
	mOnNewPacket = Callback;
}

