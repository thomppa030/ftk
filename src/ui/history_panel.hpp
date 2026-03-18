#pragma once

namespace fjell {

class CommandHistory;

class HistoryPanel {
public:
    void init(CommandHistory* history);
    void draw();

private:
    CommandHistory* history_{nullptr};
};

} // namespace fjell
