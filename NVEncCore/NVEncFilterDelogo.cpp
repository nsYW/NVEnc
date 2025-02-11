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
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#pragma warning (push)
#pragma warning (disable: 4819)
#include "cuda_runtime.h"
#include "device_launch_parameters.h"
#include "NVEncFilterDelogo.h"
#pragma warning (pop)
#include "rgy_filesystem.h"
#include "rgy_ini.h"
#include "rgy_codepage.h"

//行列式の計算
static double det3x3(const std::array<double, 9>& m) {
    return m[0]*m[4]*m[8]
        +m[3]*m[7]*m[2]
        +m[6]*m[1]*m[5]
        -m[0]*m[7]*m[5]
        -m[6]*m[4]*m[2]
        -m[3]*m[1]*m[8];
}

//逆行列の計算
static bool inv3x3(std::array<double, 9>& invm, const std::array<double, 9>& m) {
    const double det = det3x3(m);
    if (std::abs(det) < std::numeric_limits<double>::min()) {
        return false;
    }
    const double inv_det = 1.0 / det;

    invm[0] = inv_det*(m[4]*m[8] - m[5]*m[7]);
    invm[1] = inv_det*(m[2]*m[7] - m[1]*m[8]);
    invm[2] = inv_det*(m[1]*m[5] - m[2]*m[4]);

    invm[3] = inv_det*(m[5]*m[6] - m[3]*m[8]);
    invm[4] = inv_det*(m[0]*m[8] - m[2]*m[6]);
    invm[5] = inv_det*(m[2]*m[3] - m[0]*m[5]);

    invm[6] = inv_det*(m[3]*m[7] - m[4]*m[6]);
    invm[7] = inv_det*(m[1]*m[6] - m[0]*m[7]);
    invm[8] = inv_det*(m[0]*m[4] - m[1]*m[3]);

    return true;
}

//行列xベクトル積
static std::array<double, 3> mul3x3vec(const std::array<double, 9>& m, const std::array<double, 3>& v) {
    std::array<double, 3> a = { 0.0, 0.0, 0.0 };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            a[j] += m[j*3+i] * v[i];
        }
    }
    return a;
}

//2次関数の係数を最小自乗法で求める
std::array<double, 3> leastSquare2nd(const double *x, const double *y, size_t n) {
    std::array<double, 3> a = { 0.0, 0.0, 0.0 };
    if (n <= 1) {
        a[0] = y[0];
    } else if (n <= 2) {
        if (x[1] - x[0] == 0) {
            a[0] = (y[0] + y[1]) * 0.5;
        } else {
            a[1] = (y[1] - y[0]) / (x[1] - x[0]);
            a[0] = y[0] - x[0] / a[1];
        }
    } else {
        std::array<double, 5> Ae;
        std::array<double, 3> b;
        std::fill(Ae.begin(), Ae.end(), 0.0);
        std::fill(b.begin(), b.end(), 0.0);
        for (size_t i = 0; i < n; i++) {
            Ae[0] += 1.0;
            Ae[1] += x[i];
            Ae[2] += x[i] * x[i];
            Ae[3] += x[i] * x[i] * x[i];
            Ae[4] += x[i] * x[i] * x[i] * x[i];
            b[0] += y[i] * x[i] * x[i];
            b[1] += y[i] * x[i];
            b[2] += y[i];
        }
        std::array<double, 9> A; //3x3行列
        A[0] = Ae[4]; A[1] = Ae[3]; A[2] = Ae[2];
        A[3] = Ae[3]; A[4] = Ae[2]; A[5] = Ae[1];
        A[6] = Ae[2]; A[7] = Ae[1]; A[8] = Ae[0];

        std::array<double, 9> invA;
        if (inv3x3(invA, A)) {
            a = mul3x3vec(invA, b);
            std::swap(a[0], a[2]);
        }
    }
    return a;
}

//2次関数の係数の係数から最小値を求める
double minX2nd(const std::array<double, 3>& a) {
    if (a[2] <= 0.0) {
        double y0 = a[0]; //x = 0での値
        double y1 = (a[2] * LOGO_FADE_MAX + a[1]) * LOGO_FADE_MAX + a[0]; //x=LOGO_FADE_MAXでの値
        return y0 < y1 ? 0.0 : (double)LOGO_FADE_MAX;
    }
    //平方完成
    return -0.5 * a[1] / a[2];
}

double quadratic(const std::array<double, 3>& a, double x) {
    return ((a[2] * x) + a[1]) * x + a[0];
}

std::vector<double> quadratic_eq(const std::array<double, 3>& v) {
    double a = v[2], b = v[1], c = v[0];
    std::vector<double> ans;
    const double D = b*b - 4.0*a*c;
    if (D > 0.0) {
        ans.push_back((-b + std::sqrt(D))/(2.0*a));
        ans.push_back((-b - std::sqrt(D))/(2.0*a));
    } else if (D == 0) {
        ans.push_back(-b/(2.0*a));
    }
    return ans;
}

NVEncFilterDelogo::NVEncFilterDelogo() :
    m_LogoFilePath(),
    m_nLogoIdx(-1),
    m_sLogoDataList(),
    m_sProcessData(),
    m_src(),
    m_mask(),
    m_maskAdjusted(),
    m_maskNR(),
    m_maskNRAdjusted(),
    m_maskValidCount(0),
    m_maskThreshold(DELOGO_MASK_THRESHOLD_DEFAULT),
    m_bufDelogo(),
    m_bufDelogoNR(),
    m_bufEval(),
    m_adjMaskMinIndex(),
    m_adjMaskThresholdTest(),
    m_NRProcTemp(),
    m_evalCounter(),
    m_createLogoMaskValidMaskCount(),
    m_adjMaskEachFadeCount(),
    m_adjMaskMinResAndValidMaskCount(),
    m_adjMask2ValidMaskCount(),
    m_adjMask2TargetCount(),
    m_adjMaskStream(),
    m_smoothKernel(),
    m_fadeValueAdjust(),
    m_fadeValueParallel(),
    m_fadeValueTemp(),
    m_fadeArray(),
    m_frameIn(0),
    m_frameOut(0),
    m_yDepth(0),
    m_EnableAutoNR(false),
    m_logPath() {
    m_name = _T("delogo");
}

NVEncFilterDelogo::~NVEncFilterDelogo() {
    close();
}

int NVEncFilterDelogo::readLogoFile(const std::shared_ptr<NVEncFilterParamDelogo> pDelogoParam) {
    int sts = 0;
    if (pDelogoParam->delogo.logoFilePath.length() == 0) {
        return 1;
    }
    if (m_LogoFilePath == pDelogoParam->delogo.logoFilePath) {
        return -1;
    }
    auto file_deleter = [](FILE *fp) {
        fclose(fp);
    };
    AddMessage(RGY_LOG_DEBUG, _T("Opening logo file: %s\n"), pDelogoParam->delogo.logoFilePath.c_str());
    unique_ptr<FILE, decltype(file_deleter)> fp(_tfopen(pDelogoParam->delogo.logoFilePath.c_str(), _T("rb")), file_deleter);
    if (fp.get() == NULL) {
        AddMessage(RGY_LOG_ERROR, _T("could not open logo file \"%s\".\n"), pDelogoParam->delogo.logoFilePath.c_str());
        return 1;
    }
    // ファイルヘッダ取得
    int logo_header_ver = 0;
    LOGO_FILE_HEADER logo_file_header = { 0 };
    if (sizeof(logo_file_header) != fread(&logo_file_header, 1, sizeof(logo_file_header), fp.get())) {
        AddMessage(RGY_LOG_ERROR, _T("invalid logo file.\n"));
        sts = 1;
    } else if (0 == (logo_header_ver = get_logo_file_header_ver(&logo_file_header))) {
        AddMessage(RGY_LOG_ERROR, _T("invalid logo file.\n"));
        sts = 1;
    } else {
        AddMessage(RGY_LOG_DEBUG, _T("logo_header_ver: %d\n"), logo_header_ver);
        const size_t logo_header_size = (logo_header_ver == 2) ? sizeof(LOGO_HEADER) : sizeof(LOGO_HEADER_OLD);
        const int logonum = SWAP_ENDIAN(logo_file_header.logonum.l);
        AddMessage(RGY_LOG_DEBUG, _T("logonum: %d\n"), logonum);
        m_sLogoDataList.resize(logonum);

        for (int i = 0; i < logonum; i++) {
            if (logo_header_size != fread(&m_sLogoDataList[i].header, 1, logo_header_size, fp.get())) {
                AddMessage(RGY_LOG_ERROR, _T("invalid logo file.\n"));
                sts = 1;
                break;
            }
            if (logo_header_ver == 1) {
                convert_logo_header_v1_to_v2(&m_sLogoDataList[i].header);
            }

            const auto logoPixelBytes = logo_pixel_size(&m_sLogoDataList[i].header);

            // メモリ確保
            m_sLogoDataList[i].logoPixel.resize(logoPixelBytes / sizeof(m_sLogoDataList[i].logoPixel[0]), { 0 });

            if (logoPixelBytes != (int)fread(m_sLogoDataList[i].logoPixel.data(), 1, logoPixelBytes, fp.get())) {
                AddMessage(RGY_LOG_ERROR, _T("invalid logo file.\n"));
                sts = 1;
                break;
            }
        }
    }
    m_LogoFilePath = pDelogoParam->delogo.logoFilePath;
    return sts;
}

std::string NVEncFilterDelogo::logoNameList() {
    std::string strlist;
    for (int i = 0; i < (int)m_sLogoDataList.size(); i++) {
        const std::string str = char_to_string(CP_THREAD_ACP, m_sLogoDataList[i].header.name, CODE_PAGE_SJIS);
        strlist += strsprintf("%3d: %s\n", i+1, str.c_str());
    }
    return strlist;
}

int NVEncFilterDelogo::getLogoIdx(const std::string& logoName) {
    int idx = LOGO_AUTO_SELECT_INVALID;
    AddMessage(RGY_LOG_DEBUG, _T("getLogoIdx: \"%s\"\n"), char_to_tstring(logoName).c_str());
    for (int i = 0; i < (int)m_sLogoDataList.size(); i++) {
        const std::string str = char_to_string(CP_THREAD_ACP, m_sLogoDataList[i].header.name, CODE_PAGE_SJIS);
        AddMessage(RGY_LOG_DEBUG, _T("  name: %s\n"), char_to_tstring(str).c_str());
        if (str == logoName) {
            idx = i;
            break;
        }
    }
    return idx;
}

int NVEncFilterDelogo::selectLogo(const tstring& selectStr, const tstring& inputFilename) {
    if (selectStr.length() == 0) {
        if (m_sLogoDataList.size() > 1) {
            AddMessage(RGY_LOG_ERROR, _T("--vpp-delogo-select option is required to select logo from logo pack.\n"));
            AddMessage(RGY_LOG_ERROR, char_to_tstring(logoNameList()));
            return LOGO_AUTO_SELECT_INVALID;
        }
        return 0;
    }

    //ロゴ名として扱い、インデックスを取得
    {
        int idx = getLogoIdx(tchar_to_string(selectStr));
        if (idx != LOGO_AUTO_SELECT_INVALID) {
            return idx;
        }
    }
    //数字として扱い、インデックスを取得
    try {
        int j = std::stoi(selectStr);
        if (0 < j && j <= (int)m_sLogoDataList.size()) {
            return j-1;
        }
    } catch (...) {
        ;//後続の処理へ
    }

    //自動ロゴ選択ファイルか?
    std::string logoName = GetFullPathFrom(tchar_to_string(selectStr).c_str());
    if (!rgy_file_exists(selectStr.c_str())) {
        AddMessage(RGY_LOG_ERROR,
            _T("--vpp-delogo-select option has invalid param.\n")
            _T("Please set logo name or logo index (starting from 1),\n")
            _T("or auto select file.\n"));
        return LOGO_AUTO_SELECT_INVALID;
    }
    //自動選択キー
#if (defined(_WIN32) || defined(_WIN64))
    [[maybe_unused]] uint32_t codepage = CP_THREAD_ACP;
#else
    uint32_t codepage = CODE_PAGE_UNSET;
#endif
    int count = 0;
    for (;; count++) {
        char buf[512] = { 0 };
        GetPrivateProfileStringCP("LOGO_AUTO_SELECT", strsprintf("logo%d", count+1).c_str(), "", buf, sizeof(buf), logoName.c_str(), codepage);
        if (strlen(buf) == 0)
            break;
    }
    if (count == 0) {
        AddMessage(RGY_LOG_ERROR, _T("could not find any key to auto select from \"%s\".\n"), selectStr.c_str());
        return LOGO_AUTO_SELECT_INVALID;
    }
    std::vector<LOGO_SELECT_KEY> logoAutoSelectKeys;
    logoAutoSelectKeys.reserve(count);
    for (int i = 0; i < count; i++) {
        char buf[512] = { 0 };
        GetPrivateProfileStringCP("LOGO_AUTO_SELECT", strsprintf("logo%d", i+1).c_str(), "", buf, sizeof(buf), logoName.c_str(), codepage);
        char *ptr = strchr(buf, ',');
        if (ptr != NULL) {
            LOGO_SELECT_KEY selectKey;
            ptr[0] = '\0';
            selectKey.key = buf;
            strcpy_s(selectKey.logoname, ptr+1);
            logoAutoSelectKeys.push_back(std::move(selectKey));
        }
    }
    for (const auto& selectKey : logoAutoSelectKeys) {
        if (NULL != _tcsstr(inputFilename.c_str(), char_to_tstring(selectKey.key.c_str()).c_str())) {
            logoName = selectKey.logoname;
            return getLogoIdx(logoName);
        }
    }
    return LOGO_AUTO_SELECT_NOHIT;
}

RGY_ERR NVEncFilterDelogo::init(shared_ptr<NVEncFilterParam> pParam, shared_ptr<RGYLog> pPrintMes) {
    RGY_ERR sts = RGY_ERR_NONE;
    m_pLog = pPrintMes;
    auto pDelogoParam = std::dynamic_pointer_cast<NVEncFilterParamDelogo>(pParam);
    if (!pDelogoParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    //delogoは常に元のフレームを書き換え
    if (!pDelogoParam->bOutOverwrite) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid param, delogo will overwrite input frame.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    pDelogoParam->frameOut = pDelogoParam->frameIn;

    //パラメータチェック
    int ret_logofile = readLogoFile(pDelogoParam);
    if (ret_logofile > 0) {
        return RGY_ERR_INVALID_PARAM;
    }
    const int logoidx = selectLogo(pDelogoParam->delogo.logoSelect, pDelogoParam->inputFileName);
    if (logoidx < 0) {
        if (logoidx == LOGO_AUTO_SELECT_NOHIT) {
            AddMessage(RGY_LOG_ERROR, _T("no logo was selected by auto select \"%s\".\n"), pDelogoParam->delogo.logoSelect.c_str());
            return RGY_ERR_INVALID_PARAM;
        } else {
            AddMessage(RGY_LOG_ERROR, _T("could not select logo by \"%s\".\n"), pDelogoParam->delogo.logoSelect.c_str());
            AddMessage(RGY_LOG_ERROR, char_to_tstring(logoNameList()));
            return RGY_ERR_INVALID_PARAM;
        }
    }
    if (pDelogoParam->frameOut.height <= 0 || pDelogoParam->frameOut.width <= 0) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pDelogoParam->delogo.NRArea < 0 || 3 < pDelogoParam->delogo.NRArea) {
        AddMessage(RGY_LOG_ERROR, _T("nr_area must be in range of 0 - 3.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pDelogoParam->delogo.NRValue < 0 || 4 < pDelogoParam->delogo.NRValue) {
        AddMessage(RGY_LOG_ERROR, _T("nr_value must be in range of 0 - 4.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (ret_logofile == 0 || m_nLogoIdx != logoidx) {
        m_nLogoIdx = logoidx;
        m_param = pDelogoParam;

        auto& logoData = m_sLogoDataList[m_nLogoIdx];
        if (pDelogoParam->delogo.posX || pDelogoParam->delogo.posY) {
            LogoData origData;
            origData.header = logoData.header;
            origData.logoPixel = logoData.logoPixel;

            logoData.logoPixel = std::vector<LOGO_PIXEL>((logoData.header.w + 1) * (logoData.header.h + 1), { 0 });

            create_adj_exdata(logoData.logoPixel.data(), &logoData.header, origData.logoPixel.data(), &origData.header, pDelogoParam->delogo.posX, pDelogoParam->delogo.posY);
        }
        const int frameWidth  = pDelogoParam->frameIn.width;
        const int frameHeight = pDelogoParam->frameIn.height;

        const int alignWidthMinusOne = ((pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) ? 4 : 64) - 1;
        auto logo_multi = get_logo_multi_data(logoData.header.w, logoData.header.h, frameWidth, frameHeight);
        if (pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) {
            logoData.header.x = 0;
            logoData.header.y = 0;
        }

        m_sProcessData[LOGO__Y].offset[0] = (short)pDelogoParam->delogo.Y  << 4;
        m_sProcessData[LOGO__Y].offset[1] = (short)pDelogoParam->delogo.Y  << 4;
        m_sProcessData[LOGO_UV].offset[0] = (short)pDelogoParam->delogo.Cb << 4;
        m_sProcessData[LOGO_UV].offset[1] = (short)pDelogoParam->delogo.Cr << 4;
        m_sProcessData[LOGO__U].offset[0] = (short)pDelogoParam->delogo.Cb << 4;
        m_sProcessData[LOGO__U].offset[1] = (short)pDelogoParam->delogo.Cb << 4;
        m_sProcessData[LOGO__V].offset[0] = (short)pDelogoParam->delogo.Cr << 4;
        m_sProcessData[LOGO__V].offset[1] = (short)pDelogoParam->delogo.Cr << 4;

        m_sProcessData[LOGO__Y].fade = 256;
        m_sProcessData[LOGO_UV].fade = 256;
        m_sProcessData[LOGO__U].fade = 256;
        m_sProcessData[LOGO__V].fade = 256;

        m_sProcessData[LOGO__Y].depth = pDelogoParam->delogo.depth;
        m_sProcessData[LOGO_UV].depth = pDelogoParam->delogo.depth;
        m_sProcessData[LOGO__U].depth = pDelogoParam->delogo.depth;
        m_sProcessData[LOGO__V].depth = pDelogoParam->delogo.depth;

        m_sProcessData[LOGO__Y].i_start = (pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) ? 0 : (std::min)(logoData.header.x & (~alignWidthMinusOne), frameWidth);
        m_sProcessData[LOGO__Y].width   = (((std::min)(logoData.header.x + logoData.header.w, frameWidth) + alignWidthMinusOne) & (~alignWidthMinusOne)) - m_sProcessData[LOGO__Y].i_start;
        m_sProcessData[LOGO_UV].i_start = m_sProcessData[LOGO__Y].i_start;
        m_sProcessData[LOGO_UV].width   = m_sProcessData[LOGO__Y].width;
        m_sProcessData[LOGO__U].i_start = m_sProcessData[LOGO__Y].i_start >> 1;
        m_sProcessData[LOGO__U].width   = m_sProcessData[LOGO__Y].width >> 1;
        m_sProcessData[LOGO__V].i_start = m_sProcessData[LOGO__U].i_start;
        m_sProcessData[LOGO__V].width   = m_sProcessData[LOGO__U].width;
        const int yWidthOffset = logoData.header.x - m_sProcessData[LOGO__Y].i_start;

        m_sProcessData[LOGO__Y].j_start = (pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) ? 0 : (std::min)((int)logoData.header.y, frameHeight);
        m_sProcessData[LOGO__Y].height  = (std::min)(logoData.header.y + logoData.header.h, frameHeight) - m_sProcessData[LOGO__Y].j_start;
        m_sProcessData[LOGO_UV].j_start = logoData.header.y >> 1;
        m_sProcessData[LOGO_UV].height  = (((logoData.header.y + logoData.header.h + 1) & ~1) - (m_sProcessData[LOGO_UV].j_start << 1)) >> 1;
        m_sProcessData[LOGO__U].j_start = m_sProcessData[LOGO_UV].j_start;
        m_sProcessData[LOGO__U].height  = m_sProcessData[LOGO_UV].height;
        m_sProcessData[LOGO__V].j_start = m_sProcessData[LOGO__U].j_start;
        m_sProcessData[LOGO__V].height  = m_sProcessData[LOGO__U].height;

        if (logoData.header.x >= frameWidth || logoData.header.y >= frameHeight) {
            AddMessage(RGY_LOG_ERROR, _T("\"%s\" was not included in frame size %dx%d.\ndelogo disabled.\n"), pDelogoParam->delogo.logoSelect.c_str(), frameWidth, frameHeight);
            AddMessage(RGY_LOG_ERROR, _T("logo pos x=%d, y=%d, including pos offset value %d:%d.\n"), logoData.header.x, logoData.header.y, pDelogoParam->delogo.posX, pDelogoParam->delogo.posY);
            return RGY_ERR_INVALID_PARAM;
        }

        m_sProcessData[LOGO__Y].pLogoPtr.reset((int16_t *)_aligned_malloc(sizeof(int16_t) * 2 * m_sProcessData[LOGO__Y].width * m_sProcessData[LOGO__Y].height, 32));
        m_sProcessData[LOGO_UV].pLogoPtr.reset((int16_t *)_aligned_malloc(sizeof(int16_t) * 2 * m_sProcessData[LOGO_UV].width * m_sProcessData[LOGO_UV].height, 32));
        m_sProcessData[LOGO__U].pLogoPtr.reset((int16_t *)_aligned_malloc(sizeof(int16_t) * 2 * m_sProcessData[LOGO__U].width * m_sProcessData[LOGO__U].height, 32));
        m_sProcessData[LOGO__V].pLogoPtr.reset((int16_t *)_aligned_malloc(sizeof(int16_t) * 2 * m_sProcessData[LOGO__V].width * m_sProcessData[LOGO__V].height, 32));

        memset(m_sProcessData[LOGO__Y].pLogoPtr.get(), 0, sizeof(int16_t) * 2 * m_sProcessData[LOGO__Y].width * m_sProcessData[LOGO__Y].height);
        memset(m_sProcessData[LOGO_UV].pLogoPtr.get(), 0, sizeof(int16_t) * 2 * m_sProcessData[LOGO_UV].width * m_sProcessData[LOGO_UV].height);
        memset(m_sProcessData[LOGO__U].pLogoPtr.get(), 0, sizeof(int16_t) * 2 * m_sProcessData[LOGO__U].width * m_sProcessData[LOGO__U].height);
        memset(m_sProcessData[LOGO__V].pLogoPtr.get(), 0, sizeof(int16_t) * 2 * m_sProcessData[LOGO__V].width * m_sProcessData[LOGO__V].height);

        //まず輝度成分をコピーしてしまう
        for (int j = 0; j < m_sProcessData[LOGO__Y].height; j++) {
            //輝度成分はそのままコピーするだけ
            for (int i = 0; i < logoData.header.w; i++) {
                int16x2_t logoY = *(int16x2_t *)&logoData.logoPixel[j * logoData.header.w + i].dp_y;
                ((int16x2_t *)m_sProcessData[LOGO__Y].pLogoPtr.get())[j * m_sProcessData[LOGO__Y].width + i + yWidthOffset] = logoY;
            }
        }
        //まずは4:4:4->4:2:0処理時に端を気にしなくていいよう、縦横ともに2の倍数となるよう拡張する
        //CbCrの順番に並べていく
        //0で初期化しておく
        std::vector<int16x2_t> bufferCbCr444ForShrink(2 * m_sProcessData[LOGO_UV].height * 2 * m_sProcessData[LOGO__Y].width, { 0, 0 });
        int j_src = 0; //読み込み側の行
        int j_dst = 0; //書き込み側の行
        auto copyUVLineForShrink = [&]() {
            for (int i = 0; i < logoData.header.w; i++) {
                int16x2_t logoCb = *(int16x2_t *)&logoData.logoPixel[j_src * logoData.header.w + i].dp_cb;
                int16x2_t logoCr = *(int16x2_t *)&logoData.logoPixel[j_src * logoData.header.w + i].dp_cr;
                bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + i + yWidthOffset) * 2 + 0] = logoCb;
                bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + i + yWidthOffset) * 2 + 1] = logoCr;
            }
            if (yWidthOffset & 1) {
                //奇数列はじまりなら、それをその前の偶数列に拡張する
                int16x2_t logoCb = *(int16x2_t *)&bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + 0 + yWidthOffset) * 2 + 0];
                int16x2_t logoCr = *(int16x2_t *)&bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + 0 + yWidthOffset) * 2 + 1];
                bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + 0 + yWidthOffset - 1) * 2 + 0] = logoCb;
                bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + 0 + yWidthOffset - 1) * 2 + 1] = logoCr;
            }
            if ((yWidthOffset + logoData.header.w) & 1) {
                //偶数列おわりなら、それをその次の奇数列に拡張する
                int16x2_t logoCb = *(int16x2_t *)&bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + logoData.header.w + yWidthOffset) * 2 + 0];
                int16x2_t logoCr = *(int16x2_t *)&bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + logoData.header.w + yWidthOffset) * 2 + 1];
                bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + logoData.header.w + yWidthOffset) * 2 + 0] = logoCb;
                bufferCbCr444ForShrink[(j_dst * m_sProcessData[LOGO_UV].width + logoData.header.w + yWidthOffset) * 2 + 1] = logoCr;
            }
        };
        if (logoData.header.y & 1) {
            copyUVLineForShrink();
            j_dst++; //書き込み側は1行進める
        }
        for (; j_src < logoData.header.h; j_src++, j_dst++) {
            copyUVLineForShrink();
        }
        if ((logoData.header.y + logoData.header.h) & 1) {
            j_src--; //読み込み側は1行戻る
            copyUVLineForShrink();
        }

        //実際に縮小処理を行う
        //2x2->1x1の処理なのでインクリメントはそれぞれ2ずつ
        for (int j = 0; j < m_sProcessData[LOGO__Y].height; j += 2) {
            for (int i = 0; i < m_sProcessData[LOGO_UV].width; i += 2) {
                int16x2_t logoCb0 = bufferCbCr444ForShrink[((j + 0) * m_sProcessData[LOGO_UV].width + i + 0) * 2 + 0];
                int16x2_t logoCr0 = bufferCbCr444ForShrink[((j + 0) * m_sProcessData[LOGO_UV].width + i + 0) * 2 + 1];
                int16x2_t logoCb1 = bufferCbCr444ForShrink[((j + 0) * m_sProcessData[LOGO_UV].width + i + 1) * 2 + 0];
                int16x2_t logoCr1 = bufferCbCr444ForShrink[((j + 0) * m_sProcessData[LOGO_UV].width + i + 1) * 2 + 1];
                int16x2_t logoCb2 = bufferCbCr444ForShrink[((j + 1) * m_sProcessData[LOGO_UV].width + i + 0) * 2 + 0];
                int16x2_t logoCr2 = bufferCbCr444ForShrink[((j + 1) * m_sProcessData[LOGO_UV].width + i + 0) * 2 + 1];
                int16x2_t logoCb3 = bufferCbCr444ForShrink[((j + 1) * m_sProcessData[LOGO_UV].width + i + 1) * 2 + 0];
                int16x2_t logoCr3 = bufferCbCr444ForShrink[((j + 1) * m_sProcessData[LOGO_UV].width + i + 1) * 2 + 1];

                int16x2_t logoCb, logoCr;
                logoCb.x = (logoCb0.x + logoCb1.x + logoCb2.x + logoCb3.x + 2) >> 2;
                logoCb.y = (logoCb0.y + logoCb1.y + logoCb2.y + logoCb3.y + 2) >> 2;
                logoCr.x = (logoCr0.x + logoCr1.x + logoCr2.x + logoCr3.x + 2) >> 2;
                logoCr.y = (logoCr0.y + logoCr1.y + logoCr2.y + logoCr3.y + 2) >> 2;

                //単純平均により4:4:4->4:2:0に
                ((int16x2_t *)m_sProcessData[LOGO_UV].pLogoPtr.get())[(j >> 1) * m_sProcessData[LOGO_UV].width * 1 + (i >> 1) * 2 + 0] = logoCb;
                ((int16x2_t *)m_sProcessData[LOGO_UV].pLogoPtr.get())[(j >> 1) * m_sProcessData[LOGO_UV].width * 1 + (i >> 1) * 2 + 1] = logoCr;
                ((int16x2_t *)m_sProcessData[LOGO__U].pLogoPtr.get())[(j >> 1) * m_sProcessData[LOGO__U].width * 1 + (i >> 1) * 1] = logoCb;
                ((int16x2_t *)m_sProcessData[LOGO__V].pLogoPtr.get())[(j >> 1) * m_sProcessData[LOGO__V].width * 1 + (i >> 1) * 1] = logoCr;
            }
        }

        for (uint32_t i = 0; i < _countof(m_sProcessData); i++) {
            auto uptr = std::make_unique<CUFrameBuf>((int)(m_sProcessData[i].width * sizeof(int16x2_t)), m_sProcessData[i].height);
            uptr->frame.singleAlloc = true;
            sts = uptr->alloc();
            if (sts != RGY_ERR_NONE) {
                m_frameBuf.clear();
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for logo data %d: %s.\n"),
                    i, get_err_mes(sts));
                return sts;
            }
            m_sProcessData[i].pDevLogo = std::move(uptr);
            //ロゴデータをGPUに転送
            sts = err_to_rgy(cudaMemcpy2DAsync(m_sProcessData[i].pDevLogo->frame.ptr[0], m_sProcessData[i].pDevLogo->frame.pitch[0],
                (void *)m_sProcessData[i].pLogoPtr.get(), m_sProcessData[i].width * sizeof(int16x2_t),
                m_sProcessData[i].width * sizeof(int16x2_t), m_sProcessData[i].height, cudaMemcpyHostToDevice));
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("error at sending logo data %d cudaMemcpy2DAsync(%s): %s.\n"),
                    i,
                    getCudaMemcpyKindStr(cudaMemcpyHostToDevice),
                    get_err_mes(sts));
                return sts;
            }
        }

        if (pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) {
            auto logo_multi_data = get_logo_multi_data(m_sProcessData[LOGO__Y].width, m_sProcessData[LOGO__Y].height, pDelogoParam->frameOut.width, pDelogoParam->frameOut.height);
            const int block_count = logo_multi_data.block_x * logo_multi_data.block_y;

            m_DepthHost.resize(block_count);
            AddMessage(RGY_LOG_INFO, _T("logo multi block size %dx%d=%d\n"), logo_multi_data.block_x, logo_multi_data.block_y, block_count);

            m_Depth.reset(new CUMemBuf(sizeof(float) * block_count));
            m_Depth->alloc();
            for (uint32_t i = 0; i < _countof(m_sProcessData); i++) {
                m_sProcessData[i].pBlockDepth = m_Depth;
            }

            FILE *fp = nullptr;
            tstring depthfile = tstring(pDelogoParam->outputFileName) + _T(".depth.dat");
            if (_tfopen_s(&fp, depthfile.c_str(), _T("wb"))) {
                AddMessage(RGY_LOG_INFO, _T("error opening depth file \"%s\"\n"), depthfile.c_str());
                return RGY_ERR_FILE_OPEN;
            }
            m_fpDepth.reset(fp);
        }

        if (pDelogoParam->delogo.autoFade
            || pDelogoParam->delogo.autoNR
            || pDelogoParam->delogo.NRValue > 0) {
            const int logo_w     = m_sProcessData[LOGO__Y].width;
            const int logo_h     = m_sProcessData[LOGO__Y].height;

            //自動フェード関連のメモリ確保
            sts = m_src.alloc(pDelogoParam->frameIn);
            if (sts != RGY_ERR_NONE) {
                m_frameBuf.clear();
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for frame buffer: %s.\n"),
                    get_err_mes(sts));
                return RGY_ERR_MEMORY_ALLOC;
            }
            m_mask.reset(           new CUFrameBuf(logo_w, logo_h, RGY_CSP_Y16));
            m_maskAdjusted.reset(   new CUFrameBuf(logo_w, logo_h, RGY_CSP_Y16));
            m_maskNR.reset(         new CUFrameBuf(logo_w, logo_h, RGY_CSP_Y16));
            m_maskNRAdjusted.reset( new CUFrameBuf(logo_w, logo_h, RGY_CSP_Y16));
            m_mask->frame.singleAlloc           = true;
            m_maskAdjusted->frame.singleAlloc   = true;
            m_maskNR->frame.singleAlloc         = true;
            m_maskNRAdjusted->frame.singleAlloc = true;
            sts = m_mask->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_mask: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }
            sts = m_maskAdjusted->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_maskAdjusted: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }
            sts = m_maskNR->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_maskNR: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }
            sts = m_maskNRAdjusted->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_maskNRAdjusted: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            const int mask_pitch = m_mask->frame.pitch[0];
            auto pitch_check = [=](int frame_pitch, const TCHAR *buf_name) {
                if (mask_pitch != frame_pitch) {
                    AddMessage(RGY_LOG_ERROR, _T("%s pitch does not match mask pitch: logo=%d, %s=%d.\n"), buf_name, mask_pitch, buf_name, frame_pitch);
                    return RGY_ERR_UNSUPPORTED;
                }
                return RGY_ERR_NONE;
            };
            if (pitch_check(m_maskAdjusted->frame.pitch[0], _T("m_maskAdjusted")) != RGY_ERR_NONE) return RGY_ERR_INVALID_PARAM;
            if (pitch_check(m_maskNR->frame.pitch[0], _T("m_maskNR")) != RGY_ERR_NONE) return RGY_ERR_INVALID_PARAM;
            if (pitch_check(m_maskNRAdjusted->frame.pitch[0], _T("m_maskNRAdjusted")) != RGY_ERR_NONE) return RGY_ERR_INVALID_PARAM;

            m_adjMaskMinIndex.reset(new CUFrameBuf(logo_w, logo_h, RGY_CSP_Y8));
            m_adjMaskMinIndex->frame.singleAlloc = true;
            sts = m_adjMaskMinIndex->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_adjMaskMinIndex: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            m_adjMaskThresholdTest.reset(new CUFrameBuf(logo_w, logo_h * DELOGO_ADJMASK_DIV_COUNT, RGY_CSP_Y16));
            m_adjMaskThresholdTest->frame.singleAlloc = true;
            sts = m_adjMaskThresholdTest->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_adjMaskThresholdTest: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            if (   m_bufDelogo.size() != m_bufDelogoNR.size()
                || m_bufDelogo.size() != m_bufEval.size()
                || m_bufDelogo.size() != m_evalCounter.size()) {
                AddMessage(RGY_LOG_ERROR, _T("internal error, invalid array size\n"),
                    get_err_mes(sts));
                return RGY_ERR_INVALID_PARAM;
            }

            for (size_t i = 0; i < m_bufDelogo.size(); i++) {
                m_bufDelogo[i].reset(  new CUFrameBuf(logo_w, logo_h * DELOGO_PARALLEL_FADE, RGY_CSP_Y16));
                m_bufDelogoNR[i].reset(new CUFrameBuf(logo_w, logo_h * DELOGO_PARALLEL_FADE, RGY_CSP_Y16));
                m_bufEval[i].reset(    new CUFrameBuf(logo_w, logo_h * DELOGO_PARALLEL_FADE, RGY_CSP_Y16));
                m_bufDelogo[i]->frame.singleAlloc   = true;
                m_bufDelogoNR[i]->frame.singleAlloc = true;
                m_bufEval[i]->frame.singleAlloc     = true;
                sts = m_bufDelogo[i]->alloc();
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_bufDelogo[%d]: %s.\n"),
                        i, get_err_mes(sts));
                    return sts;
                }
                sts = m_bufDelogoNR[i]->alloc();
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_bufDelogoNR[%d]: %s.\n"),
                        i, get_err_mes(sts));
                    return sts;
                }
                sts = m_bufEval[i]->alloc();
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_bufEval[%d]: %s.\n"),
                        i, get_err_mes(sts));
                    return sts;
                }
                if (pitch_check(m_bufDelogo[i]->frame.pitch[0], _T("m_bufDelogo[i]")) != RGY_ERR_NONE) return RGY_ERR_INVALID_PARAM;
                if (pitch_check(m_bufDelogoNR[i]->frame.pitch[0], _T("m_bufDelogoNR[i]")) != RGY_ERR_NONE) return RGY_ERR_INVALID_PARAM;
                if (pitch_check(m_bufEval[i]->frame.pitch[0], _T("m_bufEval[i]")) != RGY_ERR_NONE) return RGY_ERR_INVALID_PARAM;

                const int maxBlocks = DELOGO_PARALLEL_FADE * divCeil(logo_w, DELOGO_BLOCK_X * 4) * divCeil(logo_h, DELOGO_BLOCK_Y * DELOGO_BLOCK_LOOP_Y);
                sts = m_evalCounter[i].alloc(sizeof(float) * maxBlocks);
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_evalCounter[%d]: %s.\n"),
                        i, get_err_mes(sts));
                    return sts;
                }

                sts = m_evalStream[i].init(pDelogoParam->cudaSchedule);
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to create stream or event for m_evalStream[%d]: %s.\n"),
                        i, get_err_mes(sts));
                    return sts;
                }
            }
            m_NRProcTemp.reset(new CUFrameBuf(logo_w, logo_h, RGY_CSP_BIT_DEPTH[pDelogoParam->frameIn.csp] > 8 ? RGY_CSP_Y16 : RGY_CSP_Y8));
            m_NRProcTemp->frame.singleAlloc = true;
            sts = m_NRProcTemp->alloc();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_NRProcTemp: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            const int maxBlocks = divCeil(logo_w, DELOGO_BLOCK_X * 4) * divCeil(logo_h, DELOGO_BLOCK_Y);
            sts = m_adjMaskMinResAndValidMaskCount.alloc(sizeof(int2) * maxBlocks);
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_adjMaskMinResAndValidMaskCount: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }
            sts = m_adjMaskEachFadeCount.alloc(sizeof(int) * (DELOGO_PRE_DIV_COUNT+1));
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_adjMaskEachFadeCount: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }
            sts = m_adjMask2ValidMaskCount.alloc(sizeof(int) * (1 + maxBlocks * DELOGO_ADJMASK_DIV_COUNT));
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_adjMask2ValidMaskCount: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            sts = m_adjMaskStream.init(pDelogoParam->cudaSchedule, true);
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to create stream or event for m_adjMaskStream: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            if (!m_adjMask2TargetCount) {
                void *ptr = nullptr;
                sts = err_to_rgy(cudaMalloc(&ptr, sizeof(int)));
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_adjMask2TargetCount: %s.\n"),
                        get_err_mes(sts));
                    return sts;
                }
                m_adjMask2TargetCount = unique_ptr<void, cudadevice_deleter>(ptr, cudadevice_deleter());
            }

            if (!m_smoothKernel) {
                void *ptr = nullptr;
                sts = err_to_rgy(cudaMalloc(&ptr, sizeof(float) * (LOGO_NR_MAX * 2 + 1)));
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_smoothKernel: %s.\n"),
                        get_err_mes(sts));
                    return sts;
                }
                m_smoothKernel = unique_ptr<void, cudadevice_deleter>(ptr, cudadevice_deleter());
                std::array<float, LOGO_NR_MAX * 2 + 1> smooth_kernel;
                std::fill(smooth_kernel.begin(), smooth_kernel.end(), 1.0f);
                sts = err_to_rgy(cudaMemcpy(m_smoothKernel.get(), smooth_kernel.data(), smooth_kernel.size() * sizeof(smooth_kernel[0]), cudaMemcpyHostToDevice));
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to copy smooth_kernel: %s.\n"),
                        get_err_mes(sts));
                    return sts;
                }
            }

            if (m_fadeValueAdjust.nSize == 0) {
                sts = m_fadeValueAdjust.alloc(sizeof(float) * (DELOGO_PRE_DIV_COUNT+1));
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_fadeValueAdjust: %s.\n"),
                        get_err_mes(sts));
                    return sts;
                }
            }
            auto fade_cpu = (float *)m_fadeValueAdjust.ptrHost;
            for (int i = 0; i <= DELOGO_PRE_DIV_COUNT; i++) {
                fade_cpu[i] = (float)LOGO_FADE_MAX * m_sProcessData[LOGO__Y].depth * i / (DELOGO_PRE_DIV_COUNT - 1);
            }
            sts = m_fadeValueAdjust.copyHtoD();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to copy fade_cpu: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            if (m_fadeValueParallel.nSize == 0) {
                sts = m_fadeValueParallel.alloc(sizeof(float) * DELOGO_PARALLEL_FADE);
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_fadeValueParallel: %s.\n"),
                        get_err_mes(sts));
                    return sts;
                }
            }
            auto parallel_fade = (float *)m_fadeValueParallel.ptrHost;
            for (int i = 0; i < DELOGO_PARALLEL_FADE; i++) {
                parallel_fade[i] = (float)LOGO_FADE_MAX * m_sProcessData[LOGO__Y].depth * i * (1.0f / 16.0f);
            }
            sts = m_fadeValueParallel.copyHtoD();
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to copy parallel_fade: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }

            if (m_fadeValueTemp.nSize == 0) {
                sts = m_fadeValueTemp.alloc(sizeof(float));
                if (sts != RGY_ERR_NONE) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for m_fadeValueTemp: %s.\n"),
                        get_err_mes(sts));
                    return sts;
                }
            }
            if (RGY_ERR_NONE != (sts = createLogoMask(cudaStreamPerThread))) {
                return sts;
            }
            if (RGY_ERR_NONE != (sts = createNRMask(m_maskNR.get(), m_mask.get(), pDelogoParam->delogo.NRArea, cudaStreamPerThread))) {
                return sts;
            }
        }

        auto logo_name = char_to_string(CP_THREAD_ACP, logoData.header.name, CODE_PAGE_SJIS);;
        tstring delogoInfo = _T("delgo:") + char_to_tstring(logo_name) + pDelogoParam->print();
        if (pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) {
            delogoInfo += strsprintf(_T("\n               multiadd block count %dx%d, logo size %dx%d, block size %dx%d, pad %dx%d, offset %dx%d"),
                logo_multi.block_x, logo_multi.block_y,
                logoData.header.w, logoData.header.h,
                logo_multi.block_width, logo_multi.block_height,
                LOGO_MULTI_PADDING, LOGO_MULTI_PADDING,
                logo_multi.block_offset_x, logo_multi.block_offset_y);
        }
        setFilterInfo(delogoInfo);
        if (pDelogoParam->delogo.log) {
            m_logPath = pDelogoParam->inputFileName + tstring(_T(".delogo_log.csv"));
            std::unique_ptr<FILE, fp_deleter> fp(_tfopen(m_logPath.c_str(), _T("w")), fp_deleter());
            _ftprintf(fp.get(), _T("%s\n\n"), m_infoStr.c_str());
            _ftprintf(fp.get(), _T(", NR, fade (adj), fade (raw)\n"));
            fp.reset();
        }
        m_nFramesProcessed = 0;
    }
    return sts;
}

tstring NVEncFilterParamDelogo::print() const {
    return delogo.print();
}

RGY_ERR NVEncFilterDelogo::createLogoMask(cudaStream_t stream) {
    for (float target_ratio = 0.1f; target_ratio >= 0.01f; target_ratio -= 0.01f) {
        for (float threshold = (float)DELOGO_MASK_THRESHOLD_DEFAULT; threshold >= 200.0f; threshold *= 0.95f) {
            auto sts = createLogoMask((int)(threshold + 0.5f), stream);
            if (sts != RGY_ERR_NONE) {
                return sts;
            };
            const float ratio = m_maskValidCount / (float)(m_sProcessData[LOGO__Y].width * m_sProcessData[LOGO__Y].height);
            AddMessage(RGY_LOG_DEBUG, _T("mask threshold %d, valid count %d [%.4f].\n"),
                (int)(threshold + 0.5f), m_maskValidCount, ratio * 100.0f);
            if (ratio >= target_ratio) {
                m_maskThreshold = (int)(threshold + 0.5f);
                return RGY_ERR_NONE;
            }
        }
    }
    return RGY_ERR_UNKNOWN;
}

RGY_ERR NVEncFilterDelogo::autoFadeLS2(float& auto_fade, const int nr_value) {
    if (m_fadeValueParallel.nSize != sizeof(float) * DELOGO_PARALLEL_FADE) {
        AddMessage(RGY_LOG_ERROR, _T("m_fadeValueParallel.nSize != sizeof(float) * DELOGO_PARALLEL_FADE (%d != %d).\n"),
            m_fadeValueParallel.nSize, sizeof(float) * DELOGO_PARALLEL_FADE);
        return RGY_ERR_INVALID_PARAM;
    }

    std::vector<float> eval(DELOGO_PARALLEL_FADE);
    auto sts = autoFadeCoef2Collect(eval, nr_value, *m_evalStream[nr_value].heEvalCopyFin.get());
    if (sts != RGY_ERR_NONE) return sts;

    const double depth_inv = 1.0 / m_sProcessData[LOGO__Y].depth;
    std::array<double, DELOGO_PARALLEL_FADE> x, y;
    for (size_t i = 0; i < x.size(); i++) {
        x[i] = ((const float *)m_fadeValueParallel.ptrHost)[i] * depth_inv;
        y[i] = (double)eval[i];
    }
    size_t minIdx = (int)std::distance(y.begin(), std::min_element(y.begin(), y.end()));
    if (minIdx == 0 || minIdx == x.size()-1) {
        const auto a = leastSquare2nd(x.data(), y.data(), x.size());
        auto_fade = (float)minX2nd(a);
    } else {
        //最小値の位置で、2つに分けて評価する
        auto a0 = leastSquare2nd(&x[0],      &y[0],      minIdx);
        auto a1 = leastSquare2nd(&x[minIdx], &y[minIdx], x.size() - minIdx);
        decltype(a0) a2;
        for (size_t i = 0; i < a2.size(); i++) {
            a2[i] = a1[i] - a0[i];
        }
        const auto ansA2 = quadratic_eq(a2);
        const auto minX0 = minX2nd(a0);
        const auto minX1 = minX2nd(a1);
        const auto minY0 = quadratic(a0, minX0);
        const auto minY1 = quadratic(a1, minX1);
        const bool minX0inRange = x[0] <= minX0 && minX0 <= x[minIdx];
        const bool minX1inRange = x[minIdx] <= minX1 && minX1 <= x.back();

        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        if (minX0inRange && minX1inRange) {
            minX = (minY0 <= minY1) ? minX0 : minX1;
            minY = std::min(minY0, minY1);
        } else if (minX0inRange) {
            minX = (minY0 <= quadratic(a1, x[minIdx])) ? minX0 : x[minIdx];
            minY = std::min(minY0, quadratic(a1, x[minIdx]));
        } else if (minX1inRange) {
            minX = (quadratic(a0, x[minIdx]) <= minY1) ? x[minIdx] : minX1;
            minY = std::min(quadratic(a0, x[minIdx]), minY1);
        }
        for (auto d : ansA2) {
            if (x.front() <= d && d <= x.back()) {
                if (quadratic(a1, d) < minY
                    || (0 < minIdx && minIdx < x.size()-1
                        && x[minIdx-1] < d && d < x[minIdx+1])) {
                    minY = quadratic(a1, d);
                    minX = d;
                }
            }
        }
        auto_fade = (float)minX;
#if 0
        std::ofstream file;
        file.open(strsprintf("test%05d.csv", m_frameOut), std::ios::out);
        for (size_t i = 0; i < x.size(); i++) {
            file << x[i] << "," << y[i] << "," << quadratic(a0, x[i]) << "," << quadratic(a1, x[i]) << std::endl;
        }
        file << std::endl;
        file << "minX = " << minX << std::endl;
        file << "y0 = " << a0[2] << " * x2 + " << a0[1] << " * x1 + " << a0[0] << std::endl;
        file << "y1 = " << a1[2] << " * x2 + " << a1[1] << " * x1 + " << a1[0] << std::endl;
        file.close();
#endif
    }
    auto_fade = clamp(auto_fade, 0.0f, LOGO_FADE_MAX * 1.15f);

    return RGY_ERR_NONE;
}

#if 0
NVENCSTATUS NVEncFilterDelogo::autoFade4(float& auto_fade, const RGYFrameInfo *frame_logo, const int nr_value, const int nr_area) {
    float minFade = 0.0f;
    float maxFade = LOGO_FADE_MAX * 1.15f;
    std::array<float, 5> results;
    int n1st = 1;

    {
        auto minResult = std::numeric_limits<float>::max();
        int minPos = -1;
        while (maxFade - minFade > 4.0f) {
            float devide = (maxFade - minFade) * 0.25f;
            //fade値は大きな方から調査する
            //初回は4～0, 2回目以降は3～1のBlock境界を調査する
            for (int i = 3 + n1st; i >= 1 - n1st; i--) {
                auto fade = minFade + devide * i;
                *(float *)m_fadeValueTemp.ptrHost = fade * m_sProcessData[LOGO__Y].depth;
                m_fadeValueTemp.copyHtoD();

                std::vector<float> eval(1);
                auto sts = autoFadeCoef2Run(false, frame_logo, nr_value, nr_area, (const float *)m_fadeValueTemp.ptrDevice, (int)eval.size());
                if (sts != RGY_ERR_NONE) return sts;

                sts = autoFadeCoef2Collect(eval, nr_value);
                if (sts != RGY_ERR_NONE) return sts;

                results[i] = eval[0];
                if (eval[0] < minResult) {
                    minResult = eval[0];
                    minPos = i;
                }
            }
            if (n1st) n1st = 0; // ２回目以降は3～1のみ調査する.
            if (minPos == 0) {
                // 次に0～2を調査する場合
                maxFade = minFade + devide;
                results[4] = results[2];
            } else if (minPos == 4) {
                // 次に2～4を調査する場合
                minFade += devide * 2.0f;
                results[0] = results[2];
            } else if (minPos == -2) {
                //最小値が更新されなかった場合は中央部を拡大して調査する
                maxFade -= devide;
                minFade += devide;
                results[0] = results[1];
                results[4] = results[3];
            } else {
                // 次に(minPos-1)～(minPos+1)を調査する場合
                maxFade = minFade + (devide * (minPos + 1));
                minFade += (devide * (minPos - 1));
                results[0] = results[minPos - 1];
                results[4] = results[minPos + 1];
                minPos = -2;
            }
        }
    }

    // 3要素以下になったら全ての要素を調査する
    auto minResult = (results[4] <= results[0]) ? results[4]: results[0];
    auto_fade = (results[4] <= results[0]) ? maxFade : minFade;
    for (auto fade = maxFade - 1.0f; fade >= minFade + 1.0f; fade--) {
        *(float *)m_fadeValueTemp.ptrHost = fade * m_sProcessData[LOGO__Y].depth;
        m_fadeValueTemp.copyHtoD();

        std::vector<float> eval(1);
        auto sts = autoFadeCoef2Run(false, frame_logo, nr_value, nr_area, (const float *)m_fadeValueTemp.ptrDevice, (int)eval.size());
        if (sts != RGY_ERR_NONE) return sts;

        sts = autoFadeCoef2Collect(eval, nr_value);
        if (sts != RGY_ERR_NONE) return sts;

        if (eval[0] < minResult) {
            minResult = eval[0];
            auto_fade = fade;
        }
    }
    return RGY_ERR_NONE;
}
#endif

RGY_ERR NVEncFilterDelogo::calcAutoFadeNRFrame(int& auto_nr, float& auto_fade, const RGYFrameInfo *pFrame, cudaStream_t stream) {
    // Frame毎に調整したMaskの作成
    auto sts = createAdjustedMask(pFrame, stream);
    if (sts != RGY_ERR_NONE) return sts;

    auto pDelogoParam = std::dynamic_pointer_cast<NVEncFilterParamDelogo>(m_param);
    if (!pDelogoParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pDelogoParam->delogo.autoNR) {
        auto_fade = 0.0f;
        for (int nh = LOGO_NR_MAX; nh >= 0; nh--) {
            cudaStreamWaitEvent(*m_evalStream[nh].stEval.get(), *m_adjMaskStream.heEvalCopyFin.get(), 0);
            if (RGY_ERR_NONE != (sts = autoFadeCoef2Run(false, pFrame, nh, pDelogoParam->delogo.NRArea,
                (const float *)m_fadeValueParallel.ptrDevice, DELOGO_PARALLEL_FADE,
                m_evalStream[nh]))) {
                return sts;
            }
        }
        for (int nh = 0; nh <= LOGO_NR_MAX; nh++) {
            float temp_fade = 0.0f;
            if (RGY_ERR_NONE != (sts = autoFadeLS2(auto_fade, nh))) {
                return sts;
            }
            if (temp_fade > auto_fade) {
                auto_fade = temp_fade;
                auto_nr = nh;
            }
        }
    } else {
        auto_nr = pDelogoParam->delogo.NRValue;
        if (RGY_ERR_NONE != (sts = autoFadeCoef2Run(false, pFrame, auto_nr, pDelogoParam->delogo.NRArea,
            (const float *)m_fadeValueParallel.ptrDevice, DELOGO_PARALLEL_FADE,
            m_evalStream[auto_nr]))) {
            return sts;
        }
        if (RGY_ERR_NONE != (sts = autoFadeLS2(auto_fade, auto_nr))) {
            return sts;
        }
    }
    return RGY_ERR_NONE;
}

#pragma warning (push)
#pragma warning (disable: 4127) //warning C4127: 条件式が定数です。
RGY_ERR NVEncFilterDelogo::calcAutoFadeNR(int& auto_nr, float& auto_fade, const RGYFrameInfo *pFrame, cudaStream_t stream) {
    auto sts = calcAutoFadeNRFrame(auto_nr, auto_fade, pFrame, stream);
    if (sts != RGY_ERR_NONE) return sts;

    m_fadeArray[m_frameOut].frameId = m_frameOut;
    m_fadeArray[m_frameOut].fade = auto_fade;
    m_fadeArray[m_frameOut].nNR = auto_nr;

    if (m_frameIn >= 3) {
        // 前後のFrameのFade値からFade値を調整する
        bool bNeedAdjust = true;
        const auto past_frame_fade = (m_fadeArray[m_frameOut-3].fade + m_fadeArray[m_frameOut-2].fade + m_fadeArray[m_frameOut-1].fade) * (1.0f / 3.0f);
        const auto future_frame_fade = (m_fadeArray[m_frameOut+1].fade + m_fadeArray[m_frameOut+2].fade + m_fadeArray[m_frameOut+3].fade) * (1.0f / 3.0f);
        const auto current_frame_fade = (m_fadeArray[m_frameOut-1].fade + m_fadeArray[m_frameOut].fade + m_fadeArray[m_frameOut+1].fade) * (1.0f / 3.0f);

        const int adjustCoef = 7;                 // 0～10の補正係数
        const auto fade_shreshold = LOGO_FADE_MAX * 0.85f;    // 調整を施す閾値.
        const auto fade_min_limit = LOGO_FADE_MAX * 0.1f;          //      V
        if (adjustCoef > 0) {   // 補正係数=0の場合は補正しない
            if (auto_fade < fade_min_limit) {
                if (past_frame_fade < fade_min_limit || future_frame_fade < fade_min_limit || current_frame_fade < fade_min_limit) {
                    auto_fade = auto_fade * (LOGO_FADE_AD_MAX - adjustCoef) / LOGO_FADE_AD_MAX;
                    bNeedAdjust = false;
                }
            } else if (auto_fade > fade_shreshold) {
                if (past_frame_fade > fade_shreshold || future_frame_fade > fade_shreshold || current_frame_fade > fade_shreshold) {
                    // Fade値が前後のFamreで継続して最大値の85%以上で推移している場合の調整
                    auto_fade += ((LOGO_FADE_MAX - auto_fade) * adjustCoef / LOGO_FADE_AD_MAX);
                    bNeedAdjust = false;
                }
            } else {
                // 前後の平均との誤差が少ない場合は調整せずにそのまま判定値を採用する
                float rate = std::min(std::abs(auto_fade - past_frame_fade), std::abs(auto_fade - future_frame_fade)) / auto_fade;
                if (rate <= 0.03f) { // 誤差が3%以下ならば調整しない
                    bNeedAdjust = false;
                }
            }
        } else {
            bNeedAdjust = false;
        }

        if (bNeedAdjust) { // 調整が必要な場合
                           // 前後2Frameの合計5Frameの中で最大/最小のFade値を除外した平均値を求める.
            float max_fade = 0.0f;
            float min_fade = std::numeric_limits<float>::max();
            float total = 0.0f;
            for (int i = -2; i <= 2; i++) {
                max_fade = std::max(max_fade, m_fadeArray[m_frameOut+i].fade);
                min_fade = std::min(min_fade, m_fadeArray[m_frameOut+i].fade);
                total += m_fadeArray[m_frameOut+i].fade;
            }
            total -= (max_fade + min_fade);
            const float ave_fade = total * (1.0f / 3.0f);

            if (auto_fade < ave_fade) {
                // 方針としては、Fade値が調整Fade値よりも小さい場合はできるだけ調整Fade値に置き換えてより大きなFade値にする。
                if (ave_fade >= LOGO_FADE_MAX) {
                    auto_fade = LOGO_FADE_MAX;
                } else if (ave_fade > fade_shreshold) {
                    // 閾値以上のFade値が継続する場合はFade値を引き上げる
                    auto_fade += ((LOGO_FADE_MAX - auto_fade) * adjustCoef / LOGO_FADE_AD_MAX);
                } else if (auto_fade < ave_fade * 0.98f) {
                    auto_fade = ave_fade;
                }
            } else if (auto_fade > ave_fade) {
                //方針としては、Fade値が調整Fade値よりも大きい場合はできるだけそのまま採用する。
                if (auto_fade >= LOGO_FADE_MAX) {
                    if (ave_fade < LOGO_FADE_MAX)
                        auto_fade = LOGO_FADE_MAX;
                    else if (auto_fade > ave_fade * 1.03f)
                        auto_fade = ave_fade;
                } else if (ave_fade > fade_shreshold) {
                    // 閾値以上のFade値が継続する場合はFade値を引き上げる
                    auto_fade += ((LOGO_FADE_MAX - auto_fade) * adjustCoef / LOGO_FADE_AD_MAX);
                } else if (auto_fade < LOGO_FADE_MAX * 0.80f  // LOGO_FADE_MAXの80%以上の場合はFade値をそのまま採用する.
                    && auto_fade > ave_fade * 1.15f) { // 平均よりも15%以上大きい場合
                    auto_fade = ave_fade;
                }
            }
        }
    }
    m_fadeArray[m_frameOut].adjFade = auto_fade;
    return RGY_ERR_NONE;
}
#pragma warning (pop)

RGY_ERR NVEncFilterDelogo::logAutoFadeNR() {
    auto pDelogoParam = std::dynamic_pointer_cast<NVEncFilterParamDelogo>(m_param);
    if (!pDelogoParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pDelogoParam->delogo.log
        && (pDelogoParam->delogo.autoFade || pDelogoParam->delogo.autoNR)) {
        std::unique_ptr<FILE, fp_deleter> fp(_tfopen(m_logPath.c_str(), _T("a")), fp_deleter());
        if (fp) {
            _ftprintf(fp.get(), _T("%7d, %d, %9.3f, %9.3f\n"),
                m_fadeArray[m_frameOut].frameId,
                m_fadeArray[m_frameOut].nNR,
                m_fadeArray[m_frameOut].adjFade,
                m_fadeArray[m_frameOut].fade);
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR NVEncFilterDelogo::run_filter(const RGYFrameInfo *pInputFrame, RGYFrameInfo **ppOutputFrames, int *pOutputFrameNum, cudaStream_t stream) {
    RGY_ERR sts = RGY_ERR_NONE;

    *pOutputFrameNum = 1;
    if (ppOutputFrames[0] && !ppOutputFrames[0]->mem_type) {
        AddMessage(RGY_LOG_ERROR, _T("only supported on device memory.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (m_param->frameOut.csp != m_param->frameIn.csp) {
        AddMessage(RGY_LOG_ERROR, _T("csp does not match.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    auto pDelogoParam = std::dynamic_pointer_cast<NVEncFilterParamDelogo>(m_param);
    if (!pDelogoParam) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (pDelogoParam->delogo.mode == DELOGO_MODE_ADD_MULTI) {
        if (m_nFramesProcessed % 1000 == 0) {
            std::random_device seed_gen;
            std::mt19937 mt_engine(seed_gen());
            std::uniform_real_distribution<float> generator(pDelogoParam->delogo.multiaddDepthMin, pDelogoParam->delogo.multiaddDepthMax);
            std::generate(m_DepthHost.begin(), m_DepthHost.end(), [&]() {
                auto rand = generator(mt_engine);
                //return (rand > 144.0f) ? 128.0f : std::max(0.0f, rand);
                return clamp(rand, 0.0f, 128.0f);
            });
            cudaMemcpy(m_Depth->ptr, m_DepthHost.data(), sizeof(m_DepthHost[0]) * m_DepthHost.size(), cudaMemcpyHostToDevice);
        }
        fwrite(m_DepthHost.data(), sizeof(m_DepthHost[0]), m_DepthHost.size(), m_fpDepth.get());
    }

    float fade = (float)m_sProcessData[LOGO__Y].fade;
    int auto_nr = pDelogoParam->delogo.NRValue;
    if (pDelogoParam->delogo.autoFade || pDelogoParam->delogo.autoNR) {
        if (pInputFrame->ptr[0] != nullptr) {
            sts = copyFrameAsync(&m_src[m_frameIn].frame, pInputFrame, stream);
            if (sts != RGY_ERR_NONE) {
                AddMessage(RGY_LOG_ERROR, _T("failed to copy input frame to buffer: %s.\n"),
                    get_err_mes(sts));
                return sts;
            }
            copyFrameProp(&m_src[m_frameIn].frame, pInputFrame);
            m_frameIn++;
        } else if (m_frameIn <= m_frameOut) {
            //出力フレームなし
            *pOutputFrameNum = 0;
            ppOutputFrames[0] = nullptr;
            return sts;
        } else {
            pInputFrame = &m_src[m_frameIn].frame;
        }
        if (RGY_ERR_NONE != (sts = calcAutoFadeNR(auto_nr, fade, pInputFrame, stream))) {
            return sts;
        }
        if (m_frameIn < 3) {
            //出力フレームなし
            *pOutputFrameNum = 0;
            ppOutputFrames[0] = nullptr;
            return sts;
        }
        ppOutputFrames[0] = &m_src[m_frameOut].frame;
        m_frameOut++;
    } else {
        if (pInputFrame->ptr[0] == nullptr) {
            //自動フェードや自動NRを使用しない場合、入力フレームがないということはない
            *pOutputFrameNum = 0;
            ppOutputFrames[0] = nullptr;
            return sts;
        }
        if (ppOutputFrames[0] == nullptr) {
            AddMessage(RGY_LOG_ERROR, _T("ppOutputFrames[0] must be set.\n"));
            return RGY_ERR_INVALID_PARAM;
        }
        m_frameIn++;
        m_frameOut++;
    }

    if (RGY_ERR_NONE != (sts = delogoY(ppOutputFrames[0], fade, stream))) {
        return sts;
    }

    if (RGY_ERR_NONE != (sts = delogoUV(ppOutputFrames[0], fade, stream))) {
        return sts;
    }

    if (RGY_ERR_NONE != (sts = logoNR(ppOutputFrames[0], pDelogoParam->delogo.NRValue, stream))) {
        return sts;
    }

    if (RGY_ERR_NONE != (sts = logAutoFadeNR())) {
        return sts;
    }
    m_nFramesProcessed++;
    return sts;
}

void NVEncFilterDelogo::close() {
    m_LogoFilePath.clear();
    m_frameBuf.clear();
    m_sLogoDataList.clear();
    m_src.clear();
    m_mask.reset();
    m_maskAdjusted.reset();
    m_maskNR.reset();
    m_maskNRAdjusted.reset();
    m_maskValidCount = 0;
    for (size_t i = 0; i < m_bufDelogo.size(); i++) {
        m_bufDelogo[i].reset();
    }
    for (size_t i = 0; i < m_bufDelogoNR.size(); i++) {
        m_bufDelogoNR[i].reset();
    }
    for (size_t i = 0; i < m_bufEval.size(); i++) {
        m_bufEval[i].reset();
    }
    for (size_t i = 0; i < m_evalCounter.size(); i++) {
        m_evalCounter[i].clear();
    }
    m_adjMaskMinIndex.reset();
    m_adjMaskThresholdTest.reset();
    m_NRProcTemp.reset();
    m_createLogoMaskValidMaskCount.clear();
    m_adjMaskEachFadeCount.clear();
    m_adjMaskMinResAndValidMaskCount.clear();
    m_adjMask2ValidMaskCount.clear();
    m_adjMask2TargetCount.reset();
    m_smoothKernel.reset();
    m_fadeValueAdjust.clear();
    m_fadeValueParallel.clear();
    m_fadeValueTemp.clear();
    m_logPath.clear();
    m_frameIn = 0;
    m_frameOut = 0;
    m_Depth.reset();
    m_fpDepth.reset();
    m_nFramesProcessed = 0;
}
