#pragma once
#include <math.h>
#include "model_rf_compact.h"
namespace rf_compact {
struct PreprocessParameter { float median; float mean; float scale; };
static constexpr PreprocessParameter kPreprocess[kNumFeatures] = {
    { 0.754870157f, 1.12062021f, 1.59676403f },
    { 0.109746365f, 1.78700405f, 3.62123839f },
    { -0.00183623265f, -0.00217109749f, 0.078999674f },
    { -0.00242252078f, -0.000903011146f, 0.0824782526f },
    { 0.0252062612f, 0.0266193761f, 0.0171715475f },
    { 0.0491453957f, 0.0550048434f, 0.0465878096f },
    { 2.25f, 2.61784659f, 2.89099168f },
    { 20.5625f, 20.7797993f, 3.75093166f },
    { 0.0935631701f, 0.145032553f, 1.64123697f },
    { 0.0368537964f, 0.299409121f, 1.05433809f },
    { 0.0f, -0.000204844209f, 5.97768046f },
    { 3.43436713e-06f, 1.27579896e-05f, 0.303590332f },
    { 0.727300398f, 1.09936218f, 1.60920119f },
    { 0.0584980481f, 1.65185996f, 3.38556696f },
    { 0.000288543629f, 0.00367081759f, 0.00738938428f },
    { 0.919588756f, 0.713301575f, 0.299858271f },
    { 369.722737f, 360.795507f, 128.934676f },
    { 3.1695394f, 3.66038096f, 1.25684446f },
    { 2.92597966f, 3.34415271f, 1.16169274f },
    { 203.755042f, 213.675547f, 47.9143714f },
    { 1302.87264f, 1330.51117f, 392.870735f },
    { 5.64663119f, 5.36117709f, 1.8314432f },
    { 0.873984452f, 0.866725225f, 0.0473570007f },
};
inline void preprocess(const float* raw, float* normalized) {
    for (uint32_t i = 0; i < kNumFeatures; ++i) {
        float value = raw[i];
        if (isnan(value)) value = kPreprocess[i].median;
        normalized[i] = (value - kPreprocess[i].mean) / kPreprocess[i].scale;
    }
}
}
