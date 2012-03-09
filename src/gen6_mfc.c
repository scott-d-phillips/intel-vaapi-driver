/*
 * Copyright © 2010-2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Zhou Chang <chang.zhou@intel.com>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "intel_batchbuffer.h"
#include "i965_defines.h"
#include "i965_structs.h"
#include "i965_drv_video.h"
#include "i965_encoder.h"
#include "i965_encoder_utils.h"
#include "gen6_mfc.h"
#include "gen6_vme.h"

static const uint32_t gen6_mfc_batchbuffer_avc_intra[][4] = {
#include "shaders/utils/mfc_batchbuffer_avc_intra.g6b"
};

static const uint32_t gen6_mfc_batchbuffer_avc_inter[][4] = {
#include "shaders/utils/mfc_batchbuffer_avc_inter.g6b"
};

static struct i965_kernel gen6_mfc_kernels[] = {
    {
        "MFC AVC INTRA BATCHBUFFER ",
        MFC_BATCHBUFFER_AVC_INTRA,
        gen6_mfc_batchbuffer_avc_intra,
        sizeof(gen6_mfc_batchbuffer_avc_intra),
        NULL
    },

    {
        "MFC AVC INTER BATCHBUFFER ",
        MFC_BATCHBUFFER_AVC_INTER,
        gen6_mfc_batchbuffer_avc_inter,
        sizeof(gen6_mfc_batchbuffer_avc_inter),
        NULL
    },
};

static void
gen6_mfc_pipe_mode_select(VADriverContextP ctx,
                          int standard_select,
                          struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;

    assert(standard_select == MFX_FORMAT_AVC);

    BEGIN_BCS_BATCH(batch, 4);

    OUT_BCS_BATCH(batch, MFX_PIPE_MODE_SELECT | (4 - 2));
    OUT_BCS_BATCH(batch,
                  (1 << 10) | /* disable Stream-Out , advanced QP/bitrate control need enable it*/
                  (1 << 9)  | /* Post Deblocking Output */
                  (0 << 8)  | /* Pre Deblocking Output */
                  (0 << 7)  | /* disable TLB prefectch */
                  (0 << 5)  | /* not in stitch mode */
                  (1 << 4)  | /* encoding mode */
                  (2 << 0));  /* Standard Select: AVC */
    OUT_BCS_BATCH(batch,
                  (0 << 20) | /* round flag in PB slice */
                  (0 << 19) | /* round flag in Intra8x8 */
                  (0 << 7)  | /* expand NOA bus flag */
                  (1 << 6)  | /* must be 1 */
                  (0 << 5)  | /* disable clock gating for NOA */
                  (0 << 4)  | /* terminate if AVC motion and POC table error occurs */
                  (0 << 3)  | /* terminate if AVC mbdata error occurs */
                  (0 << 2)  | /* terminate if AVC CABAC/CAVLC decode error occurs */
                  (0 << 1)  | /* AVC long field motion vector */
                  (0 << 0));  /* always calculate AVC ILDB boundary strength */
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_surface_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

    BEGIN_BCS_BATCH(batch, 6);

    OUT_BCS_BATCH(batch, MFX_SURFACE_STATE | (6 - 2));
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch,
                  ((mfc_context->surface_state.height - 1) << 19) |
                  ((mfc_context->surface_state.width - 1) << 6));
    OUT_BCS_BATCH(batch,
                  (MFX_SURFACE_PLANAR_420_8 << 28) | /* 420 planar YUV surface */
                  (1 << 27) | /* must be 1 for interleave U/V, hardware requirement */
                  (0 << 22) | /* surface object control state, FIXME??? */
                  ((mfc_context->surface_state.w_pitch - 1) << 3) | /* pitch */
                  (0 << 2)  | /* must be 0 for interleave U/V */
                  (1 << 1)  | /* must be y-tiled */
                  (I965_TILEWALK_YMAJOR << 0));  			/* tile walk, TILEWALK_YMAJOR */
    OUT_BCS_BATCH(batch,
                  (0 << 16) | 								/* must be 0 for interleave U/V */
                  (mfc_context->surface_state.h_pitch)); 		/* y offset for U(cb) */
    OUT_BCS_BATCH(batch, 0);
    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_pipe_buf_addr_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    int i;

    BEGIN_BCS_BATCH(batch, 24);

    OUT_BCS_BATCH(batch, MFX_PIPE_BUF_ADDR_STATE | (24 - 2));

    OUT_BCS_BATCH(batch, 0);											/* pre output addr   */

    OUT_BCS_RELOC(batch, mfc_context->post_deblocking_output.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);											/* post output addr  */	

    OUT_BCS_RELOC(batch, mfc_context->uncompressed_picture_source.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);											/* uncompressed data */
    OUT_BCS_RELOC(batch, mfc_context->macroblock_status_buffer.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);											/* StreamOut data*/
    OUT_BCS_RELOC(batch, mfc_context->intra_row_store_scratch_buffer.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);	
    OUT_BCS_RELOC(batch, mfc_context->deblocking_filter_row_store_scratch_buffer.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);
    /* 7..22 Reference pictures*/
    for (i = 0; i < ARRAY_ELEMS(mfc_context->reference_surfaces); i++) {
        if ( mfc_context->reference_surfaces[i].bo != NULL) {
            OUT_BCS_RELOC(batch, mfc_context->reference_surfaces[i].bo,
                          I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                          0);			
        } else {
            OUT_BCS_BATCH(batch, 0);
        }
    }
    OUT_BCS_RELOC(batch, mfc_context->macroblock_status_buffer.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);											/* Macroblock status buffer*/

    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_ind_obj_base_addr_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    struct gen6_vme_context *vme_context = encoder_context->vme_context;

    BEGIN_BCS_BATCH(batch, 11);

    OUT_BCS_BATCH(batch, MFX_IND_OBJ_BASE_ADDR_STATE | (11 - 2));
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    /* MFX Indirect MV Object Base Address */
    OUT_BCS_RELOC(batch, vme_context->vme_output.bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);
    OUT_BCS_BATCH(batch, 0);	
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    /*MFC Indirect PAK-BSE Object Base Address for Encoder*/	
    OUT_BCS_RELOC(batch,
                  mfc_context->mfc_indirect_pak_bse_object.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);
    OUT_BCS_RELOC(batch,
                  mfc_context->mfc_indirect_pak_bse_object.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  mfc_context->mfc_indirect_pak_bse_object.end_offset);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_bsp_buf_base_addr_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

    BEGIN_BCS_BATCH(batch, 4);

    OUT_BCS_BATCH(batch, MFX_BSP_BUF_BASE_ADDR_STATE | (4 - 2));
    OUT_BCS_RELOC(batch, mfc_context->bsd_mpc_row_store_scratch_buffer.bo,
                  I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
                  0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_avc_img_state(VADriverContextP ctx,struct encode_state *encode_state,
                       struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int height_in_mbs = (mfc_context->surface_state.height + 15) / 16;

    BEGIN_BCS_BATCH(batch, 13);
    OUT_BCS_BATCH(batch, MFX_AVC_IMG_STATE | (13 - 2));
    OUT_BCS_BATCH(batch, 
                  ((width_in_mbs * height_in_mbs) & 0xFFFF));
    OUT_BCS_BATCH(batch, 
                  (height_in_mbs << 16) | 
                  (width_in_mbs << 0));
    OUT_BCS_BATCH(batch, 
                  (0 << 24) |	  /*Second Chroma QP Offset*/
                  (0 << 16) |	  /*Chroma QP Offset*/
                  (0 << 14) |   /*Max-bit conformance Intra flag*/
                  (0 << 13) |   /*Max Macroblock size conformance Inter flag*/
                  (1 << 12) |   /*Should always be written as "1" */
                  (0 << 10) |   /*QM Preset FLag */
                  (0 << 8)  |   /*Image Structure*/
                  (0 << 0) );   /*Current Decoed Image Frame Store ID, reserved in Encode mode*/
    OUT_BCS_BATCH(batch,
                  (400 << 16) |   /*Mininum Frame size*/	
                  (0 << 15) |	/*Disable reading of Macroblock Status Buffer*/
                  (0 << 14) |   /*Load BitStream Pointer only once, 1 slic 1 frame*/
                  (0 << 13) |   /*CABAC 0 word insertion test enable*/
                  (1 << 12) |   /*MVUnpackedEnable,compliant to DXVA*/
                  (1 << 10) |   /*Chroma Format IDC, 4:2:0*/
                  (pPicParameter->pic_fields.bits.entropy_coding_mode_flag << 7)  |   /*0:CAVLC encoding mode,1:CABAC*/
                  (0 << 6)  |   /*Only valid for VLD decoding mode*/
                  (0 << 5)  |   /*Constrained Intra Predition Flag, from PPS*/
                  (pSequenceParameter->direct_8x8_inference_flag << 4)  |   /*Direct 8x8 inference flag*/
                  (pPicParameter->pic_fields.bits.transform_8x8_mode_flag << 3)  |   /*8x8 or 4x4 IDCT Transform Mode Flag*/
                  (1 << 2)  |   /*Frame MB only flag*/
                  (0 << 1)  |   /*MBAFF mode is in active*/
                  (0 << 0) );   /*Field picture flag*/
    OUT_BCS_BATCH(batch, 
                  (1<<16)   |   /*Frame Size Rate Control Flag*/  
                  (1<<12)   |   
                  (1<<9)    |	/*MB level Rate Control Enabling Flag*/
                  (1 << 3)  |   /*FrameBitRateMinReportMask*/
                  (1 << 2)  |   /*FrameBitRateMaxReportMask*/
                  (1 << 1)  |   /*InterMBMaxSizeReportMask*/
                  (1 << 0) );   /*IntraMBMaxSizeReportMask*/
    OUT_BCS_BATCH(batch, 			/*Inter and Intra Conformance Max size limit*/
                  (0x0600 << 16) |		/*InterMbMaxSz 192 Byte*/
                  (0x0800) );			/*IntraMbMaxSz 256 Byte*/
    OUT_BCS_BATCH(batch, 0x00000000);   /*Reserved : MBZReserved*/
    OUT_BCS_BATCH(batch, 0x01020304);	/*Slice QP Delta for bitrate control*/   		
    OUT_BCS_BATCH(batch, 0xFEFDFCFB);   	
    OUT_BCS_BATCH(batch, 0x80601004);   /*MAX = 128KB, MIN = 64KB*/
    OUT_BCS_BATCH(batch, 0x00800001);   
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_avc_directmode_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

    int i;

    BEGIN_BCS_BATCH(batch, 69);

    OUT_BCS_BATCH(batch, MFX_AVC_DIRECTMODE_STATE | (69 - 2));

    /* Reference frames and Current frames */
    for(i = 0; i < NUM_MFC_DMV_BUFFERS; i++) {
        if ( mfc_context->direct_mv_buffers[i].bo != NULL) { 
            OUT_BCS_RELOC(batch, mfc_context->direct_mv_buffers[i].bo,
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          0);
        } else {
            OUT_BCS_BATCH(batch, 0);
        }
    }

    /* POL list */
    for(i = 0; i < 32; i++) {
        OUT_BCS_BATCH(batch, i/2);
    }
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen6_mfc_avc_slice_state(VADriverContextP ctx,
                         VAEncSliceParameterBufferH264 *slice_param,
                         struct encode_state *encode_state,
                         struct intel_encoder_context *encoder_context,
                         int rate_control_enable,
                         int qp,
                         struct intel_batchbuffer *batch)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int height_in_mbs = (mfc_context->surface_state.height + 15) / 16;
    int beginmb = slice_param->starting_macroblock_address;
    int endmb = beginmb + slice_param->number_of_mbs;
    int beginx = beginmb % width_in_mbs;
    int beginy = beginmb / width_in_mbs;
    int nextx =  endmb % width_in_mbs;
    int nexty = endmb / width_in_mbs;
    int slice_type = slice_param->slice_type;
    int last_slice = (endmb == (width_in_mbs * height_in_mbs));
    int bit_rate_control_target, maxQpN, maxQpP;
    unsigned char correct[6], grow, shrink;
    int i;

    if (batch == NULL)
        batch = encoder_context->base.batch;

    if (slice_type == SLICE_TYPE_I)
        bit_rate_control_target = 0;
    else
        bit_rate_control_target = 1;

    maxQpN = mfc_context->bit_rate_control_context[bit_rate_control_target].MaxQpNegModifier;
    maxQpP = mfc_context->bit_rate_control_context[bit_rate_control_target].MaxQpPosModifier;

    for (i = 0; i < 6; i++)
        correct[i] = mfc_context->bit_rate_control_context[bit_rate_control_target].Correct[i];

    grow = mfc_context->bit_rate_control_context[bit_rate_control_target].GrowInit + 
        (mfc_context->bit_rate_control_context[bit_rate_control_target].GrowResistance << 4);
    shrink = mfc_context->bit_rate_control_context[bit_rate_control_target].ShrinkInit + 
        (mfc_context->bit_rate_control_context[bit_rate_control_target].ShrinkResistance << 4);

    BEGIN_BCS_BATCH(batch, 11);;

    OUT_BCS_BATCH(batch, MFX_AVC_SLICE_STATE | (11 - 2) );
    OUT_BCS_BATCH(batch, slice_type);			/*Slice Type: I:P:B Slice*/

    if (slice_type == SLICE_TYPE_I) {
        OUT_BCS_BATCH(batch, 0);			/*no reference frames and pred_weight_table*/
    } else {
        OUT_BCS_BATCH(batch, 0x00010000); 	/*1 reference frame*/
    }

    OUT_BCS_BATCH(batch, 
                  (slice_param->direct_spatial_mv_pred_flag << 29) |             /*Direct Prediction Type*/
                  (0 << 24) |                /*Enable deblocking operation*/
                  (qp << 16) | 			/*Slice Quantization Parameter*/
                  (0x0202 << 0));
    OUT_BCS_BATCH(batch, (beginy << 24) |			/*First MB X&Y , the begin postion of current slice*/
                  (beginx << 16) |
                  slice_param->starting_macroblock_address );
    OUT_BCS_BATCH(batch, (nexty << 16) | nextx);                       /*Next slice first MB X&Y*/
    OUT_BCS_BATCH(batch, 
                  (rate_control_enable << 31) |		/*in CBR mode RateControlCounterEnable = enable*/
                  (1 << 30) |		/*ResetRateControlCounter*/
                  (0 << 28) |		/*RC Triggle Mode = Always Rate Control*/
                  (4 << 24) |     /*RC Stable Tolerance, middle level*/
                  (rate_control_enable << 23) |     /*RC Panic Enable*/                 
                  (0 << 22) |     /*QP mode, don't modfiy CBP*/
                  (0 << 21) |     /*MB Type Direct Conversion Enabled*/ 
                  (0 << 20) |     /*MB Type Skip Conversion Enabled*/ 
                  (last_slice << 19) |     /*IsLastSlice*/
                  (0 << 18) | 	/*BitstreamOutputFlag Compressed BitStream Output Disable Flag 0:enable 1:disable*/
                  (1 << 17) |	    /*HeaderPresentFlag*/	
                  (1 << 16) |	    /*SliceData PresentFlag*/
                  (1 << 15) |	    /*TailPresentFlag*/
                  (1 << 13) |	    /*RBSP NAL TYPE*/	
                  (0 << 12) );    /*CabacZeroWordInsertionEnable*/
    OUT_BCS_BATCH(batch, mfc_context->mfc_indirect_pak_bse_object.offset);
    OUT_BCS_BATCH(batch,
                  (maxQpN << 24) |     /*Target QP - 24 is lowest QP*/ 
                  (maxQpP << 16) |     /*Target QP + 20 is highest QP*/
                  (shrink << 8)  |
                  (grow << 0));   
    OUT_BCS_BATCH(batch,
                  (correct[5] << 20) |
                  (correct[4] << 16) |
                  (correct[3] << 12) |
                  (correct[2] << 8) |
                  (correct[1] << 4) |
                  (correct[0] << 0));
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void gen6_mfc_avc_qm_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    int i;

    BEGIN_BCS_BATCH(batch, 58);

    OUT_BCS_BATCH(batch, MFX_AVC_QM_STATE | 56);
    OUT_BCS_BATCH(batch, 0xFF ) ; 
    for( i = 0; i < 56; i++) {
        OUT_BCS_BATCH(batch, 0x10101010); 
    }   

    ADVANCE_BCS_BATCH(batch);
}

static void gen6_mfc_avc_fqm_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    int i;

    BEGIN_BCS_BATCH(batch, 113);
    OUT_BCS_BATCH(batch, MFC_AVC_FQM_STATE | (113 - 2));

    for(i = 0; i < 112;i++) {
        OUT_BCS_BATCH(batch, 0x10001000);
    }   

    ADVANCE_BCS_BATCH(batch);	
}

static void
gen6_mfc_avc_ref_idx_state(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    int i;

    BEGIN_BCS_BATCH(batch, 10);
    OUT_BCS_BATCH(batch, MFX_AVC_REF_IDX_STATE | 8); 
    OUT_BCS_BATCH(batch, 0);                  //Select L0
    OUT_BCS_BATCH(batch, 0x80808020);         //Only 1 reference
    for(i = 0; i < 7; i++) {
        OUT_BCS_BATCH(batch, 0x80808080);
    }   
    ADVANCE_BCS_BATCH(batch);

    BEGIN_BCS_BATCH(batch, 10);
    OUT_BCS_BATCH(batch, MFX_AVC_REF_IDX_STATE | 8); 
    OUT_BCS_BATCH(batch, 1);                  //Select L1
    OUT_BCS_BATCH(batch, 0x80808022);         //Only 1 reference
    for(i = 0; i < 7; i++) {
        OUT_BCS_BATCH(batch, 0x80808080);
    }   
    ADVANCE_BCS_BATCH(batch);
}
	
static void
gen6_mfc_avc_insert_object(VADriverContextP ctx, struct intel_encoder_context *encoder_context,
                           unsigned int *insert_data, int lenght_in_dws, int data_bits_in_last_dw,
                           int skip_emul_byte_count, int is_last_header, int is_end_of_slice, int emulation_flag,
                           struct intel_batchbuffer *batch)
{
    if (batch == NULL)
        batch = encoder_context->base.batch;

    BEGIN_BCS_BATCH(batch, lenght_in_dws + 2);

    OUT_BCS_BATCH(batch, MFC_AVC_INSERT_OBJECT | (lenght_in_dws + 2 - 2));

    OUT_BCS_BATCH(batch,
                  (0 << 16) |   /* always start at offset 0 */
                  (data_bits_in_last_dw << 8) |
                  (skip_emul_byte_count << 4) |
                  (!!emulation_flag << 3) |
                  ((!!is_last_header) << 2) |
                  ((!!is_end_of_slice) << 1) |
                  (0 << 0));    /* FIXME: ??? */

    intel_batchbuffer_data(batch, insert_data, lenght_in_dws * 4);
    ADVANCE_BCS_BATCH(batch);
}

static void gen6_mfc_init(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    dri_bo *bo;
    int i;

    /*Encode common setup for MFC*/
    dri_bo_unreference(mfc_context->post_deblocking_output.bo);
    mfc_context->post_deblocking_output.bo = NULL;

    dri_bo_unreference(mfc_context->pre_deblocking_output.bo);
    mfc_context->pre_deblocking_output.bo = NULL;

    dri_bo_unreference(mfc_context->uncompressed_picture_source.bo);
    mfc_context->uncompressed_picture_source.bo = NULL;

    dri_bo_unreference(mfc_context->mfc_indirect_pak_bse_object.bo); 
    mfc_context->mfc_indirect_pak_bse_object.bo = NULL;

    for (i = 0; i < NUM_MFC_DMV_BUFFERS; i++){
        if ( mfc_context->direct_mv_buffers[i].bo != NULL);
        dri_bo_unreference(mfc_context->direct_mv_buffers[i].bo);
        mfc_context->direct_mv_buffers[i].bo = NULL;
    }

    for (i = 0; i < MAX_MFC_REFERENCE_SURFACES; i++){
        if (mfc_context->reference_surfaces[i].bo != NULL)
            dri_bo_unreference(mfc_context->reference_surfaces[i].bo);
        mfc_context->reference_surfaces[i].bo = NULL;  
    }

    dri_bo_unreference(mfc_context->intra_row_store_scratch_buffer.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      128 * 64,
                      64);
    assert(bo);
    mfc_context->intra_row_store_scratch_buffer.bo = bo;

    dri_bo_unreference(mfc_context->macroblock_status_buffer.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      128*128*16,
                      64);
    assert(bo);
    mfc_context->macroblock_status_buffer.bo = bo;

    dri_bo_unreference(mfc_context->deblocking_filter_row_store_scratch_buffer.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      49152,  /* 6 * 128 * 64 */
                      64);
    assert(bo);
    mfc_context->deblocking_filter_row_store_scratch_buffer.bo = bo;

    dri_bo_unreference(mfc_context->bsd_mpc_row_store_scratch_buffer.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      12288, /* 1.5 * 128 * 64 */
                      0x1000);
    assert(bo);
    mfc_context->bsd_mpc_row_store_scratch_buffer.bo = bo;

    dri_bo_unreference(mfc_context->mfc_batchbuffer_surface.bo);
    mfc_context->mfc_batchbuffer_surface.bo = NULL;

    dri_bo_unreference(mfc_context->aux_batchbuffer_surface.bo);
    mfc_context->aux_batchbuffer_surface.bo = NULL;

    if (mfc_context->aux_batchbuffer)
        intel_batchbuffer_free(mfc_context->aux_batchbuffer);

    mfc_context->aux_batchbuffer = intel_batchbuffer_new(&i965->intel, I915_EXEC_BSD);
    mfc_context->aux_batchbuffer_surface.bo = mfc_context->aux_batchbuffer->buffer;
    dri_bo_reference(mfc_context->aux_batchbuffer_surface.bo);
    mfc_context->aux_batchbuffer_surface.pitch = 16;
    mfc_context->aux_batchbuffer_surface.num_blocks = mfc_context->aux_batchbuffer->size / 16;
    mfc_context->aux_batchbuffer_surface.size_block = 16;

    i965_gpe_context_init(ctx, &mfc_context->gpe_context);
}

static void gen6_mfc_avc_pipeline_header_programing(VADriverContextP ctx,
                                                    struct encode_state *encode_state,
                                                    struct intel_encoder_context *encoder_context,
                                                    struct intel_batchbuffer *slice_batch)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    static int count = 0;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    int rate_control_mode = pSequenceParameter->rate_control_method;   

    if (encode_state->packed_header_data[VAEncPackedHeaderSPS]) {
        VAEncPackedHeaderParameterBuffer *param = NULL;
        unsigned int *header_data = (unsigned int *)encode_state->packed_header_data[VAEncPackedHeaderSPS]->buffer;
        unsigned int length_in_bits;

        assert(encode_state->packed_header_param[VAEncPackedHeaderSPS]);
        param = (VAEncPackedHeaderParameterBuffer *)encode_state->packed_header_param[VAEncPackedHeaderSPS]->buffer;
        length_in_bits = param->length_in_bits[0];

        mfc_context->insert_object(ctx,
                                   encoder_context,
                                   header_data,
                                   ALIGN(length_in_bits, 32) >> 5,
                                   length_in_bits & 0x1f,
                                   param->skip_emulation_check_count,
                                   0,
                                   0,
                                   param->insert_emulation_bytes,
                                   slice_batch);
    }

    if (encode_state->packed_header_data[VAEncPackedHeaderPPS]) {
        VAEncPackedHeaderParameterBuffer *param = NULL;
        unsigned int *header_data = (unsigned int *)encode_state->packed_header_data[VAEncPackedHeaderPPS]->buffer;
        unsigned int length_in_bits;

        assert(encode_state->packed_header_param[VAEncPackedHeaderPPS]);
        param = (VAEncPackedHeaderParameterBuffer *)encode_state->packed_header_param[VAEncPackedHeaderPPS]->buffer;
        length_in_bits = param->length_in_bits[0];

        mfc_context->insert_object(ctx,
                                   encoder_context,
                                   header_data,
                                   ALIGN(length_in_bits, 32) >> 5,
                                   length_in_bits & 0x1f,
                                   param->skip_emulation_check_count,
                                   0,
                                   0,
                                   param->insert_emulation_bytes,
                                   slice_batch);
    }
    
    if ( (rate_control_mode == 0) && encode_state->packed_header_data[VAEncPackedHeaderSPS]) {       // this is frist AU
        struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

        unsigned char *sei_data = NULL;
        int length_in_bits = build_avc_sei_buffering_period(mfc_context->vui_hrd.i_initial_cpb_removal_delay_length, 
                                                            mfc_context->vui_hrd.i_initial_cpb_removal_delay, 0, &sei_data);
        mfc_context->insert_object(ctx,
                                   encoder_context,
                                   (unsigned int *)sei_data,
                                   ALIGN(length_in_bits, 32) >> 5,
                                   length_in_bits & 0x1f,
                                   4,   
                                   0,   
                                   0,   
                                   1,
                                   slice_batch);  
        free(sei_data);
    }    

    // SEI pic_timing header
    if ( rate_control_mode == 0) {   
        struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
        unsigned char *sei_data = NULL;
        int length_in_bits = build_avc_sei_pic_timing( mfc_context->vui_hrd.i_cpb_removal_delay_length,
                                                       mfc_context->vui_hrd.i_cpb_removal_delay * mfc_context->vui_hrd.i_frame_number,
                                                       mfc_context->vui_hrd.i_dpb_output_delay_length,
                                                       0, &sei_data);
        mfc_context->insert_object(ctx,
                                   encoder_context,
                                   (unsigned int *)sei_data,
                                   ALIGN(length_in_bits, 32) >> 5,
                                   length_in_bits & 0x1f,
                                   4,   
                                   0,   
                                   0,   
                                   1,
                                   slice_batch);  
        free(sei_data);
    }  
    
    count++;
}

static void gen6_mfc_avc_pipeline_picture_programing( VADriverContextP ctx,
                                      struct encode_state *encode_state,
                                      struct intel_encoder_context *encoder_context)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

    mfc_context->pipe_mode_select(ctx, MFX_FORMAT_AVC, encoder_context);
    mfc_context->set_surface_state(ctx, encoder_context);
    mfc_context->ind_obj_base_addr_state(ctx, encoder_context);
    gen6_mfc_pipe_buf_addr_state(ctx, encoder_context);
    gen6_mfc_bsp_buf_base_addr_state(ctx, encoder_context);
    mfc_context->avc_img_state(ctx, encode_state, encoder_context);
    mfc_context->avc_qm_state(ctx, encoder_context);
    mfc_context->avc_fqm_state(ctx, encoder_context);
    gen6_mfc_avc_directmode_state(ctx, encoder_context); 
    gen6_mfc_avc_ref_idx_state(ctx, encoder_context);
}

static void 
gen6_mfc_free_avc_surface(void **data)
{
    struct gen6_mfc_avc_surface_aux *avc_surface = *data;

    if (!avc_surface)
        return;

    dri_bo_unreference(avc_surface->dmv_top);
    avc_surface->dmv_top = NULL;
    dri_bo_unreference(avc_surface->dmv_bottom);
    avc_surface->dmv_bottom = NULL;

    free(avc_surface);
    *data = NULL;
}

static void
gen6_mfc_bit_rate_control_context_init(struct encode_state *encode_state, 
                                       struct gen6_mfc_context *mfc_context)
{
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int height_in_mbs = (mfc_context->surface_state.height + 15) / 16;
    float fps =  pSequenceParameter->time_scale * 0.5 / pSequenceParameter->num_units_in_tick ;
    int inter_mb_size = pSequenceParameter->bits_per_second * 1.0 / (fps+4.0) / width_in_mbs / height_in_mbs;
    int intra_mb_size = inter_mb_size * 5.0;
    int i;
    
    mfc_context->bit_rate_control_context[0].target_mb_size = intra_mb_size;
    mfc_context->bit_rate_control_context[0].target_frame_size = intra_mb_size * width_in_mbs * height_in_mbs;
    mfc_context->bit_rate_control_context[1].target_mb_size = inter_mb_size;
    mfc_context->bit_rate_control_context[1].target_frame_size = inter_mb_size * width_in_mbs * height_in_mbs;

    for(i = 0 ; i < 2; i++) {
        mfc_context->bit_rate_control_context[i].QpPrimeY = 26;
        mfc_context->bit_rate_control_context[i].MaxQpNegModifier = 6;
        mfc_context->bit_rate_control_context[i].MaxQpPosModifier = 6;
        mfc_context->bit_rate_control_context[i].GrowInit = 6;
        mfc_context->bit_rate_control_context[i].GrowResistance = 4;
        mfc_context->bit_rate_control_context[i].ShrinkInit = 6;
        mfc_context->bit_rate_control_context[i].ShrinkResistance = 4;
        
        mfc_context->bit_rate_control_context[i].Correct[0] = 8;
        mfc_context->bit_rate_control_context[i].Correct[1] = 4;
        mfc_context->bit_rate_control_context[i].Correct[2] = 2;
        mfc_context->bit_rate_control_context[i].Correct[3] = 2;
        mfc_context->bit_rate_control_context[i].Correct[4] = 4;
        mfc_context->bit_rate_control_context[i].Correct[5] = 8;
    }
    
    mfc_context->bit_rate_control_context[0].TargetSizeInWord = (intra_mb_size + 16)/ 16;
    mfc_context->bit_rate_control_context[1].TargetSizeInWord = (inter_mb_size + 16)/ 16;

    mfc_context->bit_rate_control_context[0].MaxSizeInWord = mfc_context->bit_rate_control_context[0].TargetSizeInWord * 1.5;
    mfc_context->bit_rate_control_context[1].MaxSizeInWord = mfc_context->bit_rate_control_context[1].TargetSizeInWord * 1.5;
}

static int gen6_mfc_bit_rate_control_context_update(struct encode_state *encode_state, 
                                                    struct gen6_mfc_context *mfc_context,
                                                    int current_frame_size)
{
    VAEncSliceParameterBufferH264 *pSliceParameter = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[0]->buffer; 
    int control_index = 1 - (pSliceParameter->slice_type == SLICE_TYPE_I);
    int oldQp = mfc_context->bit_rate_control_context[control_index].QpPrimeY;

    /*
      printf("conrol_index = %d, start_qp = %d, result = %d, target = %d\n", control_index, 
      mfc_context->bit_rate_control_context[control_index].QpPrimeY, current_frame_size,
      mfc_context->bit_rate_control_context[control_index].target_frame_size );
    */

    if ( current_frame_size > mfc_context->bit_rate_control_context[control_index].target_frame_size * 4.0 ) {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY += 4;
    } else if ( current_frame_size > mfc_context->bit_rate_control_context[control_index].target_frame_size * 2.0 ) {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY += 3;
    } else if ( current_frame_size > mfc_context->bit_rate_control_context[control_index].target_frame_size * 1.50 ) {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY += 2;
    } else if ( current_frame_size > mfc_context->bit_rate_control_context[control_index].target_frame_size * 1.20 ) {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY ++;
    } else if (current_frame_size < mfc_context->bit_rate_control_context[control_index].target_frame_size * 0.30 )  {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY -= 3;
    } else if (current_frame_size < mfc_context->bit_rate_control_context[control_index].target_frame_size * 0.50 )  {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY -= 2;
    } else if (current_frame_size < mfc_context->bit_rate_control_context[control_index].target_frame_size * 0.80 )  {
        mfc_context->bit_rate_control_context[control_index].QpPrimeY --;
    }
    
    if ( mfc_context->bit_rate_control_context[control_index].QpPrimeY > 51)
        mfc_context->bit_rate_control_context[control_index].QpPrimeY = 51;
    if ( mfc_context->bit_rate_control_context[control_index].QpPrimeY < 1)
        mfc_context->bit_rate_control_context[control_index].QpPrimeY = 1;
 
    if ( mfc_context->bit_rate_control_context[control_index].QpPrimeY != oldQp)
        return 0;

    return 1;
}

static void 
gen6_mfc_hrd_context_init(struct encode_state *encode_state, 
                          struct gen6_mfc_context *mfc_context) 
{
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    int rate_control_mode = pSequenceParameter->rate_control_method;   
    int target_bit_rate = pSequenceParameter->bits_per_second;
    
    // current we only support CBR mode.
    if ( rate_control_mode == 0) {
        mfc_context->vui_hrd.i_bit_rate_value = target_bit_rate >> 10;
        mfc_context->vui_hrd.i_cpb_size_value = (target_bit_rate * 8) >> 10;
        mfc_context->vui_hrd.i_initial_cpb_removal_delay = mfc_context->vui_hrd.i_cpb_size_value * 0.5 * 1024 / target_bit_rate * 90000;
        mfc_context->vui_hrd.i_cpb_removal_delay = 2;
        mfc_context->vui_hrd.i_frame_number = 0;

        mfc_context->vui_hrd.i_initial_cpb_removal_delay_length = 24; 
        mfc_context->vui_hrd.i_cpb_removal_delay_length = 24;
        mfc_context->vui_hrd.i_dpb_output_delay_length = 24;
    }

}

static void 
gen6_mfc_hrd_context_update(struct encode_state *encode_state, 
                          struct gen6_mfc_context *mfc_context) 
{
    mfc_context->vui_hrd.i_frame_number++;
}

static VAStatus gen6_mfc_avc_prepare(VADriverContextP ctx, 
                                     struct encode_state *encode_state,
                                     struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    struct object_surface *obj_surface;	
    struct object_buffer *obj_buffer;
    struct gen6_mfc_avc_surface_aux* gen6_avc_surface;
    dri_bo *bo;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    int rate_control_mode = pSequenceParameter->rate_control_method;   
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i;

    /*Setup all the input&output object*/

    /* Setup current frame and current direct mv buffer*/
    obj_surface = SURFACE(pPicParameter->CurrPic.picture_id);
    assert(obj_surface);
    i965_check_alloc_surface_bo(ctx, obj_surface, 1, VA_FOURCC('N','V','1','2'), SUBSAMPLE_YUV420);

    if ( obj_surface->private_data == NULL) {
        gen6_avc_surface = calloc(sizeof(struct gen6_mfc_avc_surface_aux), 1);
        gen6_avc_surface->dmv_top = 
            dri_bo_alloc(i965->intel.bufmgr,
                         "Buffer",
                         68*8192, 
                         64);
        gen6_avc_surface->dmv_bottom = 
            dri_bo_alloc(i965->intel.bufmgr,
                         "Buffer",
                         68*8192, 
                         64);
        assert(gen6_avc_surface->dmv_top);
        assert(gen6_avc_surface->dmv_bottom);
        obj_surface->private_data = (void *)gen6_avc_surface;
        obj_surface->free_private_data = (void *)gen6_mfc_free_avc_surface; 
    }
    gen6_avc_surface = (struct gen6_mfc_avc_surface_aux*) obj_surface->private_data;
    mfc_context->direct_mv_buffers[NUM_MFC_DMV_BUFFERS - 2].bo = gen6_avc_surface->dmv_top;
    mfc_context->direct_mv_buffers[NUM_MFC_DMV_BUFFERS - 1].bo = gen6_avc_surface->dmv_bottom;
    dri_bo_reference(gen6_avc_surface->dmv_top);
    dri_bo_reference(gen6_avc_surface->dmv_bottom);

    mfc_context->post_deblocking_output.bo = obj_surface->bo;
    dri_bo_reference(mfc_context->post_deblocking_output.bo);

    mfc_context->surface_state.width = obj_surface->orig_width;
    mfc_context->surface_state.height = obj_surface->orig_height;
    mfc_context->surface_state.w_pitch = obj_surface->width;
    mfc_context->surface_state.h_pitch = obj_surface->height;
    
    /* Setup reference frames and direct mv buffers*/
    for(i = 0; i < MAX_MFC_REFERENCE_SURFACES; i++) {
        if ( pPicParameter->ReferenceFrames[i].picture_id != VA_INVALID_ID ) { 
            obj_surface = SURFACE(pPicParameter->ReferenceFrames[i].picture_id);
            assert(obj_surface);
            if (obj_surface->bo != NULL) {
                mfc_context->reference_surfaces[i].bo = obj_surface->bo;
                dri_bo_reference(obj_surface->bo);
            }
            /* Check DMV buffer */
            if ( obj_surface->private_data == NULL) {
                
                gen6_avc_surface = calloc(sizeof(struct gen6_mfc_avc_surface_aux), 1);
                gen6_avc_surface->dmv_top = 
                    dri_bo_alloc(i965->intel.bufmgr,
                                 "Buffer",
                                 68*8192, 
                                 64);
                gen6_avc_surface->dmv_bottom = 
                    dri_bo_alloc(i965->intel.bufmgr,
                                 "Buffer",
                                 68*8192, 
                                 64);
                assert(gen6_avc_surface->dmv_top);
                assert(gen6_avc_surface->dmv_bottom);
                obj_surface->private_data = gen6_avc_surface;
                obj_surface->free_private_data = gen6_mfc_free_avc_surface; 
            }
    
            gen6_avc_surface = (struct gen6_mfc_avc_surface_aux*) obj_surface->private_data;
            /* Setup DMV buffer */
            mfc_context->direct_mv_buffers[i*2].bo = gen6_avc_surface->dmv_top;
            mfc_context->direct_mv_buffers[i*2+1].bo = gen6_avc_surface->dmv_bottom; 
            dri_bo_reference(gen6_avc_surface->dmv_top);
            dri_bo_reference(gen6_avc_surface->dmv_bottom);
        } else {
            break;
        }
    }
	
    obj_surface = SURFACE(encoder_context->input_yuv_surface);
    assert(obj_surface && obj_surface->bo);
    mfc_context->uncompressed_picture_source.bo = obj_surface->bo;
    dri_bo_reference(mfc_context->uncompressed_picture_source.bo);

    obj_buffer = BUFFER (pPicParameter->CodedBuf); /* FIXME: fix this later */
    bo = obj_buffer->buffer_store->bo;
    assert(bo);
    mfc_context->mfc_indirect_pak_bse_object.bo = bo;
    mfc_context->mfc_indirect_pak_bse_object.offset = ALIGN(sizeof(VACodedBufferSegment), 64);
    mfc_context->mfc_indirect_pak_bse_object.end_offset = ALIGN (obj_buffer->size_element - 0x1000, 0x1000);
    dri_bo_reference(mfc_context->mfc_indirect_pak_bse_object.bo);

    /*Programing bit rate control */
    if ( mfc_context->bit_rate_control_context[0].MaxSizeInWord == 0 )
        gen6_mfc_bit_rate_control_context_init(encode_state, mfc_context);

    /*Programing HRD control */
    if ( (rate_control_mode == 0) && (mfc_context->vui_hrd.i_cpb_size_value == 0) )
        gen6_mfc_hrd_context_init(encode_state, mfc_context);

    return vaStatus;
}

static VAStatus gen6_mfc_run(VADriverContextP ctx, 
                             struct encode_state *encode_state,
                             struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;

    intel_batchbuffer_flush(batch);		//run the pipeline

    return VA_STATUS_SUCCESS;
}

static VAStatus
gen6_mfc_stop(VADriverContextP ctx, 
              struct encode_state *encode_state,
              struct intel_encoder_context *encoder_context,
              int *encoded_bits_size)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    unsigned int *status_mem;
    unsigned int buffer_size_bits = 0;
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int height_in_mbs = (mfc_context->surface_state.height + 15) / 16;
    int i;

    dri_bo_map(mfc_context->macroblock_status_buffer.bo, 1);
    status_mem = (unsigned int *)mfc_context->macroblock_status_buffer.bo->virtual;
    //Detecting encoder buffer size and bit rate control result
    for(i = 0; i < width_in_mbs * height_in_mbs; i++) {
        unsigned short current_mb = status_mem[1] >> 16;
        buffer_size_bits += current_mb;
        status_mem += 4;
    }    
    dri_bo_unmap(mfc_context->macroblock_status_buffer.bo);

    *encoded_bits_size = buffer_size_bits;

    return VA_STATUS_SUCCESS;
}

#if __SOFTWARE__

static int
gen6_mfc_avc_pak_object_intra(VADriverContextP ctx, int x, int y, int end_mb, int qp,unsigned int *msg,
                              struct intel_encoder_context *encoder_context,
                              unsigned char target_mb_size, unsigned char max_mb_size,
                              struct intel_batchbuffer *batch)
{
    int len_in_dwords = 11;

    if (batch == NULL)
        batch = encoder_context->base.batch;

    BEGIN_BCS_BATCH(batch, len_in_dwords);

    OUT_BCS_BATCH(batch, MFC_AVC_PAK_OBJECT | (len_in_dwords - 2));
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 
                  (0 << 24) |		/* PackedMvNum, Debug*/
                  (0 << 20) | 		/* No motion vector */
                  (1 << 19) |		/* CbpDcY */
                  (1 << 18) |		/* CbpDcU */
                  (1 << 17) |		/* CbpDcV */
                  (msg[0] & 0xFFFF) );

    OUT_BCS_BATCH(batch, (0xFFFF << 16) | (y << 8) | x);		/* Code Block Pattern for Y*/
    OUT_BCS_BATCH(batch, 0x000F000F);							/* Code Block Pattern */		
    OUT_BCS_BATCH(batch, (0 << 27) | (end_mb << 26) | qp);	/* Last MB */

    /*Stuff for Intra MB*/
    OUT_BCS_BATCH(batch, msg[1]);			/* We using Intra16x16 no 4x4 predmode*/	
    OUT_BCS_BATCH(batch, msg[2]);	
    OUT_BCS_BATCH(batch, msg[3]&0xFC);		
    
    /*MaxSizeInWord and TargetSzieInWord*/
    OUT_BCS_BATCH(batch, (max_mb_size << 24) |
                  (target_mb_size << 16) );

    ADVANCE_BCS_BATCH(batch);

    return len_in_dwords;
}

static int
gen6_mfc_avc_pak_object_inter(VADriverContextP ctx, int x, int y, int end_mb, int qp, unsigned int offset,
                              struct intel_encoder_context *encoder_context,
                              unsigned char target_mb_size,unsigned char max_mb_size, int slice_type,
                              struct intel_batchbuffer *batch)
{
    int len_in_dwords = 11;

    if (batch == NULL)
        batch = encoder_context->base.batch;

    BEGIN_BCS_BATCH(batch, len_in_dwords);

    OUT_BCS_BATCH(batch, MFC_AVC_PAK_OBJECT | (len_in_dwords - 2));

    OUT_BCS_BATCH(batch, 32);         /* 32 MV*/
    OUT_BCS_BATCH(batch, offset);

    OUT_BCS_BATCH(batch, 
                  (1 << 24) |     /* PackedMvNum, Debug*/
                  (4 << 20) |     /* 8 MV, SNB don't use it*/
                  (1 << 19) |     /* CbpDcY */
                  (1 << 18) |     /* CbpDcU */
                  (1 << 17) |     /* CbpDcV */
                  (0 << 15) |     /* Transform8x8Flag = 0*/
                  (0 << 14) |     /* Frame based*/
                  (0 << 13) |     /* Inter MB */
                  (1 << 8)  |     /* MbType = P_L0_16x16 */   
                  (0 << 7)  |     /* MBZ for frame */
                  (0 << 6)  |     /* MBZ */
                  (2 << 4)  |     /* MBZ for inter*/
                  (0 << 3)  |     /* MBZ */
                  (0 << 2)  |     /* SkipMbFlag */
                  (0 << 0));      /* InterMbMode */

    OUT_BCS_BATCH(batch, (0xFFFF<<16) | (y << 8) | x);        /* Code Block Pattern for Y*/
    OUT_BCS_BATCH(batch, 0x000F000F);                         /* Code Block Pattern */  
#if 0 
    if ( slice_type == SLICE_TYPE_B) {
        OUT_BCS_BATCH(batch, (0xF<<28) | (end_mb << 26) | qp);	/* Last MB */
    } else {
        OUT_BCS_BATCH(batch, (end_mb << 26) | qp);	/* Last MB */
    }
#else
    OUT_BCS_BATCH(batch, (end_mb << 26) | qp);	/* Last MB */
#endif


    /*Stuff for Inter MB*/
    OUT_BCS_BATCH(batch, 0x0);        
    OUT_BCS_BATCH(batch, 0x0);    
    OUT_BCS_BATCH(batch, 0x0);        

    /*MaxSizeInWord and TargetSzieInWord*/
    OUT_BCS_BATCH(batch, (max_mb_size << 24) |
                  (target_mb_size << 16) );

    ADVANCE_BCS_BATCH(batch);

    return len_in_dwords;
}

static void 
gen6_mfc_avc_pipeline_slice_programing(VADriverContextP ctx,
                                       struct encode_state *encode_state,
                                       struct intel_encoder_context *encoder_context,
                                       int slice_index,
                                       struct intel_batchbuffer *slice_batch)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;
    VAEncSliceParameterBufferH264 *pSliceParameter = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[slice_index]->buffer; 
    VAEncH264DecRefPicMarkingBuffer *pDecRefPicMarking = NULL;
    unsigned int *msg = NULL, offset = 0;
    int is_intra = pSliceParameter->slice_type == SLICE_TYPE_I;
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int height_in_mbs = (mfc_context->surface_state.height + 15) / 16;
    int last_slice = (pSliceParameter->starting_macroblock_address + pSliceParameter->number_of_mbs) == (width_in_mbs * height_in_mbs);
    int i,x,y;
    int qp = pPicParameter->pic_init_qp + pSliceParameter->slice_qp_delta;
    int rate_control_mode = pSequenceParameter->rate_control_method;   
    unsigned char *slice_header = NULL;
    int slice_header_length_in_bits = 0;
    unsigned int tail_data[] = { 0x0, 0x0 };

    gen6_mfc_avc_slice_state(ctx, pSliceParameter,
                             encode_state, encoder_context,
                             (rate_control_mode == 0), qp, slice_batch);

    if ( slice_index == 0) 
        gen6_mfc_avc_pipeline_header_programing(ctx, encode_state, encoder_context, slice_batch);

    if (encode_state->dec_ref_pic_marking)
        pDecRefPicMarking = (VAEncH264DecRefPicMarkingBuffer *)encode_state->dec_ref_pic_marking->buffer;
    slice_header_length_in_bits = build_avc_slice_header(pSequenceParameter, pPicParameter, pSliceParameter, pDecRefPicMarking, &slice_header);

    // slice hander
    mfc_context->insert_object(ctx, encoder_context,
                               (unsigned int *)slice_header, ALIGN(slice_header_length_in_bits, 32) >> 5, slice_header_length_in_bits & 0x1f,
                               5,  /* first 5 bytes are start code + nal unit type */
                               1, 0, 1, slice_batch);

    if ( rate_control_mode == 0) {
        qp = mfc_context->bit_rate_control_context[1-is_intra].QpPrimeY;
    }

    if (is_intra) {
        dri_bo_map(vme_context->vme_output.bo , 1);
        msg = (unsigned int *)vme_context->vme_output.bo->virtual;
    }
   
    for (i = pSliceParameter->starting_macroblock_address; 
         i < pSliceParameter->starting_macroblock_address + pSliceParameter->number_of_mbs; i++) {
        int last_mb = (i == (pSliceParameter->starting_macroblock_address + pSliceParameter->number_of_mbs - 1) );
        x = i % width_in_mbs;
        y = i / width_in_mbs;

        if (is_intra) {
            assert(msg);
            gen6_mfc_avc_pak_object_intra(ctx, x, y, last_mb, qp, msg, encoder_context, 0, 0, slice_batch);
            msg += 4;
        } else {
            gen6_mfc_avc_pak_object_inter(ctx, x, y, last_mb, qp, offset, encoder_context, 0, 0, pSliceParameter->slice_type, slice_batch);
            offset += 64;
        }
    }
   
    if (is_intra)
        dri_bo_unmap(vme_context->vme_output.bo);
    if ( last_slice ) {    
        mfc_context->insert_object(ctx, encoder_context,
                                   tail_data, 2, 8,
                                   2, 1, 1, 0, slice_batch);
    } else {
        mfc_context->insert_object(ctx, encoder_context,
                                   tail_data, 1, 8,
                                   1, 1, 1, 0, slice_batch);
    }

    free(slice_header);

}

static dri_bo *
gen6_mfc_avc_software_batchbuffer(VADriverContextP ctx,
                                  struct encode_state *encode_state,
                                  struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct intel_batchbuffer *batch = intel_batchbuffer_new(&i965->intel, I915_EXEC_BSD);
    dri_bo *batch_bo = batch->buffer;
    int i, used;

    for (i = 0; i < encode_state->num_slice_params_ext; i++) {
        gen6_mfc_avc_pipeline_slice_programing(ctx, encode_state, encoder_context, i, batch);
    }

    used = intel_batchbuffer_used_size(batch);

    if ((used & 4) == 0) {
        BEGIN_BCS_BATCH(batch, 2);
        OUT_BCS_BATCH(batch, 0);
        OUT_BCS_BATCH(batch, MI_BATCH_BUFFER_END);
    }

    dri_bo_reference(batch_bo);
    intel_batchbuffer_free(batch);

    return batch_bo;
}

#else

static void
gen6_mfc_batchbuffer_surfaces_input(VADriverContextP ctx,
                                    struct encode_state *encode_state,
                                    struct intel_encoder_context *encoder_context)

{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

    assert(vme_context->vme_output.bo);
    mfc_context->buffer_suface_setup(ctx,
                                     &mfc_context->gpe_context,
                                     &vme_context->vme_output,
                                     BINDING_TABLE_OFFSET(BIND_IDX_VME_OUTPUT),
                                     SURFACE_STATE_OFFSET(BIND_IDX_VME_OUTPUT));
    assert(mfc_context->aux_batchbuffer_surface.bo);
    mfc_context->buffer_suface_setup(ctx,
                                     &mfc_context->gpe_context,
                                     &mfc_context->aux_batchbuffer_surface,
                                     BINDING_TABLE_OFFSET(BIND_IDX_MFC_SLICE_HEADER),
                                     SURFACE_STATE_OFFSET(BIND_IDX_MFC_SLICE_HEADER));
}

static void
gen6_mfc_batchbuffer_surfaces_output(VADriverContextP ctx,
                                     struct encode_state *encode_state,
                                     struct intel_encoder_context *encoder_context)

{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    int width_in_mbs = pSequenceParameter->picture_width_in_mbs;
    int height_in_mbs = pSequenceParameter->picture_height_in_mbs;
    mfc_context->mfc_batchbuffer_surface.num_blocks = width_in_mbs * height_in_mbs + encode_state->num_slice_params_ext * 2 + 1;
    mfc_context->mfc_batchbuffer_surface.size_block = 48; /* 3 OWORDs */
    mfc_context->mfc_batchbuffer_surface.pitch = 16;
    mfc_context->mfc_batchbuffer_surface.bo = dri_bo_alloc(i965->intel.bufmgr, 
                                                           "MFC batchbuffer",
                                                           mfc_context->mfc_batchbuffer_surface.num_blocks * mfc_context->mfc_batchbuffer_surface.size_block,
                                                           0x1000);
    mfc_context->buffer_suface_setup(ctx,
                                     &mfc_context->gpe_context,
                                     &mfc_context->mfc_batchbuffer_surface,
                                     BINDING_TABLE_OFFSET(BIND_IDX_MFC_BATCHBUFFER),
                                     SURFACE_STATE_OFFSET(BIND_IDX_MFC_BATCHBUFFER));
}

static void
gen6_mfc_batchbuffer_surfaces_setup(VADriverContextP ctx, 
                                    struct encode_state *encode_state,
                                    struct intel_encoder_context *encoder_context)
{
    gen6_mfc_batchbuffer_surfaces_input(ctx, encode_state, encoder_context);
    gen6_mfc_batchbuffer_surfaces_output(ctx, encode_state, encoder_context);
}

static void
gen6_mfc_batchbuffer_idrt_setup(VADriverContextP ctx, 
                                struct encode_state *encode_state,
                                struct intel_encoder_context *encoder_context)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    struct gen6_interface_descriptor_data *desc;   
    int i;
    dri_bo *bo;

    bo = mfc_context->gpe_context.idrt.bo;
    dri_bo_map(bo, 1);
    assert(bo->virtual);
    desc = bo->virtual;

    for (i = 0; i < mfc_context->gpe_context.num_kernels; i++) {
        struct i965_kernel *kernel;

        kernel = &mfc_context->gpe_context.kernels[i];
        assert(sizeof(*desc) == 32);

        /*Setup the descritor table*/
        memset(desc, 0, sizeof(*desc));
        desc->desc0.kernel_start_pointer = (kernel->bo->offset >> 6);
        desc->desc2.sampler_count = 0;
        desc->desc2.sampler_state_pointer = 0;
        desc->desc3.binding_table_entry_count = 2;
        desc->desc3.binding_table_pointer = (BINDING_TABLE_OFFSET(0) >> 5);
        desc->desc4.constant_urb_entry_read_offset = 0;
        desc->desc4.constant_urb_entry_read_length = 4;
 		
        /*kernel start*/
        dri_bo_emit_reloc(bo,	
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          0,
                          i * sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc0),
                          kernel->bo);
        desc++;
    }

    dri_bo_unmap(bo);
}

static void
gen6_mfc_batchbuffer_constant_setup(VADriverContextP ctx, 
                                    struct encode_state *encode_state,
                                    struct intel_encoder_context *encoder_context)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    
    (void)mfc_context;
}

static void
gen6_mfc_batchbuffer_emit_object_command(struct intel_batchbuffer *batch,
                                         int index,
                                         int head_offset,
                                         int batchbuffer_offset,
                                         int head_size,
                                         int tail_size,
                                         int number_mb_cmds,
                                         int first_object,
                                         int last_object,
                                         int last_slice,
                                         int mb_x,
                                         int mb_y,
                                         int width_in_mbs,
                                         int qp)
{
    BEGIN_BATCH(batch, 12);
    
    OUT_BATCH(batch, CMD_MEDIA_OBJECT | (12 - 2));
    OUT_BATCH(batch, index);
    OUT_BATCH(batch, 0);
    OUT_BATCH(batch, 0);
    OUT_BATCH(batch, 0);
    OUT_BATCH(batch, 0);
   
    /*inline data */
    OUT_BATCH(batch, head_offset);
    OUT_BATCH(batch, batchbuffer_offset);
    OUT_BATCH(batch, 
              head_size << 16 |
              tail_size);
    OUT_BATCH(batch,
              number_mb_cmds << 16 |
              first_object << 2 |
              last_object << 1 |
              last_slice);
    OUT_BATCH(batch,
              mb_y << 8 |
              mb_x);
    OUT_BATCH(batch,
              qp << 16 |
              width_in_mbs);

    ADVANCE_BATCH(batch);
}

static void
gen6_mfc_avc_batchbuffer_slice_command(VADriverContextP ctx,
                                       struct intel_encoder_context *encoder_context,
                                       VAEncSliceParameterBufferH264 *slice_param,
                                       int head_offset,
                                       unsigned short head_size,
                                       unsigned short tail_size,
                                       int batchbuffer_offset,
                                       int qp,
                                       int last_slice)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int total_mbs = slice_param->number_of_mbs;
    int number_mb_cmds = 512;
    int starting_mb = 0;
    int last_object = 0;
    int first_object = 1;
    int i;
    int mb_x, mb_y;
    int index = (slice_param->slice_type == SLICE_TYPE_I) ? MFC_BATCHBUFFER_AVC_INTRA : MFC_BATCHBUFFER_AVC_INTER;

    for (i = 0; i < total_mbs / number_mb_cmds; i++) {
        last_object = (total_mbs - starting_mb) == number_mb_cmds;
        mb_x = (slice_param->starting_macroblock_address + starting_mb) % width_in_mbs;
        mb_y = (slice_param->starting_macroblock_address + starting_mb) / width_in_mbs;
        assert(mb_x <= 255 && mb_y <= 255);

        starting_mb += number_mb_cmds;

        gen6_mfc_batchbuffer_emit_object_command(batch,
                                                 index,
                                                 head_offset,
                                                 batchbuffer_offset,
                                                 head_size,
                                                 tail_size,
                                                 number_mb_cmds,
                                                 first_object,
                                                 last_object,
                                                 last_slice,
                                                 mb_x,
                                                 mb_y,
                                                 width_in_mbs,
                                                 qp);

        if (first_object) {
            head_offset += head_size;
            batchbuffer_offset += head_size;
        }

        if (last_object) {
            head_offset += tail_size;
            batchbuffer_offset += tail_size;
        }

        batchbuffer_offset += number_mb_cmds * 3;

        first_object = 0;
    }

    if (!last_object) {
        last_object = 1;
        number_mb_cmds = total_mbs % number_mb_cmds;
        mb_x = (slice_param->starting_macroblock_address + starting_mb) % width_in_mbs;
        mb_y = (slice_param->starting_macroblock_address + starting_mb) / width_in_mbs;
        assert(mb_x <= 255 && mb_y <= 255);
        starting_mb += number_mb_cmds;

        gen6_mfc_batchbuffer_emit_object_command(batch,
                                                 index,
                                                 head_offset,
                                                 batchbuffer_offset,
                                                 head_size,
                                                 tail_size,
                                                 number_mb_cmds,
                                                 first_object,
                                                 last_object,
                                                 last_slice,
                                                 mb_x,
                                                 mb_y,
                                                 width_in_mbs,
                                                 qp);
    }
}
                          
/*
 * return size in Owords (16bytes)
 */         
static int
gen6_mfc_avc_batchbuffer_slice(VADriverContextP ctx,
                               struct encode_state *encode_state,
                               struct intel_encoder_context *encoder_context,
                               int slice_index,
                               int batchbuffer_offset)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    struct intel_batchbuffer *slice_batch = mfc_context->aux_batchbuffer;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;
    VAEncSliceParameterBufferH264 *pSliceParameter = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[slice_index]->buffer; 
    VAEncH264DecRefPicMarkingBuffer *pDecRefPicMarking = NULL;
    int is_intra = pSliceParameter->slice_type == SLICE_TYPE_I;
    int width_in_mbs = (mfc_context->surface_state.width + 15) / 16;
    int height_in_mbs = (mfc_context->surface_state.height + 15) / 16;
    int last_slice = (pSliceParameter->starting_macroblock_address + pSliceParameter->number_of_mbs) == (width_in_mbs * height_in_mbs);
    int qp = pPicParameter->pic_init_qp + pSliceParameter->slice_qp_delta;
    int rate_control_mode = pSequenceParameter->rate_control_method;   
    unsigned char *slice_header = NULL;
    int slice_header_length_in_bits = 0;
    unsigned int tail_data[] = { 0x0, 0x0 };
    long head_offset;
    int old_used = intel_batchbuffer_used_size(slice_batch), used;
    unsigned short head_size, tail_size;

    head_offset = old_used / 16;
    gen6_mfc_avc_slice_state(ctx,
                             pSliceParameter,
                             encode_state,
                             encoder_context,
                             (rate_control_mode == 0),
                             qp,
                             slice_batch);

    if (slice_index == 0)
        gen6_mfc_avc_pipeline_header_programing(ctx, encode_state, encoder_context, slice_batch);

    if (encode_state->dec_ref_pic_marking)
        pDecRefPicMarking = (VAEncH264DecRefPicMarkingBuffer *)encode_state->dec_ref_pic_marking->buffer;

    slice_header_length_in_bits = build_avc_slice_header(pSequenceParameter, pPicParameter, pSliceParameter, pDecRefPicMarking, &slice_header);

    // slice hander
    mfc_context->insert_object(ctx,
                               encoder_context,
                               (unsigned int *)slice_header,
                               ALIGN(slice_header_length_in_bits, 32) >> 5,
                               slice_header_length_in_bits & 0x1f,
                               5,  /* first 5 bytes are start code + nal unit type */
                               1,
                               0,
                               1,
                               slice_batch);
    free(slice_header);

    intel_batchbuffer_align(slice_batch, 16); /* aligned by an Oword */
    used = intel_batchbuffer_used_size(slice_batch);
    head_size = (used - old_used) / 16;
    old_used = used;

    if (rate_control_mode == 0) {
        qp = mfc_context->bit_rate_control_context[1 - is_intra].QpPrimeY;
    }

    /* tail */
    if (last_slice) {    
        mfc_context->insert_object(ctx,
                                   encoder_context,
                                   tail_data,
                                   2,
                                   8,
                                   2,
                                   1,
                                   1,
                                   0,
                                   slice_batch);
    } else {
        mfc_context->insert_object(ctx,
                                   encoder_context,
                                   tail_data,
                                   1,
                                   8,
                                   1,
                                   1,
                                   1,
                                   0,
                                   slice_batch);
    }

    intel_batchbuffer_align(slice_batch, 16); /* aligned by an Oword */
    used = intel_batchbuffer_used_size(slice_batch);
    tail_size = (used - old_used) / 16;

   
    gen6_mfc_avc_batchbuffer_slice_command(ctx,
                                           encoder_context,
                                           pSliceParameter,
                                           head_offset,
                                           head_size,
                                           tail_size,
                                           batchbuffer_offset,
                                           qp,
                                           last_slice);

    return head_size + tail_size + pSliceParameter->number_of_mbs * 3;
}

static void
gen6_mfc_avc_batchbuffer_pipeline(VADriverContextP ctx,
                                  struct encode_state *encode_state,
                                  struct intel_encoder_context *encoder_context)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    int i, size, offset = 0;
    intel_batchbuffer_start_atomic(batch, 0x4000); 
    gen6_gpe_pipeline_setup(ctx, &mfc_context->gpe_context, batch);

    for ( i = 0; i < encode_state->num_slice_params_ext; i++) {
        size = gen6_mfc_avc_batchbuffer_slice(ctx, encode_state, encoder_context, i, offset);
        offset += size;
    }

    intel_batchbuffer_end_atomic(batch);
    intel_batchbuffer_flush(batch);
}

static void
gen6_mfc_build_avc_batchbuffer(VADriverContextP ctx, 
                               struct encode_state *encode_state,
                               struct intel_encoder_context *encoder_context)
{
    gen6_mfc_batchbuffer_surfaces_setup(ctx, encode_state, encoder_context);
    gen6_mfc_batchbuffer_idrt_setup(ctx, encode_state, encoder_context);
    gen6_mfc_batchbuffer_constant_setup(ctx, encode_state, encoder_context);
    gen6_mfc_avc_batchbuffer_pipeline(ctx, encode_state, encoder_context);
}

static dri_bo *
gen6_mfc_avc_hardware_batchbuffer(VADriverContextP ctx,
                                  struct encode_state *encode_state,
                                  struct intel_encoder_context *encoder_context)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;

    gen6_mfc_build_avc_batchbuffer(ctx, encode_state, encoder_context);
    dri_bo_reference(mfc_context->mfc_batchbuffer_surface.bo);

    return mfc_context->mfc_batchbuffer_surface.bo;
}

#endif

static void
gen6_mfc_avc_pipeline_programing(VADriverContextP ctx,
                                 struct encode_state *encode_state,
                                 struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;

#if __SOFTWARE__
    dri_bo *slice_batch_bo = gen6_mfc_avc_software_batchbuffer(ctx, encode_state, encoder_context);
#else
    dri_bo *slice_batch_bo = gen6_mfc_avc_hardware_batchbuffer(ctx, encode_state, encoder_context);
#endif

    // begin programing
    intel_batchbuffer_start_atomic_bcs(batch, 0x4000); 
    intel_batchbuffer_emit_mi_flush(batch);
    
    // picture level programing
    gen6_mfc_avc_pipeline_picture_programing(ctx, encode_state, encoder_context);

    BEGIN_BCS_BATCH(batch, 2);
    OUT_BCS_BATCH(batch, MI_BATCH_BUFFER_START | (1 << 8));
    OUT_BCS_RELOC(batch,
                  slice_batch_bo,
                  I915_GEM_DOMAIN_COMMAND, 0, 
                  0);
    ADVANCE_BCS_BATCH(batch);

    // end programing
    intel_batchbuffer_end_atomic(batch);

    dri_bo_unreference(slice_batch_bo);
}

static VAStatus
gen6_mfc_avc_encode_picture(VADriverContextP ctx, 
                            struct encode_state *encode_state,
                            struct intel_encoder_context *encoder_context)
{
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    int rate_control_mode = pSequenceParameter->rate_control_method;  
    int MAX_CBR_INTERATE = 4;
    int current_frame_bits_size;
    int i;
 
    for(i = 0; i < MAX_CBR_INTERATE; i++) {
        gen6_mfc_init(ctx, encoder_context);
        gen6_mfc_avc_prepare(ctx, encode_state, encoder_context);
        /*Programing bcs pipeline*/
        gen6_mfc_avc_pipeline_programing(ctx, encode_state, encoder_context);	//filling the pipeline
        gen6_mfc_run(ctx, encode_state, encoder_context);
        gen6_mfc_stop(ctx, encode_state, encoder_context, &current_frame_bits_size);
        if ( rate_control_mode == 0) {
            //gen6_mfc_hrd_context_check(encode_state, mfc_context);
            if ( gen6_mfc_bit_rate_control_context_update( encode_state, mfc_context, current_frame_bits_size) ) {
                gen6_mfc_hrd_context_update(encode_state, mfc_context);
                break;
            }
        } else {
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus
gen6_mfc_pipeline(VADriverContextP ctx,
                  VAProfile profile,
                  struct encode_state *encode_state,
                  struct intel_encoder_context *encoder_context)
{
    VAStatus vaStatus;

    switch (profile) {
    case VAProfileH264Baseline:
        vaStatus = gen6_mfc_avc_encode_picture(ctx, encode_state, encoder_context);
        break;

        /* FIXME: add for other profile */
    default:
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    }

    return vaStatus;
}

void
gen6_mfc_context_destroy(void *context)
{
    struct gen6_mfc_context *mfc_context = context;
    int i;

    dri_bo_unreference(mfc_context->post_deblocking_output.bo);
    mfc_context->post_deblocking_output.bo = NULL;

    dri_bo_unreference(mfc_context->pre_deblocking_output.bo);
    mfc_context->pre_deblocking_output.bo = NULL;

    dri_bo_unreference(mfc_context->uncompressed_picture_source.bo);
    mfc_context->uncompressed_picture_source.bo = NULL;

    dri_bo_unreference(mfc_context->mfc_indirect_pak_bse_object.bo); 
    mfc_context->mfc_indirect_pak_bse_object.bo = NULL;

    for (i = 0; i < NUM_MFC_DMV_BUFFERS; i++){
        dri_bo_unreference(mfc_context->direct_mv_buffers[i].bo);
        mfc_context->direct_mv_buffers[i].bo = NULL;
    }

    dri_bo_unreference(mfc_context->intra_row_store_scratch_buffer.bo);
    mfc_context->intra_row_store_scratch_buffer.bo = NULL;

    dri_bo_unreference(mfc_context->macroblock_status_buffer.bo);
    mfc_context->macroblock_status_buffer.bo = NULL;

    dri_bo_unreference(mfc_context->deblocking_filter_row_store_scratch_buffer.bo);
    mfc_context->deblocking_filter_row_store_scratch_buffer.bo = NULL;

    dri_bo_unreference(mfc_context->bsd_mpc_row_store_scratch_buffer.bo);
    mfc_context->bsd_mpc_row_store_scratch_buffer.bo = NULL;


    for (i = 0; i < MAX_MFC_REFERENCE_SURFACES; i++){
        dri_bo_unreference(mfc_context->reference_surfaces[i].bo);
        mfc_context->reference_surfaces[i].bo = NULL;  
    }

    i965_gpe_context_destroy(&mfc_context->gpe_context);

    dri_bo_unreference(mfc_context->mfc_batchbuffer_surface.bo);
    mfc_context->mfc_batchbuffer_surface.bo = NULL;

    dri_bo_unreference(mfc_context->aux_batchbuffer_surface.bo);
    mfc_context->aux_batchbuffer_surface.bo = NULL;

    if (mfc_context->aux_batchbuffer)
        intel_batchbuffer_free(mfc_context->aux_batchbuffer);

    mfc_context->aux_batchbuffer = NULL;

    free(mfc_context);
}

Bool gen6_mfc_context_init(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct gen6_mfc_context *mfc_context = calloc(1, sizeof(struct gen6_mfc_context));

    mfc_context->gpe_context.surface_state_binding_table.length = (SURFACE_STATE_PADDED_SIZE + sizeof(unsigned int)) * MAX_MEDIA_SURFACES_GEN6;

    mfc_context->gpe_context.idrt.max_entries = MAX_GPE_KERNELS;
    mfc_context->gpe_context.idrt.entry_size = sizeof(struct gen6_interface_descriptor_data);

    mfc_context->gpe_context.curbe.length = 32 * 4;

    mfc_context->gpe_context.vfe_state.max_num_threads = 60 - 1;
    mfc_context->gpe_context.vfe_state.num_urb_entries = 16;
    mfc_context->gpe_context.vfe_state.gpgpu_mode = 0;
    mfc_context->gpe_context.vfe_state.urb_entry_size = 59 - 1;
    mfc_context->gpe_context.vfe_state.curbe_allocation_size = 37 - 1;

    i965_gpe_load_kernels(ctx,
                          &mfc_context->gpe_context,
                          gen6_mfc_kernels,
                          NUM_MFC_KERNEL);

    mfc_context->pipe_mode_select = gen6_mfc_pipe_mode_select;
    mfc_context->set_surface_state = gen6_mfc_surface_state;
    mfc_context->ind_obj_base_addr_state = gen6_mfc_ind_obj_base_addr_state;
    mfc_context->avc_img_state = gen6_mfc_avc_img_state;
    mfc_context->avc_qm_state = gen6_mfc_avc_qm_state;
    mfc_context->avc_fqm_state = gen6_mfc_avc_fqm_state;
    mfc_context->insert_object = gen6_mfc_avc_insert_object;
    mfc_context->buffer_suface_setup = i965_gpe_buffer_suface_setup;

    encoder_context->mfc_context = mfc_context;
    encoder_context->mfc_context_destroy = gen6_mfc_context_destroy;
    encoder_context->mfc_pipeline = gen6_mfc_pipeline;

    return True;
}
