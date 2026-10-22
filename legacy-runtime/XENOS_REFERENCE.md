# Xenos GPU reference (condensed from xenia ucode.h / registers.h / xenos.h; used by the renderer)

## Registers (index)
SQ_PROGRAM_CNTL 2180 (vs_num_reg:0-5, ps_num_reg:8-13, vs_resource16, ps_resource17, param_gen18, gen_index_pix19,
 vs_export_count 20-23 (count-1), vs_export_mode 24-26, ps_export_mode 27-30, gen_index_vtx31); SQ_CONTEXT_MISC 2181;
SQ_INTERPOLATOR_CNTL 2182; PA_SU_SC_MODE_CNTL 2205 (cull_front0 cull_back1 face2(1=CW front) poly_mode 3-4 ... multi_prim_ib 21);
PA_CL_CLIP_CNTL 2204 (clip_disable 16, dx_clip_space_def 19); PA_CL_VTE_CNTL 2206 (xscale0 xoff1 yscale2 yoff3 zscale4 zoff5
 xy_fmt8 z_fmt9 w0_fmt10); PA_CL_VPORT X/Y/Z SCALE/OFFSET 210F..2114 (xs,xo,ys,yo,zs,zo); PA_SC_WINDOW_OFFSET 2080
 (x:0-14 signed, y:16-30 signed); WINDOW_SCISSOR TL/BR 2081/2082 (x 0-13, y 16-29, TL bit31 = window_offset_disable);
SCREEN_SCISSOR 200E/200F; VGT_DRAW_INITIATOR 21FC (prim 0-5, src 6-7, major 8-9, idx_size 11 (1=32bit), num_indices 16-31);
VGT_INDX_OFFSET 2102; MIN/MAX_VTX 2101/2100; VGT_DMA_BASE 21FA; VGT_DMA_SIZE 21FB (words 0-23, swap 30-31);
VGT_MULTI_PRIM_IB_RESET_INDX 2103; RB_MODECONTROL 2208 (0 nop, 4 color+depth, 5 depth only, 6 copy);
RB_SURFACE_INFO 2000 (pitch 0-13, msaa 16-17); RB_COLOR_INFO 2001 (base 0-10 tiles(+bit11), fmt 16-19, exp_bias 20-25);
COLOR1/2/3_INFO 2003/2004/2005; RB_DEPTH_INFO 2002 (base 0-10, fmt 16); RB_DEPTHCONTROL 2200 (stencil_en0 z_en1 zwrite2
 zfunc 4-6 backface7 sfunc 8-10 sfail 11-13 szpass 14-16 szfail 17-19; _bf 20..); RB_COLORCONTROL 2202 (alpha_func 0-2,
 alpha_test 3); RB_BLENDCONTROL0..3 2201/2209/220A/220B (csrc 0-4, cop 5-7, cdst 8-12, asrc 16-20, aop 21-23, adst 24-28);
RB_COLOR_MASK 2104 (4 bits per RT); RB_BLEND_RGBA 2105..2108; RB_STENCILREFMASK 210D (ref0-7 mask8-15 wmask16-23), _BF 210C;
RB_ALPHA_REF 210E; RB_COPY_CONTROL 2318 (src_select 0-2 (4=depth), sample_sel 4-6, color_clear 8, depth_clear 9, cmd 20-21);
RB_COPY_DEST_BASE 2319; RB_COPY_DEST_PITCH 231A (pitch 0-13, height 16-29); RB_COPY_DEST_INFO 231B (endian 0-2, array3,
 slice 4-6, format 7-12, number 13-15, exp_bias 16-21, swap 24); RB_DEPTH_CLEAR 231D; RB_COLOR_CLEAR 231E; _LO 231F;
RB_COPY_FUNC 2320; REF 2321; MASK 2322; PA_SC_AA_CONFIG 2301; PA_SC_AA_MASK 2312; RB_SAMPLE_COUNT_CTL 2324;
SQ_VS_CONST 2307; SQ_PS_CONST 2308; PA_SU_POINT_SIZE 2280; PA_SU_VTX_CNTL 2302; VGT_OUTPUT_PATH_CNTL 2284;
ALU constants 4000 (VS c0.. 256 vec4 at 4000-43FF, PS 4400-47FF); fetch constants 4800 (6 dwords each, 32);
bool 4900; loop 4908.

## Enums
Prim: 1 point,2 linelist,3 linestrip,4 trilist,5 trifan,6 tristrip,8 rectlist,0xC lineloop,0xD quadlist,0xE quadstrip,0xF polygon,
 0x13 2DCopyRectListV3 etc.  Source: 0 DMA, 1 immediate, 2 auto-index.  IndexFormat 0=16, 1=32.
Endian: 0 none, 1 8in16, 2 8in32, 3 16in32.  Endian128 adds 4 8in64, 5 8in128.
ColorRT fmt: 0 8888, 1 8888_GAMMA, 2 2_10_10_10, 3 2_10_10_10_FLOAT, 4 16_16, 5 16x4, 6 16_16_F, 7 16x4_F, 14 32F, 15 32_32F.
Depth fmt: 0 D24S8, 1 D24FS8.
TextureFormat: 2 8, 3 1_5_5_5, 4 5_6_5, 5 6_5_5, 6 8888, 7 2_10_10_10, 8 8_A, 9 8_B, 10 8_8, 14 8888_A, 15 4444, 16 10_11_11,
 17 11_11_10, 18 DXT1, 19 DXT2_3, 20 DXT4_5, 22 24_8, 23 24_8_F, 24 16, 25 16_16, 26 16x4, 30 16F, 31 16_16F, 32 16x4F, 33 32,
 34 32_32, 35 32x4, 36 32F, 37 32_32F, 38 32x4F, 49 DXN, 57 32_32_32F, 58 DXT3A, 59 DXT5A, 60 CTX1.
Compare: 0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always.
Stencil: 0 keep 1 zero 2 replace 3 inc_clamp 4 dec_clamp 5 invert 6 inc_wrap 7 dec_wrap.
Blend factor: 0 zero 1 one 4 src 5 1-src 6 srcA 7 1-srcA 8 dst 9 1-dst 10 dstA 11 1-dstA 12 const 13 1-const 14 constA
 15 1-constA 16 srcA_sat.  Blend op: 0 add 1 sub 2 min 3 max 4 revsub.
Clamp: 0 repeat 1 mirror 2 clamp-edge 3 mirror-clamp-edge 4 clamp-half 5 mirror-half 6 border 7 mirror-border.
Filter: 0 point 1 linear 2 basemap 3 use fetch const.  Sign: 0 unsigned 1 signed 2 biased 3 gamma.
Texture swizzle 3 bits/channel (bits 0,3,6,9): 0 X,1 Y,2 Z,3 W,4 zero,5 one.
CopyCommand: 0 raw 1 convert 2 const-one 3 null.

## Fetch constants (6 dwords)  texture:
dw0 type 0-1 (2=tex,3=vtx) sign xyzw 2,4,6,8 (2b each) clamp_x 10-12 y 13-15 z 16-18 pitch 22-30 (units of 32 texels) tiled 31
dw1 format 0-5 endian 6-7 base_address 12-31 (>>12)
dw2 2D: width-1 0-12, height-1 13-25
dw3 num_format0 swizzle 1-12 exp_adjust 13-18 mag 19-20 min 21-22 mip 23-24 aniso 25-27
dw4 mip_min 2-5 mip_max 6-9 ; dw5 dimension 9-10, packed_mips 11, mip_address 12-31
vertex fetch (2 dwords used): dw0 type 0-1, address 2-31 (dwords); dw1 endian 0-1, size 2-25 (words)

## Shader ucode
CF: 3 dwords hold 2 instrs of 48 bits: A=(d0, d1&0xFFFF) B=((d1>>16)|(d2<<16), d2>>16). Fields (word0/word1 of the 48):
 exec: address 0-11, count 12-14, yield 15, sequence 16-27 (2 bits/instr: bit0 fetch, bit1 serialize); w1: vc_lo, is_pred_clean bit 9?,
 address_mode, opcode = top 4 bits of the 16 (bits 12-15 of w1).  cond_exec: bool_address w1 bits 2-9, condition bit 10.
 loop_start: addr 0-12, repeat 13, loop_id 16-20.  loop_end: addr 0-12, loop_id 16-20, pred_break 21, cond w1 bit 10.
 call/jmp: addr 0-12, unconditional 13, predicated 14; w1: bool_addr 2-9 (jmp: direction bit 1), cond 10.
 opcodes: 0 nop 1 exec 2 exec_end 3 cond_exec 4 cond_exec_end 5 cond_exec_pred 6 ..pred_end 7 loop_start 8 loop_end 9 cond_call
 10 return 11 cond_jmp 12 alloc 13 cond_exec_pred_clean 14 ..end 15 mark_vs_fetch_done.
ALU (3 dwords): d0: vdest 0-5, vdest_rel 6, abs_const 7, sdest 8-13, sdest_rel 14, export 15, vmask 16-19, smask 20-23,
 vclamp 24, sclamp 25, scalar_opc 26-31.  d1: src3_swiz 0-7, src2_swiz 8-15, src1_swiz 16-23, src3_neg 24, src2_neg 25,
 src1_neg 26, pred_cond 27, is_pred 28, const_addr_reg_rel 29, const1_rel_abs 30, const0_rel_abs 31.
 d2: src3_reg 0-7, src2_reg 8-15, src1_reg 16-23, vector_opc 24-28, src3_sel 29, src2_sel 30, src1_sel 31 (sel=1 -> register, 0 -> const).
Vector ops: 0 add 1 mul 2 max 3 min 4 seq 5 sgt 6 sge 7 sne 8 frc 9 trunc 10 floor 11 mad 12 cndeq 13 cndge 14 cndgt 15 dp4
 16 dp3 17 dp2add 18 cube 19 max4 20-23 setp_{eq,ne,gt,ge}_push 24-27 kill{eq,gt,ge,ne} 28 dst 29 maxa.
Scalar ops: 0 adds 1 adds_prev 2 muls 3 muls_prev 4 muls_prev2 5 maxs 6 mins 7 seqs 8 sgts 9 sges 10 snes 11 frcs 12 truncs
 13 floors 14 exp 15 logc 16 log 17 rcpc 18 rcpf 19 rcp 20 rsqc 21 rsqf 22 rsq 23 maxas 24 maxasf 25 subs 26 subs_prev
 27 setp_eq 28 ne 29 gt 30 ge 31 setp_inv 32 pop 33 clr 34 rstr 35-38 kills_{eq,gt,ge,ne} 39 kills_one 40 sqrt
 42/43 mulsc0/1 44/45 addsc0/1 46/47 subsc0/1 48 sin 49 cos 50 retain_prev.
Vertex fetch (3 dwords): d0 opcode 0-4, src_reg 5-10, src_am 11, dst_reg 12-17, dst_am 18, must_be_one 19, const_index 20-24,
 const_index_sel 25-26, prefetch 27-29, src_swiz 30-31; d1 dst_swiz 0-11, fomat_comp_all 12, num_format_all 13, signed 14,
 is_index_rounded 15, format 16-21, exp_adjust 24-29, is_mini 30, is_pred 31; d2 stride 0-7, offset 8-30 (dwords), pred_cond 31.
Texture fetch (3 dwords): d0 opcode 0-4 (1 = tfetch), src_reg 5-10, src_am 11, dst_reg 12-17, dst_am 18, fetch_valid_only 19,
 const_index 20-24, tx_coord_denorm 25, src_swiz 26-31; d1 dst_swiz 0-11, mag 12-13, min 14-15, mip 16-17, aniso 18-20,
 arbitrary 21-23, vol_mag 24-25, vol_min 26-27, use_comp_lod 28, use_reg_lod 29, unk 30, is_pred 31;
 d2 use_reg_grad 0, sample_loc 1, lod_bias 2-8, dimension 14-15, offset_x 16-20 y 21-25 z 26-30, pred_cond 31.
Fetch opcodes (ucode): vfetch=0 (bit?), tfetch=1, 2 getTexBorderColorFrac, 16 getTexComputedLod, 17 getTexGradients, 18 getTexWeights, 24 setTexLod, 25 setTexGradH, 26 setTexGradV.
