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
#include "convert_csp.h"
#include "rgy_filesystem.h"
#include "NVEncFilterAfs.h"
#include "NVEncParam.h"
#include "NVEncUtil.h"
#include "afs_stg.h"
#pragma warning (push)

static void afs_get_motion_count(int *motion_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order);
static void afs_get_stripe_count(int *stripe_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order);

template<typename T>
T max3(T a, T b, T c) {
    return std::max(std::max(a, b), c);
}
template<typename T>
T absdiff(T a, T b) {
    T a_b = a - b;
    T b_a = b - a;
    return (a >= b) ? a_b : b_a;
}

afsSourceCache::afsSourceCache() :
    m_sourceArray(),
    m_nFramesInput(0) {
}

RGY_ERR afsSourceCache::alloc(const RGYFrameInfo& frameInfo) {
    for (int i = 0; i < _countof(m_sourceArray); i++) {
        m_sourceArray[i].frame = frameInfo;
        auto ret = m_sourceArray[i].alloc();
        if (ret != RGY_ERR_NONE) {
            m_sourceArray[i].clear();
            return ret;
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR afsSourceCache::add(const RGYFrameInfo *pInputFrame, cudaStream_t stream) {
    const int iframe = m_nFramesInput++;
    auto pDstFrame = get(iframe);
    auto ret = copyFrameAsync(&pDstFrame->frame, pInputFrame, stream);
    if (ret != RGY_ERR_NONE) {
        return ret;
    }
    copyFrameProp(&pDstFrame->frame, pInputFrame);
    return RGY_ERR_NONE;
}

void afsSourceCache::clear() {
    for (int i = 0; i < _countof(m_sourceArray); i++) {
        m_sourceArray[i].clear();
    }
    m_nFramesInput = 0;
}

afsSourceCache::~afsSourceCache() {
    clear();
}

afsScanCache::afsScanCache() :
    m_scanArray() {
}

void afsScanCache::clearcache(int iframe) {
    auto data = get(iframe);
    data->status = 0;
    data->frame = 0;
    data->tb_order = 0;
    data->thre_shift = 0;
    data->thre_deint = 0;
    data->thre_Ymotion = 0;
    data->thre_Cmotion = 0;
    memset(&data->clip, 0, sizeof(data->clip));
    data->ff_motion = 0;
    data->lf_motion = 0;
    data->cuevent.reset();
}

void afsScanCache::initcache(int iframe) {
    clearcache(iframe);
    auto data = get(iframe);
    data->cuevent = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
    cudaEventCreateWithFlags(data->cuevent.get(), cudaEventDisableTiming | cudaEventBlockingSync);
}

RGY_ERR afsScanCache::alloc(const RGYFrameInfo& frameInfo) {
    for (int i = 0; i < _countof(m_scanArray); i++) {
        initcache(i);
        m_scanArray[i].map.frame = frameInfo;
        m_scanArray[i].map.frame.csp = RGY_CSP_NV12;
        auto ret = m_scanArray[i].map.alloc();
        if (ret != RGY_ERR_NONE) {
            m_scanArray[i].map.clear();
            return ret;
        }
        m_scanArray[i].status = 0;
    }
    return RGY_ERR_NONE;
}

void afsScanCache::clear() {
    for (int i = 0; i < _countof(m_scanArray); i++) {
        m_scanArray[i].map.clear();
        m_scanArray[i].cuevent.reset();
        clearcache(i);
    }
}

afsScanCache::~afsScanCache() {
    clear();
}

afsStripeCache::afsStripeCache() :
    m_stripeArray() {
}

void afsStripeCache::clearcache(int iframe) {
    auto data = get(iframe);
    data->status = 0;
    data->frame = 0;
    data->count0 = 0;
    data->count1 = 0;
    data->cuevent.reset();
}

void afsStripeCache::initcache(int iframe) {
    auto data = get(iframe);
    data->cuevent = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
    cudaEventCreateWithFlags(data->cuevent.get(), cudaEventDisableTiming | cudaEventBlockingSync);
}

void afsStripeCache::expire(int iframe) {
    auto stp = get(iframe);
    if (stp->frame == iframe && stp->status > 0) {
        stp->status = 0;
    }
}

RGY_ERR afsStripeCache::alloc(const RGYFrameInfo& frameInfo) {
    for (int i = 0; i < _countof(m_stripeArray); i++) {
        initcache(i);
        m_stripeArray[i].map.frame = frameInfo;
        m_stripeArray[i].map.frame.csp = RGY_CSP_NV12;
        auto ret = m_stripeArray[i].map.alloc();
        if (ret != RGY_ERR_NONE) {
            m_stripeArray[i].map.clear();
            return ret;
        }
    }
    return RGY_ERR_NONE;
}

AFS_STRIPE_DATA *afsStripeCache::filter(int iframe, int analyze, cudaStream_t stream, RGY_ERR *pErr) {
    auto sip = get(iframe);
    if (analyze > 1) {
        auto sip_dst = getFiltered();
        if (RGY_ERR_NONE != (*pErr = map_filter(sip_dst, sip, stream))) {
            sip_dst = nullptr;
        }
        sip = sip_dst;
    }
    return sip;
}

void afsStripeCache::clear() {
    for (int i = 0; i < _countof(m_stripeArray); i++) {
        m_stripeArray[i].map.clear();
        m_stripeArray[i].buf_count_stripe.clear();
        m_stripeArray[i].cuevent.reset();
        clearcache(i);
    }
}

afsStripeCache::~afsStripeCache() {
    clear();
}

afsStreamStatus::afsStreamStatus() :
    m_initialized(false),
    m_quarter_jitter(0),
    m_additional_jitter(0),
    m_phase24(0),
    m_position24(0),
    m_prev_jitter(0),
    m_prev_rff_smooth(0),
    m_prev_status(0),
    m_set_frame(-1),
    m_pos(),
    m_fpLog() {
};

afsStreamStatus::~afsStreamStatus() {
    m_fpLog.reset();
}

void afsStreamStatus::init(uint8_t status, int drop24) {
    m_prev_status = status;
    m_prev_jitter = 0;
    m_additional_jitter = 0;
    m_prev_rff_smooth = 0;
    m_phase24 = 4;
    m_position24 = 0;
    if (drop24 ||
        (!(status & AFS_FLAG_SHIFT0) &&
        (status & AFS_FLAG_SHIFT1) &&
            (status & AFS_FLAG_SHIFT2))) {
        m_phase24 = 0;
    }
    if (status & AFS_FLAG_FORCE24) {
        m_position24++;
    } else {
        m_phase24 -= m_position24 + 1;
        m_position24 = 0;
    }
    m_initialized = true;
}

int afsStreamStatus::open_log(const tstring& log_filename) {
    FILE *fp = NULL;
    if (_tfopen_s(&fp, log_filename.c_str(), _T("w"))) {
        return 1;
    }
    m_fpLog = unique_ptr<FILE, fp_deleter>(fp, fp_deleter());
    fprintf(m_fpLog.get(), " iframe,  sts,       ,        pos,   orig_pts, q_jit, prevjit, pos24, phase24, rff_smooth\n");
    return 0;
}

void afsStreamStatus::write_log(const afsFrameTs *const frameTs) {
    if (!m_fpLog) {
        return;
    }
    fprintf(m_fpLog.get(), "%7d, 0x%2x, %s%s%s%s%s%s, %10lld, %10lld, %3d, %3d, %3d, %3d, %3d\n",
        frameTs->iframe,
        m_prev_status,
        m_prev_status & AFS_FLAG_PROGRESSIVE ? "p" : "i",
        ISRFF(m_prev_status) ? "r" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT0) ? "0" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT1) ? "1" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT2) ? "2" : "-",
        (((m_prev_status & AFS_FLAG_PROGRESSIVE) ? 0 : m_prev_status) & AFS_FLAG_SHIFT3) ? "3" : "-",
        (long long int)frameTs->pos, (long long int)frameTs->orig_pts,
        m_quarter_jitter, m_prev_jitter, m_position24, m_phase24, m_prev_rff_smooth);
    return;
}

int afsStreamStatus::set_status(int iframe, uint8_t status, int drop24, int64_t orig_pts) {
    afsFrameTs *const frameTs = &m_pos[iframe & 15];
    frameTs->iframe = iframe;
    frameTs->orig_pts = orig_pts;
    if (!m_initialized) {
        init(status, 0);
        frameTs->pos = orig_pts;
        m_set_frame = iframe;
        write_log(frameTs);
        return 0;
    }
    if (iframe > m_set_frame + 1) {
        return 1;
    }
    m_set_frame = iframe;

    int pull_drop = 0;
    int quarter_jitter = 0;
    int rff_smooth = 0;
    if (status & AFS_FLAG_PROGRESSIVE) {
        if (status & (AFS_FLAG_FORCE24 | AFS_FLAG_SMOOTHING)) {
            if (!m_prev_rff_smooth) {
                if (ISRFF(m_prev_status)) rff_smooth = -1;
                else if ((m_prev_status & AFS_FLAG_PROGRESSIVE) && ISRFF(status)) rff_smooth = 1;
            }
            quarter_jitter = rff_smooth;
        }
        pull_drop = 0;
        m_additional_jitter = 0;
        drop24 = 0;
    } else {
        if (status & AFS_FLAG_SHIFT0) {
            quarter_jitter = -2;
        } else if (m_prev_status & AFS_FLAG_SHIFT0) {
            quarter_jitter = (status & AFS_FLAG_SMOOTHING) ? -1 : -2;
        } else {
            quarter_jitter = 0;
        }
        quarter_jitter += ((status & AFS_FLAG_SMOOTHING) || m_additional_jitter != -1) ? m_additional_jitter : -2;

        if (status & (AFS_FLAG_FORCE24 | AFS_FLAG_SMOOTHING)) {
            if (!m_prev_rff_smooth) {
                if (ISRFF(m_prev_status)) rff_smooth = -1;
                else if ((m_prev_status & AFS_FLAG_PROGRESSIVE) && ISRFF(status)) rff_smooth = 1;
            }
        }
        quarter_jitter += rff_smooth;
        m_position24 += rff_smooth;

        pull_drop = (status & AFS_FLAG_FRAME_DROP)
            && !((m_prev_status|status) & AFS_FLAG_SHIFT0)
            && (status & AFS_FLAG_SHIFT1);
        m_additional_jitter = pull_drop ? -1 : 0;

        drop24 = drop24 ||
            (!(status & AFS_FLAG_SHIFT0) &&
              (status & AFS_FLAG_SHIFT1) &&
              (status & AFS_FLAG_SHIFT2));
    }

    if (drop24) m_phase24 = (m_position24 + 100) % 5;
    drop24 = 0;
    if (m_position24 >= m_phase24 &&
        ((m_position24 + 100) % 5 == m_phase24 ||
         (m_position24 +  99) % 5 == m_phase24)) {
        m_position24 -= 5;
        drop24 = 1;
    }

    if (status & AFS_FLAG_FORCE24) {
        pull_drop = drop24;
        if (status & AFS_FLAG_PROGRESSIVE) {
            quarter_jitter += m_position24;
        } else {
            quarter_jitter = m_position24++;
        }
    } else if (!(status & AFS_FLAG_PROGRESSIVE)) {
        m_phase24 -= m_position24 + 1;
        m_position24 = 0;
    }
    int drop_thre = (status & AFS_FLAG_FRAME_DROP) ? 0 : -3;
    if (!(status & AFS_FLAG_PROGRESSIVE) && ISRFF(m_prev_status)) {
        //rffからの切替時はなるべくdropさせない
        drop_thre = -3;
    }
    int drop = (quarter_jitter - m_prev_jitter < drop_thre);

    m_quarter_jitter = quarter_jitter;
    m_prev_rff_smooth = rff_smooth;
    m_prev_status = status;

    drop |= pull_drop;
    if (drop) {
        m_prev_jitter -= 4;
        m_quarter_jitter = 0;
        frameTs->pos = AFS_SSTS_DROP; //drop
    } else {
        m_prev_jitter = m_quarter_jitter;
        frameTs->pos = frameTs->orig_pts + m_quarter_jitter;
    }
    write_log(frameTs);
    return 0;
}

int64_t afsStreamStatus::get_duration(int64_t iframe) {
    if (m_set_frame < iframe + 2) {
        return AFS_SSTS_ERROR;
    }
    auto iframe_pos = m_pos[(iframe + 0) & 15].pos;
    if (iframe_pos < 0) {
        return AFS_SSTS_DROP;
    }
    auto next_pos = m_pos[(iframe + 1) & 15].pos;
    if (next_pos < 0) {
        //iframe + 1がdropならその先のフレームを参照
        next_pos = m_pos[(iframe + 2) & 15].pos;
    }
    if (next_pos < 0) {
        //iframe + 1がdropならその先のフレームを参照
        next_pos = m_pos[(iframe + 3) & 15].pos;
    }
    const auto duration = next_pos - iframe_pos;
    return (duration > 0) ? duration : AFS_SSTS_DROP;
}

NVEncFilterAfs::NVEncFilterAfs() :
    m_streamAnalyze(),
    m_streamCopy(),
    m_eventSrcAdd(),
    m_eventScanFrame(),
    m_eventMergeScan(),
    m_nFrame(0),
    m_nPts(0),
    m_source(),
    m_scan(),
    m_stripe(),
    m_status(),
    m_streamsts(),
    m_count_motion(),
    m_fpTimecode() {
    m_name = _T("afs");
}

NVEncFilterAfs::~NVEncFilterAfs() {
    close();
}

RGY_ERR NVEncFilterAfs::check_param(shared_ptr<NVEncFilterParamAfs> pAfsParam) {
    if (pAfsParam->frameOut.height <= 0 || pAfsParam->frameOut.width <= 0) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    const int hight_mul = (RGY_CSP_CHROMA_FORMAT[pAfsParam->frameOut.csp] == RGY_CHROMAFMT_YUV420) ? 4 : 2;
    if ((pAfsParam->frameOut.height % hight_mul) != 0) {
        AddMessage(RGY_LOG_ERROR, _T("Height must be multiple of %d.\n"), hight_mul);
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.top < 0 || pAfsParam->afs.clip.top >= pAfsParam->frameOut.height) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.top).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.bottom < 0 || pAfsParam->afs.clip.bottom >= pAfsParam->frameOut.height) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.bottom).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.top + pAfsParam->afs.clip.bottom >= pAfsParam->frameOut.height) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.top + clip.bottom).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.left < 0 || pAfsParam->afs.clip.left >= pAfsParam->frameOut.width) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.left).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.left % 4 != 0) {
        AddMessage(RGY_LOG_ERROR, _T("parameter \"left\" rounded to multiple of 4.\n"));
        pAfsParam->afs.clip.left = (pAfsParam->afs.clip.left + 2) & ~3;
    }
    if (pAfsParam->afs.clip.right < 0 || pAfsParam->afs.clip.right >= pAfsParam->frameOut.width) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.right).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.clip.right % 4 != 0) {
        AddMessage(RGY_LOG_ERROR, _T("parameter \"right\" rounded to multiple of 4.\n"));
        pAfsParam->afs.clip.right = (pAfsParam->afs.clip.right + 2) & ~3;
    }
    if (pAfsParam->afs.clip.left + pAfsParam->afs.clip.right >= pAfsParam->frameOut.width) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (clip.left + clip.right).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.method_switch < 0 || pAfsParam->afs.method_switch > 256) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (method_switch).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.coeff_shift < 0 || pAfsParam->afs.coeff_shift > 256) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (coeff_shift).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_shift < 0 || pAfsParam->afs.thre_shift > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_shift).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_deint < 0 || pAfsParam->afs.thre_deint > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_deint).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_Ymotion < 0 || pAfsParam->afs.thre_Ymotion > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_Ymotion).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.thre_Cmotion < 0 || pAfsParam->afs.thre_Cmotion > 1024) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (thre_Cmotion).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pAfsParam->afs.analyze < 0 || pAfsParam->afs.analyze > 5) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter (level).\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (!pAfsParam->afs.shift) {
        AddMessage(RGY_LOG_WARN, _T("shift was off, so drop and smooth will also be off.\n"));
        pAfsParam->afs.drop = false;
        pAfsParam->afs.smooth = false;
    }
    return RGY_ERR_NONE;
}

RGY_ERR NVEncFilterAfs::init(shared_ptr<NVEncFilterParam> pParam, shared_ptr<RGYLog> pPrintMes) {
    RGY_ERR sts = RGY_ERR_NONE;
    m_pLog = pPrintMes;
    auto pAfsParam = std::dynamic_pointer_cast<NVEncFilterParamAfs>(pParam);
    if (!pAfsParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    //パラメータチェック
    if (check_param(pAfsParam) != RGY_ERR_NONE) {
        return RGY_ERR_INVALID_PARAM;
    }
    if (rgy_csp_has_alpha(pParam->frameIn.csp)) {
        AddMessage(RGY_LOG_ERROR, _T("vpp-afs does not support alpha channel.\n"));
        return RGY_ERR_UNSUPPORTED;
    }

    auto err = AllocFrameBuf(pAfsParam->frameOut, 1);
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), get_err_mes(err));
        return RGY_ERR_MEMORY_ALLOC;
    }
    for (int i = 0; i < RGY_CSP_PLANES[pParam->frameOut.csp]; i++) {
        pAfsParam->frameOut.pitch[i] = m_frameBuf[0]->frame.pitch[i];
    }
    AddMessage(RGY_LOG_DEBUG, _T("allocated output buffer: %dx%d, pitch %d, %s.\n"),
        m_frameBuf[0]->frame.width, m_frameBuf[0]->frame.height, m_frameBuf[0]->frame.pitch[0], RGY_CSP_NAMES[m_frameBuf[0]->frame.csp]);

    if (RGY_ERR_NONE != (err = m_source.alloc(pAfsParam->frameOut))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), get_err_mes(err));
        return RGY_ERR_MEMORY_ALLOC;
    }
    AddMessage(RGY_LOG_DEBUG, _T("allocated source buffer: %dx%d, pitch %d, %s.\n"),
        m_source.get(0)->frame.width, m_source.get(0)->frame.height, m_source.get(0)->frame.pitch[0], RGY_CSP_NAMES[m_source.get(0)->frame.csp]);

    if (RGY_ERR_NONE != (err = m_scan.alloc(pAfsParam->frameOut))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), get_err_mes(err));
        return RGY_ERR_MEMORY_ALLOC;
    }
    AddMessage(RGY_LOG_DEBUG, _T("allocated scan buffer: %dx%d, pitch %d, %s.\n"),
        m_scan.get(0)->map.frame.width, m_scan.get(0)->map.frame.height, m_scan.get(0)->map.frame.pitch[0], RGY_CSP_NAMES[m_scan.get(0)->map.frame.csp]);

    if (RGY_ERR_NONE != (err = m_stripe.alloc(pAfsParam->frameOut))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), get_err_mes(err));
        return RGY_ERR_MEMORY_ALLOC;
    }
    AddMessage(RGY_LOG_DEBUG, _T("allocated stripe buffer: %dx%d, pitch %d, %s.\n"),
        m_stripe.get(0)->map.frame.width, m_stripe.get(0)->map.frame.height, m_stripe.get(0)->map.frame.pitch[0], RGY_CSP_NAMES[m_stripe.get(0)->map.frame.csp]);

    m_streamAnalyze = std::unique_ptr<cudaStream_t, cudastream_deleter>(new cudaStream_t(), cudastream_deleter());
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaStreamCreateWithFlags(m_streamAnalyze.get(), cudaStreamNonBlocking)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to cudaStreamCreateWithFlags: %s.\n"), get_err_mes(err));
        return RGY_ERR_CUDA;
    }

    m_streamCopy = std::unique_ptr<cudaStream_t, cudastream_deleter>(new cudaStream_t(), cudastream_deleter());
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaStreamCreateWithFlags(m_streamCopy.get(), cudaStreamNonBlocking)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to cudaStreamCreateWithFlags: %s.\n"), get_err_mes(err));
        return RGY_ERR_CUDA;
    }

    const uint32_t cudaEventFlags = (pAfsParam->cudaSchedule & CU_CTX_SCHED_BLOCKING_SYNC) ? cudaEventBlockingSync : 0;

    m_eventSrcAdd = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaEventCreateWithFlags(m_eventSrcAdd.get(), cudaEventFlags | cudaEventDisableTiming)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to cudaEventCreateWithFlags: %s.\n"), get_err_mes(err));
        return RGY_ERR_CUDA;
    }

    m_eventScanFrame = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaEventCreateWithFlags(m_eventScanFrame.get(), cudaEventFlags | cudaEventDisableTiming)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to cudaEventCreateWithFlags: %s.\n"), get_err_mes(err));
        return RGY_ERR_CUDA;
    }

    m_eventMergeScan = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaEventCreateWithFlags(m_eventMergeScan.get(), cudaEventFlags | cudaEventDisableTiming)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to cudaEventCreateWithFlags: %s.\n"), get_err_mes(err));
        return RGY_ERR_CUDA;
    }

    pAfsParam->frameOut.picstruct = RGY_PICSTRUCT_FRAME;
    m_nFrame = 0;
    m_nPts = 0;
    m_pathThrough &= (~(FILTER_PATHTHROUGH_PICSTRUCT | FILTER_PATHTHROUGH_TIMESTAMP | FILTER_PATHTHROUGH_FLAGS));
    if (pAfsParam->afs.force24) {
        pAfsParam->baseFps *= rgy_rational<int>(4, 5);
    }

    if (pAfsParam->afs.timecode) {
        const tstring tc_filename = PathRemoveExtensionS(pAfsParam->outFilename) + ((pAfsParam->afs.timecode == 2) ? _T(".timecode.afs.txt") : _T(".timecode.txt"));
        if (open_timecode(tc_filename)) {
            errno_t error = errno;
            AddMessage(RGY_LOG_ERROR, _T("failed to open timecode file \"%s\": %s.\n"), tc_filename.c_str(), _tcserror(error));
            return RGY_ERR_FILE_OPEN; // Couldn't open file
        }
        AddMessage(RGY_LOG_DEBUG, _T("opened timecode file \"%s\".\n"), tc_filename.c_str());
    }

    if (pAfsParam->afs.log) {
        const tstring log_filename = PathRemoveExtensionS(pAfsParam->outFilename) + _T(".afslog.csv");
        if (m_streamsts.open_log(log_filename)) {
            errno_t error = errno;
            AddMessage(RGY_LOG_ERROR, _T("failed to open afs log file \"%s\": %s.\n"), log_filename.c_str(), _tcserror(error));
            return RGY_ERR_FILE_OPEN; // Couldn't open file
        }
        AddMessage(RGY_LOG_DEBUG, _T("opened afs log file \"%s\".\n"), log_filename.c_str());
    }

    setFilterInfo(pParam->print());
    m_param = pParam;
    return sts;
}

tstring NVEncFilterParamAfs::print() const {
    return afs.print();
}

bool NVEncFilterAfs::scan_frame_result_cached(int frame, const VppAfs *pAfsPrm) {
    auto sp = m_scan.get(frame);
    const int mode = pAfsPrm->analyze == 0 ? 0 : 1;
    return sp->status > 0 && sp->frame == frame && sp->tb_order == pAfsPrm->tb_order && sp->thre_shift == pAfsPrm->thre_shift &&
        ((mode == 0) ||
        (mode == 1 && sp->mode == 1 && sp->thre_deint == pAfsPrm->thre_deint && sp->thre_Ymotion == pAfsPrm->thre_Ymotion && sp->thre_Cmotion == pAfsPrm->thre_Cmotion));
}

RGY_ERR NVEncFilterAfs::scan_frame(int iframe, int force, const NVEncFilterParamAfs *pAfsPrm, cudaStream_t stream) {
    if (!force && scan_frame_result_cached(iframe, &pAfsPrm->afs)) {
        return RGY_ERR_NONE;
    }
    auto p1 = m_source.get(iframe-1);
    auto p0 = m_source.get(iframe);
    auto sp = m_scan.get(iframe);

    const int mode = pAfsPrm->afs.analyze == 0 ? 0 : 1;
    m_stripe.expire(iframe - 1);
    m_stripe.expire(iframe);
    sp->status = 1;
    sp->frame = iframe, sp->mode = mode, sp->tb_order = pAfsPrm->afs.tb_order;
    sp->thre_shift = pAfsPrm->afs.thre_shift, sp->thre_deint = pAfsPrm->afs.thre_deint;
    sp->thre_Ymotion = pAfsPrm->afs.thre_Ymotion, sp->thre_Cmotion = pAfsPrm->afs.thre_Cmotion;
    sp->clip.top = sp->clip.bottom = sp->clip.left = sp->clip.right = -1;
    auto err = analyze_stripe(p0, p1, sp, &m_count_motion, pAfsPrm, stream);
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed analyze_stripe: %s.\n"), get_err_mes(err));
        return err;
    }
    if (stream != cudaStreamDefault) {
        cudaEventRecord(*m_eventScanFrame.get(), stream);
        cudaStreamWaitEvent(*m_streamCopy.get(), *m_eventScanFrame.get(), 0);
        err = m_count_motion.copyDtoHAsync(*m_streamCopy.get());
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed m_count_motion.copyDtoHAsync: %s.\n"), get_err_mes(err));
            return err;
        }
        cudaEventRecord(*sp->cuevent.get(), *m_streamCopy.get());
        sp = m_scan.get(iframe-1);
    }

    err = count_motion(sp, &pAfsPrm->afs.clip);
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed analyze_stripe: %s.\n"), get_err_mes(err));
        return err;
    }
    return err;
}

RGY_ERR NVEncFilterAfs::count_motion(AFS_SCAN_DATA *sp, const AFS_SCAN_CLIP *clip) {
    sp->clip = *clip;

    auto err = RGY_ERR_NONE;
    if (STREAM_OPT) {
        cudaEventSynchronize(*sp->cuevent.get());
    } else {
        err = m_count_motion.copyDtoH();
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed m_count_motion.copyDtoH: %s.\n"), get_err_mes(err));
            return err;
        }
    }

    const int nSize = (int)(m_count_motion.nSize / sizeof(uint32_t));
    int count0 = 0;
    int count1 = 0;
    uint32_t *ptrCount = (uint32_t *)m_count_motion.ptrHost;
    for (int i = 0; i < nSize; i++) {
        uint32_t count = ptrCount[i];
        count0 += count & 0xffff;
        count1 += count >> 16;
    }
    sp->ff_motion = count0;
    sp->lf_motion = count1;
    //AddMessage(RGY_LOG_INFO, _T("count_motion[%6d]: %6d - %6d (ff,lf)"), sp->frame, sp->ff_motion, sp->lf_motion);
#if 0
    uint8_t *ptr = nullptr;
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaMallocHost(&ptr, sp->map.frame.pitch * sp->map.frame.height)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMallocHost: %s.\n"), get_err_mes(err));
        return cudaerr;
    }
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaMemcpy2D(ptr, sp->map.frame.pitch, sp->map.frame.ptr, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, cudaMemcpyDeviceToHost)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMemcpy2D: %s.\n"), get_err_mes(err));
        return cudaerr;
    }

    int motion_count[2] = { 0, 0 };
    afs_get_motion_count(motion_count, ptr, &sp->clip, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, sp->tb_order);
    AddMessage((count0 == motion_count[0] && count1 == motion_count[1]) ? RGY_LOG_INFO : RGY_LOG_ERROR, _T("count_motion(ret, debug) = (%6d, %6d) / (%6d, %6d)\n"), count0, motion_count[0], count1, motion_count[1]);
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaFreeHost(ptr)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaFreeHost: %s.\n"), get_err_mes(err));
        return cudaerr;
    }
#endif
    return RGY_ERR_NONE;
}

RGY_ERR NVEncFilterAfs::get_stripe_info(int iframe, int mode, const NVEncFilterParamAfs *pAfsPrm, cudaStream_t stream) {
    AFS_STRIPE_DATA *sp = m_stripe.get(iframe);
    if (sp->status > mode && sp->status < 4 && sp->frame == iframe) {
        if (sp->status == 2) {
            auto err = count_stripe(sp, &pAfsPrm->afs.clip, pAfsPrm->afs.tb_order);
            if (err != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed count_stripe: %s.\n"), get_err_mes(err));
                return err;
            }
            sp->status = 3;
        }
        return RGY_ERR_NONE;
    }

    AFS_SCAN_DATA *sp0 = m_scan.get(iframe);
    AFS_SCAN_DATA *sp1 = m_scan.get(iframe + 1);
    auto err = merge_scan(sp, sp0, sp1, &sp->buf_count_stripe, pAfsPrm, (STREAM_OPT) ? *m_streamAnalyze.get() : stream);
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed merge_scan: %s.\n"), get_err_mes(err));
        return err;
    }
    sp->status = 2;
    sp->frame = iframe;

    if (STREAM_OPT) {
        cudaEventRecord(*m_eventMergeScan.get(), *m_streamAnalyze.get());
        cudaStreamWaitEvent(*m_streamCopy.get(), *m_eventMergeScan.get(), 0);
        err = sp->buf_count_stripe.copyDtoHAsync(*m_streamCopy.get());
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed m_count_motion.copyDtoHAsync: %s.\n"), get_err_mes(err));
            return err;
        }
        cudaEventRecord(*sp->cuevent.get(), *m_streamCopy.get());
    } else {
        if (RGY_ERR_NONE != (err = count_stripe(sp, &pAfsPrm->afs.clip, pAfsPrm->afs.tb_order))) {
            AddMessage(RGY_LOG_ERROR, _T("failed count_stripe: %s.\n"), get_err_mes(err));
            return err;
        }
        sp->status = 3;
    }
    return RGY_ERR_NONE;
}

RGY_ERR NVEncFilterAfs::count_stripe(AFS_STRIPE_DATA *sp, const AFS_SCAN_CLIP *clip, int tb_order) {
    auto err = RGY_ERR_NONE;
    if (STREAM_OPT) {
        cudaEventSynchronize(*sp->cuevent.get());
    } else {
        //cudaStreamSynchronize(*m_stream.get());
        err = sp->buf_count_stripe.copyDtoH();
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed m_count_stripe.copyDtoH: %s.\n"), get_err_mes(err));
            return err;
        }
    }

    const int nSize = (int)(sp->buf_count_stripe.nSize / sizeof(uint32_t));
    int count0 = 0;
    int count1 = 0;
    uint32_t *ptrCount = (uint32_t *)sp->buf_count_stripe.ptrHost;
    for (int i = 0; i < nSize; i++) {
        uint32_t count = ptrCount[i];
        count0 += count & 0xffff;
        count1 += count >> 16;
    }
    sp->count0 = count0;
    sp->count1 = count1;
    //AddMessage(RGY_LOG_INFO, _T("count_stripe[%6d]: %6d - %6d"), sp->frame, count0, count1);
#if 0
    uint8_t *ptr = nullptr;
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaMallocHost(&ptr, sp->map.frame.pitch * sp->map.frame.height)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMallocHost: %s.\n"), get_err_mes(err));
        return cudaerr;
    }
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaMemcpy2D(ptr, sp->map.frame.pitch, sp->map.frame.ptr, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, cudaMemcpyDeviceToHost)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaMemcpy2D: %s.\n"), get_err_mes(err));
        return cudaerr;
    }

    int stripe_count[2] = { 0, 0 };
    afs_get_stripe_count(stripe_count, ptr, clip, sp->map.frame.pitch, sp->map.frame.width, sp->map.frame.height, tb_order);
    AddMessage((count0 == stripe_count[0] && count1 == stripe_count[1]) ? RGY_LOG_INFO : RGY_LOG_ERROR, _T("count_stripe(ret, debug) = (%6d, %6d) / (%6d, %6d)\n"), count0, stripe_count[0], count1, stripe_count[1]);
    if (RGY_ERR_NONE != (err = err_to_rgy(cudaFreeHost(ptr)))) {
        AddMessage(RGY_LOG_ERROR, _T("failed cudaFreeHost: %s.\n"), get_err_mes(err));
        return cudaerr;
    }
#else
    UNREFERENCED_PARAMETER(tb_order);
    UNREFERENCED_PARAMETER(clip);
#endif
    return RGY_ERR_NONE;
}

int NVEncFilterAfs::detect_telecine_cross(int iframe, int coeff_shift) {
    const AFS_SCAN_DATA *sp1 = m_scan.get(iframe - 1);
    const AFS_SCAN_DATA *sp2 = m_scan.get(iframe + 0);
    const AFS_SCAN_DATA *sp3 = m_scan.get(iframe + 1);
    const AFS_SCAN_DATA *sp4 = m_scan.get(iframe + 2);
    int shift = 0;

    if (std::max(absdiff(sp1->lf_motion + sp2->lf_motion, sp2->ff_motion),
        absdiff(sp3->ff_motion + sp4->ff_motion, sp3->lf_motion)) * coeff_shift >
        max3(absdiff(sp1->ff_motion + sp2->ff_motion, sp1->lf_motion),
            absdiff(sp2->ff_motion + sp3->ff_motion, sp2->lf_motion),
            absdiff(sp3->lf_motion + sp4->lf_motion, sp4->ff_motion)) * 256)
        if (std::max(sp2->lf_motion, sp3->ff_motion) * coeff_shift > sp2->ff_motion * 256)
            shift = 1;

    if (std::max(absdiff(sp1->lf_motion + sp2->lf_motion, sp2->ff_motion),
        absdiff(sp3->ff_motion + sp4->ff_motion, sp3->lf_motion)) * coeff_shift >
        max3(absdiff(sp1->ff_motion + sp2->ff_motion, sp1->lf_motion),
            absdiff(sp2->lf_motion + sp3->lf_motion, sp3->ff_motion),
            absdiff(sp3->lf_motion + sp4->lf_motion, sp4->ff_motion)) * 256)
        if (std::max(sp2->lf_motion, sp3->ff_motion) * coeff_shift > sp3->lf_motion * 256)
            shift = 1;

    return shift;
}

RGY_ERR NVEncFilterAfs::analyze_frame(int iframe, const NVEncFilterParamAfs *pAfsPrm, int reverse[4], int assume_shift[4], int result_stat[4], cudaStream_t stream) {
    for (int i = 0; i < 4; i++) {
        assume_shift[i] = detect_telecine_cross(iframe + i, pAfsPrm->afs.coeff_shift);
    }

    AFS_SCAN_DATA *scp = m_scan.get(iframe);
    const int scan_w = m_param->frameIn.width;
    const int scan_h = m_param->frameIn.height;
    int total = 0;
    if (scan_h - scp->clip.bottom - ((scan_h - scp->clip.top - scp->clip.bottom) & 1) > scp->clip.top && scan_w - scp->clip.right > scp->clip.left)
        total = (scan_h - scp->clip.bottom - ((scan_h - scp->clip.top - scp->clip.bottom) & 1) - scp->clip.top) * (scan_w - scp->clip.right - scp->clip.left);
    const int threshold = (total * pAfsPrm->afs.method_switch) >> 12;

    for (int i = 0; i < 4; i++) {
        auto err = get_stripe_info(iframe + i, 0, pAfsPrm, stream);
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed on get_stripe_info(iframe=%d): %s.\n"), iframe + i, get_err_mes(err));
            return err;
        }
        AFS_STRIPE_DATA *stp = m_stripe.get(iframe + i);
        result_stat[i] = (stp->count0 * pAfsPrm->afs.coeff_shift > stp->count1 * 256) ? 1 : 0;
        if (threshold > stp->count1 && threshold > stp->count0)
            result_stat[i] += 2;
    }

    uint8_t status = AFS_STATUS_DEFAULT;
    if (result_stat[0] & 2)
        status |= assume_shift[0] ? AFS_FLAG_SHIFT0 : 0;
    else
        status |= (result_stat[0] & 1) ? AFS_FLAG_SHIFT0 : 0;
    if (reverse[0]) status ^= AFS_FLAG_SHIFT0;

    if (result_stat[1] & 2)
        status |= assume_shift[1] ? AFS_FLAG_SHIFT1 : 0;
    else
        status |= (result_stat[1] & 1) ? AFS_FLAG_SHIFT1 : 0;
    if (reverse[1]) status ^= AFS_FLAG_SHIFT1;

    if (result_stat[2] & 2)
        status |= assume_shift[2] ? AFS_FLAG_SHIFT2 : 0;
    else
        status |= (result_stat[2] & 1) ? AFS_FLAG_SHIFT2 : 0;
    if (reverse[2]) status ^= AFS_FLAG_SHIFT2;

    if (result_stat[3] & 2)
        status |= assume_shift[3] ? AFS_FLAG_SHIFT3 : 0;
    else
        status |= (result_stat[3] & 1) ? AFS_FLAG_SHIFT3 : 0;
    if (reverse[3]) status ^= AFS_FLAG_SHIFT3;

    const auto& frameinfo = m_source.get(iframe)->frame;
    if (!interlaced(frameinfo)) {
        status |= AFS_FLAG_PROGRESSIVE;
        if (frameinfo.flags & RGY_FRAME_FLAG_RFF) status |= AFS_FLAG_RFF;
    }
    if (pAfsPrm->afs.drop) {
        if (interlaced(frameinfo)) status |= AFS_FLAG_FRAME_DROP;
        if (pAfsPrm->afs.smooth) status |= AFS_FLAG_SMOOTHING;
    }
    if (pAfsPrm->afs.force24) status |= AFS_FLAG_FORCE24;
    if (iframe < 1) status &= AFS_MASK_SHIFT0;

    m_status[iframe] = status;
    return RGY_ERR_NONE;
}

RGY_ERR NVEncFilterAfs::run_filter(const RGYFrameInfo *pInputFrame, RGYFrameInfo **ppOutputFrames, int *pOutputFrameNum, cudaStream_t stream) {
    RGY_ERR sts = RGY_ERR_NONE;

    auto pAfsParam = std::dynamic_pointer_cast<NVEncFilterParamAfs>(m_param);
    if (!pAfsParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }

    const int iframe = m_source.inframe();
    if (pInputFrame->ptr[0] == nullptr && m_nFrame >= iframe) {
        //終了
        *pOutputFrameNum = 0;
        ppOutputFrames[0] = nullptr;
        return sts;
    } else if (pInputFrame->ptr[0] != nullptr) {
        //エラーチェック
        const auto memcpyKind = getCudaMemcpyKind(pInputFrame->mem_type, m_frameBuf[0]->frame.mem_type);
        if (memcpyKind != cudaMemcpyDeviceToDevice) {
            AddMessage(RGY_LOG_ERROR, _T("only supported on device memory.\n"));
            return RGY_ERR_INVALID_CALL;
        }
        if (m_param->frameOut.csp != m_param->frameIn.csp) {
            AddMessage(RGY_LOG_ERROR, _T("csp does not match.\n"));
            return RGY_ERR_INVALID_PARAM;
        }
        //sourceキャッシュにコピー
        sts = m_source.add(pInputFrame, stream);
        if (sts != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed to add frame to sorce buffer: %s.\n"), get_err_mes(sts));
            return sts;
        }
        if (STREAM_OPT) {
            cudaEventSynchronize(*m_eventSrcAdd.get());
            cudaEventRecord(*m_eventSrcAdd.get(), stream);
            cudaStreamWaitEvent(*m_streamAnalyze.get(), *m_eventSrcAdd.get(), 0);
        }
        if (iframe == 0) {
            // scan_frame(p1 = -2, p0 = -1)のscan_frameも必要
            if (RGY_ERR_NONE != (sts = scan_frame(iframe-1, false, pAfsParam.get(), (STREAM_OPT) ? *m_streamAnalyze.get() : stream))) {
                AddMessage(RGY_LOG_ERROR, _T("failed on scan_frame(iframe-1=%d): %s.\n"), iframe-1, get_err_mes(sts));
                return sts;
            }
        }
        if (RGY_ERR_NONE != (sts = scan_frame(iframe, false, pAfsParam.get(), (STREAM_OPT) ? *m_streamAnalyze.get() : stream))) {
            AddMessage(RGY_LOG_ERROR, _T("failed on scan_frame(iframe=%d): %s.\n"), iframe, get_err_mes(sts));
            return sts;
        }
    }

    if (iframe >= 5) {
        int reverse[4] = { 0 }, assume_shift[4] = { 0 }, result_stat[4] = { 0 };
        sts = analyze_frame(iframe - 5, pAfsParam.get(), reverse, assume_shift, result_stat, stream);
        if (sts != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed on scan_frame(iframe=%d): %s.\n"), iframe - 5, get_err_mes(sts));
            return sts;
        }
    }
    static const int preread_len = 3;
    //十分な数のフレームがたまった、あるいはdrainモードならフレームを出力
    if (iframe >= (5+preread_len+STREAM_OPT) || pInputFrame->ptr[0] == nullptr) {
        int reverse[4] = { 0 }, assume_shift[4] = { 0 }, result_stat[4] = { 0 };

        //m_streamsts.get_durationを呼ぶには、3フレーム先までstatusをセットする必要がある
        //そのため、analyze_frameを使って、3フレーム先までstatusを計算しておく
        for (int i = preread_len; i >= 0; i--) {
            //ここでは、これまで発行したanalyze_frameの結果からstatusの更新を行う(analyze_frameの内部で行われる)
            sts = analyze_frame(m_nFrame + i, pAfsParam.get(), reverse, assume_shift, result_stat, stream);
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("error on analyze_frame(m_nFrame=%d, iframe=%d): %s.\n"),
                    m_nFrame, m_nFrame + i, iframe, get_err_mes(sts));
                return sts;
            }
        }

        if (m_nFrame == 0) {
            //m_nFrame == 0のときは、下記がセットされていない
            for (int i = 0; i < preread_len; i++) {
                if (m_streamsts.set_status(i, m_status[i], i, m_source.get(i)->frame.timestamp) != 0) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to set afs_status(%d).\n"), i);
                    return RGY_ERR_CUDA;
                }
            }
        }
        //m_streamsts.get_durationを呼ぶには、3フレーム先までstatusをセットする必要がある
        {
            auto timestamp = m_source.get(m_nFrame+preread_len)->frame.timestamp;
            //読み込まれた範囲を超える部分のtimestampは外挿する
            //こうしないと最終フレームのdurationが正しく計算されない
            if (m_nFrame+preread_len >= m_source.inframe()) {
                //1フレームの平均時間
                auto inframe_avg_duration = (m_source.get(m_source.inframe()-1)->frame.timestamp + m_source.inframe() / 2) / m_source.inframe();
                //外挿するフレーム数をかけて足し込む
                timestamp += (m_nFrame+preread_len - (m_source.inframe()-1)) * inframe_avg_duration;
            }
            if (m_streamsts.set_status(m_nFrame+preread_len, m_status[m_nFrame+preread_len], 0, timestamp) != 0) {
                AddMessage(RGY_LOG_ERROR, _T("failed to set afs_status(%d).\n"), m_nFrame+preread_len);
                return RGY_ERR_CUDA;
            }
        }
        const auto afs_duration = m_streamsts.get_duration(m_nFrame);
        if (afs_duration == afsStreamStatus::AFS_SSTS_DROP) {
            //出力フレームなし
            *pOutputFrameNum = 0;
            ppOutputFrames[0] = nullptr;
        } else if (afs_duration < 0) {
            AddMessage(RGY_LOG_ERROR, _T("invalid call for m_streamsts.get_duration(%d).\n"), m_nFrame);
            return RGY_ERR_INVALID_CALL;
        } else {
            //出力先のフレーム
            CUFrameBuf *pOutFrame = nullptr;
            *pOutputFrameNum = 1;
            if (ppOutputFrames[0] == nullptr) {
                pOutFrame = m_frameBuf[m_nFrameIdx].get();
                ppOutputFrames[0] = &pOutFrame->frame;
                m_nFrameIdx = (m_nFrameIdx + 1) % m_frameBuf.size();
            }

            if (pAfsParam->afs.timecode) {
                write_timecode(m_nPts, pAfsParam->outTimebase);
            }

            pOutFrame->frame.flags = m_source.get(m_nFrame)->frame.flags & (~(RGY_FRAME_FLAG_RFF | RGY_FRAME_FLAG_RFF_COPY | RGY_FRAME_FLAG_RFF_BFF | RGY_FRAME_FLAG_RFF_TFF));
            pOutFrame->frame.inputFrameId = m_source.get(m_nFrame)->frame.inputFrameId;
            pOutFrame->frame.picstruct = RGY_PICSTRUCT_FRAME;
            pOutFrame->frame.duration = rational_rescale(afs_duration, pAfsParam->inTimebase, pAfsParam->outTimebase);
            pOutFrame->frame.timestamp = m_nPts;
            m_nPts += pOutFrame->frame.duration;

            //出力するフレームを作成
            get_stripe_info(m_nFrame, 1, pAfsParam.get(), stream);
            sts = RGY_ERR_NONE;
            auto sip_filtered = m_stripe.filter(m_nFrame, pAfsParam->afs.analyze, stream, &sts);
            if (sip_filtered == nullptr || sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed m_stripe.filter(m_nFrame=%d, iframe=%d): %s.\n"), m_nFrame, iframe - (5+preread_len), get_err_mes(sts));
                return sts;
            }

            if (interlaced(m_source.get(m_nFrame)->frame) || pAfsParam->afs.tune) {
                sts = synthesize(m_nFrame, pOutFrame, m_source.get(m_nFrame), m_source.get(m_nFrame-1), sip_filtered, pAfsParam.get(), stream);
            } else {
                sts = copyFrameAsync(&pOutFrame->frame, &m_source.get(m_nFrame)->frame, stream);
            }
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("error on synthesize(m_nFrame=%d, iframe=%d): %s.\n"), m_nFrame, iframe - (5+preread_len), get_err_mes(sts));
                return sts;
            }
        }

        m_nFrame++;

        // drain中にdropが発生した場合には、次のフレームを出力するようにする
        if (pInputFrame->ptr[0] == nullptr && afs_duration == afsStreamStatus::AFS_SSTS_DROP) {
            return run_filter(pInputFrame, ppOutputFrames, pOutputFrameNum, stream);
        }
    } else {
        //出力フレームなし
        *pOutputFrameNum = 0;
        ppOutputFrames[0] = nullptr;
    }
    return sts;
}

int NVEncFilterAfs::open_timecode(tstring tc_filename) {
    FILE *fp = NULL;
    if (_tfopen_s(&fp, tc_filename.c_str(), _T("w"))) {
        return 1;
    }
    m_fpTimecode = unique_ptr<FILE, fp_deleter>(fp, fp_deleter());
    fprintf(m_fpTimecode.get(), "# timecode format v2\n");
    return 0;
}

void NVEncFilterAfs::write_timecode(int64_t pts, const rgy_rational<int>& timebase) {
    if (pts >= 0) {
        fprintf(m_fpTimecode.get(), "%.6lf\n", pts * timebase.qdouble() * 1000.0);
    }
}

void NVEncFilterAfs::close() {
    m_streamAnalyze.reset();
    m_streamCopy.reset();
    m_eventSrcAdd.reset();
    m_eventScanFrame.reset();
    m_eventMergeScan.reset();
    m_nFrame = 0;
    m_frameBuf.clear();
    m_source.clear();
    m_scan.clear();
    m_stripe.clear();
    m_status.clear();
    m_count_motion.clear();
    m_fpTimecode.reset();
    AddMessage(RGY_LOG_DEBUG, _T("closed afs filter.\n"));
}

static inline BOOL is_latter_field(int pos_y, int tb_order) {
    return ((pos_y & 1) == tb_order);
}

#if defined(_M_IX86) || defined(_M_X64)
#include <emmintrin.h>
#endif // #if defined(_M_IX86) || defined(_M_X64)

static void afs_get_stripe_count(int *stripe_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order) {
    static const uint8_t STRIPE_COUNT_CHECK_MASK[][16] = {
        { 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50 },
        { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60 },
    };
    const int y_fin = scan_h - clip->bottom - ((scan_h - clip->top - clip->bottom) & 1);
    const uint32_t check_mask[2] = { 0x50, 0x60 };
    for (int pos_y = clip->top; pos_y < y_fin; pos_y++) {
        const uint8_t *sip = ptr + pos_y * pitch + clip->left;
        const int x_count = scan_w - clip->right - clip->left;
        const int first_field_flag = !is_latter_field(pos_y, tb_order);
#if defined(_M_IX86) || defined(_M_X64)
        __m128i xMask, x0, x1;
        __m128i xZero = _mm_setzero_si128();
        xMask = _mm_loadu_si128((const __m128i*)STRIPE_COUNT_CHECK_MASK[first_field_flag]);
        const uint8_t *sip_fin = sip + (x_count & ~31);
        for (; sip < sip_fin; sip += 32) {
            x0 = _mm_loadu_si128((const __m128i*)(sip +  0));
            x1 = _mm_loadu_si128((const __m128i*)(sip + 16));
            x0 = _mm_and_si128(x0, xMask);
            x1 = _mm_and_si128(x1, xMask);
            x0 = _mm_cmpeq_epi8(x0, xZero);
            x1 = _mm_cmpeq_epi8(x1, xZero);
            uint32_t count0 = _mm_movemask_epi8(x0);
            uint32_t count1 = _mm_movemask_epi8(x1);
            stripe_count[first_field_flag] += popcnt32(((count1 << 16) | count0));
        }
        if (x_count & 16) {
            x0 = _mm_loadu_si128((const __m128i*)sip);
            x0 = _mm_and_si128(x0, xMask);
            x0 = _mm_cmpeq_epi8(x0, xZero);
            uint32_t count0 = _mm_movemask_epi8(x0);
            stripe_count[first_field_flag] += popcnt32(count0);
            sip += 16;
        }
        sip_fin = sip + (x_count & 15);
        for (; sip < sip_fin; sip++)
            stripe_count[first_field_flag] += (!(*sip & check_mask[first_field_flag]));
#else
        if (first_field_flag) {
            for (int pos_x = 0; pos_x < x_count; pos_x++) {
                if (!(*sip & 0x50)) stripe_count[0]++;
                sip++;
            }
        } else {
            for (int pos_x = 0; pos_x < x_count; pos_x++) {
                if (!(*sip & 0x60)) stripe_count[1]++;
                sip++;
            }
        }
#endif
    }
}

static void afs_get_motion_count(int *motion_count, const uint8_t *ptr, const AFS_SCAN_CLIP *clip, int pitch, int scan_w, int scan_h, int tb_order) {
    const int y_fin = scan_h - clip->bottom - ((scan_h - clip->top - clip->bottom) & 1);
    for (int pos_y = clip->top; pos_y < y_fin; pos_y++) {
        const uint8_t *sip = ptr + pos_y * pitch + clip->left;
        const int x_count = scan_w - clip->right - clip->left;
        const int is_latter_feild = is_latter_field(pos_y, tb_order);
#if defined(_M_IX86) || defined(_M_X64)
        __m128i xMotion = _mm_set1_epi8(0x40);
        __m128i x0, x1;
        const uint8_t *sip_fin = sip + (x_count & ~31);
        for (; sip < sip_fin; sip += 32) {
            x0 = _mm_loadu_si128((const __m128i*)(sip +  0));
            x1 = _mm_loadu_si128((const __m128i*)(sip + 16));
            x0 = _mm_andnot_si128(x0, xMotion);
            x1 = _mm_andnot_si128(x1, xMotion);
            x0 = _mm_cmpeq_epi8(x0, xMotion);
            x1 = _mm_cmpeq_epi8(x1, xMotion);
            uint32_t count0 = _mm_movemask_epi8(x0);
            uint32_t count1 = _mm_movemask_epi8(x1);
            motion_count[is_latter_feild] += popcnt32(((count1 << 16) | count0));
        }
        if (x_count & 16) {
            x0 = _mm_loadu_si128((const __m128i*)sip);
            x0 = _mm_andnot_si128(x0, xMotion);
            x0 = _mm_cmpeq_epi8(x0, xMotion);
            uint32_t count0 = _mm_movemask_epi8(x0);
            motion_count[is_latter_feild] += popcnt32(count0);
            sip += 16;
        }
        sip_fin = sip + (x_count & 15);
        for (; sip < sip_fin; sip++)
            motion_count[is_latter_feild] += ((~*sip & 0x40) >> 6);
#else
        if (is_latter_feild) {
            for (int pos_x = 0; pos_x < x_count; pos_x++) {
                motion_count[1] += ((~*sip & 0x40) >> 6);
                sip++;
            }
        } else {
            for (int pos_x = 0; pos_x < x_count; pos_x++) {
                motion_count[0] += ((~*sip & 0x40) >> 6);
                sip++;
            }
        }
#endif
    }
}