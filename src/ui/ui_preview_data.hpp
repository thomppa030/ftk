#pragma once

#include "ui/game_ui/game_model_world.hpp"
#include "ui/game_ui/ui_value.hpp"
#include "ui/game_ui/widget_asset.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class GameUi;
class Project;
class Scene;
class ScriptManager;
class UiContext;
struct InspectorContext;
struct OwnedTypeDesc;

/// What the UI editor's preview reads where a game would feed it: the open
/// widget's value props from their defaults; its component Models, and props
/// typed as a component, read from the open scene; and its controller's
/// Models, and props typed as a struct, built from their defaults. All of it
/// is edited in the Data panel (UI editor data panel, kit spec) and none of
/// it is saved.
class UiPreviewData {
public:
    UiPreviewData(const ScriptManager& scripts, const Scene* scene);
    ~UiPreviewData();
    UiPreviewData(const UiPreviewData&) = delete;
    UiPreviewData& operator=(const UiPreviewData&) = delete;
    UiPreviewData(UiPreviewData&&) = delete;
    UiPreviewData& operator=(UiPreviewData&&) = delete;

    /// What `widget`, read from `path`, declares becomes the preview's data.
    /// A name declared the same way as before keeps its value and its picked
    /// node, so a widget read again after an edit keeps what was set.
    void set_widget(const WidgetAsset& widget, const std::string& path, const Project* project);

    /// Before the tree loads: the value props and the objects built here into
    /// the UI's root scope, so what loading resolves reads them. A Model's
    /// name holds nothing until attach() finds it.
    void fill(UiContext& root);
    /// After the tree loaded: the Models read from the scene, the widget's
    /// own and those of the widgets it places, bound once.
    void attach(GameUi& ui);
    /// Reads the scene again: once a frame, before the preview updates.
    void bind(GameUi& ui);
    /// The game module is about to go: everything built from it goes first,
    /// and the root scope stops pointing at it. The next set_widget() builds
    /// it again from whatever module is loaded then.
    void release();

    /// The Data panel's contents.
    void draw(GameUi& ui, const InspectorContext& ctx);

private:
    /// A reflected object built from its type's defaults, owned here.
    class Owned {
    public:
        explicit Owned(const OwnedTypeDesc& type);
        ~Owned();
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        Owned(Owned&&) = delete;
        Owned& operator=(Owned&&) = delete;

        /// Back to the defaults, in the same place.
        void reset();
        [[nodiscard]] const OwnedTypeDesc& type() const { return *type_; }
        [[nodiscard]] void* get() const { return storage_; }

    private:
        const OwnedTypeDesc* type_;
        void* storage_;
    };

    struct Entry {
        enum class Kind { value, owned, component };
        Kind kind{Kind::value};
        std::string name{};
        std::string type{};              ///< the prop's declared type, or the struct's or component's name
        UiValue value{};                 ///< a value prop
        std::unique_ptr<Owned> object{}; ///< an object built here
        ModelDecl decl{};              ///< a component: what it reads and by which rule
        uint32_t picked{0};            ///< a node picked here over the rule; 0 for none
    };

    /// An entry for an object of `type` named `name`, taking the object an
    /// entry of the same name and type had in `before`.
    [[nodiscard]] static Entry owned_entry(const std::string& name, const OwnedTypeDesc& type,
                                           std::vector<Entry>& before);
    void draw_component(Entry& entry, GameUi& ui, const InspectorContext& ctx);
    void draw_owned(Entry& entry, const InspectorContext& ctx);
    void draw_values(GameUi& ui);

    const ScriptManager& scripts_;
    const Scene* scene_;
    GameModelWorld world_;
    std::vector<Entry> models_;
    std::vector<Entry> props_;
    std::string widget_path_;
    const Project* project_{nullptr};
    /// The node that places the widget in the scene, for `from="self"`, and
    /// the scene and structure it was looked for in.
    uint32_t self_node_{0};
    uint64_t looked_scene_{0};
    uint64_t looked_structure_{0};
    bool looked_{false};
    /// The scope fill() wrote into, which release() empties again.
    UiContext* root_{nullptr};
};

} // namespace fjell
