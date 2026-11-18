// MouseX / MouseY without a mouse: a plug-in cannot follow the screen's pointer (SuperCollider's
// UIUGens open an X11 thread that aborts when the plug-in unloads). They hold their minimum, as
// if the pointer were in the corner. Only StrudelDirt's `pmsin` test synth uses them.
#include "SC_PlugIn.h"

static InterfaceTable* ft;

struct ConchaMouse : public Unit {};

static void ConchaMouse_next(ConchaMouse* unit, int)
{
    ZOUT0(0) = ZIN0(0); // minval
}

static void ConchaMouse_Ctor(ConchaMouse* unit)
{
    SETCALC(ConchaMouse_next);
    ConchaMouse_next(unit, 1);
}

void ConchaUI_Load(InterfaceTable* inTable)
{
    ft = inTable;
    (*ft->fDefineUnit)("MouseX", sizeof(ConchaMouse), (UnitCtorFunc)&ConchaMouse_Ctor, nullptr, 0);
    (*ft->fDefineUnit)("MouseY", sizeof(ConchaMouse), (UnitCtorFunc)&ConchaMouse_Ctor, nullptr, 0);
}
