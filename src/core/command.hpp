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

} // namespace fjell
