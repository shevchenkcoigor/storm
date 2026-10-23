# Compile-Time SQL Generation

Storm ORM generates SQL at compile-time using ConstexprString, eliminating runtime SQL construction overhead.

## ConstexprString Utility

Fixed-size compile-time string builder:

```cpp
template <size_t N>
struct ConstexprString {
    std::array<char, N> data{};
    size_t len = 0;

    consteval auto append(std::string_view str) -> ConstexprString& {
        for (char c : str) {
            data[len++] = c;
        }
        return *this;
    }
};
```

## Compile-Time SQL Generation

Generate SQL during compilation:

```cpp
// Simplified sketch of InsertGrammar (src/orm/statements/insert_grammar.cppm, #671):
// the real builder is build_insert_sql_array_impl<bool> -> insert_sql_array/insert_sql_string.
template <typename T> struct InsertGrammar {
    static consteval auto build_insert_sql_array() {
        ConstexprString<sql_size> result;
        result.append("INSERT INTO ");
        result.append(Base::table_name_);
        result.append(" (").append(field_names_).append(") VALUES (");
        result.append(placeholders_).append(")");
        return result;
    }

    // Generated at compile-time
    static constexpr auto sql_array = build_insert_sql_array();

    // Runtime string (one-time construction)
    static inline const std::string sql_string =
        std::string{sql_array.data.data(), sql_array.len};
};
```

## Benefits

✅ **Zero runtime generation** - SQL built during compilation
✅ **Exact memory allocation** - Know size at compile-time
✅ **Compile-time validation** - Syntax errors caught early
✅ **Cache-friendly** - Static strings live in read-only memory

## See Also

- [Reflection](REFLECTION.md) - How field info enables SQL generation
- [Statement Caching](STATEMENT_CACHING.md) - What happens after generation
