#include "parser.hpp"
#include "types.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace fjreflect {

namespace {

bool has(const std::vector<std::string>& list, const char* name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

/// Resolves struct names across every header and reflects each struct a
/// component's state or Model reaches, with everything it reaches in turn.
class Linker {
public:
    Linker(const std::vector<ClassDecl>& classes, LinkResult& out) : out_(out) {
        for (const auto& decl : classes) classes_.emplace(decl.qualified_name, &decl);
    }

    void link_component(ComponentInfo& comp) {
        for (auto& field : comp.fields) {
            if (field.fj_field_type == "FJELL_FIELD_STRUCT") {
                field.struct_type = reach(field.struct_type, field.scope, comp.class_name + "::" + field.name,
                                          comp.source_file, field.line);
            } else if (field.element_fj_type == "FJELL_FIELD_STRUCT") {
                field.element_struct = reach(field.element_struct, field.scope,
                                             comp.class_name + "::" + field.name, comp.source_file, field.line);
            }
        }
    }

    /// [[fj::state]] in a struct no component reaches means nothing: say so.
    void warn_unreached() {
        for (const auto& [name, decl] : classes_) {
            if (decl->is_component || reflected_.contains(name)) continue;
            for (const auto& m : decl->members) {
                if (!has(m.fj_attributes, "state")) continue;
                out_.warnings.push_back(decl->source_file + ":" + std::to_string(m.line) +
                                        ": warning: [[fj::state]] on '" + m.name + "' in " + decl->name +
                                        ", which no component's state or Model holds");
                break;
            }
        }
    }

private:
    void error(const std::string& file, unsigned line, const std::string& message) {
        out_.errors.push_back(file + ":" + std::to_string(line) + ": error: " + message);
    }

    /// The qualified name `written` means from inside `scope`: the innermost
    /// enclosing scope that declares it wins, as in C++. Empty when none does.
    [[nodiscard]] std::string resolve(const std::string& written, const std::string& scope) const {
        if (written.starts_with("::")) {
            const auto name = written.substr(2);
            return classes_.contains(name) ? name : std::string();
        }
        std::string enclosing = scope;
        while (true) {
            const auto candidate = enclosing.empty() ? written : enclosing + "::" + written;
            if (classes_.contains(candidate)) return candidate;
            if (enclosing.empty()) return {};
            const auto cut = enclosing.rfind("::");
            enclosing = cut == std::string::npos ? std::string() : enclosing.substr(0, cut);
        }
    }

    /// Resolve and reflect the struct a field holds; its qualified name, or
    /// what was written when it cannot be found (after reporting).
    std::string reach(const std::string& written, const std::string& scope, const std::string& chain,
                      const std::string& file, unsigned line) {
        const auto qualified = resolve(written, scope);
        if (qualified.empty()) {
            error(file, line, "'" + chain + "' holds '" + written + "', which no script header declares; "
                              "a Model's structs live in scripts/");
            return written;
        }
        reflect(qualified, chain, file, line);
        return qualified;
    }

    void reflect(const std::string& qualified, const std::string& chain, const std::string& file, unsigned line) {
        if (reflected_.contains(qualified)) return;
        const ClassDecl& decl = *classes_.at(qualified);
        if (decl.is_component) {
            error(file, line, "'" + chain + "' holds " + decl.name + ", a component; a Model holds "
                              "plain data, not components");
            return;
        }
        reflected_.insert(qualified); // before the members: a struct may hold a list of itself

        StructInfo info;
        info.name = decl.name;
        info.qualified_name = decl.qualified_name;
        info.source_file = decl.source_file;
        info.line = decl.line;
        for (const auto& m : decl.members) {
            if (m.is_static) continue;
            const auto where = chain + " → " + decl.name + "::" + m.name;
            if (m.is_function) {
                if (!has(m.fj_attributes, "state")) continue;
                add_getter(info, decl, m, where);
                continue;
            }
            add_member(info, decl, m, where);
        }
        out_.structs.push_back(std::move(info));
    }

    void add_member(StructInfo& info, const ClassDecl& decl, const MemberDecl& m, const std::string& where) {
        if (!m.is_readable) {
            error(decl.source_file, m.line, "'" + where + "': a Model struct's member is one plain "
                                            "declaration, 'Type name;'");
            return;
        }
        if (!m.is_public) {
            error(decl.source_file, m.line, "'" + where + "' is private; the UI reads a Model struct's "
                                            "members directly, so they are public");
            return;
        }
        const auto value = classify_value(m.type);
        if (!value.error.empty()) {
            error(decl.source_file, m.line, "'" + where + "': " + value.error);
            return;
        }
        if (!value.ref_target.empty()) {
            error(decl.source_file, m.line, "'" + where + "' is a reference; a Model struct holds values, "
                                            "structs and lists");
            return;
        }
        FieldInfo fi;
        fi.name = m.name;
        fi.cpp_type = m.type;
        fi.line = m.line;
        fi.scope = decl.qualified_name;
        fi.is_state = true;
        fi.fj_field_type = value.fj;
        fi.element_cpp_type = value.element_cpp;
        fi.element_fj_type = value.element_fj;
        if (value.fj == "FJELL_FIELD_STRUCT") {
            fi.struct_type = reach(value.struct_name, decl.qualified_name, where, decl.source_file, m.line);
        } else if (value.element_fj == "FJELL_FIELD_STRUCT") {
            fi.element_struct = reach(value.element_struct, decl.qualified_name, where, decl.source_file, m.line);
        }
        info.fields.push_back(std::move(fi));
    }

    void add_getter(StructInfo& info, const ClassDecl& decl, const MemberDecl& m, const std::string& where) {
        if (!m.is_public) {
            error(decl.source_file, m.line, "getter '" + where + "' is private; the UI calls a Model "
                                            "struct's getters directly, so they are public");
            return;
        }
        if (!m.is_const || !m.param_types.empty()) {
            error(decl.source_file, m.line, "getter '" + where + "' must be const and take no parameters");
            return;
        }
        FieldInfo fi;
        fi.name = m.name;
        fi.line = m.line;
        fi.scope = decl.qualified_name;
        fi.cpp_type = value_type_of(m.type);
        std::string ref;
        fi.fj_field_type = map_field_type(fi.cpp_type, ref);
        if (!is_plain_value(fi.fj_field_type)) {
            error(decl.source_file, m.line, "getter '" + where + "' returns '" + m.type + "'; a getter "
                                            "returns bool, int, float, glm::vec2/3/4 or std::string");
            return;
        }
        fi.is_getter = true;
        fi.is_state = true;
        info.fields.push_back(std::move(fi));
    }

    LinkResult& out_;
    std::map<std::string, const ClassDecl*> classes_;
    std::set<std::string> reflected_;
};

} // namespace

LinkResult link(std::vector<ComponentInfo>& components, const std::vector<ClassDecl>& classes,
                const std::vector<PipeInfo>& pipes) {
    LinkResult out;
    Linker linker(classes, out);
    for (auto& comp : components) linker.link_component(comp);
    linker.warn_unreached();

    std::map<std::string, const PipeInfo*> by_name;
    for (const auto& pipe : pipes) {
        auto [it, inserted] = by_name.emplace(pipe.name, &pipe);
        if (inserted) continue;
        out.errors.push_back(pipe.source_file + ":" + std::to_string(pipe.line) + ": error: a pipe named '" +
                             pipe.name + "' is already declared at " + it->second->source_file + ":" +
                             std::to_string(it->second->line) + "; markup names pipes, so each name is one pipe");
    }
    return out;
}

} // namespace fjreflect
