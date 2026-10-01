#pragma once

namespace fjell {

class CommandHistory;

class HistoryPanel {
public:
    void init(CommandHistory* history);
    void draw(const char* title = "History");

private:
    CommandHistory* history_{nullptr};
};

} // namespace fjell
