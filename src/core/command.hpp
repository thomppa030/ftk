#pragma once

#include <memory>
#include <string>

namespace fjell {

class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    [[nodiscard]] virtual std::string description() const = 0;
};

using CommandPtr = std::unique_ptr<Command>;

/// An undo step's text for one value that changed: what it is, then the old
/// value and the new, marked so the History panel shows the old one in red
/// and the new in green ("Crate · Rigidbody: Mass 1.00 kg -> 2.50 kg").
inline std::string describe_change(const std::string& what, const std::string& before,
                                   const std::string& after) {
    return what + " \x01" + before + "\x02 -> \x03" + after + "\x04";
}

} // namespace fjell
