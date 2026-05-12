#pragma once

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#if __has_include(<expected>)
#include <expected>
#endif

#if !defined(__cpp_lib_expected)
namespace std {

template <typename E>
class unexpected {
public:
    constexpr explicit unexpected(const E& error) : m_error(error) {}
    constexpr explicit unexpected(E&& error) : m_error(std::move(error)) {}

    constexpr const E& error() const& noexcept { return m_error; }
    constexpr E& error() & noexcept { return m_error; }
    constexpr E&& error() && noexcept { return std::move(m_error); }

private:
    E m_error;
};

template <typename E>
unexpected(E) -> unexpected<E>;

template <typename E>
class bad_expected_access : public std::runtime_error {
public:
    explicit bad_expected_access(E error)
        : std::runtime_error("bad expected access"), m_error(std::move(error)) {}

    const E& error() const& noexcept { return m_error; }
    E& error() & noexcept { return m_error; }

private:
    E m_error;
};

template <typename T, typename E>
class expected {
public:
    expected() requires std::is_default_constructible_v<T> : m_storage(T{}) {}
    expected(const T& value) : m_storage(value) {}
    expected(T&& value) : m_storage(std::move(value)) {}
    expected(const unexpected<E>& error) : m_storage(error.error()) {}
    expected(unexpected<E>&& error) : m_storage(std::move(error).error()) {}

    constexpr bool has_value() const noexcept { return std::holds_alternative<T>(m_storage); }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        if (!has_value()) {
            throw bad_expected_access<E>(std::get<E>(m_storage));
        }
        return std::get<T>(m_storage);
    }

    const T& value() const& {
        if (!has_value()) {
            throw bad_expected_access<E>(std::get<E>(m_storage));
        }
        return std::get<T>(m_storage);
    }

    T&& value() && {
        if (!has_value()) {
            throw bad_expected_access<E>(std::get<E>(m_storage));
        }
        return std::move(std::get<T>(m_storage));
    }

    E& error() & { return std::get<E>(m_storage); }
    const E& error() const& { return std::get<E>(m_storage); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    std::variant<T, E> m_storage;
};

template <typename E>
class expected<void, E> {
public:
    expected() : m_storage(std::monostate{}) {}
    expected(const unexpected<E>& error) : m_storage(error.error()) {}
    expected(unexpected<E>&& error) : m_storage(std::move(error).error()) {}

    constexpr bool has_value() const noexcept { return std::holds_alternative<std::monostate>(m_storage); }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) {
            throw bad_expected_access<E>(std::get<E>(m_storage));
        }
    }

    E& error() & { return std::get<E>(m_storage); }
    const E& error() const& { return std::get<E>(m_storage); }

private:
    std::variant<std::monostate, E> m_storage;
};

} // namespace std
#endif
