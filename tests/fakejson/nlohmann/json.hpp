// Host-side fake of <nlohmann/json.hpp> for tests that only need the tiny
// slice of the API the modules and the mod-menu builder use (contains / items /
// operator[] / get<T> / scalar assignment). Prefer the real nlohmann_json
// headers from the xmake package cache when they are available (see
// scripts/run_tests.sh); this fake exists so those tests can still run
// standalone.
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

    // Real nlohmann_json returns a proxy whose iterator yields key/value pairs;
    // the backing map already does, which is all the callers here need.
    std::map<std::string, json>& items() { return m_children; }
    const std::map<std::string, json>& items() const { return m_children; }

    json& operator[](const std::string& key) { return m_children[key]; }

    const json& operator[](const std::string& key) const {
        static const json empty;
        const auto it = m_children.find(key);
        return it == m_children.end() ? empty : it->second;
    }

    bool is_string() const { return m_type == Type::String; }
    bool is_number_integer() const { return m_type == Type::Integer; }
    bool is_number_float() const { return m_type == Type::Float; }
    bool is_number() const { return is_number_integer() || is_number_float(); }
    bool is_boolean() const { return m_type == Type::Boolean; }

    template <class T>
    T get() const {
        if constexpr (std::is_same_v<T, bool>) {
            return m_integer != 0;
        } else if constexpr (std::is_integral_v<T>) {
            return static_cast<T>(m_integer);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (m_type == Type::Float) return static_cast<T>(m_float);
            return static_cast<T>(m_integer);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return m_string;
        } else {
            return T{};
        }
    }

    json& operator=(int value) { m_type = Type::Integer; m_integer = value; m_float = static_cast<double>(value); return *this; }
    // ModuleMenu.cpp writes strtol() results back, which are long; without an
    // exact overload the assignment is ambiguous against the float ones.
    json& operator=(long value) { m_type = Type::Integer; m_integer = value; m_float = static_cast<double>(value); return *this; }
    json& operator=(bool value) { m_type = Type::Boolean; m_integer = value ? 1 : 0; m_float = value ? 1.0 : 0.0; return *this; }
    json& operator=(float value) { m_type = Type::Float; m_float = value; m_integer = static_cast<long long>(value); return *this; }
    json& operator=(double value) { m_type = Type::Float; m_float = value; m_integer = static_cast<long long>(value); return *this; }
    json& operator=(const char* value) { return (*this) = std::string(value); }
    json& operator=(const std::string& value) { m_type = Type::String; m_string = value; return *this; }

private:
    enum class Type { Null, Boolean, Integer, Float, String };

    std::map<std::string, json> m_children;
    Type m_type = Type::Null;
    long long m_integer = 0;
    double m_float = 0.0;
    std::string m_string;
};

} // namespace nlohmann
