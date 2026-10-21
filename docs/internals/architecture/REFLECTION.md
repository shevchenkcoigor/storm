# C++26 Reflection in Storm ORM

Storm ORM uses cutting-edge C++26 reflection (`std::meta`) to automatically map C++ structs to database tables without macros.

## Core Concepts

### Reflection Attributes

Mark fields with reflection attributes:

```cpp
struct Person {
    [[=primary]] int id;
    std::string name;
    int age;
};
```

**Supported attributes**:
- `primary` - Marks primary key field
- Foreign keys (`fk<>`), many-to-many, and auto-timestamps are now supported; more attributes (unique, index) are planned

### Compile-Time Field Discovery

Extract field information at compile-time:

```cpp
template <typename T>
static consteval auto get_all_field_members() {
    std::array<std::meta::info, field_count> members;
    size_t idx = 0;

    // Iterate over struct members
    for (auto member : members_of(^^T)) {
        if (is_nonstatic_data_member(member)) {
            members[idx++] = member;
        }
    }

    return members;
}
```

### Field Metadata Extraction

Use reflection to extract field properties:

```cpp
for (auto member : all_members_) {
    // Get field name
    std::string_view name = std::meta::identifier_of(member);

    // Get field type
    auto type_info = std::meta::type_of(member);

    // Check for attributes
    bool is_primary = has_attribute(member, primary);
}
```

## WHERE Clause Reflection

Pure C++26 reflection for type-safe WHERE clauses:

```cpp
template <std::meta::info FieldInfo>
auto field() {
    // Extract at compile-time
    constexpr auto field_name = std::meta::identifier_of(FieldInfo);
    constexpr auto field_type = std::meta::type_of(FieldInfo);
    constexpr auto parent_type = std::meta::parent_of(FieldInfo);

    return Field<parent_type, field_type>{
        .table_name = get_table_name<parent_type>(),
        .field_name = field_name
    };
}

// Usage: fields::Person.age > 30
```

**No macros needed** - fully module-compatible!

## Reflection Splice Operator

Access fields dynamically using reflection:

```cpp
// Get primary key value from object
auto pk_value = obj.[:primary_key_:];

// Set field value
obj.[:member:] = extracted_value;
```

**Benefits**:
- Dynamic field access without `std::get`
- Type-safe at compile-time
- Zero runtime overhead

## See Also

- [SQL Generation](SQL_GENERATION.md) - How reflection enables SQL generation
- [WHERE Clauses](../../guide/features/WHERE_CLAUSES.md) - Reflection-based filtering
- [Module System](MODULE_SYSTEM.md) - How reflection works with modules
