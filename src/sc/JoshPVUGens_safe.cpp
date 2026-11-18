// sc3-plugins' JoshPVUGens.cpp, compiled unmodified through this file: units with a destructor
// get their memory zeroed before their constructor. PV_Freeze (and others) free buffers they only
// allocate on the first FFT frame, so a synth freed earlier freed garbage pointers (a crash in
// SuperCollider too). The registration macro is replaced before the source is included.
#include "SC_PlugIn.h"

#include <cstring>

template <class U, void (*Ctor)(U*)> static void ConchaZeroedCtor(U* unit)
{
    std::memset(reinterpret_cast<char*>(unit) + sizeof(Unit), 0, sizeof(U) - sizeof(Unit));
    Ctor(unit);
}

#undef DefineDtorUnit
#define DefineDtorUnit(name)                                                                                    \
    (*ft->fDefineUnit)(#name, sizeof(name), (UnitCtorFunc)&ConchaZeroedCtor<name, name##_Ctor>,                \
                       (UnitDtorFunc)&name##_Dtor, 0);

#include "JoshUGens/JoshPVUGens.cpp"
