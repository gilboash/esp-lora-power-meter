// Minimal shim so the decoder can be compiled and tested on the host.
// It uses only these few Arduino conveniences; nothing hardware-specific.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
using std::max;
using std::min;
#define constrain(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
