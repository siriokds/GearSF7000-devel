#include "NEC765.h"

#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>

namespace
{
constexpr std::size_t kStatus1Offset = 5;
constexpr std::size_t kStatus3Offset = 7;
constexpr std::uint8_t kStatus1NotWritable = 0x02;
constexpr std::uint8_t kStatus3WriteProtect = 0x40;

std::string SaveControllerState(NEC765* controller)
{
    std::ostringstream state(std::ios::binary);
    nec765SaveState(controller, state);
    return state.str();
}

std::uint8_t ByteAt(const std::string& state, std::size_t offset)
{
    assert(state.size() > offset);
    return static_cast<std::uint8_t>(state[offset]);
}
}

int main()
{
    NEC765* controller = nec765Create();
    assert(controller != nullptr);

    nec765SetReadOnly(controller, true);
    std::string state = SaveControllerState(controller);
    assert((ByteAt(state, kStatus1Offset) & kStatus1NotWritable) != 0);
    assert((ByteAt(state, kStatus3Offset) & kStatus3WriteProtect) != 0);

    // Replacing a protected image with a writable one must clear the FDC's
    // cached write-protect state as well as the drive-level property.
    nec765SetReadOnly(controller, false);
    state = SaveControllerState(controller);
    assert((ByteAt(state, kStatus1Offset) & kStatus1NotWritable) == 0);
    assert((ByteAt(state, kStatus3Offset) & kStatus3WriteProtect) == 0);

    nec765Destroy(controller);
    return 0;
}
