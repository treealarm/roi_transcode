// Every enumerator of every enum the Python bindings expose -- one half of a surface check.
//
// §58.3 asks the wrapper to bind the library's vocabulary, and an enum is the part of that
// vocabulary a binding can lose silently: a value added in C++ compiles, links, passes every
// C++ test and simply does not exist in Python. Slice Z8 found five such holes in structs by
// reading them side by side; the enums were read the same way and three holes survived it --
// the native oneVPL backend, and two status codes a caught exception could already carry.
// Reading does not scale, so this prints what C++ has and integration/scripts/
// python-enum-parity.py checks that Python has exactly that.
//
// **How it stays honest as the library grows.** Each enum is spelled out in a switch with no
// `default:`, so adding an enumerator makes the compiler warn here (-Wswitch, in -Wall) --
// and if that warning is missed, the value never reaches the dump and the parity step fails
// with the value named. Both ends have to be ignored for a hole to open.
//
// The spelling printed is the C++ enumerator, not the display name: `backend_name` says
// "qsv" for IntelOneVpl, and what is being compared here is the surface, not the wording.

#include "enum_table.h"

#include "sve/backend_selection.h"
#include "sve/encoder.h"
#include "sve/frame_buffer.h"
#include "sve/logging.h"
#include "sve/object_priority.h"
#include "sve/policy.h"
#include "sve/status.h"
#include "sve/types.h"

#include <cstdio>

namespace {

template <class E>
void dump(const char* key)
{
    sve_enum_table::for_each<E>([key](E value, const char* name) {
        std::printf("%s %d %s\n", key, static_cast<int>(value), name);
    });
}

} // namespace

int main()
{
    dump<sve::Backend>("Backend");
    dump<sve::Codec>("Codec");
    dump<sve::EncoderState>("EncoderState");
    dump<sve::LogLevel>("LogLevel");
    dump<sve::ObjectPriority>("ObjectPriority");
    dump<sve::PixelFormat>("PixelFormat");
    dump<sve::SidecarFormat>("SidecarFormat");
    dump<sve::StatusCode>("StatusCode");
    return 0;
}
