// The symbols the kit's fields show after their numbers: the same for the
// kit drawn by either implementation, so they are in ftk-ui-kit.

#include "ftk/ui/kit/field.hpp"

namespace ftk::ui {

const char* unit_symbol(Unit unit) {
    switch (unit) {
        case Unit::None:            return "";
        case Unit::Metres:          return "m";
        case Unit::Kilometres:      return "km";
        case Unit::SquareMetres:    return "m\xc2\xb2";
        case Unit::MetresPerSecond: return "m/s";
        case Unit::MetresPerSecondSquared: return "m/s\xc2\xb2";
        case Unit::Degrees:         return "\xc2\xb0";
        case Unit::Radians:         return "rad";
        case Unit::Milliseconds:    return "ms";
        case Unit::Seconds:         return "s";
        case Unit::Minutes:         return "min";
        case Unit::Hertz:           return "Hz";
        case Unit::Kilograms:       return "kg";
        case Unit::Pixels:          return "px";
        case Unit::Percent:         return "%";
        case Unit::ExposureValue:   return "EV";
        case Unit::Times:           return "\xc3\x97";
        case Unit::Megabytes:       return "MB";
    }
    return "";
}

} // namespace ftk::ui
