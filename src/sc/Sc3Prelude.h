#pragma once

// Included before each sc3-plugins source, which is then wrapped in a namespace of its own
// (cmake/Sc3Plugins.cmake): every header those sources include from outside their own folder is
// included here first, at global scope, so that their #include inside the namespace is a no-op.
// A source compiled as its own shared library in sc3-plugins keeps its names to itself this way
// (classes, helpers, Faust's dsp / UI / mydsp), and its PluginLoad(Name) becomes
// concha_sc3_<unit>::Name_Load.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__SSE__) || defined(_M_X64)
#    include <xmmintrin.h>
#endif
#if defined(__SSE2__) || defined(_M_X64)
#    include <emmintrin.h>
#endif

#ifndef _WIN32
#    include <pthread.h>
#    include <sys/time.h>
#    include <unistd.h>
#endif

#include "SC_PlugIn.h"
#include "SC_PlugIn.hpp"
#include "SCComplex.h"
#include "SC_fftlib.h"
#include "SC_RGen.h"
#include "SC_Constants.h"
// not FFT_UGens.h: it declares `extern InterfaceTable* ft`, which must be the unit's own
