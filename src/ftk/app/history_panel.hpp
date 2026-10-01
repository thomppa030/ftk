#pragma once

namespace ftk {

class CommandHistory;

class HistoryPanel {
public:
    void init(CommandHistory* history);
    void draw(const char* title = "History");

private:
    CommandHistory* history_{nullptr};
};

} // namespace ftk
