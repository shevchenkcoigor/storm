module;

// The one home of the textual <meta> include (#756). `import std;` does not export
// std::meta::, so reflection code needs <meta>; included in every module's global fragment
// it made each BMI carry its own copy of the declarations (~10.7 MB floor per module).
// Modules import this instead and pay for them once.
//
// Only the std::meta names the tree uses are re-exported: a new one is a clear
// "no member named ... in namespace 'std::meta'" error until it is added below.
// Not covered here: the bare `^^std::optional` (any std template named without arguments)
// is a using-declarator under `import std;` and cannot be reflected; reflect a concrete
// specialization instead, `template_of(dealias(^^std::optional<int>))`.

#include <meta>

export module storm_std_meta;

import std; // not used here, but it puts `std` in the BMI closure of a TU that imports only this module

// Re-exporting into std::meta is the point of this module, and the names are consumed by the
// importers, never here. The using-declarations introduce no new entities, they re-export the
// ones <meta> declares (the technique libc++'s own std.cppm uses); [namespace.std] still calls
// adding to namespace std undefined, so this is a deliberate choice pinned to this toolchain,
// and the module becomes redundant once libc++'s std module exports std::meta.
// NOLINTBEGIN(cert-dcl58-cpp, misc-unused-using-decls)
export namespace std::meta {
    using std::meta::access_context;
    using std::meta::annotations_of;
    using std::meta::data_member_spec;
    using std::meta::dealias;
    using std::meta::define_aggregate;
    using std::meta::extract;
    using std::meta::has_default_member_initializer;
    using std::meta::has_identifier;
    using std::meta::has_template_arguments;
    using std::meta::identifier_of;
    using std::meta::info;
    using std::meta::is_class_type;
    using std::meta::is_nonstatic_data_member;
    using std::meta::is_type;
    using std::meta::nonstatic_data_members_of;
    using std::meta::parent_of;
    using std::meta::reflect_constant;
    using std::meta::substitute;
    using std::meta::template_arguments_of;
    using std::meta::template_of;
    using std::meta::type_of;
} // namespace std::meta
// NOLINTEND(cert-dcl58-cpp, misc-unused-using-decls)
