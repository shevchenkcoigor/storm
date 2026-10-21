#include <meta>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/list.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

import storm;
import std;

using storm::QuerySet;
using storm::db::DefaultConnection;
using storm::orm::schema::SchemaStatement;

namespace nb = nanobind;

// Minimal model for proof of concept — hardcoded in C++.
// Phase 2 would register user-defined structs dynamically.
struct PyPerson {
    [[= storm::primary]] int id{};
    std::string              name;
    int                      age{};
};

using PersonQS = QuerySet<PyPerson, storm::db::sqlite::Connection>;

namespace {

    // Pre-compute field members at compile time (must be consteval for reflection).
    consteval auto py_person_field_count() -> size_t {
        return std::meta::nonstatic_data_members_of(^^PyPerson, std::meta::access_context::unchecked()).size();
    }

    consteval auto py_person_fields() -> std::array<std::meta::info, py_person_field_count()> {
        auto members = std::meta::nonstatic_data_members_of(^^PyPerson, std::meta::access_context::unchecked());
        std::array<std::meta::info, py_person_field_count()> result{};
        for (size_t i = 0; i < members.size(); ++i) {
            result[i] = members[i];
        }
        return result;
    }

    constexpr auto kPyPersonFields = py_person_fields();

    // ── Module-level cached QuerySet ─────────────────────────────────────
    // Avoids constructing a new PersonQS on every Python call.
    // Reset by connect() so it picks up the new thread-local connection.
    std::optional<PersonQS> g_qs;

    auto get_qs() -> PersonQS& {
        if (!g_qs) {
            g_qs.emplace();
        }
        return *g_qs;
    }

    // ── Filter expression objects ────────────────────────────────────────
    // Captured by Python-side operator overloads (FieldProxy.__gt__, etc.)
    // and dispatched to compile-time Storm expressions in select_where().

    struct FilterExpr {
        std::string field_name;
        std::string op;
        nb::object  value;
    };

    struct FieldProxy {
        std::string field_name;
    };

    // ── Reflection-based WHERE dispatch ──────────────────────────────────
    // Expansion statement iterates PyPerson fields at compile time;
    // runtime string comparison picks the matching branch.

    auto execute_where(const FilterExpr& expr) -> std::vector<PyPerson> {
        using storm::orm::where::Field;

        auto run = [](auto&& e) -> std::vector<PyPerson> {
            PersonQS q;
            auto     res = q.where(std::forward<decltype(e)>(e)).select().execute();
            if (!res) {
                throw std::runtime_error("Select failed: " + std::string(res.error().message()));
            }
            std::vector<PyPerson> out;
            for (const auto& p : res.value()) {
                out.push_back(p);
            }
            return out;
        };

        template for (constexpr auto member : kPyPersonFields) {
            if (expr.field_name == std::meta::identifier_of(member)) {
                using T = typename[:std::meta::type_of(member):];
                auto v  = nb::cast<T>(expr.value);

                if (expr.op == "==")
                    return run(Field<member>{} == v);
                if (expr.op == "!=")
                    return run(Field<member>{} != v);

                if constexpr (std::is_arithmetic_v<T>) {
                    if (expr.op == ">")
                        return run(Field<member>{} > v);
                    if (expr.op == ">=")
                        return run(Field<member>{} >= v);
                    if (expr.op == "<")
                        return run(Field<member>{} < v);
                    if (expr.op == "<=")
                        return run(Field<member>{} <= v);
                }
                if constexpr (std::is_same_v<T, std::string>) {
                    if (expr.op == "like")
                        return run(Field<member>{}.like(v));
                }

                throw std::invalid_argument("Unsupported op '" + expr.op + "' for field '" + expr.field_name + "'");
            }
        }
        throw std::invalid_argument("Unknown field: " + expr.field_name);
    }

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage,modernize-use-trailing-return-type,readability-named-parameter)
NB_MODULE(_storm, m) {
    m.doc() = "Storm ORM — Python bindings (proof of concept)";

    // ── FilterExpr + FieldProxy (WHERE expression objects) ──────────
    nb::class_<FilterExpr>(m, "FilterExpr").def("__repr__", [](const FilterExpr& e) {
        return "FilterExpr(" + e.field_name + " " + e.op + " ...)";
    });

    nb::class_<FieldProxy>(m, "FieldProxy")
            .def("__eq__", [](const FieldProxy& f, nb::object v) { return FilterExpr{f.field_name, "==", v}; })
            .def("__ne__", [](const FieldProxy& f, nb::object v) { return FilterExpr{f.field_name, "!=", v}; })
            .def("__gt__", [](const FieldProxy& f, nb::object v) { return FilterExpr{f.field_name, ">", v}; })
            .def("__ge__", [](const FieldProxy& f, nb::object v) { return FilterExpr{f.field_name, ">=", v}; })
            .def("__lt__", [](const FieldProxy& f, nb::object v) { return FilterExpr{f.field_name, "<", v}; })
            .def("__le__", [](const FieldProxy& f, nb::object v) { return FilterExpr{f.field_name, "<=", v}; })
            .def("like",
                 [](const FieldProxy& f, const std::string& pat) {
                     return FilterExpr{f.field_name, "like", nb::cast(pat)};
                 })
            .def("__repr__", [](const FieldProxy& f) { return "FieldProxy('" + f.field_name + "')"; });

    // ── Person class ────────────────────────────────────────────────
    auto person_cls = nb::class_<PyPerson>(m, "Person")
                              .def(nb::init<>())
                              .def(
                                      "__init__",
                                      [](PyPerson* p, std::string name, int age) {
                                          new (p) PyPerson{.name = std::move(name), .age = age};
                                      },
                                      nb::arg("name"),
                                      nb::arg("age")
                              )
                              .def_rw("id", &PyPerson::id)
                              .def_rw("name", &PyPerson::name)
                              .def_rw("age", &PyPerson::age)
                              .def("__repr__", [](const PyPerson& p) {
                                  return "Person(id=" + std::to_string(p.id) + ", name='" + p.name +
                                         "', age=" + std::to_string(p.age) + ")";
                              });

    // Register column proxies as class attributes (Person.c_age, Person.c_name, ...)
    // using reflection — no hardcoded field list.
    // NOLINTBEGIN(modernize-use-trailing-return-type)
    auto register_proxy = [&person_cls](const std::string& name) {
        person_cls.def_prop_ro_static(std::string("c_" + name).c_str(), [name](nb::handle) {
            return FieldProxy{name};
        });
    };
    // NOLINTEND(modernize-use-trailing-return-type)
    template for (constexpr auto member : kPyPersonFields) {
        register_proxy(std::string(std::meta::identifier_of(member)));
    }

    // ── Connection management ───────────────────────────────────────
    m.def(
            "connect",
            [](const std::string& db_path) {
                auto result = DefaultConnection<>::set(db_path);
                if (!result) {
                    throw std::runtime_error("Failed to connect: " + std::string(result.error().message()));
                }
                g_qs.emplace(); // reset cached QS to bind to new connection
            },
            nb::arg("db_path"),
            "Open a SQLite database and set it as the default connection."
    );

    // ── Schema management ───────────────────────────────────────────
    m.def("create_table", []() {
        auto conn   = DefaultConnection<>::get();
        auto result = SchemaStatement<PyPerson>::create_table_if_not_exists(conn);
        if (!result) {
            throw std::runtime_error("Failed to create table: " + std::string(result.error().message()));
        }
    });

    // ── Insert ──────────────────────────────────────────────────────
    m.def(
            "insert",
            [](PyPerson& person) -> int {
                auto result = get_qs().insert(person).execute();
                if (!result) {
                    throw std::runtime_error("Insert failed: " + std::string(result.error().message()));
                }
                person.id = static_cast<int>(result.value());
                return person.id;
            },
            nb::arg("person"),
            "Insert a single Person. Returns the auto-generated ID."
    );

    // fast_insert: const char* avoids intermediate std::string — nanobind passes
    // Python's internal UTF-8 buffer pointer directly.
    m.def(
            "fast_insert",
            [](const char* name, int age) -> int {
                PyPerson p{.name = name, .age = age};
                auto     result = get_qs().insert(p).execute();
                if (!result) {
                    throw std::runtime_error("Insert failed: " + std::string(result.error().message()));
                }
                return static_cast<int>(result.value());
            },
            nb::arg("name"),
            nb::arg("age"),
            "Insert a Person by field values (no Person object needed). Returns the auto-generated ID."
    );

    // fast_insert_many: loop in C++ — pays nanobind crossing cost once, not N times.
    m.def(
            "fast_insert_many",
            [](const std::vector<std::string>& names, const std::vector<int>& ages) {
                if (names.size() != ages.size()) {
                    throw std::invalid_argument("names and ages must have equal length");
                }
                auto& qs = get_qs();
                for (size_t i = 0; i < names.size(); ++i) {
                    PyPerson p{.name = names[i], .age = ages[i]};
                    auto     result = qs.insert(p).execute();
                    if (!result) {
                        throw std::runtime_error("Insert failed: " + std::string(result.error().message()));
                    }
                }
            },
            nb::arg("names"),
            nb::arg("ages"),
            "Insert many Persons from parallel name/age lists — loop runs in C++."
    );

    m.def(
            "bulk_insert",
            [](std::vector<PyPerson>& persons) {
                auto result = get_qs().insert(std::span<const PyPerson>(persons)).execute();
                if (!result) {
                    throw std::runtime_error("Bulk insert failed: " + std::string(result.error().message()));
                }
            },
            nb::arg("persons"),
            "Insert multiple Persons in a single batch operation."
    );

    // ── Select ──────────────────────────────────────────────────────
    m.def("select", []() -> std::vector<PyPerson> {
        PersonQS qs;
        auto     result = qs.select().execute();
        if (!result) {
            throw std::runtime_error("Select failed: " + std::string(result.error().message()));
        }
        std::vector<PyPerson> out;
        for (const auto& p : result.value()) {
            out.push_back(p);
        }
        return out;
    });

    // ── Select as NumPy structured array (zero Python object construction) ──
    // dtype: [('id','<i4'),('name','S64'),('age','<i4')] — 72 bytes/row, no padding.
    // numpy allocates the buffer; we write into it directly via the buffer protocol.
    m.def(
            "select_array",
            []() -> nb::object {
                PersonQS qs;
                auto     result = qs.select().execute();
                if (!result) {
                    throw std::runtime_error("Select failed: " + std::string(result.error().message()));
                }
                const auto&  hive = result.value();
                const size_t n    = hive.size();

                nb::object np = nb::module_::import_("numpy");
                // np.dtype() needs a LIST of (name,dtype) tuples — a tuple is interpreted
                // as (base_dtype, shape), causing "Tuple must have size 2" errors.
                nb::list field_specs;
                field_specs.append(nb::make_tuple("id", "<i4"));
                field_specs.append(nb::make_tuple("name", "S64"));
                field_specs.append(nb::make_tuple("age", "<i4"));
                nb::object dtype = np.attr("dtype")(field_specs);
                nb::object arr   = np.attr("empty")(n, dtype);

                // Write directly into numpy's buffer via the CPython buffer protocol.
                // Layout confirmed: id@0(4) name@4(64) age@68(4) itemsize=72 — no padding.
                static constexpr size_t ITEMSIZE = 72;
                static constexpr size_t OFF_ID   = 0;
                static constexpr size_t OFF_NAME = 4;
                static constexpr size_t OFF_AGE  = 68;

                Py_buffer view{};
                if (PyObject_GetBuffer(arr.ptr(), &view, PyBUF_WRITABLE) != 0) {
                    throw std::runtime_error("Failed to get writable buffer from numpy array");
                }
                auto* buf = static_cast<uint8_t*>(view.buf);

                size_t i = 0;
                for (const auto& p : hive) {
                    uint8_t* row                               = buf + i * ITEMSIZE;
                    *reinterpret_cast<int32_t*>(row + OFF_ID)  = static_cast<int32_t>(p.id);
                    *reinterpret_cast<int32_t*>(row + OFF_AGE) = static_cast<int32_t>(p.age);
                    const size_t len                           = std::min(p.name.size(), size_t{63});
                    std::memcpy(row + OFF_NAME, p.name.data(), len);
                    std::memset(row + OFF_NAME + len, '\0', 64 - len);
                    ++i;
                }
                PyBuffer_Release(&view);
                return arr;
            },
            "Select all persons as a NumPy structured array — zero Python object construction per row."
    );

    // ── Filtered select (WHERE) ─────────────────────────────────────
    m.def(
            "select_where",
            [](const FilterExpr& expr) -> std::vector<PyPerson> { return execute_where(expr); },
            nb::arg("expr"),
            "Select persons matching a WHERE expression.\n"
            "Usage: select_where(Person.c_age > 30)"
    );

    // ── Remove ──────────────────────────────────────────────────────
    m.def("remove_all", []() {
        auto result = get_qs().erase_all().execute();
        if (!result) {
            throw std::runtime_error("Remove failed: " + std::string(result.error().message()));
        }
    });

    m.def(
            "remove",
            [](const PyPerson& person) {
                auto result = get_qs().erase(person).execute();
                if (!result) {
                    throw std::runtime_error("Remove failed: " + std::string(result.error().message()));
                }
            },
            nb::arg("person"),
            "Remove a single Person by primary key."
    );

    // ── Count ───────────────────────────────────────────────────────
    m.def("count", []() -> int64_t {
        auto result = get_qs().count().execute();
        if (!result) {
            throw std::runtime_error("Count failed: " + std::string(result.error().message()));
        }
        return result.value();
    });
}
