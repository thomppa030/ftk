#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ftk {

class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    [[nodiscard]] virtual std::string description() const = 0;
};

using CommandPtr = std::unique_ptr<Command>;

/// Several commands as one undo step ("Delete 3 objects"). Each is carried
/// out as it is added, so it sees the scene the one before it left, the
/// way it would have one step at a time; the history's first execute() then
/// has nothing left to do. Undo runs them backwards.
class CommandGroup : public Command {
public:
    explicit CommandGroup(std::string description) : description_{std::move(description)} {}

    void add(CommandPtr command) {
        command->execute();
        commands_.push_back(std::move(command));
    }
    [[nodiscard]] bool empty() const { return commands_.empty(); }

    void execute() override {
        if (applied_) {
            applied_ = false;
            return;
        }
        for (auto& command : commands_) command->execute();
    }
    void undo() override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) (*it)->undo();
    }
    [[nodiscard]] std::string description() const override { return description_; }

private:
    std::string description_;
    std::vector<CommandPtr> commands_;
    // Everything added has already been carried out.
    bool applied_{true};
};

/// An undo step's text for one value that changed: what it is, then the old
/// value and the new, marked so the History panel shows the old one in red
/// and the new in green ("Crate · Rigidbody: Mass 1.00 kg -> 2.50 kg").
inline std::string describe_change(const std::string& what, const std::string& before,
                                   const std::string& after) {
    return what + " \x01" + before + "\x02 -> \x03" + after + "\x04";
}

} // namespace ftk
