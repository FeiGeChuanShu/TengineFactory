
#include <algorithm>
#include <map>
#include "AutoStorage.h"
#include "Macro.h"
#include "ImageBlitter.hpp"
#include "ImageFloatBlitter.hpp"
#include "ImageSampler.hpp"
#include "ErrorCode.hpp"
#define CACHE_SIZE 128

namespace Tengine {
namespace CV {
struct ImageProcess::Inside {
    Config config;
    AutoStorage<uint8_t> cacheBuffer;
    AutoStorage<uint8_t> cacheBufferRGBA;
};

ImageProcess::~ImageProcess() {
    delete mInside;
}

ImageProcess::ImageProcess(const Config& config) {
    mInside         = new Inside;
    mInside->config = config;
    mInside->cacheBuffer.reset(4 * CACHE_SIZE);
    mInside->cacheBufferRGBA.reset(4 * CACHE_SIZE);
    for (int i = 0; i < 4; ++i) {
        mInside->config.mean[i]   = config.mean[i];
        mInside->config.normal[i] = config.normal[i];
    }
}

ImageProcess* ImageProcess::create(const Config& config) {
    return new ImageProcess(config);
}

ImageProcess* ImageProcess::create(const ImageFormat sourceFormat, const ImageFormat destFormat, const float* means,
                                   const int meanCount, const float* normals, const int normalCount) {
    Tengine::CV::ImageProcess::Config config;
    if (means != nullptr && meanCount > 0) {
        ::memcpy(config.mean, means, sizeof(float) * meanCount);
    }
    if (normals != nullptr && normalCount > 0) {
        ::memcpy(config.normal, normals, sizeof(float) * normalCount);
    }
    config.sourceFormat = sourceFormat;
    config.destFormat   = destFormat;
    return new ImageProcess(config);
}

void ImageProcess::setMatrix(const Matrix& matrix) {
    mTransform = matrix;
    mTransform.invert(&mTransformInvert);
}

static int _getBpp(ImageFormat format) {
    switch (format) {
        case RGB:
        case BGR:
            return 3;
        case RGBA:
        case BGRA:
            return 4;
        case GRAY:
            return 1;

        default:
            break;
    }
    return 0;
}

static int LEFT   = 1 << 0;
static int RIGHT  = 1 << 1;
static int TOP    = 1 << 2;
static int BOTTOM = 1 << 3;
inline static uint8_t _encode(const Point& p, int iw, int ih) {
    uint8_t mask = 0;
    if (p.fX < 0) {
        mask |= LEFT;
    }
    if (p.fX > iw - 1) {
        mask |= RIGHT;
    }
    if (p.fY < 0) {
        mask |= TOP;
    }
    if (p.fY > ih - 1) {
        mask |= BOTTOM;
    }
    return mask;
}

static std::pair<int, int> _computeClip(Point* points, int iw, int ih, const Matrix& invert, int xStart, int count) {
    auto code1 = _encode(points[0], iw, ih);
    auto code2 = _encode(points[1], iw, ih);
    int sta    = 0;
    int end    = count;

    float x1     = points[0].fX;
    float x2     = points[1].fX;
    float y1     = points[0].fY;
    float y2     = points[1].fY;
    int code     = 0;
    int pIndex   = 0;
    float deltaY = y2 - y1;
    float deltaX = x2 - x1;
    if (deltaX > 0.01f || deltaX < -0.01f) {
        deltaY = (y2 - y1) / (x2 - x1);
    } else {
        deltaY = 0;
    }
    if (deltaY > 0.01f || deltaY < -0.01f) {
        deltaX = (x2 - x1) / (y2 - y1);
    } else {
        deltaX = 0;
    }
    while (code1 != 0 || code2 != 0) {
        if ((code1 & code2) != 0) {
            sta = end;
            break;
        }
        if (code1 != 0) {
            code   = code1;
            pIndex = 0;
        } else if (code2 != 0) {
            code   = code2;
            pIndex = 1;
        }
        if ((LEFT & code) != 0) {
            points[pIndex].fY = points[pIndex].fY + deltaY * (0 - points[pIndex].fX);
            points[pIndex].fX = 0;
        } else if ((RIGHT & code) != 0) {
            points[pIndex].fY = points[pIndex].fY + deltaY * (iw - 1 - points[pIndex].fX);
            points[pIndex].fX = iw - 1;
        } else if ((BOTTOM & code) != 0) {
            points[pIndex].fX = points[pIndex].fX + deltaX * (ih - 1 - points[pIndex].fY);
            points[pIndex].fY = ih - 1;
        } else if ((TOP & code) != 0) {
            points[pIndex].fX = points[pIndex].fX + deltaX * (0 - points[pIndex].fY);
            points[pIndex].fY = 0;
        }
        auto tmp = invert.mapXY(points[pIndex].fX, points[pIndex].fY);
        if (0 == pIndex) {
            code1 = _encode(points[pIndex], iw, ih);
            // FUNC_PRINT_ALL(tmp.fX, f);
            sta = (int)::ceilf(tmp.fX) - xStart;
        } else {
            code2 = _encode(points[pIndex], iw, ih);
            // FUNC_PRINT_ALL(tmp.fX, f);
            end = (int)::ceilf(tmp.fX) - xStart;
        }
    }
    return std::make_pair(sta, end);
}

static ImageFormat _correctImageFormat(int outputBpp, ImageFormat format) {
    if (outputBpp != 4) {
        return format;
    }

    static std::map<ImageFormat, ImageFormat> imageFormatTable = {{RGB, RGBA}, {BGR, BGRA}, {GRAY, RGBA}};
    if (imageFormatTable.find(format) != imageFormatTable.end()) {
        return imageFormatTable.find(format)->second;
    }
    return format;
}


ErrorCode ImageProcess::convert(const uint8_t* source, int iw, int ih, int stride, void* dest, int ow, int oh, halide_type_t type) {
    auto sourceBpp = _getBpp(mInside->config.sourceFormat);
    auto outputBpp = _getBpp(mInside->config.destFormat);
    if (0 == stride) {
        stride = iw * sourceBpp;
    }

    // AUTOTIME;
    auto& config      = mInside->config;
    auto sourceFormat = config.sourceFormat;
    auto destFormat   = config.destFormat;
    destFormat        = _correctImageFormat(outputBpp, destFormat);
    auto blitter      = ImageBlitter::choose(sourceFormat, destFormat);
    if (nullptr == blitter) {
        return INPUT_DATA_ERROR;
    }
    // 判断输入比输出大于
    bool identity = mTransform.isIdentity() && iw >= ow && ih >= oh; // TODO, no need for iw, ih limit
    auto sampler  = ImageSampler::choose(sourceFormat, config.filterType, identity);
    if (nullptr == sampler) {
        return INPUT_DATA_ERROR;
    }

    int tileCount = UP_DIV(ow, CACHE_SIZE);
    Point points[2];
    auto sampleBuffer = (uint8_t*)mInside->cacheBuffer.get();
    auto srcData      = source;
    auto destBytes    = type.bytes();
    auto bpp          = outputBpp;
    auto needBlit     = sourceFormat != destFormat;
    bool isFloat      = type.code == halide_type_float;
    
    auto blitFloat = ImageFloatBlitter::choose(destFormat, bpp);

    for (int dy = 0; dy < oh; ++dy) {
        auto dstY = (uint8_t*)dest + dy * destBytes * ow * bpp;
        for (int tIndex = 0; tIndex < tileCount; ++tIndex) {
            int xStart    = tIndex * CACHE_SIZE;
            int count     = std::min(CACHE_SIZE, ow - xStart);
            auto dstStart = dstY + destBytes * bpp * xStart;

            auto samplerDest = sampleBuffer;
            auto blitDest    = mInside->cacheBufferRGBA.get();

            if (!isFloat) {
                blitDest = dstStart;
            }
            if (!needBlit) {
                samplerDest = blitDest;
            }

            // Sample
            {
                // Compute position
                points[0].fX = xStart;
                points[0].fY = dy;

                points[1].fX = xStart + count;
                points[1].fY = dy;

                mTransform.mapPoints(points, 2);
                float deltaY = points[1].fY - points[0].fY;
                float deltaX = points[1].fX - points[0].fX;

                int sta = 0;
                int end = count;

                // FUNC_PRINT(sta);
                if (config.wrap == ZERO) {
                    // Clip: Cohen-Sutherland
                    auto clip    = _computeClip(points, iw, ih, mTransformInvert, xStart, count);
                    sta          = clip.first;
                    end          = clip.second;
                    points[0].fX = sta + xStart;
                    points[0].fY = dy;

                    mTransform.mapPoints(points, 1);
                    if (sta != 0 || end < count) {
                        if (sourceBpp > 0) {
                            if (sta > 0) {
                                ::memset(samplerDest, 0, sourceBpp * sta);
                            }
                            if (end < count) {
                                ::memset(samplerDest + end * sourceBpp, 0, (count - end) * sourceBpp);
                            }
                        } else {
                            // TODO, Only support NV12 / NV21
                            ::memset(samplerDest, 0, count);
                            ::memset(samplerDest + count, 128, UP_DIV(count, 2) * 2);
                        }
                    }
                }
                points[1].fX = (deltaX) / (float)(count);
                points[1].fY = (deltaY) / (float)(count);

                sampler(srcData, samplerDest, points, sta, end - sta, count, iw, ih, stride);
            }
            // Convert format
            if (needBlit) {
                blitter(samplerDest, blitDest, count);
            }
            // Turn float
            if (isFloat) {
                auto normal = mInside->config.normal;
                auto mean   = mInside->config.mean;
                blitFloat(blitDest, (float*)dstStart, mean, normal, count);
            }
        }
    }
    return NO_ERROR;
}

} // namespace CV
} // namespace Tengine
