//========= ASTC runtime recompression (added on top of Valve TOGL) - v2 =======//
//
// astc_texcompress.h
//
// Every ARGB/RGBA-family texture (and every DXT1/3/5 texture, decoded to RGBA
// first) is re-encoded to ASTC at upload time. Encoding is done by ARM's
// astcenc (Apache-2.0, https://github.com/ARM-software/astc-encoder).
//
// v2 changes:
//   * One persistent encoder thread pool. No std::thread is spawned per texture.
//   * astcenc contexts are built once per (profile, block, quality, flags,
//     threads) and shared by every upload. This is where the expensive
//     lookup tables live, so they are not rebuilt per texture.
//   * On-disk encode cache (gl_astc_cache). Identical source + parameters are
//     loaded from disk instead of re-encoded, so the second and later launches
//     don't stall on texture uploads.
//   * Zero-copy fast path for BGRA/RGBA 8-bit sources. Channel order and
//     forced alpha are handled with astcenc swizzles, not by a CPU copy.
//   * Alpha-weighted encoding for textures with alpha, and perceptual error
//     metrics for LDR (both convars, on by default).
//   * ASTC_MakeBlankTexture() is exported so the GL layer can allocate ASTC
//     storage without data (placeholder / ResetSRGB) instead of falling back
//     to an uncompressed glTexImage2D, which would break later compressed
//     writes to the same texture.
//
// ASTC recompression is MANDATORY: ASTC_CompressTextureRequired() never lets an
// eligible texture reach the GPU uncompressed. If astcenc isn't built in
// (HAVE_ASTCENC) or encoding fails it calls Error(). The only textures that
// stay uncompressed are render targets (incl. MSAA) and formats outside
// ASTC_IsEligibleFormat().
//
//===============================================================================

#ifndef ASTC_TEXCOMPRESS_H
#define ASTC_TEXCOMPRESS_H

#pragma once

#include <stdint.h>
#include "tier1/convar.h"

// GL enums for ASTC (KHR_texture_compression_astc_ldr / _hdr). Guarded in case
// the local GL headers already provide them.
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR    0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x4_KHR    0x93B1
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR    0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x5_KHR    0x93B3
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR    0x93B4
#define GL_COMPRESSED_RGBA_ASTC_8x5_KHR    0x93B5
#define GL_COMPRESSED_RGBA_ASTC_8x6_KHR    0x93B6
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR    0x93B7
#define GL_COMPRESSED_RGBA_ASTC_10x5_KHR   0x93B8
#define GL_COMPRESSED_RGBA_ASTC_10x6_KHR   0x93B9
#define GL_COMPRESSED_RGBA_ASTC_10x8_KHR   0x93BA
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR  0x93BB
#define GL_COMPRESSED_RGBA_ASTC_12x10_KHR  0x93BC
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR  0x93BD
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR   0x93D0
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR   0x93D1
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR   0x93D2
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR   0x93D3
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR   0x93D4
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR   0x93D5
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR   0x93D6
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR   0x93D7
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR  0x93D8
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR  0x93D9
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR  0x93DA
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR 0x93DB
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR 0x93DC
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR 0x93DD
#endif

enum EASTCProfile
{
	kASTCProfile_LDR = 0,	// clamped [0,1], UNORM decode
	kASTCProfile_HDR = 1,	// unclamped, FP16 decode (GL_KHR_texture_compression_astc_hdr)
};

struct ASTCEncodeResult
{
	void*		m_pData;			// free with ASTC_FreeResult()
	uint32_t	m_nDataSize;		// bytes in m_pData
	uint32_t	m_glInternalFormat;	// pass to glCompressedTexImage2D
	int			m_blockW;
	int			m_blockH;
	EASTCProfile m_profile;
};

// d3dFormat is an int so this header doesn't need d3d9types.h.
bool ASTC_IsEligibleFormat( int d3dFormat );
bool ASTC_IsHDRFormat( int d3dFormat );
bool ASTC_FormatHasAlpha( int d3dFormat );

bool ASTC_IsValidBlockSize( int w, int h );

// Encodes an image. Returns false (outResult zeroed) if astcenc isn't built in,
// the source layout is unknown, or encoding failed. Prefer
// ASTC_CompressTextureRequired(), which treats those as fatal.
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
	ASTCEncodeResult* outResult );

// Mandatory entry point used by every upload path. Uses the configured block
// size (gl_astc_block_ldr / gl_astc_block_hdr). Never returns an uncompressed
// result; failure is fatal.
void ASTC_CompressTextureRequired(
	bool isHDR,
	bool isSRGB,
	bool forceOpaque,
	const void* srcData,
	int width,
	int height,
	unsigned int srcGLFormat,
	unsigned int srcGLType,
	ASTCEncodeResult* outResult );

// Valid ASTC blocks that encode a solid color (opaque black, or transparent
// black if !opaqueAlpha). No encoder is needed. Use this for placeholder and
// no-data storage, so the texture stays compressed.
bool ASTC_MakeBlankTexture(
	bool isHDR,
	bool isSRGB,
	bool opaqueAlpha,
	int width,
	int height,
	ASTCEncodeResult* outResult );

void ASTC_FreeResult( ASTCEncodeResult* result );

void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH );

// Convars (defined in astc_texcompress.cpp).
extern ConVar gl_astc_recompress;	// legacy, read for config compatibility only
extern ConVar gl_astc_block_ldr;	// "4x4".."12x12"; default 6x6
extern ConVar gl_astc_block_hdr;	// default 4x4
extern ConVar gl_astc_quality;		// 0..100
extern ConVar gl_astc_threads;		// encoder worker threads; 0 = auto (max 4)
extern ConVar gl_astc_cache;		// 1 = use on-disk encode cache
extern ConVar gl_astc_cache_dir;	// cache directory, relative to the game's working dir
extern ConVar gl_astc_alpha_weight;	// 1 = alpha-weighted encoding when a texture has alpha
extern ConVar gl_astc_perceptual;	// 1 = perceptual error metric for LDR profiles

#endif // ASTC_TEXCOMPRESS_H
