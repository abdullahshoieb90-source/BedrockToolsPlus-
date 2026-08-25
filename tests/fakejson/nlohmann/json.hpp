// Host-side fake of <nlohmann/json.hpp> for tests that only need the tiny
// slice of the API the modules use (contains / operator[] / get<T> /
// scalar assignment). Prefer the real nlohmann_json headers from the xmake
// package cache when they are available (see scripts/run_tests.sh); this
// fake exists so those tests can still run standalone.
#pragma once

#include <map>
#include <string>
#include <type_traits>

namespace nlohmann {

class json {
public:
    json() = default;
    json(const json&) = default;
    json& operator=(const json&) = default;

    bool contains(const std::string& key) const {
        return m_children.find(key) != m_children.end();
    }

    json& operator[](const std::string& key) { return m_children[key]; }

    const json& operator[](const std::string& key) const {
        static const json empty;
        const auto it = m_children.find(key);
        return it == m_children.end() ? empty : it->second;
    }

    bool is_string() const { return m_type == Type::String; }
    bool is_number_integer() const { return m_type == Type::Integer; }
    bool is_boolean() const { return m_type == Type::Boolean; }

    template <class T>
    T get() const {
        if constexpr (std::is_same_v<T, bool>) {
            return m_integer != 0;
        } else if constexpr (std::is_integral_v<T>) {
            return static_cast<T>(m_integer);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return m_string;
        } else {
            return T{};
        }
    }

    json& operator=(int value) { m_type = Type::Integer; m_integer = value; return *this; }
    json& operator=(bool value) { m_type = Type::Boolean; m_integer = value ? 1 : 0; return *this; }
    json& operator=(double value) { m_type = Type::Integer; m_integer = static_cast<long long>(value); return *this; }
    json& operator=(const char* value) { return (*this) = std::string(value); }
    json& operator=(const std::string& value) { m_type = Type::String; m_string = value; return *this; }

private:
    enum class Type { Null, Boolean, Integer, String };

    std::map<std::string, json> m_children;
    Type m_type = Type::Null;
    long long m_integer = 0;
    std::string m_string;
};

} // namespace nlohmann
