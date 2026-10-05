//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.cpp
//
// See astc_texcompress.h for the feature overview. This file:
//   1. classifies D3DFORMATs into "eligible for ASTC" / "LDR" / "HDR" / "has alpha"
//   2. converts whatever GL (format,type) the caller has in memory into the
//      plain RGBA8 (LDR) / RGBA float (HDR) buffer astcenc wants. This part is
//      always compiled (no astcenc needed), so it can be unit tested.
//   3. calls into ARM's astcenc (HAVE_ASTCENC) to produce real ASTC blocks,
//      choosing ASTCENC_PRF_LDR / LDR_SRGB / HDR, with a per-thread context
//      cache and optional multi-threaded encoding
//   4. builds solid-color "blank" ASTC images without the encoder
//
//===============================================================================

#include "astc_texcompress.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "tier0/dbg.h"	// Error() / Warning() / Msg()

#if defined( HAVE_ASTCENC )
	#include <astcenc.h>
	#include <thread>
	#include <vector>
#endif
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
// ---------------------------------------------------------------------------
// GL enums used to describe the *source* pixel layout. Individually guarded so
// this file works with or without the real GL headers in front of it.
// ---------------------------------------------------------------------------
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE					0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT					0x1403
#endif
#ifndef GL_FLOAT
#define GL_FLOAT							0x1406
#endif
#ifndef GL_HALF_FLOAT_ARB
#define GL_HALF_FLOAT_ARB					0x140B
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES					0x8D61
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4			0x8033
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1			0x8034
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8
#define GL_UNSIGNED_INT_8_8_8_8				0x8035
#endif
#ifndef GL_UNSIGNED_INT_10_10_10_2
#define GL_UNSIGNED_INT_10_10_10_2			0x8036
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5				0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV		0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV		0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV			0x8367
#endif
#ifndef GL_UNSIGNED_INT_2_10_10_10_REV
#define GL_UNSIGNED_INT_2_10_10_10_REV		0x8368
#endif
#ifndef GL_RED
#define GL_RED								0x1903
#endif
#ifndef GL_ALPHA
#define GL_ALPHA							0x1906
#endif
#ifndef GL_RGB
#define GL_RGB								0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA								0x1908
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE						0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA					0x190A
#endif
#ifndef GL_BGR
#define GL_BGR								0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA								0x80E1
#endif
#ifndef GL_RG
#define GL_RG								0x8227
#endif

// ---------------------------------------------------------------------------
// ConVars (declared extern in astc_texcompress.h so cglmtex.cpp etc. can see them)
// ---------------------------------------------------------------------------

ConVar gl_astc_recompress( "gl_astc_recompress", "1", FCVAR_ARCHIVE,
	"Legacy toggle, kept only so existing configs/launch options that set "
	"this don't fail to parse. ASTC recompression of every eligible "
	"texture is mandatory and this convar's value is not read." );

ConVar gl_astc_block_ldr( "gl_astc_block_ldr", "6x6", FCVAR_ARCHIVE,
	"ASTC block footprint for 8/16-bit normalized sources (this includes "
	"DXT1/3/5, which are decoded to RGBA before re-encoding). Smaller "
	"blocks (4x4) = higher quality/bigger; larger blocks (8x8) = smaller. "
	"Invalid values fall back to 6x6." );

ConVar gl_astc_block_hdr( "gl_astc_block_hdr", "4x4", FCVAR_ARCHIVE,
	"ASTC block footprint for half-float / float (HDR) sources. "
	"Invalid values fall back to 4x4." );

ConVar gl_astc_quality( "gl_astc_quality", "60", FCVAR_ARCHIVE,
	"ASTC encoder quality/speed tradeoff, 0 (fastest) - 100 (exhaustive)." );

ConVar gl_astc_threads( "gl_astc_threads", "0", FCVAR_ARCHIVE,
	"Encoder threads used for each texture upload. 0 = auto (up to 4), 1 = single threaded." );

ConVar gl_astc_shadow_max_kb( "gl_astc_shadow_max_kb", "4096", FCVAR_ARCHIVE,
	"Non-mipped ASTC textures up to this size (KB, uncompressed) keep a CPU copy of their pixels "
	"after Unlock so that later partial (sub-rect) locks can be merged and re-encoded. "
	"Larger/mipped textures free the copy to save RAM." );

// ---------------------------------------------------------------------------
// Format classification
// ---------------------------------------------------------------------------

bool ASTC_IsEligibleFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_R8G8B8:
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
		case D3DFMT_R5G6B5:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A8:
		case D3DFMT_A2B10G10R10:
		case D3DFMT_A2R10G10B10:
		case D3DFMT_A16B16G16R16:
		case D3DFMT_L8:
		case D3DFMT_A8L8:
		case D3DFMT_Q8W8V8U8:		// straight RGBA-shaped bytes (shader does the scale/bias)
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_R32F:
		case D3DFMT_A32B32G32R32F:
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

bool ASTC_FormatHasAlpha( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_X8R8G8B8:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_R5G6B5:
		case D3DFMT_R8G8B8:
		case D3DFMT_L8:
		case D3DFMT_R32F:
			return false;
		default:
			return true;
	}
}

// ---------------------------------------------------------------------------
// GPU capability
// ---------------------------------------------------------------------------

static bool s_bHWLDR = true;
static bool s_bHWHDR = true;

void ASTC_SetHardwareSupport( bool ldr, bool hdr )
{
	s_bHWLDR = ldr;
	s_bHWHDR = hdr;
}

bool ASTC_HardwareSupportsLDR() { return s_bHWLDR; }
bool ASTC_HardwareSupportsHDR() { return s_bHWHDR; }

// ---------------------------------------------------------------------------
// Block sizes
// ---------------------------------------------------------------------------

struct ASTCBlockEntry { int w, h; uint32_t linearEnum; uint32_t srgbEnum; };

static const ASTCBlockEntry s_blockTable[] =
{
	{ 4,  4,  GL_COMPRESSED_RGBA_ASTC_4x4_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR   },
	{ 5,  4,  GL_COMPRESSED_RGBA_ASTC_5x4_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR   },
	{ 5,  5,  GL_COMPRESSED_RGBA_ASTC_5x5_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR   },
	{ 6,  5,  GL_COMPRESSED_RGBA_ASTC_6x5_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR   },
	{ 6,  6,  GL_COMPRESSED_RGBA_ASTC_6x6_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR   },
	{ 8,  5,  GL_COMPRESSED_RGBA_ASTC_8x5_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR   },
	{ 8,  6,  GL_COMPRESSED_RGBA_ASTC_8x6_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR   },
	{ 8,  8,  GL_COMPRESSED_RGBA_ASTC_8x8_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR   },
	{ 10, 5,  GL_COMPRESSED_RGBA_ASTC_10x5_KHR,  GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR  },
	{ 10, 6,  GL_COMPRESSED_RGBA_ASTC_10x6_KHR,  GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR  },
	{ 10, 8,  GL_COMPRESSED_RGBA_ASTC_10x8_KHR,  GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR  },
	{ 10, 10, GL_COMPRESSED_RGBA_ASTC_10x10_KHR, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR },
	{ 12, 10, GL_COMPRESSED_RGBA_ASTC_12x10_KHR, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR },
	{ 12, 12, GL_COMPRESSED_RGBA_ASTC_12x12_KHR, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR },
};

static const ASTCBlockEntry* FindBlockEntry( int w, int h )
{
	for ( size_t i = 0; i < sizeof( s_blockTable ) / sizeof( s_blockTable[0] ); ++i )
	{
		if ( s_blockTable[i].w == w && s_blockTable[i].h == h )
			return &s_blockTable[i];
	}
	return NULL;
}

bool ASTC_IsValidBlockSize( int w, int h )
{
	return FindBlockEntry( w, h ) != NULL;
}

// Parse "6x6" style convar strings. Anything unparsable or not a real ASTC
// footprint (e.g. "7x7", "3x3", "garbage") falls back to the profile default.
static void ParseBlockSize( const char* str, int defW, int defH, int* outW, int* outH )
{
	int w = 0, h = 0;
	if ( !str || sscanf( str, "%dx%d", &w, &h ) != 2 || !ASTC_IsValidBlockSize( w, h ) )
	{
		w = defW;
		h = defH;
	}
	*outW = w;
	*outH = h;
}

void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH )
{
	if ( isHDR )
		ParseBlockSize( gl_astc_block_hdr.GetString(), 4, 4, outW, outH );	// HDR default: 4x4
	else
		ParseBlockSize( gl_astc_block_ldr.GetString(), 6, 6, outW, outH );	// LDR default: 6x6
}

static uint32_t GLInternalFormatForBlock( int blockW, int blockH, bool srgb )
{
	const ASTCBlockEntry* e = FindBlockEntry( blockW, blockH );
	if ( !e )
		e = FindBlockEntry( 6, 6 );		// unreachable for validated sizes
	return srgb ? e->srgbEnum : e->linearEnum;
}

// ---------------------------------------------------------------------------
// Source pixel normalization
//
// Whatever (glFormat, glType) the caller has in memory is turned into a
// tightly packed RGBA buffer: U8 RGBA8 for LDR, F32 RGBA for HDR. The
// (glFormat, glType) pair describes the REAL memory layout, e.g.
// GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV = bytes B,G,R,A (D3DFMT_A8R8G8B8).
// ---------------------------------------------------------------------------

enum ChanLayout
{
	kLay_RGBA, kLay_BGRA, kLay_RGB, kLay_BGR, kLay_RG, kLay_R, kLay_L, kLay_LA, kLay_A, kLay_Invalid
};

static ChanLayout LayoutFromGLFormat( unsigned int fmt, int* outComps )
{
	switch ( fmt )
	{
		case GL_RGBA:				*outComps = 4; return kLay_RGBA;
		case GL_BGRA:				*outComps = 4; return kLay_BGRA;
		case GL_RGB:				*outComps = 3; return kLay_RGB;
		case GL_BGR:				*outComps = 3; return kLay_BGR;
		case GL_RG:					*outComps = 2; return kLay_RG;
		case GL_RED:				*outComps = 1; return kLay_R;
		case GL_LUMINANCE:			*outComps = 1; return kLay_L;
		case GL_LUMINANCE_ALPHA:	*outComps = 2; return kLay_LA;
		case GL_ALPHA:				*outComps = 1; return kLay_A;
		default:					*outComps = 0; return kLay_Invalid;
	}
}

// Unaligned-safe load (backing stores / decoded buffers are not guaranteed aligned).
template< typename T >
static inline T LoadT( const uint8_t* p )
{
	T v;
	memcpy( &v, p, sizeof( T ) );
	return v;
}

// Scale a w-bit unsigned field to 8 bits with round-to-nearest.
static inline uint8_t ScaleBitsTo8( uint32_t v, int w )
{
	const uint32_t maxv = ( 1u << w ) - 1u;
	return (uint8_t)( ( v * 255u + ( maxv >> 1 ) ) / maxv );
}

static inline uint8_t ToU8( uint8_t v )  { return v; }
static inline uint8_t ToU8( uint16_t v ) { return (uint8_t)( ( (uint32_t)v * 255u + 32767u ) / 65535u ); }

// Reorders up to 4 raw components (in memory/format order) into R,G,B,A.
template< typename V >
static inline void MapChannels( ChanLayout lay, const V* c, V one, V* r, V* g, V* b, V* a )
{
	V zero = (V)0;
	*r = zero; *g = zero; *b = zero; *a = one;
	switch ( lay )
	{
		case kLay_RGBA: *r = c[0]; *g = c[1]; *b = c[2]; *a = c[3]; break;
		case kLay_BGRA: *b = c[0]; *g = c[1]; *r = c[2]; *a = c[3]; break;
		case kLay_RGB:  *r = c[0]; *g = c[1]; *b = c[2]; break;
		case kLay_BGR:  *b = c[0]; *g = c[1]; *r = c[2]; break;
		case kLay_RG:   *r = c[0]; *g = c[1]; break;
		case kLay_R:    *r = c[0]; break;
		case kLay_L:    *r = c[0]; *g = c[0]; *b = c[0]; break;
		case kLay_LA:   *r = c[0]; *g = c[0]; *b = c[0]; *a = c[1]; break;
		case kLay_A:    *a = c[0]; break;
		default: break;
	}
}

// Plain (non-packed) 8 or 16 bit unsigned normalized channels.
template< typename T >
static void ConvertPlanarUNorm( const uint8_t* src, size_t n, ChanLayout lay, int comps, bool forceOpaque, uint8_t* dst )
{
	for ( size_t i = 0; i < n; ++i )
	{
		uint8_t c[4] = { 0, 0, 0, 0 };
		for ( int k = 0; k < comps; ++k )
			c[k] = ToU8( LoadT<T>( src + ( i * (size_t)comps + k ) * sizeof( T ) ) );

		uint8_t r, g, b, a;
		MapChannels<uint8_t>( lay, c, 255, &r, &g, &b, &a );
		if ( forceOpaque )
			a = 255;

		dst[i*4+0] = r; dst[i*4+1] = g; dst[i*4+2] = b; dst[i*4+3] = a;
	}
}

// Packed types. Widths are in COMPONENT order; 'rev' means the first component
// sits in the least significant bits (the GL *_REV types).
struct PackDesc { int nComps; int w[4]; bool rev; int totalBits; int storageBytes; };

static bool GetPackDesc( unsigned int type, PackDesc* d )
{
	switch ( type )
	{
		case GL_UNSIGNED_SHORT_5_6_5:			{ PackDesc t = { 3, {5,6,5,0},   false, 16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_4_4_4_4:			{ PackDesc t = { 4, {4,4,4,4},   false, 16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:		{ PackDesc t = { 4, {4,4,4,4},   true,  16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_5_5_5_1:			{ PackDesc t = { 4, {5,5,5,1},   false, 16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:		{ PackDesc t = { 4, {5,5,5,1},   true,  16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_INT_8_8_8_8:			{ PackDesc t = { 4, {8,8,8,8},   false, 32, 4 }; *d = t; return true; }
		case GL_UNSIGNED_INT_8_8_8_8_REV:		{ PackDesc t = { 4, {8,8,8,8},   true,  32, 4 }; *d = t; return true; }
		case GL_UNSIGNED_INT_10_10_10_2:		{ PackDesc t = { 4, {10,10,10,2}, false, 32, 4 }; *d = t; return true; }
		case GL_UNSIGNED_INT_2_10_10_10_REV:	{ PackDesc t = { 4, {10,10,10,2}, true,  32, 4 }; *d = t; return true; }
		default: return false;
	}
}

static bool ConvertPackedUNorm( const uint8_t* src, size_t n, unsigned int fmt, unsigned int type, bool forceOpaque, uint8_t* dst )
{
	PackDesc d;
	if ( !GetPackDesc( type, &d ) )
		return false;

	ChanLayout lay;
	if ( d.nComps == 3 )
		lay = ( fmt == GL_BGR ) ? kLay_BGR : kLay_RGB;
	else
		lay = ( fmt == GL_BGRA ) ? kLay_BGRA : kLay_RGBA;

	// Per-component shift / mask.
	int shift[4] = { 0, 0, 0, 0 };
	int acc = 0;
	for ( int k = 0; k < d.nComps; ++k )
	{
		shift[k] = d.rev ? acc : ( d.totalBits - acc - d.w[k] );
		acc += d.w[k];
	}

	for ( size_t i = 0; i < n; ++i )
	{
		uint32_t v = ( d.storageBytes == 2 ) ? (uint32_t)LoadT<uint16_t>( src + i * 2 ) : LoadT<uint32_t>( src + i * 4 );

		uint8_t c[4] = { 0, 0, 0, 0 };
		for ( int k = 0; k < d.nComps; ++k )
		{
			const uint32_t field = ( v >> shift[k] ) & ( ( 1u << d.w[k] ) - 1u );
			c[k] = ScaleBitsTo8( field, d.w[k] );
		}

		uint8_t r, g, b, a;
		MapChannels<uint8_t>( lay, c, 255, &r, &g, &b, &a );
		if ( forceOpaque )
			a = 255;

		dst[i*4+0] = r; dst[i*4+1] = g; dst[i*4+2] = b; dst[i*4+3] = a;
	}
	return true;
}

// Returns malloc'd n*4 bytes, or NULL if (glFormat, glType) isn't understood.
static uint8_t* ConvertToRGBA8( const void* srcData, size_t n, unsigned int glFormat, unsigned int glType, bool forceOpaque )
{
	uint8_t* dst = (uint8_t*)malloc( n * 4 );
	if ( !dst )
		return NULL;

	const uint8_t* src = (const uint8_t*)srcData;
	bool ok = false;

	int comps = 0;
	ChanLayout lay = LayoutFromGLFormat( glFormat, &comps );

	if ( glType == GL_UNSIGNED_BYTE )
	{
		if ( lay != kLay_Invalid )
		{
			ConvertPlanarUNorm<uint8_t>( src, n, lay, comps, forceOpaque, dst );
			ok = true;
		}
	}
	else if ( glType == GL_UNSIGNED_SHORT )
	{
		if ( lay != kLay_Invalid )
		{
			ConvertPlanarUNorm<uint16_t>( src, n, lay, comps, forceOpaque, dst );
			ok = true;
		}
	}
	else
	{
		ok = ConvertPackedUNorm( src, n, glFormat, glType, forceOpaque, dst );
	}

	if ( !ok )
	{
		free( dst );
		return NULL;
	}
	return dst;
}

static float HalfToFloat( uint16_t h )
{
	uint32_t sign = ( h & 0x8000u ) << 16;
	uint32_t exp  = ( h >> 10 ) & 0x1F;
	uint32_t mant = h & 0x3FF;
	uint32_t bits;
	if ( exp == 0 )
	{
		if ( mant == 0 )
		{
			bits = sign;
		}
		else
		{
			// subnormal: normalize
			int e = -1;
			do { e++; mant <<= 1; } while ( !( mant & 0x400 ) );
			mant &= 0x3FF;
			bits = sign | ( (uint32_t)( 127 - 15 - e ) << 23 ) | ( mant << 13 );
		}
	}
	else if ( exp == 31 )
	{
		bits = sign | 0x7F800000u | ( mant << 13 );
	}
	else
	{
		bits = sign | ( ( exp + 112 ) << 23 ) | ( mant << 13 );
	}
	float f;
	memcpy( &f, &bits, sizeof( f ) );
	return f;
}

// astcenc's HDR profile cannot represent NaN/negative/huge values; clamp to what FP16 can hold.
static inline float SanitizeHDR( float v )
{
	if ( !( v >= 0.0f ) )		// catches NaN and negatives
		return 0.0f;
	if ( v > 65504.0f )
		return 65504.0f;
	return v;
}

// Returns malloc'd n*4 floats, or NULL if (glFormat, glType) isn't understood.
static float* ConvertToRGBAF32( const void* srcData, size_t n, unsigned int glFormat, unsigned int glType, bool forceOpaque )
{
	float* dst = (float*)malloc( n * 4 * sizeof( float ) );
	if ( !dst )
		return NULL;

	const uint8_t* src = (const uint8_t*)srcData;

	int comps = 0;
	ChanLayout lay = LayoutFromGLFormat( glFormat, &comps );
	const bool isHalf  = ( glType == GL_HALF_FLOAT_ARB || glType == GL_HALF_FLOAT_OES );
	const bool isFloat = ( glType == GL_FLOAT );

	if ( lay != kLay_Invalid && ( isHalf || isFloat ) )
	{
		for ( size_t i = 0; i < n; ++i )
		{
			float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for ( int k = 0; k < comps; ++k )
			{
				const size_t idx = i * (size_t)comps + k;
				c[k] = isHalf ? HalfToFloat( LoadT<uint16_t>( src + idx * 2 ) ) : LoadT<float>( src + idx * 4 );
			}

			float r, g, b, a;
			MapChannels<float>( lay, c, 1.0f, &r, &g, &b, &a );
			if ( forceOpaque )
				a = 1.0f;

			dst[i*4+0] = SanitizeHDR( r ); dst[i*4+1] = SanitizeHDR( g );
			dst[i*4+2] = SanitizeHDR( b ); dst[i*4+3] = SanitizeHDR( a );
		}
		return dst;
	}

	// An HDR encode was requested for an integer source: widen it.
	uint8_t* tmp = ConvertToRGBA8( srcData, n, glFormat, glType, forceOpaque );
	if ( !tmp )
	{
		free( dst );
		return NULL;
	}
	for ( size_t i = 0; i < n * 4; ++i )
		dst[i] = tmp[i] * ( 1.0f / 255.0f );
	free( tmp );
	return dst;
}

// ---------------------------------------------------------------------------
// astcenc glue
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )

static float QualityFromPreset( int qualityPreset )
{
	if ( qualityPreset <= 10 )		return ASTCENC_PRE_FASTEST;
	else if ( qualityPreset <= 35 )	return ASTCENC_PRE_FAST;
	else if ( qualityPreset <= 65 )	return ASTCENC_PRE_MEDIUM;
	else if ( qualityPreset <= 90 )	return ASTCENC_PRE_THOROUGH;
	return ASTCENC_PRE_EXHAUSTIVE;
}

// Creating an astcenc context builds large lookup tables (many ms), so each
// thread keeps a few of them around instead of creating one per texture.
struct CachedContext
{
	astcenc_context*	ctx;
	astcenc_profile		profile;
	int					blockW, blockH;
	float				quality;
	unsigned int		threads;
};

struct ContextCache
{
	enum { kSlots = 4 };
	CachedContext	slot[kSlots];
	int				nextVictim;

	ContextCache() { memset( slot, 0, sizeof( slot ) ); nextVictim = 0; }
	~ContextCache()
	{
		for ( int i = 0; i < kSlots; ++i )
		{
			if ( slot[i].ctx )
				astcenc_context_free( slot[i].ctx );
		}
	}
};

static astcenc_context* AcquireContext( astcenc_profile profile, int bw, int bh, float quality, unsigned int threads )
{
	static thread_local ContextCache cache;

	for ( int i = 0; i < ContextCache::kSlots; ++i )
	{
		CachedContext& c = cache.slot[i];
		if ( c.ctx && c.profile == profile && c.blockW == bw && c.blockH == bh && c.quality == quality && c.threads == threads )
			return c.ctx;
	}

	astcenc_config config;
	astcenc_error status = astcenc_config_init( profile, bw, bh, 1, quality, 0, &config );
	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_config_init(%dx%d) failed: %s\n", bw, bh, astcenc_get_error_string( status ) );
		return NULL;
	}

	astcenc_context* ctx = NULL;
	status = astcenc_context_alloc( &config, threads, &ctx );
	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_context_alloc failed: %s\n", astcenc_get_error_string( status ) );
		return NULL;
	}

	CachedContext& victim = cache.slot[cache.nextVictim];
	cache.nextVictim = ( cache.nextVictim + 1 ) % ContextCache::kSlots;
	if ( victim.ctx )
		astcenc_context_free( victim.ctx );

	victim.ctx = ctx;
	victim.profile = profile;
	victim.blockW = bw;
	victim.blockH = bh;
	victim.quality = quality;
	victim.threads = threads;
	return ctx;
}

static unsigned int PickThreadCount( int width, int height )
{
	// Tiny images (most mip tails, small UI bits) are not worth the thread spin-up.
	if ( (int64_t)width * height < 128 * 128 )
		return 1;

	int configured = gl_astc_threads.GetInt();
	if ( configured > 0 )
		return (unsigned int)( configured > 16 ? 16 : configured );

	unsigned int hw = std::thread::hardware_concurrency();
	if ( hw == 0 ) hw = 1;
	return hw > 4 ? 4 : hw;
}

static astcenc_error RunCompress( astcenc_context* ctx, astcenc_image* image, const astcenc_swizzle* swizzle,
								  uint8_t* out, size_t outSize, unsigned int threads )
{
	if ( threads <= 1 )
		return astcenc_compress_image( ctx, image, swizzle, out, outSize, 0 );

	// astcenc is designed for this: every thread calls compress_image on the same
	// context with its own thread_index and they share the work internally.
	std::vector<astcenc_error> status( threads, ASTCENC_SUCCESS );
	std::vector<std::thread> workers;
	workers.reserve( threads - 1 );
	for ( unsigned int t = 1; t < threads; ++t )
	{
		workers.push_back( std::thread( [ctx, image, swizzle, out, outSize, t, &status]() {
			status[t] = astcenc_compress_image( ctx, image, swizzle, out, outSize, t );
		} ) );
	}
	status[0] = astcenc_compress_image( ctx, image, swizzle, out, outSize, 0 );
	for ( size_t i = 0; i < workers.size(); ++i )
		workers[i].join();

	for ( unsigned int t = 0; t < threads; ++t )
	{
		if ( status[t] != ASTCENC_SUCCESS )
			return status[t];
	}
	return ASTCENC_SUCCESS;
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
		bool isSRGB,
		bool forceOpaque,
		int blockW,
		int blockH,
		int qualityPreset,
		ASTCEncodeResult* outResult )
{
	if ( outResult )
		memset( outResult, 0, sizeof( *outResult ) );

#if !defined( HAVE_ASTCENC )
	// astc-encoder isn't vendored/enabled in this build.
	(void)srcData; (void)width; (void)height; (void)srcGLFormat; (void)srcGLType;
	(void)isHDR; (void)isSRGB; (void)forceOpaque; (void)blockW; (void)blockH; (void)qualityPreset;
	return false;
#else
	if ( !srcData || width <= 0 || height <= 0 || !outResult || !ASTC_IsValidBlockSize( blockW, blockH ) )
		return false;

	const size_t nPixels = (size_t)width * (size_t)height;

	void* rgba = isHDR ? (void*)ConvertToRGBAF32( srcData, nPixels, srcGLFormat, srcGLType, forceOpaque )
					   : (void*)ConvertToRGBA8( srcData, nPixels, srcGLFormat, srcGLType, forceOpaque );
	if ( !rgba )
	{
		Warning( "ASTC: unsupported source layout (GL format 0x%X, type 0x%X)\n", srcGLFormat, srcGLType );
		return false;
	}

	// sRGB is an LDR-only concept. The encoder must be told, otherwise it
	// models the wrong endpoint expansion for what the GPU will decode.
	const astcenc_profile profile = isHDR ? ASTCENC_PRF_HDR : ( isSRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR );
	const unsigned int threads = PickThreadCount( width, height );

	astcenc_context* context = AcquireContext( profile, blockW, blockH, QualityFromPreset( qualityPreset ), threads );
	if ( !context )
	{
		free( rgba );
		return false;
	}

	void* sliceArray[1] = { rgba };
	astcenc_image image;
	memset( &image, 0, sizeof( image ) );
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = isHDR ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8;
	image.data = sliceArray;

	astcenc_swizzle swizzle;
	swizzle.r = ASTCENC_SWZ_R;
	swizzle.g = ASTCENC_SWZ_G;
	swizzle.b = ASTCENC_SWZ_B;
	swizzle.a = ASTCENC_SWZ_A;

	const size_t xBlocks = ( (size_t)width  + blockW - 1 ) / blockW;
	const size_t yBlocks = ( (size_t)height + blockH - 1 ) / blockH;
	const size_t compSize = xBlocks * yBlocks * 16;	// ASTC blocks are always 16 bytes

	uint8_t* compData = (uint8_t*)malloc( compSize );
	if ( !compData )
	{
		free( rgba );
		return false;
	}

	astcenc_error status = RunCompress( context, &image, &swizzle, compData, compSize, threads );
	astcenc_compress_reset( context );		// required before this context can encode another image
	free( rgba );

	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_compress_image failed: %s\n", astcenc_get_error_string( status ) );
		free( compData );
		return false;
	}

	outResult->m_pData = compData;
	outResult->m_nDataSize = (uint32_t)compSize;
	outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
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
		result->m_pData = NULL;
		result->m_nDataSize = 0;
	}
}

// ---------------------------------------------------------------------------
// Mandatory compression entry point (no uncompressed fallback allowed).
// ---------------------------------------------------------------------------
void ASTC_CompressTextureRequired(
		bool isHDR,
		bool isSRGB,
		bool forceOpaque,
		const void* srcData,
		int width,
		int height,
		unsigned int srcGLFormat,
		unsigned int srcGLType,
		ASTCEncodeResult* outResult )
{
	if ( outResult )
		memset( outResult, 0, sizeof( *outResult ) );

	if ( ( isHDR && !ASTC_HardwareSupportsHDR() ) || ( !isHDR && !ASTC_HardwareSupportsLDR() ) )
	{
		Error( "ASTC_CompressTextureRequired: this GPU/driver does not report %s. "
			   "Mandatory ASTC recompression cannot be used on it.\n",
			   isHDR ? "GL_KHR_texture_compression_astc_hdr" : "GL_KHR_texture_compression_astc_ldr" );
		return;
	}

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	if ( !ASTC_CompressTexture( srcData, width, height, srcGLFormat, srcGLType,
								isHDR, isSRGB, forceOpaque, blockW, blockH, gl_astc_quality.GetInt(),
								outResult ) )
	{
		// ASTC-eligible textures are never allowed to reach the GPU uncompressed.
		// Either this build wasn't compiled with HAVE_ASTCENC / astcenc linked in,
		// the source layout is unknown, or the encode itself failed.
		Error( "ASTC_CompressTextureRequired: mandatory %s ASTC compression failed "
			   "for a %dx%d texture (block %dx%d, GL format 0x%X type 0x%X). "
			   "Uncompressed upload of this texture is disabled -- build with HAVE_ASTCENC "
			   "defined and the astc-encoder sources linked in.\n",
			   isHDR ? "HDR" : "LDR", width, height, blockW, blockH, srcGLFormat, srcGLType );
		return;
	}
}

// ---------------------------------------------------------------------------
// Solid-color ASTC image (void-extent blocks). No encoder needed.
//
// ASTC void-extent block (2D): bits[8:0]=0x1FC, bit9 = D (0 LDR / 1 HDR),
// bits[11:10]=11, extent coordinates all-ones (= "no extent given"), then
// R,G,B,A as 16-bit values (UNORM16 for LDR, IEEE half for HDR).
// ---------------------------------------------------------------------------
bool ASTC_MakeBlankTexture(
		bool isHDR,
		bool isSRGB,
		bool opaqueAlpha,
		int width,
		int height,
		ASTCEncodeResult* outResult )
{
	if ( !outResult )
		return false;
	memset( outResult, 0, sizeof( *outResult ) );

	if ( width <= 0 || height <= 0 )
		return false;

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	const size_t xBlocks = ( (size_t)width  + blockW - 1 ) / blockW;
	const size_t yBlocks = ( (size_t)height + blockH - 1 ) / blockH;
	const size_t size = xBlocks * yBlocks * 16;

	uint8_t* data = (uint8_t*)malloc( size );
	if ( !data )
		return false;

	const uint16_t alphaBits = opaqueAlpha ? ( isHDR ? 0x3C00 : 0xFFFF ) : 0x0000;	// 1.0 as half / unorm16

	uint8_t block[16];
	block[0] = 0xFC;
	block[1] = isHDR ? 0xFF : 0xFD;
	memset( block + 2, 0xFF, 6 );
	memset( block + 8, 0x00, 6 );					// R = G = B = 0
	block[14] = (uint8_t)( alphaBits & 0xFF );
	block[15] = (uint8_t)( alphaBits >> 8 );

	for ( size_t i = 0; i < xBlocks * yBlocks; ++i )
		memcpy( data + i * 16, block, 16 );

	outResult->m_pData = data;
	outResult->m_nDataSize = (uint32_t)size;
	outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
}
