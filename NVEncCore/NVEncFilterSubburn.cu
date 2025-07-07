﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#include <map>
#include <array>
#include <algorithm>
#include "convert_csp.h"
#include "NVEncFilterSubburn.h"
#include "NVEncParam.h"
#pragma warning (push)
#pragma warning (disable: 4819)
#include "cuda_runtime.h"
#include "device_launch_parameters.h"
#pragma warning (pop)
#include "rgy_cuda_util.h"

#if ENABLE_AVSW_READER && ENABLE_LIBASS_SUBBURN

static __device__ float lerpf(float a, float b, float c) {
    return a + (b - a) * c;
}

template<typename TypePixel, int bit_depth>
__inline__ __device__
TypePixel blend(TypePixel pix, uint8_t alpha, uint8_t val, float transparency_offset, float pix_offset, float contrast) {
    //alpha値は 0が透明, 255が不透明
    float subval = val * (1.0f / (float)(1 << 8));
    subval = contrast * (subval - 0.5f) + 0.5f + pix_offset;
    float ret = lerpf((float)pix, subval * (float)(1<<bit_depth), alpha * (1.0f / 255.0f) * (1.0f - transparency_offset));
    return (TypePixel)clamp(ret, 0.0f, (1<<bit_depth)-0.5f);
}

template<typename TypePixel2, int bit_depth>
__inline__ __device__
void blend(void *pix, const void *alpha, const void *val, float transparency_offset, float pix_offset, float contrast) {
    uchar2 a = *(uchar2 *)alpha;
    uchar2 v = *(uchar2 *)val;
    TypePixel2 p = *(TypePixel2 *)pix;
    p.x = blend<decltype(TypePixel2::x), bit_depth>(p.x, a.x, v.x, transparency_offset, pix_offset, contrast);
    p.y = blend<decltype(TypePixel2::x), bit_depth>(p.y, a.y, v.y, transparency_offset, pix_offset, contrast);
    *(TypePixel2 *)pix = p;
}

template<typename TypePixel, int bit_depth, bool yuv420>
__global__ void kernel_subburn(
    uint8_t *__restrict__ pPlaneY,
    uint8_t *__restrict__ pPlaneU,
    uint8_t *__restrict__ pPlaneV,
    const int pitchFrameY,
    const int pitchFrameU,
    const int pitchFrameV,
    const uint8_t *__restrict__ pSubY, const uint8_t *__restrict__ pSubU, const uint8_t *__restrict__ pSubV, const uint8_t *__restrict__ pSubA,
    const int pitchSub,
    const int width, const int height, bool interlaced, float transparency_offset, float brightness, float contrast) {
    //縦横2x2pixelを1スレッドで処理する
    const int ix = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    const int iy = (blockIdx.y * blockDim.y + threadIdx.y) * 2;

    struct __align__(sizeof(TypePixel) * 2) TypePixel2 {
        TypePixel x, y;
    };
    if (ix < width && iy < height) {
        pPlaneY += iy * pitchFrameY + ix * sizeof(TypePixel);
        pSubY   += iy * pitchSub + ix;
        pSubU   += iy * pitchSub + ix;
        pSubV   += iy * pitchSub + ix;
        pSubA   += iy * pitchSub + ix;

        blend<TypePixel2, bit_depth>(pPlaneY,               pSubA,            pSubY,            transparency_offset, brightness, contrast);
        blend<TypePixel2, bit_depth>(pPlaneY + pitchFrameY, pSubA + pitchSub, pSubY + pitchSub, transparency_offset, brightness, contrast);

        if (yuv420) {
            pPlaneU += (iy>>1) * pitchFrameU + (ix>>1) * sizeof(TypePixel);
            pPlaneV += (iy>>1) * pitchFrameV + (ix>>1) * sizeof(TypePixel);
            uint8_t subU, subV, subA;
            if (interlaced) {
                if (((iy>>1) & 1) == 0) {
                    const int offset_y1 = (iy+2<height) ? pitchSub*2 : 0;
                    subU = (pSubU[0] * 3 + pSubU[offset_y1] + 2) >> 2;
                    subV = (pSubV[0] * 3 + pSubV[offset_y1] + 2) >> 2;
                    subA = (pSubA[0] * 3 + pSubA[offset_y1] + 2) >> 2;
                } else {
                    subU = (pSubU[-pitchSub] + pSubU[pitchSub] * 3 + 2) >> 2;
                    subV = (pSubV[-pitchSub] + pSubV[pitchSub] * 3 + 2) >> 2;
                    subA = (pSubA[-pitchSub] + pSubA[pitchSub] * 3 + 2) >> 2;
                }
            } else {
                subU = (pSubU[0] + pSubU[pitchSub] + 1) >> 1;
                subV = (pSubV[0] + pSubV[pitchSub] + 1) >> 1;
                subA = (pSubA[0] + pSubA[pitchSub] + 1) >> 1;
            }
            *(TypePixel *)pPlaneU = blend<TypePixel, bit_depth>(*(TypePixel *)pPlaneU, subA, subU, transparency_offset, 0.0f, 1.0f);
            *(TypePixel *)pPlaneV = blend<TypePixel, bit_depth>(*(TypePixel *)pPlaneV, subA, subV, transparency_offset, 0.0f, 1.0f);
        } else {
            pPlaneU += iy * pitchFrameU + ix * sizeof(TypePixel);
            pPlaneV += iy * pitchFrameV + ix * sizeof(TypePixel);
            blend<TypePixel2, bit_depth>(pPlaneU,               pSubA,            pSubU,            transparency_offset, 0.0f, 1.0f);
            blend<TypePixel2, bit_depth>(pPlaneU + pitchFrameU, pSubA + pitchSub, pSubU + pitchSub, transparency_offset, 0.0f, 1.0f);
            blend<TypePixel2, bit_depth>(pPlaneV,               pSubA,            pSubV,            transparency_offset, 0.0f, 1.0f);
            blend<TypePixel2, bit_depth>(pPlaneV + pitchFrameV, pSubA + pitchSub, pSubV + pitchSub, transparency_offset, 0.0f, 1.0f);
        }
    }
}

template<typename TypePixel, int bit_depth>
RGY_ERR proc_frame(RGYFrameInfo *pFrame,
    const RGYFrameInfo *pSubImg,
    int pos_x, int pos_y,
    float transparency_offset, float brightness, float contrast,
    cudaStream_t stream) {
    //焼きこみフレームの範囲内に収まるようチェック
    const int burnWidth  = std::min((pos_x & ~1) + pSubImg->width,  pFrame->width)  - (pos_x & ~1);
    const int burnHeight = std::min((pos_y & ~1) + pSubImg->height, pFrame->height) - (pos_y & ~1);
    if (burnWidth <= 0 || burnHeight <= 0) {
        return RGY_ERR_NONE;
    }

    dim3 blockSize(32, 8);
    dim3 gridSize(divCeil(burnWidth, blockSize.x * 2), divCeil(burnHeight, blockSize.y * 2)); // 2x2pixel/thread
    auto planeFrameY = getPlane(pFrame, RGY_PLANE_Y);
    auto planeFrameU = getPlane(pFrame, RGY_PLANE_U);
    auto planeFrameV = getPlane(pFrame, RGY_PLANE_V);
    auto planeSubY = getPlane(pSubImg, RGY_PLANE_Y);
    auto planeSubU = getPlane(pSubImg, RGY_PLANE_U);
    auto planeSubV = getPlane(pSubImg, RGY_PLANE_V);
    auto planeSubA = getPlane(pSubImg, RGY_PLANE_A);

    const int subPosX_Y = (pos_x & ~1);
    const int subPosY_Y = (pos_y & ~1);
    const int subPosX_UV = (RGY_CSP_CHROMA_FORMAT[pFrame->csp] == RGY_CHROMAFMT_YUV420) ? (pos_x >> 1) : (pos_x & ~1);
    const int subPosY_UV = (RGY_CSP_CHROMA_FORMAT[pFrame->csp] == RGY_CHROMAFMT_YUV420) ? (pos_y >> 1) : (pos_y & ~1);
    const int frameOffsetByteY = subPosY_Y  * planeFrameY.pitch[0] + subPosX_Y  * sizeof(TypePixel);
    const int frameOffsetByteU = subPosY_UV * planeFrameU.pitch[0] + subPosX_UV * sizeof(TypePixel);
    const int frameOffsetByteV = subPosY_UV * planeFrameV.pitch[0] + subPosX_UV * sizeof(TypePixel);

    if (   planeSubY.pitch[0] != planeSubU.pitch[0]
        || planeSubY.pitch[0] != planeSubV.pitch[0]
        || planeSubY.pitch[0] != planeSubA.pitch[0]) {
        return RGY_ERR_UNSUPPORTED;
    }

    cudaError_t cudaerr = cudaSuccess;
    if (RGY_CSP_CHROMA_FORMAT[pFrame->csp] == RGY_CHROMAFMT_YUV420) {
        kernel_subburn<TypePixel, bit_depth, true> << <gridSize, blockSize, 0, stream >> > (
            planeFrameY.ptr[0] + frameOffsetByteY,
            planeFrameU.ptr[0] + frameOffsetByteU,
            planeFrameV.ptr[0] + frameOffsetByteV,
            planeFrameY.pitch[0],
            planeFrameU.pitch[0],
            planeFrameV.pitch[0],
            planeSubY.ptr[0], planeSubU.ptr[0], planeSubV.ptr[0], planeSubA.ptr[0], planeSubY.pitch[0],
            burnWidth, burnHeight, interlaced(*pFrame), transparency_offset, brightness, contrast);
    } else {
        kernel_subburn<TypePixel, bit_depth, false> << <gridSize, blockSize, 0, stream >> > (
            planeFrameY.ptr[0] + frameOffsetByteY,
            planeFrameU.ptr[0] + frameOffsetByteU,
            planeFrameV.ptr[0] + frameOffsetByteV,
            planeFrameY.pitch[0],
            planeFrameU.pitch[0],
            planeFrameV.pitch[0],
            planeSubY.ptr[0], planeSubU.ptr[0], planeSubV.ptr[0], planeSubA.ptr[0], planeSubY.pitch[0],
            burnWidth, burnHeight, interlaced(*pFrame), transparency_offset, brightness, contrast);
    }
    cudaerr = cudaGetLastError();
    if (cudaerr != cudaSuccess) {
        return err_to_rgy(cudaerr);
    }
    return RGY_ERR_NONE;
}

SubImageData NVEncFilterSubburn::textRectToImage(const ASS_Image *image, cudaStream_t stream) {
    //YUV420の関係で縦横2pixelずつ処理するので、2で割り切れている必要がある
    const int x_offset = ((image->dst_x % 2) != 0) ? 1 : 0;
    const int y_offset = ((image->dst_y % 2) != 0) ? 1 : 0;
    RGYFrameInfo img;
    img.csp = RGY_CSP_YUVA444;
    img.width  = ALIGN(image->w + x_offset, 2);
    img.height = ALIGN(image->h + y_offset, 2);
    img.mem_type = RGY_MEM_TYPE_CPU;
    img.picstruct = RGY_PICSTRUCT_FRAME;
    auto bufCPU = std::make_unique<CUFrameBuf>(img);
    auto err = bufCPU->allocHost();
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to allocate host memory for subtitle image %dx%d: %s.\n"), image->w, image->h, get_err_mes(err));
        return SubImageData(
            std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
            std::unique_ptr<CUFrameBuf>(), 0, 0);
    }

    auto planeY = getPlane(&bufCPU->frame, RGY_PLANE_Y);
    auto planeU = getPlane(&bufCPU->frame, RGY_PLANE_U);
    auto planeV = getPlane(&bufCPU->frame, RGY_PLANE_V);
    auto planeA = getPlane(&bufCPU->frame, RGY_PLANE_A);

    //とりあえずすべて0で初期化しておく
    memset(planeY.ptr[0], 0, (size_t)planeY.pitch[0] * planeY.height);

    //とりあえずすべて0で初期化しておく
    //Alpha=0で透明なので都合がよい
    memset(planeA.ptr[0], 0, (size_t)planeA.pitch[0] * planeA.height);

    for (int j = 0; j < planeU.height; j++) {
        auto ptr = planeU.ptr[0] + j * planeU.pitch[0];
        for (int i = 0; i < planeU.pitch[0] / (int)sizeof(ptr[0]); i++) {
            ptr[i] = 128;
        }
    }
    for (int j = 0; j < planeV.height; j++) {
        auto ptr = planeV.ptr[0] + j * planeV.pitch[0];
        for (int i = 0; i < planeV.pitch[0] / (int)sizeof(ptr[0]); i++) {
            ptr[i] = 128;
        }
    }

    const uint32_t subColor = image->color;
    const uint8_t subR = (uint8_t) (subColor >> 24);
    const uint8_t subG = (uint8_t)((subColor >> 16) & 0xff);
    const uint8_t subB = (uint8_t)((subColor >>  8) & 0xff);
    const uint8_t subA = (uint8_t)(255 - (subColor        & 0xff));

    const uint8_t subY = (uint8_t)clamp((( 66 * subR + 129 * subG +  25 * subB + 128) >> 8) +  16, 0, 255);
    const uint8_t subU = (uint8_t)clamp(((-38 * subR -  74 * subG + 112 * subB + 128) >> 8) + 128, 0, 255);
    const uint8_t subV = (uint8_t)clamp(((112 * subR -  94 * subG -  18 * subB + 128) >> 8) + 128, 0, 255);

    //YUVで字幕の画像データを構築
    for (int j = 0; j < image->h; j++) {
        for (int i = 0; i < image->w; i++) {
            const int src_idx = j * image->stride + i;
            const uint8_t alpha = image->bitmap[src_idx];

            #define PLANE_DST(plane, x, y) (plane.ptr[0][(y) * plane.pitch[0] + (x)])
            PLANE_DST(planeY, i + x_offset, j + y_offset) = subY;
            PLANE_DST(planeU, i + x_offset, j + y_offset) = subU;
            PLANE_DST(planeV, i + x_offset, j + y_offset) = subV;
            PLANE_DST(planeA, i + x_offset, j + y_offset) = (uint8_t)clamp(((int)subA * alpha) >> 8, 0, 255);
            #undef PLANE_DST
        }
    }
    //GPUへ転送
    auto frame = std::make_unique<CUFrameBuf>(bufCPU->frame.width, bufCPU->frame.height, bufCPU->frame.csp);
    err = frame->alloc();
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to allocate device memory for subtitle image %dx%d: %s.\n"), image->w, image->h, get_err_mes(err));
        return SubImageData(
            std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
            std::unique_ptr<CUFrameBuf>(), 0, 0);
    }
    frame->copyFrameAsync(&bufCPU->frame, stream);
    return SubImageData(std::move(frame), std::unique_ptr<CUFrameBuf>(), std::move(bufCPU), image->dst_x, image->dst_y);
}

RGY_ERR NVEncFilterSubburn::procFrameText(RGYFrameInfo *pOutputFrame, int64_t frameTimeMs, cudaStream_t stream) {
    int nDetectChange = 0;
    const auto frameImages = ass_render_frame(m_assRenderer.get(), m_assTrack.get(), frameTimeMs, &nDetectChange);

    if (!frameImages) {
        m_subImages.clear();
    } else if (nDetectChange) {
        m_subImages.clear();
        for (auto image = frameImages; image; image = image->next) {
            m_subImages.push_back(textRectToImage(image, stream));
        }
    }
    auto prm = std::dynamic_pointer_cast<NVEncFilterParamSubburn>(m_param);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (m_subImages.size()) {
        static const std::map<RGY_CSP, decltype(proc_frame<uint8_t, 8>) *> func_list ={
            { RGY_CSP_YV12,      proc_frame<uint8_t,   8> },
            { RGY_CSP_YV12_16,   proc_frame<uint16_t, 16> },
            { RGY_CSP_YUV444,    proc_frame<uint8_t,   8> },
            { RGY_CSP_YUV444_16, proc_frame<uint16_t, 16> }
        };
        if (func_list.count(pOutputFrame->csp) == 0) {
            AddMessage(RGY_LOG_ERROR, _T("unsupported csp %s.\n"), RGY_CSP_NAMES[pOutputFrame->csp]);
            return RGY_ERR_UNSUPPORTED;
        }
        for (uint32_t irect = 0; irect < m_subImages.size(); irect++) {
            const RGYFrameInfo *pSubImg = &m_subImages[irect].image->frame;
            auto sts = func_list.at(pOutputFrame->csp)(pOutputFrame, pSubImg, m_subImages[irect].x, m_subImages[irect].y,
                prm->subburn.transparency_offset, prm->subburn.brightness, prm->subburn.contrast, stream);
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("error at subburn(%s): %s.\n"),
                    RGY_CSP_NAMES[pOutputFrame->csp],
                    get_err_mes(sts));
                return sts;
            }
        }
    }
    return RGY_ERR_NONE;
}

SubImageData NVEncFilterSubburn::bitmapRectToImage(const AVSubtitleRect *rect, const RGYFrameInfo *outputFrame, const sInputCrop &crop, cudaStream_t stream) {
    //YUV420の関係で縦横2pixelずつ処理するので、2で割り切れている必要がある
    const int x_offset = ((rect->x % 2) != 0) ? 1 : 0;
    const int y_offset = ((rect->y % 2) != 0) ? 1 : 0;
    RGYFrameInfo img;
    img.csp = RGY_CSP_YUVA444;
    img.width  = ALIGN(rect->w + x_offset, 2);
    img.height = ALIGN(rect->h + y_offset, 2);
    img.mem_type = RGY_MEM_TYPE_CPU;
    img.picstruct = RGY_PICSTRUCT_FRAME;
    auto bufCPU = std::make_unique<CUFrameBuf>(img);
    auto err = bufCPU->allocHost();
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to allocate host memory for subtitle image %dx%d: %s.\n"), rect->w, rect->h, get_err_mes(err));
        return SubImageData(
            std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
            std::unique_ptr<CUFrameBuf>(), 0, 0);
    }

    auto planeY = getPlane(&bufCPU->frame, RGY_PLANE_Y);
    auto planeU = getPlane(&bufCPU->frame, RGY_PLANE_U);
    auto planeV = getPlane(&bufCPU->frame, RGY_PLANE_V);
    auto planeA = getPlane(&bufCPU->frame, RGY_PLANE_A);

    //とりあえずすべて0で初期化しておく
    memset(planeY.ptr[0], 0, (size_t)planeY.pitch[0] * planeY.height);

    //とりあえずすべて0で初期化しておく
    //Alpha=0で透明なので都合がよい
    memset(planeA.ptr[0], 0, (size_t)planeA.pitch[0] * planeA.height);

    for (int j = 0; j < planeU.height; j++) {
        auto ptr = planeU.ptr[0] + j * planeU.pitch[0];
        for (int i = 0; i < planeU.pitch[0] / (int)sizeof(ptr[0]); i++) {
            ptr[i] = 128;
        }
    }
    for (int j = 0; j < planeV.height; j++) {
        auto ptr = planeV.ptr[0] + j * planeV.pitch[0];
        for (int i = 0; i < planeV.pitch[0] / (int)sizeof(ptr[0]); i++) {
            ptr[i] = 128;
        }
    }

    //色テーブルをRGBA->YUVAに変換
    const uint32_t *pColorARGB = (uint32_t *)rect->data[1];
    alignas(32) uint32_t colorTableYUVA[256];
    memset(colorTableYUVA, 0, sizeof(colorTableYUVA));

    const uint32_t nColorTableSize = rect->nb_colors;
    assert(nColorTableSize <= _countof(colorTableYUVA));
    for (uint32_t ic = 0; ic < nColorTableSize; ic++) {
        const uint32_t subColor = pColorARGB[ic];
        const uint8_t subA = (uint8_t)(subColor >> 24);
        const uint8_t subR = (uint8_t)((subColor >> 16) & 0xff);
        const uint8_t subG = (uint8_t)((subColor >>  8) & 0xff);
        const uint8_t subB = (uint8_t)(subColor        & 0xff);

        const uint8_t subY = (uint8_t)clamp((( 66 * subR + 129 * subG +  25 * subB + 128) >> 8) +  16, 0, 255);
        const uint8_t subU = (uint8_t)clamp(((-38 * subR -  74 * subG + 112 * subB + 128) >> 8) + 128, 0, 255);
        const uint8_t subV = (uint8_t)clamp(((112 * subR -  94 * subG -  18 * subB + 128) >> 8) + 128, 0, 255);

        colorTableYUVA[ic] = ((subA << 24) | (subV << 16) | (subU << 8) | subY);
    }

    //YUVで字幕の画像データを構築
    for (int j = 0; j < rect->h; j++) {
        for (int i = 0; i < rect->w; i++) {
            const int src_idx = j * rect->linesize[0] + i;
            const int ic = rect->data[0][src_idx];

            const uint32_t subColor = colorTableYUVA[ic];
            const uint8_t subA = (uint8_t)(subColor >> 24);
            const uint8_t subV = (uint8_t)((subColor >> 16) & 0xff);
            const uint8_t subU = (uint8_t)((subColor >>  8) & 0xff);
            const uint8_t subY = (uint8_t)(subColor        & 0xff);

            #define PLANE_DST(plane, x, y) (plane.ptr[0][(y) * plane.pitch[0] + (x)])
            PLANE_DST(planeY, i + x_offset, j + y_offset) = subY;
            PLANE_DST(planeU, i + x_offset, j + y_offset) = subU;
            PLANE_DST(planeV, i + x_offset, j + y_offset) = subV;
            PLANE_DST(planeA, i + x_offset, j + y_offset) = subA;
            #undef PLANE_DST
        }
    }

    //GPUへ転送
    auto frameTemp = std::make_unique<CUFrameBuf>(bufCPU->frame.width, bufCPU->frame.height, bufCPU->frame.csp);
    err = frameTemp->alloc();
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to allocate device memory for subtitle image %dx%d: %s.\n"), rect->w, rect->h, get_err_mes(err));
        return SubImageData(
            std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
            std::unique_ptr<CUFrameBuf>(), 0, 0);
    }
    frameTemp->copyFrameAsync(&bufCPU->frame, stream);
    auto prm = std::dynamic_pointer_cast<NVEncFilterParamSubburn>(m_param);

    decltype(frameTemp) frame;
    if (prm->subburn.scale == 1.0f) {
        frame = std::move(frameTemp);
    } else {
#if 0
        RGYFrameInfo tempframe = img;
        std::vector<uint8_t> temp(imgInfoEx.frame_size);
        memcpy(temp.data(), img.ptr, temp.size());
        tempframe.ptr = temp.data();
        auto tmpY = getPlane(&tempframe, RGY_PLANE_Y);
        auto tmpU = getPlane(&tempframe, RGY_PLANE_U);
        auto tmpV = getPlane(&tempframe, RGY_PLANE_V);
        for (int j = 0; j < rect->h; j++) {
            for (int i = 0; i < rect->w; i++) {
                #define IDX(x, y) ((clamp(y,0,rect->h)+y_offset) * img.pitch + (clamp(x,0,rect->w)+x_offset))
                const int dst_idx = IDX(i,j);
                if (planeA.ptr[dst_idx] == 0) {
                    int minidx = -1;
                    uint8_t minval = 255;
                    for (int jy = -1; jy <= 1; jy++) {
                        for (int ix = -1; ix <= 1; ix++) {
                            int idx = IDX(i+ix, j+jy);
                            if (planeA.ptr[idx] != 0) {
                                auto val = tmpY.ptr[idx];
                                if (val < minval) {
                                    minidx = idx;
                                    minval = val;
                                }
                            }
                        }
                    }
                    if (minidx >= 0) {
                        planeY.ptr[dst_idx] = tmpY.ptr[minidx];
                        planeU.ptr[dst_idx] = tmpU.ptr[minidx];
                        planeV.ptr[dst_idx] = tmpV.ptr[minidx];
                    }
                }
                #undef IDX
            }
        }
#endif

        frame = std::make_unique<CUFrameBuf>(
            ALIGN((int)(bufCPU->frame.width  * prm->subburn.scale + 0.5f), 4),
            ALIGN((int)(bufCPU->frame.height * prm->subburn.scale + 0.5f), 4), bufCPU->frame.csp);
        err = frame->alloc();
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to allocate device memory for scaled subtitle image %dx%d: %s.\n"), frame->width(), frame->height(), get_err_mes(err));
            return SubImageData(
                std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
                std::unique_ptr<CUFrameBuf>(), 0, 0);
        }
        unique_ptr<NVEncFilterResize> filterResize(new NVEncFilterResize());
        shared_ptr<NVEncFilterParamResize> paramResize(new NVEncFilterParamResize());
        paramResize->frameIn = frameTemp->frame;
        paramResize->frameOut = frame->frame;
        paramResize->baseFps = prm->baseFps;
        paramResize->frameOut.mem_type = RGY_MEM_TYPE_GPU;
        paramResize->bOutOverwrite = false;
        paramResize->interp = RGY_VPP_RESIZE_BILINEAR;
        filterResize->init(paramResize, m_pLog);
        m_resize = std::move(filterResize);

        int filterOutputNum = 0;
        RGYFrameInfo *filterOutput[1] = { &frame->frame };
        m_resize->filter(&frameTemp->frame, (RGYFrameInfo **)&filterOutput, &filterOutputNum, stream);
    }
    int x_pos = ALIGN((int)(prm->subburn.scale * rect->x + 0.5f) - ((crop.e.left + crop.e.right) / 2), 2);
    int y_pos = ALIGN((int)(prm->subburn.scale * rect->y + 0.5f) - crop.e.up - crop.e.bottom, 2);
    if (m_outCodecDecodeCtx->height > 0) {
        const double y_factor = rect->y / (double)m_outCodecDecodeCtx->height;
        y_pos = ALIGN((int)(outputFrame->height * y_factor + 0.5f), 2);
        y_pos = std::min(y_pos, outputFrame->height - rect->h);
    }
    return SubImageData(std::move(frame), std::move(frameTemp), std::move(bufCPU), x_pos, y_pos);
}


RGY_ERR NVEncFilterSubburn::procFrameBitmap(RGYFrameInfo *pOutputFrame, const int64_t frameTimeMs, const sInputCrop &crop, const bool forced_subs_only, cudaStream_t stream) {
    if (m_subData) {
        if (m_subData->num_rects != m_subImages.size()) {
            for (uint32_t irect = 0; irect < m_subData->num_rects; irect++) {
                const AVSubtitleRect *rect = m_subData->rects[irect];
                if (forced_subs_only && !(rect->flags & AV_SUBTITLE_FLAG_FORCED)) {
                    AddMessage(RGY_LOG_DEBUG, _T("skipping non-forced sub at %s\n"), getTimestampString(frameTimeMs, av_make_q(1, 1000)).c_str());
                    // 空の値をいれる
                    m_subImages.push_back(SubImageData(
                        std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
                        std::unique_ptr<CUFrameBuf>(), 0, 0));
                } else if (rect->w == 0 || rect->h == 0) {
                    // 空の値をいれる
                    m_subImages.push_back(SubImageData(
                        std::unique_ptr<CUFrameBuf>(), std::unique_ptr<CUFrameBuf>(),
                        std::unique_ptr<CUFrameBuf>(), 0, 0));
                } else {
                    m_subImages.push_back(bitmapRectToImage(rect, pOutputFrame, crop, stream));
                }
            }
        }
        if ((m_subData->num_rects != m_subImages.size())) {
            AddMessage(RGY_LOG_ERROR, _T("unexpected error.\n"));
            return RGY_ERR_UNKNOWN;
        }
        auto prm = std::dynamic_pointer_cast<NVEncFilterParamSubburn>(m_param);
        if (!prm) {
            AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
            return RGY_ERR_INVALID_PARAM;
        }
        static const std::map<RGY_CSP, decltype(proc_frame<uint8_t, 8>) *> func_list = {
            { RGY_CSP_YV12,      proc_frame<uint8_t,   8> },
            { RGY_CSP_YV12_16,   proc_frame<uint16_t, 16> },
            { RGY_CSP_YUV444,    proc_frame<uint8_t,   8> },
            { RGY_CSP_YUV444_16, proc_frame<uint16_t, 16> }
        };
        if (func_list.count(pOutputFrame->csp) == 0) {
            AddMessage(RGY_LOG_ERROR, _T("unsupported csp %s.\n"), RGY_CSP_NAMES[pOutputFrame->csp]);
            return RGY_ERR_UNSUPPORTED;
        }
        for (uint32_t irect = 0; irect < m_subImages.size(); irect++) {
            if (m_subImages[irect].image) {
                const RGYFrameInfo *pSubImg = &m_subImages[irect].image->frame;
                auto sts = func_list.at(pOutputFrame->csp)(pOutputFrame, pSubImg, m_subImages[irect].x, m_subImages[irect].y,
                    prm->subburn.transparency_offset, prm->subburn.brightness, prm->subburn.contrast, stream);
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("error at subburn(%s): %s.\n"),
                        RGY_CSP_NAMES[pOutputFrame->csp],
                        get_err_mes(sts));
                    return sts;
                }
            }
        }
    }
    return RGY_ERR_NONE;
}

#endif //#if ENABLE_AVSW_READER
