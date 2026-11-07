// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>

struct FixtureEvaluation {
    const void *feature = nullptr;
    ID3D12Resource *colour = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
    unsigned reset = 0;
    float localTone = 0;
};
