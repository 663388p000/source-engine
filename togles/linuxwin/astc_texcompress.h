//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.h
//
// Purpose
// -------
// Every legacy D3D9 "ARGB" / "RGBA" style texture format that TOGL knows about
// (_A8R8G8B8, _X8R8G8B8, _Q8W8V8U8, _A2R10G10B10, _A16B16G16R16,
//  _A16B16G16R16F, _A32B32G32R32F, ...) can optionally be re-encoded into an
// ASTC compressed texture at upload time instead of being pushed to the GPU
// as an uncompressed GL_RGBA/GL_BGRA image.
//
// ASTC has two wire-compatible "profiles" that use the *same* block layout
// and the *same* GL internal-format enums (GL_COMPRESSED_RGBA_ASTC_*_KHR) --
// the difference is entirely in how the 128-bit blocks are produced and in
// which decode profile the GPU/driver is told to use:
//
//   * LDR (Low Dynamic Range)  - values are clamped/normalized to [0,1] and
//                                decode as regular UNORM colors. This is the
//                                right choice for every 8/16-bit-normalized
//                                "byte" ARGB/RGBA format (_A8R8G8B8,
//                                _X8R8G8B8, _Q8W8V8U8, _A2R10G10B10,
//                                _A16B16G16R16, ...).
//
//   * HDR (High Dynamic Range) - values may exceed 1.0 and decode as floats.
//                                This is the right choice for the floating
//                                point / half-float ARGB formats
//                                (_A16B16G16R16F, _A32B32G32R32F, _R32F).
//
// This module decides LDR vs HDR automatically from the source D3DFORMAT,
// and hands the actual block encoding off to ARM's reference "astcenc"
// library (Apache-2.0, https://github.com/ARM-software/astc-encoder).
//
// astc-encoder is NOT vendored in this tree (no network access was available
// to fetch it while building this patch). Drop the astcenc "Source" folder
// under e.g. thirdparty/astcenc/, add it to the build, and define
// HAVE_ASTCENC=1 for your build target.
//
// ASTC recompression is MANDATORY, not optional: every eligible ARGB/RGBA
// upload -- and every DXT1/3/5 texture, decoded to RGBA first -- goes
// through ASTC_CompressTextureRequired() below instead of a plain
// glTexImage2D() / glCompressedTexImage2D(DXT). That function does not fail
// quietly: if astcenc isn't built in or encoding fails, it calls Error()
// rather than letting the caller push the texture uncompressed. Build with
// HAVE_ASTCENC defined and astcenc linked in before shipping, or texture
// loads will hard-error. The only textures still allowed to stay
// uncompressed are render targets (including MSAA) and formats outside the
// eligible set (depth/stencil, luminance, etc.) -- see
// ASTC_IsEligibleFormat() below.
//
//===============================================================================

#ifndef ASTC_TEXCOMPRESS_H
#define ASTC_TEXCOMPRESS_H

#pragma once

#include <stdint.h>
#include "tier1/convar.h"	// needed for the extern ConVar declarations below (callers use .GetBool()/.GetInt()/.GetString())

// ---------------------------------------------------------------------------
// GL enums for ASTC (from KHR_texture_compression_astc_ldr / _hdr).
// Defined here defensively in case the local GL header set predates them --
// remove the #ifndef guards if your gl headers already provide these.
// ---------------------------------------------------------------------------
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
// sRGB variants (LDR-only; there is no sRGB+HDR combination in the spec)
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

// Note: HDR profile textures are uploaded with the *same* enums above --
// KHR_texture_compression_astc_hdr re-uses the LDR tokens. The driver knows
// it is HDR data purely because the astcenc encode profile that produced the
// bitstream (ASTCENC_PRF_HDR) is different; there is no separate GL enum.

enum EASTCProfile
{
	kASTCProfile_LDR = 0,	// clamped [0,1], UNORM decode
	kASTCProfile_HDR = 1,	// unclamped, FP16 decode (needs GL_KHR_texture_compression_astc_hdr)
};

struct ASTCEncodeResult
{
	void*		m_pData;			// caller must free with ASTC_FreeResult()
	uint32_t	m_nDataSize;		// size in bytes of m_pData
	uint32_t	m_glInternalFormat;	// GL_COMPRESSED_RGBA_ASTC_WxH_KHR to pass to glCompressedTexImage2D
	int			m_blockW;
	int			m_blockH;
	EASTCProfile m_profile;			// LDR or HDR -- informational, also implied by m_glInternalFormat + profile used to encode
};

// Returns true if this D3DFORMAT is one of the "RGBA family" formats this
// feature targets (any straight ARGB/RGBA/BGRA layout, normalized or float).
// fmt is passed as int here so this header doesn't need d3d9types.h; the
// .cpp does the real D3DFORMAT switch.
bool ASTC_IsEligibleFormat( int d3dFormat );

// Returns true if the format's payload is floating point / half-float and
// therefore must be encoded with the ASTC HDR profile instead of LDR.
bool ASTC_IsHDRFormat( int d3dFormat );

// Returns false for the "X" (no-alpha) D3DFORMATs -- X8R8G8B8, X1R5G5B5,
// R5G6B5, R8G8B8, L8, R32F -- and true for everything else. Callers pass
// the inverse of this as ASTC_CompressTextureRequired()'s forceOpaque so
// formats with no real alpha channel don't encode whatever garbage bits
// happen to be sitting in the unused channel.
bool ASTC_FormatHasAlpha( int d3dFormat );

// Encodes an RGBA (or RGBA-compatible) source image into ASTC blocks.
//   srcData        - tightly packed source texels, top row first
//   width, height  - texel dimensions (need not be a multiple of the block size;
//                    astcenc pads internally)
//   srcGLFormat    - GL_RGBA, GL_BGRA, GL_RGB, ... describing srcData's channel order
//   srcGLType      - GL_UNSIGNED_BYTE, GL_UNSIGNED_INT_8_8_8_8_REV, GL_HALF_FLOAT_ARB, GL_FLOAT, ...
//   isHDR          - from ASTC_IsHDRFormat()
//   isSRGB         - true if the source should decode with an sRGB gamma curve
//                    (ignored when isHDR is true -- sRGB is an LDR-only concept)
//   forceOpaque    - true to force alpha to fully opaque while converting the
//                    source to RGBA, regardless of the source alpha channel;
//                    see ASTC_FormatHasAlpha() above
//   blockW, blockH - ASTC block footprint, e.g. 4,4 (highest quality/least
//                    compression) up to 12,12 (most compression). This
//                    tree's defaults are 6x6 for LDR and 4x4 for HDR (see
//                    gl_astc_block_ldr / gl_astc_block_hdr below) -- HDR
//                    payloads get the finer block since there's usually far
//                    fewer of them (lightmaps, a handful of float targets).
//   qualityPreset  - 0=fastest .. 100=thorough (maps to ASTCENC_PRE_FASTEST..THOROUGH)
//   outResult      - filled in on success
// Returns false (and leaves *outResult untouched) if astcenc isn't compiled
// in (HAVE_ASTCENC not defined), the format isn't supported, or encoding
// failed for any reason. Most callers should use
// ASTC_CompressTextureRequired() below instead, which treats all three of
// those as fatal rather than returning a failure code to fall back on.
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

// Mandatory compression entry point used by every upload path in this tree
// (CGLMTex::WriteTexels and the DXT software-decompress path in
// cglmtex.cpp). Looks up the configured LDR/HDR block size itself
// (gl_astc_block_ldr / gl_astc_block_hdr) and always produces ASTC block
// data for the given image -- there is no "give up and let the caller push
// it uncompressed" outcome. If astcenc isn't compiled in (HAVE_ASTCENC) or
// encoding fails, this calls Error() (fatal) instead of returning a failure
// code, because uncompressed upload of these formats is no longer a
// supported path.
//
//   isHDR   - true for float/half-float sources (ASTCENC_PRF_HDR), false for
//             normalized 8/16-bit sources (ASTCENC_PRF_LDR). DXT1/3/5
//             sources are always false -- there is no HDR DXT format.
//   isSRGB  - true if the source should decode with an sRGB gamma curve.
//             Ignored when isHDR is true (sRGB is an LDR-only concept --
//             see the GL_COMPRESSED_SRGB8_ALPHA8_ASTC_* enums above).
//   forceOpaque - true to force alpha to fully opaque instead of using
//             whatever's in the source's alpha channel. Pass
//             !ASTC_FormatHasAlpha(d3dFormat) for a format-driven caller,
//             or compute it from local context (e.g. an already-decoded
//             DXT1-no-alpha buffer) like cglmtex.cpp does.
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

void ASTC_FreeResult( ASTCEncodeResult* result );

// Reads gl_astc_block_ldr / gl_astc_block_hdr and parses them into block
// dimensions for the given profile. Used by the WriteTexels upload hook.
void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH );

// Convars (defined in astc_texcompress.cpp). Declared extern here so any
// other translation unit -- cglmtex.cpp included -- can reference them
// after including this header; without this each .cpp only sees its own
// copy and the linker/compiler has no idea gl_astc_recompress etc. exist.
extern ConVar gl_astc_recompress;	// legacy toggle, kept for config/launch-option compatibility -- no longer read; compression is mandatory now (see ASTC_CompressTextureRequired)
extern ConVar gl_astc_block_ldr;	// "4x4" / "5x5" / "6x6" / "8x8" ... - block size used for LDR (8-bit) sources
extern ConVar gl_astc_block_hdr;	// "4x4" / "5x5" / "6x6" / "8x8" ... - block size used for HDR (float) sources
extern ConVar gl_astc_quality;		// 0-100 - encoder quality/speed tradeoff

#endif // ASTC_TEXCOMPRESS_H
