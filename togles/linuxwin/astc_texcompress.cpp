//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.cpp
//
// See astc_texcompress.h for the feature overview. This file:
//   1. classifies D3DFORMATs into "eligible for ASTC" / "LDR" / "HDR"
//   2. converts whatever GL (format,type) the caller has in memory into the
//      plain byte/half/float RGBA buffer astcenc wants
//   3. calls into ARM's astcenc (if HAVE_ASTCENC is compiled in) to produce
//      real ASTC blocks, choosing ASTCENC_PRF_LDR vs ASTCENC_PRF_HDR
//
//===============================================================================

#include "astc_texcompress.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "tier0/dbg.h"	// for Error() -- used by ASTC_CompressTextureRequired()

// Pull in the handful of D3DFMT_* constants we need to classify. These come
// from the same D3D9-format header the rest of TOGL uses (d3d9types.h /
// dxabstract_types.h depending on branch). That header was not part of the
// uploaded file subset, so we declare the handful of enumerants we need
// defensively; if your tree already defines D3DFORMAT this whole block is a
// silent no-op (harmless redefinition guarded by the same macro name used
// elsewhere in TOGL headers).
#ifndef D3DFMT_A8R8G8B8
	#define D3DFMT_A8R8G8B8      21
	#define D3DFMT_X8R8G8B8      22
	#define D3DFMT_A4R4G4B4      26
	#define D3DFMT_A1R5G5B5      25
	#define D3DFMT_X1R5G5B5      24
	#define D3DFMT_A2R10G10B10   35
	#define D3DFMT_A2B10G10R10   31
	#define D3DFMT_Q8W8V8U8      63
	#define D3DFMT_A16B16G16R16   36
	#define D3DFMT_A16B16G16R16F 113
	#define D3DFMT_A32B32G32R32F 116
	#define D3DFMT_R32F          114
#endif

#if defined( HAVE_ASTCENC )
	#include <astcenc.h>
#endif

// ---------------------------------------------------------------------------
// ConVars (declared extern in astc_texcompress.h so cglmtex.cpp etc. can see them)
// ---------------------------------------------------------------------------

ConVar gl_astc_recompress( "gl_astc_recompress", "1", FCVAR_ARCHIVE,
	"Legacy toggle, kept only so existing configs/launch options that set "
	"this don't fail to parse. ASTC recompression of every eligible "
	"ARGB/RGBA and DXT1/3/5 texture is now mandatory and this convar's "
	"value is no longer read -- see ASTC_CompressTextureRequired()." );

ConVar gl_astc_block_ldr( "gl_astc_block_ldr", "6x6", FCVAR_ARCHIVE,
	"ASTC block footprint used for 8/16-bit normalized ARGB/RGBA sources "
	"(this includes DXT1/3/5 textures, which are decoded to RGBA before "
	"re-encoding). Smaller blocks (4x4) = higher quality/bigger; larger "
	"blocks (8x8) = more compression/smaller." );

ConVar gl_astc_block_hdr( "gl_astc_block_hdr", "4x4", FCVAR_ARCHIVE,
	"ASTC block footprint used for floating point / half-float ARGB/RGBA "
	"(HDR) sources. Defaults finer than LDR's since there are usually far "
	"fewer HDR textures in flight." );

ConVar gl_astc_quality( "gl_astc_quality", "60", FCVAR_ARCHIVE,
	"ASTC encoder quality/speed tradeoff, 0 (fastest) - 100 (thorough)." );

// ---------------------------------------------------------------------------
// Format classification
// ---------------------------------------------------------------------------

bool ASTC_IsEligibleFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_A2R10G10B10:
		case D3DFMT_A2B10G10R10:
		case D3DFMT_Q8W8V8U8:			// straight RGBA-shaped bytes (see cglmtex.cpp comment)
		case D3DFMT_A16B16G16R16:
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A32B32G32R32F:
		case D3DFMT_R32F:
			return true;
		default:
			return false;
	}
}

bool ASTC_IsHDRFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A32B32G32R32F:
		case D3DFMT_R32F:
			return true;
		default:
			return false;	// everything else is a normalized ("byte-ish") format -> LDR
	}
}

// Parse "4x4" / "6x6" / "8x8" style convar strings into two ints.
static void ParseBlockSize( const char* str, int* outW, int* outH )
{
	int w = 4, h = 4;
	if ( str && sscanf( str, "%dx%d", &w, &h ) != 2 )
	{
		w = 4; h = 4;
	}
	*outW = w;
	*outH = h;
}

static uint32_t GLBlockSizeToInternalFormat( int blockW, int blockH )
{
	struct Entry { int w, h; uint32_t enumVal; };
	static const Entry table[] =
	{
		{ 4,  4,  GL_COMPRESSED_RGBA_ASTC_4x4_KHR   },
		{ 5,  4,  GL_COMPRESSED_RGBA_ASTC_5x4_KHR   },
		{ 5,  5,  GL_COMPRESSED_RGBA_ASTC_5x5_KHR   },
		{ 6,  5,  GL_COMPRESSED_RGBA_ASTC_6x5_KHR   },
		{ 6,  6,  GL_COMPRESSED_RGBA_ASTC_6x6_KHR   },
		{ 8,  5,  GL_COMPRESSED_RGBA_ASTC_8x5_KHR   },
		{ 8,  6,  GL_COMPRESSED_RGBA_ASTC_8x6_KHR   },
		{ 8,  8,  GL_COMPRESSED_RGBA_ASTC_8x8_KHR   },
		{ 10, 5,  GL_COMPRESSED_RGBA_ASTC_10x5_KHR  },
		{ 10, 6,  GL_COMPRESSED_RGBA_ASTC_10x6_KHR  },
		{ 10, 8,  GL_COMPRESSED_RGBA_ASTC_10x8_KHR  },
		{ 10, 10, GL_COMPRESSED_RGBA_ASTC_10x10_KHR },
		{ 12, 10, GL_COMPRESSED_RGBA_ASTC_12x10_KHR },
		{ 12, 12, GL_COMPRESSED_RGBA_ASTC_12x12_KHR },
	};
	for ( const Entry& e : table )
	{
		if ( e.w == blockW && e.h == blockH )
			return e.enumVal;
	}
	return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;	// safe fallback
}

// Maps a *linear* ASTC GL internal format (as returned in
// outResult->m_glInternalFormat by ASTC_CompressTexture(), via
// GLBlockSizeToInternalFormat() above) to its sRGB-decode equivalent.
// Falls back to returning the input unchanged for any block size that
// doesn't have a known sRGB enum (shouldn't happen -- every LDR block size
// this file supports has one, see the table in astc_texcompress.h).
static uint32_t ToSRGBInternalFormat( uint32_t linearEnum )
{
	switch ( linearEnum )
	{
		case GL_COMPRESSED_RGBA_ASTC_4x4_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR;
		case GL_COMPRESSED_RGBA_ASTC_5x4_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR;
		case GL_COMPRESSED_RGBA_ASTC_5x5_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR;
		case GL_COMPRESSED_RGBA_ASTC_6x5_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR;
		case GL_COMPRESSED_RGBA_ASTC_6x6_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR;
		case GL_COMPRESSED_RGBA_ASTC_8x5_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR;
		case GL_COMPRESSED_RGBA_ASTC_8x6_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR;
		case GL_COMPRESSED_RGBA_ASTC_8x8_KHR:   return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR;
		case GL_COMPRESSED_RGBA_ASTC_10x5_KHR:  return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR;
		case GL_COMPRESSED_RGBA_ASTC_10x6_KHR:  return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR;
		case GL_COMPRESSED_RGBA_ASTC_10x8_KHR:  return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR;
		case GL_COMPRESSED_RGBA_ASTC_10x10_KHR: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR;
		case GL_COMPRESSED_RGBA_ASTC_12x10_KHR: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR;
		case GL_COMPRESSED_RGBA_ASTC_12x12_KHR: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR;
		default:                                return linearEnum;
	}
}

// ---------------------------------------------------------------------------
// Source pixel normalization: whatever (glFormat, glType) the caller has,
// produce a tightly packed RGBA buffer astcenc can consume directly --
// U8 RGBA8 for LDR, F32 RGBA for HDR (astcenc also accepts F16, but F32
// keeps this glue code simple and is only used at asset-bake/upload time,
// not per-frame).
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )

// Minimal local copies of the GL enums we branch on, in case gl.h isn't
// pulled in ahead of this file in some build configs.
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE               0x1401
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV    0x8367
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT              0x1403
#endif
#ifndef GL_HALF_FLOAT_ARB
#define GL_HALF_FLOAT_ARB              0x140B
#endif
#ifndef GL_FLOAT
#define GL_FLOAT                       0x1406
#endif
#ifndef GL_RGBA
#define GL_RGBA                        0x1908
#endif
#ifndef GL_BGRA
#define GL_BGRA                        0x80E1
#endif
#ifndef GL_RGB
#define GL_RGB                         0x1907
#endif
#ifndef GL_BGR
#define GL_BGR                         0x80E0
#endif

static float HalfToFloat( uint16_t h )
{
	uint32_t sign = (h & 0x8000u) << 16;
	uint32_t exp  = (h >> 10) & 0x1F;
	uint32_t mant = h & 0x3FF;
	uint32_t bits;
	if ( exp == 0 )
	{
		if ( mant == 0 ) { bits = sign; }
		else
		{
			// subnormal
			exp = 1;
			while ( !(mant & 0x400) ) { mant <<= 1; exp--; }
			mant &= 0x3FF;
			bits = sign | ((exp + 112) << 23) | (mant << 13);
		}
	}
	else if ( exp == 31 )
	{
		bits = sign | 0x7F800000u | (mant << 13);
	}
	else
	{
		bits = sign | ((exp + 112) << 23) | (mant << 13);
	}
	float f;
	memcpy( &f, &bits, sizeof(f) );
	return f;
}

// Expands srcData into a freshly malloc'd RGBA buffer, either 8-bit (LDR) or
// 32-bit float (HDR) per channel. Caller frees with free().
static void* BuildRGBABuffer( const void* srcData, int width, int height,
							   unsigned int glFormat, unsigned int glType,
							   bool isHDR )
{
	const int nPixels = width * height;

	if ( !isHDR )
	{
		uint8_t* dst = (uint8_t*)malloc( (size_t)nPixels * 4 );
		const uint8_t* src8 = (const uint8_t*)srcData;

		for ( int i = 0; i < nPixels; ++i )
		{
			uint8_t r, g, b, a;
			if ( glType == GL_UNSIGNED_INT_8_8_8_8_REV )
			{
				uint32_t texel = ((const uint32_t*)srcData)[i];
				// D3D/GL "REV" packing: byte order in memory is B,G,R,A for
				// GL_BGRA + UNSIGNED_INT_8_8_8_8_REV (matches _A8R8G8B8 etc.)
				b = (texel      ) & 0xFF;
				g = (texel >>  8) & 0xFF;
				r = (texel >> 16) & 0xFF;
				a = (texel >> 24) & 0xFF;
			}
			else // GL_UNSIGNED_BYTE, tightly packed 4 bytes/texel
			{
				int base = i * 4;
				if ( glFormat == GL_BGRA )
				{
					b = src8[base+0]; g = src8[base+1]; r = src8[base+2]; a = src8[base+3];
				}
				else // GL_RGBA
				{
					r = src8[base+0]; g = src8[base+1]; b = src8[base+2]; a = src8[base+3];
				}
			}
			dst[i*4+0] = r; dst[i*4+1] = g; dst[i*4+2] = b; dst[i*4+3] = a;
		}
		return dst;
	}
	else
	{
		float* dst = (float*)malloc( (size_t)nPixels * 4 * sizeof(float) );

		if ( glType == GL_HALF_FLOAT_ARB )
		{
			const uint16_t* src16 = (const uint16_t*)srcData;
			for ( int i = 0; i < nPixels; ++i )
			{
				dst[i*4+0] = HalfToFloat( src16[i*4+0] );
				dst[i*4+1] = HalfToFloat( src16[i*4+1] );
				dst[i*4+2] = HalfToFloat( src16[i*4+2] );
				dst[i*4+3] = HalfToFloat( src16[i*4+3] );
			}
		}
		else // GL_FLOAT
		{
			memcpy( dst, srcData, (size_t)nPixels * 4 * sizeof(float) );
		}
		return dst;
	}
}

#endif // HAVE_ASTCENC

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
bool ASTC_CompressTexture(
	const void* srcData,
	int width,
	int height,
	unsigned int srcGLFormat,
	unsigned int srcGLType,
	bool isHDR,
	int blockW,
	int blockH,
	int qualityPreset,
	ASTCEncodeResult* outResult )
{
#if !defined( HAVE_ASTCENC )
	// astc-encoder isn't vendored/enabled in this build -- caller must fall
	// back to the normal uncompressed upload path. See astc_texcompress.h.
	(void)srcData; (void)width; (void)height; (void)srcGLFormat; (void)srcGLType;
	(void)isHDR; (void)blockW; (void)blockH; (void)qualityPreset; (void)outResult;
	return false;
#else
	if ( !srcData || width <= 0 || height <= 0 || !outResult )
		return false;

	void* rgba = BuildRGBABuffer( srcData, width, height, srcGLFormat, srcGLType, isHDR );
	if ( !rgba )
		return false;

	astcenc_profile profile = isHDR ? ASTCENC_PRF_HDR : ASTCENC_PRF_LDR;

	float quality = ASTCENC_PRE_MEDIUM;
	if ( qualityPreset <= 10 )      quality = ASTCENC_PRE_FASTEST;
	else if ( qualityPreset <= 35 ) quality = ASTCENC_PRE_FAST;
	else if ( qualityPreset <= 65 ) quality = ASTCENC_PRE_MEDIUM;
	else if ( qualityPreset <= 90 ) quality = ASTCENC_PRE_THOROUGH;
	else                            quality = ASTCENC_PRE_EXHAUSTIVE;

	astcenc_config config;
	astcenc_error status = astcenc_config_init(
		profile, blockW, blockH, 1, quality, 0, &config );
	if ( status != ASTCENC_SUCCESS )
	{
		free( rgba );
		return false;
	}

	astcenc_context* context = nullptr;
	status = astcenc_context_alloc( &config, 1 /*thread count*/, &context );
	if ( status != ASTCENC_SUCCESS )
	{
		free( rgba );
		return false;
	}

	astcenc_image image;
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = isHDR ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8;
	uint8_t* sliceArray[1] = { (uint8_t*)rgba };
	image.data = (void**)sliceArray;

	astcenc_swizzle swizzle { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

	int xBlocks = (width  + blockW - 1) / blockW;
	int yBlocks = (height + blockH - 1) / blockH;
	size_t compSize = (size_t)xBlocks * yBlocks * 16; // ASTC blocks are always 16 bytes

	uint8_t* compData = (uint8_t*)malloc( compSize );
	if ( !compData )
	{
		astcenc_context_free( context );
		free( rgba );
		return false;
	}

	status = astcenc_compress_image( context, &image, &swizzle, compData, compSize, 0 );

	astcenc_context_free( context );
	free( rgba );

	if ( status != ASTCENC_SUCCESS )
	{
		free( compData );
		return false;
	}

	outResult->m_pData = compData;
	outResult->m_nDataSize = (uint32_t)compSize;
	outResult->m_glInternalFormat = GLBlockSizeToInternalFormat( blockW, blockH );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
#endif
}

void ASTC_FreeResult( ASTCEncodeResult* result )
{
	if ( result && result->m_pData )
	{
		free( result->m_pData );
		result->m_pData = nullptr;
		result->m_nDataSize = 0;
	}
}

// Convenience used by the WriteTexels hook: picks the configured block size
// for the given HDR-ness straight from the convars.
void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH )
{
	ParseBlockSize( isHDR ? gl_astc_block_hdr.GetString() : gl_astc_block_ldr.GetString(), outW, outH );
}

// ---------------------------------------------------------------------------
// Mandatory compression entry point (no uncompressed fallback allowed).
// ---------------------------------------------------------------------------
void ASTC_CompressTextureRequired(
	bool isHDR,
	bool isSRGB,
	const void* srcData,
	int width,
	int height,
	unsigned int srcGLFormat,
	unsigned int srcGLType,
	ASTCEncodeResult* outResult )
{
	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	if ( !ASTC_CompressTexture( srcData, width, height, srcGLFormat, srcGLType,
								 isHDR, blockW, blockH, gl_astc_quality.GetInt(),
								 outResult ) )
	{
		// By policy (gl_astc_recompress), ASTC-eligible textures are never
		// allowed to reach the GPU uncompressed. Either this build wasn't
		// compiled with HAVE_ASTCENC / astcenc linked in, or the encode call
		// itself failed -- both are fatal here instead of silently falling
		// back, since preventing exactly that silent fallback is the reason
		// this function exists.
		Error( "ASTC_CompressTextureRequired: mandatory %s ASTC compression failed "
			   "for a %dx%d texture (block %dx%d). Uncompressed upload of this "
			   "texture is disabled -- build with HAVE_ASTCENC defined and the "
			   "astc-encoder sources linked in.\n",
			   isHDR ? "HDR" : "LDR", width, height, blockW, blockH );
		return;
	}

	// sRGB is an LDR-only concept for ASTC (see astc_texcompress.h) -- an
	// HDR source is never sRGB, so isSRGB is ignored in that case.
	if ( isSRGB && !isHDR )
		outResult->m_glInternalFormat = ToSRGBInternalFormat( outResult->m_glInternalFormat );
}
