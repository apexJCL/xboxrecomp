/**
 * HLSL emitters for the NV2A vertex programs (d3d8_vsh_hlsl.c) and register
 * combiners (d3d8_combiners_hlsl.c): parsed state in, shader source text out.
 * String building only, no graphics API, so they build and can be tested on
 * every platform; the D3D11 side (d3d8_vsh.c, d3d8_combiners.c,
 * nv2a_pb_d3d11.c) compiles the text. The reference for any other emitter.
 */

#ifndef XBOXRECOMP_D3D8_HLSL_H
#define XBOXRECOMP_D3D8_HLSL_H

#include "d3d8_vsh_parse.h"
#include "d3d8_combiners_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate HLSL vertex shader source from parsed program.
 *
 * @param program   Parsed program
 * @param buf       Output buffer for HLSL source
 * @param bufsize   Size of output buffer
 * @return Number of characters written, or -1 on error
 */
int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize);

/**
 * oPos arrives in surface pixels with the clip w in .w (what every NV2A
 * program produces: the hardware has no viewport stage, so the XDK appends
 * one to each program). The generated VS converts it back to clip space
 * for D3D11 with a cbuffer at b2:
 *
 *   cbuffer VSH_Viewport : register(b2) { float4 vp_scale; float4 vp_off; }
 *
 *   ndc.xy = oPos.xy * vp_scale.xy + vp_off.xy
 *   ndc.z  = oPos.z  * vp_scale.z
 *   SV_POSITION = float4(ndc * oPos.w, oPos.w)
 *
 * For a W x H surface with depth range zmax: vp_scale = (2/W, -2/H, 1/zmax),
 * vp_off = (-1, 1) (plus any pixel-centre offset, in NDC).
 */
#define NV2A_VSH_HLSL_SCREEN_SPACE 0x1

/** d3d8_vsh_generate_hlsl with NV2A_VSH_HLSL_* flags. */
int d3d8_vsh_generate_hlsl_ex(const NV2AVshProgram *program, unsigned flags,
                               char *buf, int bufsize);

/**
 * Generate HLSL pixel shader source from combiner state.
 *
 * The shader evaluates the combiners the way nv2a_combiner.c's evaluator does
 * on the CPU, operation for operation. Its input struct matches the vertex
 * shader output of d3d8_vsh_hlsl.c (SV_POSITION, COLOR0, COLOR1, TEXCOORD0-3);
 * its constants are NV2APSConstants at b0; stage n samples tN with sN.
 *
 * @param state   The combiner configuration
 * @param buf     Output buffer for HLSL source
 * @param bufsize Size of output buffer in bytes
 * @return        Number of characters written (excluding null terminator),
 *                or -1 on error
 */
int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_HLSL_H */
