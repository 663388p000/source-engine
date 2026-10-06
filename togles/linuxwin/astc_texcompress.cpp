//========= ASTC runtime recompression (added on top of Valve TOGL) - v2 =======//
//
// astc_texcompress.cpp
//
// See astc_texcompress.h for the overview. This file:
//   1. classifies D3DFORMATs (eligible / LDR / HDR / has-alpha)
//   2. normalizes the caller's (GL format, type) into what astcenc wants,
//      with zero-copy handling for 8-bit BGRA/RGBA sources
//   3. runs astcenc on a persistent thread pool with shared contexts
//   4. reads and writes the on-disk encode cache
//   5. builds solid-color blank ASTC images without the encoder
//
//===============================================================================

#include "astc_texcompress.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "tier0/dbg.h"	// Error() / Warning()

#if defined( HAVE_ASTCENC )
	#include <astcenc.h>
	#include <thread>
	#include <mutex>
	#include <condition_variable>
	#include <functional>
	#include <vector>
	#include <cstdint>
	#ifdef _WIN32
		#include <direct.h>
		#define ASTC_MKDIR( p ) _mkdir( p )
	#else
		#include <sys/stat.h>
		#define ASTC_MKDIR( p ) mkdir( p, 0755 )
	#endif
#endif

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
#define D3DFMT_R8G8B8        20
#define D3DFMT_R5G6B5        23
#define D3DFMT_A8            28
#define D3DFMT_L8            50
#define D3DFMT_A8L8          51

// GL source-layout enums. Guarded so this file builds with or without the GL headers.
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE				0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT				0x1403
#endif
#ifndef GL_FLOAT
#define GL_FLOAT						0x1406
#endif
#ifndef GL_HALF_FLOAT_ARB
#define GL_HALF_FLOAT_ARB				0x140B
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES				0x8D61
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4		0x8033
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1		0x8034
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8
#define GL_UNSIGNED_INT_8_8_8_8			0x8035
#endif
#ifndef GL_UNSIGNED_INT_10_10_10_2
#define GL_UNSIGNED_INT_10_10_10_2		0x8036
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5			0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV	0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV	0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV		0x8367
#endif
#ifndef GL_UNSIGNED_INT_2_10_10_10_REV
#define GL_UNSIGNED_INT_2_10_10_10_REV	0x8368
#endif
#ifndef GL_RED
#define GL_RED							0x1903
#endif
#ifndef GL_ALPHA
#define GL_ALPHA						0x1906
#endif
#ifndef GL_RGB
#define GL_RGB							0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA							0x1908
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE					0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA				0x190A
#endif
#ifndef GL_BGR
#define GL_BGR							0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA							0x80E1
#endif
#ifndef GL_RG
#define GL_RG							0x8227
#endif

// ---------------------------------------------------------------------------
// ConVars
// ---------------------------------------------------------------------------

ConVar gl_astc_recompress( "gl_astc_recompress", "1", FCVAR_ARCHIVE,
	"Legacy toggle, kept so existing configs still parse. ASTC recompression is mandatory and this value is not read." );

ConVar gl_astc_block_ldr( "gl_astc_block_ldr", "6x6", FCVAR_ARCHIVE,
	"ASTC block footprint for 8/16-bit normalized sources (includes DXT1/3/5, which are decoded first). "
	"4x4 = best quality, largest. 8x8 = smallest. Invalid values fall back to 6x6." );

ConVar gl_astc_block_hdr( "gl_astc_block_hdr", "4x4", FCVAR_ARCHIVE,
	"ASTC block footprint for half/float (HDR) sources. Invalid values fall back to 4x4." );

ConVar gl_astc_quality( "gl_astc_quality", "60", FCVAR_ARCHIVE,
	"ASTC encoder effort, 0 (fastest) - 100 (exhaustive). Higher is slower on first encode; the disk cache removes that cost after the first launch." );

ConVar gl_astc_threads( "gl_astc_threads", "0", FCVAR_ARCHIVE,
	"Encoder worker threads. 0 = auto (hardware threads, max 4). 1 = single threaded. Applied at first use; restart to change." );

ConVar gl_astc_cache( "gl_astc_cache", "1", FCVAR_ARCHIVE,
	"1 = cache encoded ASTC blocks on disk, keyed by source pixels and encode parameters. Later loads of the same texture skip encoding." );

ConVar gl_astc_cache_dir( "gl_astc_cache_dir", "astc_cache", FCVAR_ARCHIVE,
	"Directory for the ASTC encode cache, relative to the game's working directory. Safe to delete at any time." );

ConVar gl_astc_alpha_weight( "gl_astc_alpha_weight", "1", FCVAR_ARCHIVE,
	"1 = alpha-weighted encoding for textures that have an alpha channel. Improves color accuracy in translucent areas." );

ConVar gl_astc_perceptual( "gl_astc_perceptual", "1", FCVAR_ARCHIVE,
	"1 = use the perceptual error metric for LDR (8/16-bit) encodes. Matches how the eye judges color error." );

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
		case D3DFMT_Q8W8V8U8:
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
			return false;
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
		ParseBlockSize( gl_astc_block_hdr.GetString(), 4, 4, outW, outH );
	else
		ParseBlockSize( gl_astc_block_ldr.GetString(), 6, 6, outW, outH );
}

static uint32_t GLInternalFormatForBlock( int blockW, int blockH, bool srgb )
{
	const ASTCBlockEntry* e = FindBlockEntry( blockW, blockH );
	if ( !e )
		e = FindBlockEntry( 6, 6 );
	return srgb ? e->srgbEnum : e->linearEnum;
}

// ---------------------------------------------------------------------------
// Source layout helpers (used by the conversion fallback)
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

template< typename T >
static inline T LoadT( const uint8_t* p )
{
	T v;
	memcpy( &v, p, sizeof( T ) );	// unaligned-safe
	return v;
}

static inline uint8_t ScaleBitsTo8( uint32_t v, int w )
{
	const uint32_t maxv = ( 1u << w ) - 1u;
	return (uint8_t)( ( v * 255u + ( maxv >> 1 ) ) / maxv );
}

static inline uint8_t ToU8( uint8_t v )  { return v; }
static inline uint8_t ToU8( uint16_t v ) { return (uint8_t)( ( (uint32_t)v * 255u + 32767u ) / 65535u ); }

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

// Returns malloc'd n*4 bytes (RGBA8), or NULL if (fmt, type) isn't understood.
static uint8_t* ConvertToRGBA8( const void* srcData, size_t n, unsigned int glFormat, unsigned int glType, bool forceOpaque )
{
	uint8_t* dst = (uint8_t*)malloc( n * 4 );
	if ( !dst )
		return NULL;

	const uint8_t* src = (const uint8_t*)srcData;
	bool ok = false;

	int comps = 0;
	ChanLayout lay = LayoutFromGLFormat( glFormat, &comps );

	if ( glType == GL_UNSIGNED_BYTE && lay != kLay_Invalid )
	{
		ConvertPlanarUNorm<uint8_t>( src, n, lay, comps, forceOpaque, dst );
		ok = true;
	}
	else if ( glType == GL_UNSIGNED_SHORT && lay != kLay_Invalid )
	{
		ConvertPlanarUNorm<uint16_t>( src, n, lay, comps, forceOpaque, dst );
		ok = true;
	}
	else if ( glType != GL_UNSIGNED_BYTE && glType != GL_UNSIGNED_SHORT )
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

static inline float SanitizeHDR( float v )
{
	if ( !( v >= 0.0f ) )
		return 0.0f;
	if ( v > 65504.0f )
		return 65504.0f;
	return v;
}

// Returns malloc'd n*4 floats, or NULL if (fmt, type) isn't understood.
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

	// Integer source for an HDR encode: widen to float.
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

// Bytes per texel for (fmt, type). Used for cache keys.
static size_t SrcTexelBytes( unsigned int fmt, unsigned int type )
{
	PackDesc d;
	if ( GetPackDesc( type, &d ) )
		return (size_t)d.storageBytes;

	int comps = 0;
	LayoutFromGLFormat( fmt, &comps );
	size_t cs = 0;
	if ( type == GL_UNSIGNED_BYTE )
		cs = 1;
	else if ( type == GL_UNSIGNED_SHORT || type == GL_HALF_FLOAT_ARB || type == GL_HALF_FLOAT_OES )
		cs = 2;
	else if ( type == GL_FLOAT )
		cs = 4;
	return (size_t)comps * cs;
}

// ---------------------------------------------------------------------------
// astcenc glue
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )

static const uint32_t kCacheVersion = 2;

static float QualityFromPreset( int qualityPreset )
{
	if ( qualityPreset <= 10 )		return ASTCENC_PRE_FASTEST;
	else if ( qualityPreset <= 35 )	return ASTCENC_PRE_FAST;
	else if ( qualityPreset <= 65 )	return ASTCENC_PRE_MEDIUM;
	else if ( qualityPreset <= 90 )	return ASTCENC_PRE_THOROUGH;
	return ASTCENC_PRE_EXHAUSTIVE;
}

// ---- Persistent worker pool -------------------------------------------------
// astcenc is built for this pattern: N threads call astcenc_compress_image() on
// the same context with thread indices 0..N-1 and share the work. A persistent
// pool avoids creating and joining threads for every texture.
class EncodePool
{
public:
	explicit EncodePool( unsigned n ) : m_size( n ? n : 1 ), m_gen( 0 ), m_pending( 0 ), m_active( 0 ), m_stop( false ), m_fn( NULL )
	{
		for ( unsigned t = 1; t < m_size; ++t )
			m_workers.push_back( std::thread( &EncodePool::WorkerMain, this, t ) );
	}

	~EncodePool()
	{
		{
			std::lock_guard<std::mutex> lock( m_mtx );
			m_stop = true;
		}
		m_cv.notify_all();
		for ( size_t i = 0; i < m_workers.size(); ++i )
			m_workers[i].join();
	}

	unsigned Size() const { return m_size; }

	// Runs fn(0..active-1) concurrently. The calling thread is index 0.
	void Run( unsigned active, const std::function<void( unsigned )>& fn )
	{
		std::lock_guard<std::mutex> runLock( m_runMtx );
		if ( active > 1 )
		{
			{
				std::lock_guard<std::mutex> lock( m_mtx );
				m_fn = &fn;
				m_active = active;
				m_pending = active - 1;
				++m_gen;
			}
			m_cv.notify_all();
		}

		fn( 0 );

		if ( active > 1 )
		{
			std::unique_lock<std::mutex> lock( m_mtx );
			m_doneCv.wait( lock, [this]() { return m_pending == 0; } );
			m_fn = NULL;
		}
	}

private:
	void WorkerMain( unsigned idx )
	{
		uint64_t seen = 0;
		for ( ;; )
		{
			const std::function<void( unsigned )>* fn = NULL;
			{
				std::unique_lock<std::mutex> lock( m_mtx );
				m_cv.wait( lock, [this, &seen]() { return m_stop || m_gen != seen; } );
				if ( m_stop )
					return;
				seen = m_gen;
				if ( idx >= m_active )
					continue;
				fn = m_fn;
			}

			( *fn )( idx );

			std::lock_guard<std::mutex> lock( m_mtx );
			if ( --m_pending == 0 )
				m_doneCv.notify_one();
		}
	}

	unsigned					m_size;
	std::vector<std::thread>	m_workers;
	std::mutex					m_mtx;
	std::mutex					m_runMtx;
	std::condition_variable		m_cv;
	std::condition_variable		m_doneCv;
	uint64_t					m_gen;
	unsigned					m_pending;
	unsigned					m_active;
	bool						m_stop;
	const std::function<void( unsigned )>* m_fn;
};

static unsigned ChooseWorkerCount()
{
	const int configured = gl_astc_threads.GetInt();
	if ( configured > 0 )
		return (unsigned)( configured > 16 ? 16 : configured );

	unsigned hw = std::thread::hardware_concurrency();
	if ( hw == 0 )
		hw = 1;
	return hw > 4 ? 4 : hw;
}

// Intentionally never destroyed: avoids static-destruction-order issues at exit.
static EncodePool* GetPool()
{
	static EncodePool* pool = new EncodePool( ChooseWorkerCount() );
	return pool;
}

// ---- Shared, read-only-after-build encoder contexts --------------------------
// Building an astcenc context creates its large search tables. One context per
// (profile, block, quality, flags, threads) is built and then reused by every
// texture upload that needs it.
struct SharedCtx
{
	astcenc_profile	profile;
	int				blockW, blockH;
	int				qualityKey;		// quality * 1000, integer for exact matching
	uint32_t		flags;
	unsigned		threads;
	astcenc_context* ctx;
};

static std::mutex				s_ctxMutex;
static std::vector<SharedCtx>	s_ctxs;
static std::mutex				s_encodeMutex;	// an astcenc context must not be used by two calls at once

static astcenc_context* AcquireSharedContext( astcenc_profile profile, int bw, int bh, float quality, uint32_t flags, unsigned threads )
{
	std::lock_guard<std::mutex> lock( s_ctxMutex );

	const int qKey = (int)( quality * 1000.0f + 0.5f );
	for ( size_t i = 0; i < s_ctxs.size(); ++i )
	{
		const SharedCtx& c = s_ctxs[i];
		if ( c.profile == profile && c.blockW == bw && c.blockH == bh && c.qualityKey == qKey && c.flags == flags && c.threads == threads )
			return c.ctx;
	}

	astcenc_config config;
	astcenc_error status = astcenc_config_init( profile, bw, bh, 1, quality, flags, &config );
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

	SharedCtx entry = { profile, bw, bh, qKey, flags, threads, ctx };
	s_ctxs.push_back( entry );
	return ctx;
}

static astcenc_error EncodeImage( astcenc_profile profile, int bw, int bh, float quality, uint32_t flags,
								  const astcenc_image& image, const astcenc_swizzle& swz,
								  uint8_t* out, size_t outSize, bool smallImage )
{
	// Tiny images are not worth the fan-out. Use a single-thread context.
	const unsigned threads = smallImage ? 1u : GetPool()->Size();

	astcenc_context* ctx = AcquireSharedContext( profile, bw, bh, quality, flags, threads );
	if ( !ctx )
		return ASTCENC_ERR_BAD_PARAM;

	std::lock_guard<std::mutex> lock( s_encodeMutex );

	std::vector<astcenc_error> status( threads, ASTCENC_SUCCESS );
	if ( threads == 1 )
	{
		status[0] = astcenc_compress_image( ctx, &image, &swz, out, outSize, 0 );
	}
	else
	{
		GetPool()->Run( threads, [&]( unsigned t )
		{
			status[t] = astcenc_compress_image( ctx, &image, &swz, out, outSize, t );
		} );
	}
	astcenc_compress_reset( ctx );	// required before the context can encode another image

	for ( unsigned t = 0; t < threads; ++t )
	{
		if ( status[t] != ASTCENC_SUCCESS )
			return status[t];
	}
	return ASTCENC_SUCCESS;
}

// ---- Source preparation -----------------------------------------------------
struct SourcePlan
{
	const void*			ptr;		// what astcenc reads
	void*				owned;		// malloc'd conversion buffer, or NULL
	bool				isFloat;
	astcenc_swizzle		swz;
};

static void SetIdentitySwizzle( astcenc_swizzle* s )
{
	s->r = ASTCENC_SWZ_R;
	s->g = ASTCENC_SWZ_G;
	s->b = ASTCENC_SWZ_B;
	s->a = ASTCENC_SWZ_A;
}

// Fast path: 8-bit BGRA/RGBA memory order needs no copy. Channel order and
// forced alpha are handled by the swizzle. Everything else gets converted.
static bool PrepareSource( const void* src, size_t n, unsigned int fmt, unsigned int type,
						   bool isHDR, bool forceOpaque, SourcePlan* plan )
{
	memset( plan, 0, sizeof( *plan ) );
	SetIdentitySwizzle( &plan->swz );

	if ( !isHDR )
	{
		const bool byteOrder = ( type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV )
							&& ( fmt == GL_RGBA || fmt == GL_BGRA );
		if ( byteOrder )
		{
			plan->ptr = src;
			if ( fmt == GL_BGRA )
			{
				// Memory is B,G,R,A. astcenc reads memory slots as r,g,b,a,
				// so swap the red and blue slots.
				plan->swz.r = ASTCENC_SWZ_B;
				plan->swz.b = ASTCENC_SWZ_R;
			}
			if ( forceOpaque )
				plan->swz.a = ASTCENC_SWZ_1;
			return true;
		}

		uint8_t* rgba = ConvertToRGBA8( src, n, fmt, type, forceOpaque );
		if ( !rgba )
			return false;
		plan->owned = rgba;
		plan->ptr = rgba;
		return true;
	}

	float* rgbaF = ConvertToRGBAF32( src, n, fmt, type, forceOpaque );
	if ( !rgbaF )
		return false;
	plan->owned = rgbaF;
	plan->ptr = rgbaF;
	plan->isFloat = true;
	return true;
}

// ---- Disk cache -------------------------------------------------------------
static uint64_t MixHash( uint64_t h, const void* data, size_t n )
{
	const uint8_t* p = (const uint8_t*)data;
	size_t i = 0;
	for ( ; i + 8 <= n; i += 8 )
	{
		uint64_t w;
		memcpy( &w, p + i, 8 );
		h = ( h ^ w ) * 0x9E3779B97F4A7C15ULL;
		h ^= h >> 29;
	}
	for ( ; i < n; ++i )
		h = ( h ^ p[i] ) * 0x100000001B3ULL;
	return h;
}

struct CacheHeader
{
	char		magic[4];	// "ASTC"
	uint32_t	version;
	uint32_t	dataSize;
	uint64_t	key;
};

static uint64_t ComputeCacheKey( const void* src, size_t srcBytes, const uint32_t params[12] )
{
	uint64_t h = 0xCBF29CE484222325ULL;
	h = MixHash( h, params, sizeof( uint32_t ) * 12 );
	h = MixHash( h, src, srcBytes );
	return h;
}

static void CachePath( char* out, size_t outSize, uint64_t key )
{
	snprintf( out, outSize, "%s/%016llx.astc", gl_astc_cache_dir.GetString(), (unsigned long long)key );
}

static void EnsureCacheDir()
{
	static bool s_done = false;
	if ( !s_done )
	{
		ASTC_MKDIR( gl_astc_cache_dir.GetString() );	// EEXIST is fine
		s_done = true;
	}
}

static bool CacheLoad( uint64_t key, uint8_t* dst, size_t dstSize )
{
	char path[1024];
	CachePath( path, sizeof( path ), key );

	FILE* f = fopen( path, "rb" );
	if ( !f )
		return false;

	CacheHeader hd;
	bool ok = fread( &hd, sizeof( hd ), 1, f ) == 1
		   && memcmp( hd.magic, "ASTC", 4 ) == 0
		   && hd.version == kCacheVersion
		   && hd.key == key
		   && hd.dataSize == dstSize
		   && fread( dst, dstSize, 1, f ) == 1;
	fclose( f );
	return ok;
}

static void CacheStore( uint64_t key, const uint8_t* data, size_t size )
{
	EnsureCacheDir();

	char path[1024], tmp[1100];
	CachePath( path, sizeof( path ), key );
	snprintf( tmp, sizeof( tmp ), "%s.%016llx.tmp", path, (unsigned long long)key );

	FILE* f = fopen( tmp, "wb" );
	if ( !f )
		return;

	CacheHeader hd;
	memcpy( hd.magic, "ASTC", 4 );
	hd.version = kCacheVersion;
	hd.dataSize = (uint32_t)size;
	hd.key = key;

	bool ok = fwrite( &hd, sizeof( hd ), 1, f ) == 1 && fwrite( data, size, 1, f ) == 1;
	fclose( f );

	// Write-then-rename so a crash never leaves a half-written cache entry.
	if ( ok )
		rename( tmp, path );
	else
		remove( tmp );
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
	(void)srcData; (void)width; (void)height; (void)srcGLFormat; (void)srcGLType;
	(void)isHDR; (void)isSRGB; (void)forceOpaque; (void)blockW; (void)blockH; (void)qualityPreset;
	return false;
#else
	if ( !srcData || width <= 0 || height <= 0 || !outResult || !ASTC_IsValidBlockSize( blockW, blockH ) )
		return false;

	const size_t nPixels = (size_t)width * (size_t)height;
	const size_t xBlocks = ( (size_t)width  + blockW - 1 ) / blockW;
	const size_t yBlocks = ( (size_t)height + blockH - 1 ) / blockH;
	const size_t compSize = xBlocks * yBlocks * 16;	// every ASTC block is 16 bytes

	const astcenc_profile profile = isHDR ? ASTCENC_PRF_HDR : ( isSRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR );
	const float quality = QualityFromPreset( qualityPreset );

	uint32_t flags = 0;
	if ( !forceOpaque && gl_astc_alpha_weight.GetBool() )
		flags |= ASTCENC_FLG_USE_ALPHA_WEIGHT;
	if ( !isHDR && gl_astc_perceptual.GetBool() )
		flags |= ASTCENC_FLG_USE_PERCEPTUAL;

	uint8_t* compData = (uint8_t*)malloc( compSize );
	if ( !compData )
		return false;

	// Disk cache lookup. Keyed on the exact source bytes plus every parameter that changes the output.
	const bool useCache = gl_astc_cache.GetBool() && gl_astc_cache_dir.GetString()[0] != '\0';
	uint64_t key = 0;
	if ( useCache )
	{
		const uint32_t params[12] = {
			kCacheVersion, srcGLFormat, srcGLType, (uint32_t)width, (uint32_t)height,
			isHDR, isSRGB, forceOpaque, (uint32_t)blockW, (uint32_t)blockH,
			(uint32_t)qualityPreset, flags
		};
		key = ComputeCacheKey( srcData, nPixels * SrcTexelBytes( srcGLFormat, srcGLType ), params );

		if ( CacheLoad( key, compData, compSize ) )
		{
			outResult->m_pData = compData;
			outResult->m_nDataSize = (uint32_t)compSize;
			outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
			outResult->m_blockW = blockW;
			outResult->m_blockH = blockH;
			outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
			return true;
		}
	}

	// Cache miss: prepare the source and encode.
	SourcePlan plan;
	if ( !PrepareSource( srcData, nPixels, srcGLFormat, srcGLType, isHDR, forceOpaque, &plan ) )
	{
		Warning( "ASTC: unsupported source layout (GL format 0x%X, type 0x%X)\n", srcGLFormat, srcGLType );
		free( compData );
		return false;
	}

	void* sliceArray[1] = { const_cast<void*>( plan.ptr ) };
	astcenc_image image;
	memset( &image, 0, sizeof( image ) );
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = plan.isFloat ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8;
	image.data = sliceArray;

	const bool smallImage = (int64_t)width * height < 128 * 128;
	astcenc_error status = EncodeImage( profile, blockW, blockH, quality, flags, image, plan.swz,
										compData, compSize, smallImage );

	free( plan.owned );

	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_compress_image failed: %s\n", astcenc_get_error_string( status ) );
		free( compData );
		return false;
	}

	if ( useCache )
		CacheStore( key, compData, compSize );

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
// Mandatory compression entry point (no uncompressed fallback)
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

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	if ( !ASTC_CompressTexture( srcData, width, height, srcGLFormat, srcGLType,
								isHDR, isSRGB, forceOpaque, blockW, blockH, gl_astc_quality.GetInt(),
								outResult ) )
	{
		Error( "ASTC_CompressTextureRequired: mandatory %s ASTC compression failed for a %dx%d texture "
			   "(block %dx%d, GL format 0x%X type 0x%X). Uncompressed upload is disabled; build with "
			   "HAVE_ASTCENC and link astcenc.\n",
			   isHDR ? "HDR" : "LDR", width, height, blockW, blockH, srcGLFormat, srcGLType );
	}
}

// ---------------------------------------------------------------------------
// Solid-color ASTC image (void-extent blocks). No encoder needed.
//
// 2D void-extent block: bits[8:0] = 0x1FC, bit 9 = D (0 LDR, 1 HDR),
// bits[11:10] = 11, extent coords all ones, then R,G,B,A as 16-bit values
// (UNORM16 for LDR, IEEE half for HDR).
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

	const uint16_t alphaBits = opaqueAlpha ? ( isHDR ? 0x3C00 : 0xFFFF ) : 0x0000;

	uint8_t block[16];
	block[0] = 0xFC;
	block[1] = isHDR ? 0xFF : 0xFD;
	memset( block + 2, 0xFF, 6 );
	memset( block + 8, 0x00, 6 );	// R = G = B = 0
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
