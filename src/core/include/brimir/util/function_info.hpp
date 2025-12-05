#pragma once

/**
@file
@brief Defines `fninfo::FunctionInfo`, a type that extracts information from any function type.
*/

#include <tuple>
#include <type_traits>

namespace brimir::fninfo {

/// @brief Information about function types matching `TReturn(TArgs...)`
///
/// Function types describe the signature of a function.
///
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the argument types of the function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename TReturn, typename... TArgs>
struct FunctionType {
    using pointer_type = TReturn (*)(TArgs...);
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = isVariadic;
};

// ----------------------------------------------------------------------------

/// @brief Information about freestanding functions matching `TReturn (*)(TArgs...)`.
///
/// Freestanding functions are either regular C-like functions, coerced lambdas without capture, or static class member
/// functions.
///
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the argument types of the function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename TReturn, typename... TArgs>
struct FreestandingFunction {
    using pointer_type = TReturn (*)(TArgs...);
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = isVariadic;
};

// ----------------------------------------------------------------------------

/// @brief Information about member functions.
///
/// Member functions belong to a class and must be invoked with a particular instance of that class, that is, they are
/// non-static methods of a class.
///
/// @tparam T the type of the member function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename T>
struct MemberFunction {};

/// @brief Information about member function pointers matching `TReturn (TClass::*)(TArgs...)` without cv-qualifiers.
/// @tparam TReturn the function return type
/// @tparam TClass the class type that contains the function
/// @tparam ...TArgs the argument types of the function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename TReturn, typename TClass, typename... TArgs>
struct MemberFunction<isVariadic, TReturn (TClass::*)(TArgs...)> {
    using pointer_type = TReturn (TClass::*)(TArgs...);
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = true;
    static constexpr bool is_variadic = isVariadic;
};

/// @brief Information about member function pointers matching `TReturn (TClass::*)(TArgs...) const`.
/// @tparam TReturn the function return type
/// @tparam TClass the class type that contains the function
/// @tparam ...TArgs the argument types of the function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename TReturn, typename TClass, typename... TArgs>
struct MemberFunction<isVariadic, TReturn (TClass::*)(TArgs...) const> {
    using pointer_type = TReturn (TClass::*)(TArgs...) const;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = true;
    static constexpr bool is_variadic = isVariadic;
};

/// @brief Information about member function pointers matching `TReturn (TClass::*)(TArgs...) volatile`.
/// @tparam TReturn the function return type
/// @tparam TClass the class type that contains the function
/// @tparam ...TArgs the argument types of the function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename TReturn, typename TClass, typename... TArgs>
struct MemberFunction<isVariadic, TReturn (TClass::*)(TArgs...) volatile> {
    using pointer_type = TReturn (TClass::*)(TArgs...);
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = true;
    static constexpr bool is_variadic = isVariadic;
};

/// @brief Information about member function pointers matching `TReturn (TClass::*)(TArgs...) const volatile`.
/// @tparam TReturn the function return type
/// @tparam TClass the class type that contains the function
/// @tparam ...TArgs the argument types of the function
/// @tparam isVariadic whether the function takes variadic arguments (`...`)
template <bool isVariadic, typename TReturn, typename TClass, typename... TArgs>
struct MemberFunction<isVariadic, TReturn (TClass::*)(TArgs...) const volatile> {
    using pointer_type = TReturn (TClass::*)(TArgs...) const volatile;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = true;
    static constexpr bool is_variadic = isVariadic;
};

// ----------------------------------------------------------------------------

/// @brief Information about functors.
///
/// Functors are structs/classes containing the callable `operator()`. This includes lambdas, both with and without
/// capture.
///
/// @tparam T the functor type
template <typename T>
struct Functor {};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs...)` without cv-qualifiers.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs...)> {
    using pointer_type = const TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = false;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs..., ...)` without cv-qualifiers.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs..., ...)> {
    using pointer_type = const TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = true;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs...) const`.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs...) const> {
    using pointer_type = TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_variadic = false;
    static constexpr bool is_member_function_pointer = false;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs..., ...) const`.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs..., ...) const> {
    using pointer_type = TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = true;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs...) volatile`.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs...) volatile> {
    using pointer_type = const TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = false;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs..., ...) volatile`.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs..., ...) volatile> {
    using pointer_type = const TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = true;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs...) const volatile`.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs...) const volatile> {
    using pointer_type = TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = false;
};

/// @brief Information about functions matching `TReturn TClass::operator()(TArgs..., ...) const volatile`.
/// @tparam TClass the class type of the functor
/// @tparam TReturn the return type of the function
/// @tparam ...TArgs the arguments of the function
template <typename TClass, typename TReturn, typename... TArgs>
struct Functor<TReturn (TClass::*)(TArgs..., ...) const volatile> {
    using pointer_type = TClass *;
    using function_type = TReturn(TArgs...);
    using return_type = TReturn;
    using arg_types = std::tuple<TArgs...>;
    static constexpr bool is_member_function_pointer = false;
    static constexpr bool is_variadic = true;
};

// ----------------------------------------------------------------------------

namespace detail {
    /// @brief Base template type that automatically identifies function types from the raw type.
    /// @tparam TFunc the (possible) function type
    template <typename TFunc>
    struct FunctionIdentifier {
        // TFunc is not a callable function, functor or member function pointer
        // type is intentionally left undefined to enable SFINAE
    };

    /// @brief Identifies function types.
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn(TArgs...)> {
        using type = FunctionType<false, TReturn, TArgs...>;
    };

    /// @brief Identifies variadic function types.
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn(TArgs..., ...)> {
        using type = FunctionType<true, TReturn, TArgs...>;
    };

    /// @brief Identifies freestanding functions passed by reference.
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (&)(TArgs...)> {
        using type = FreestandingFunction<false, TReturn, TArgs...>;
    };

    /// @brief Identifies variadic freestanding functions passed by reference.
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (&)(TArgs..., ...)> {
        using type = FreestandingFunction<true, TReturn, TArgs...>;
    };

    /// @brief Identifies freestanding functions passed by pointer.
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (*)(TArgs...)> {
        using type = FreestandingFunction<false, TReturn, TArgs...>;
    };

    /// @brief Identifies variadic freestanding functions passed by pointer.
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (*)(TArgs..., ...)> {
        using type = FreestandingFunction<true, TReturn, TArgs...>;
    };

    /// @brief Identifies member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs...)> {
        using type = MemberFunction<false, TReturn (TClass::*)(TArgs...)>;
    };

    /// @brief Identifies variadic member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs..., ...)> {
        using type = MemberFunction<true, TReturn (TClass::*)(TArgs...)>;
    };

    /// @brief Identifies `const`-qualified member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs...) const> {
        using type = MemberFunction<false, TReturn (TClass::*)(TArgs...) const>;
    };

    /// @brief Identifies variadic `const`-qualified member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs..., ...) const> {
        using type = MemberFunction<true, TReturn (TClass::*)(TArgs...) const>;
    };

    /// @brief Identifies `volatile`-qualified member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs...) volatile> {
        using type = MemberFunction<false, TReturn (TClass::*)(TArgs...) volatile>;
    };

    /// @brief Identifies variadic `volatile`-qualified member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs..., ...) volatile> {
        using type = MemberFunction<true, TReturn (TClass::*)(TArgs...) volatile>;
    };

    /// @brief Identifies `const`- and `volatile`-qualified member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs...) const volatile> {
        using type = MemberFunction<false, TReturn (TClass::*)(TArgs...) const volatile>;
    };

    /// @brief Identifies variadic `const`- and `volatile`-qualified member function pointers.
    /// @tparam TClass the class type
    /// @tparam TReturn the return type of the function
    /// @tparam ...TArgs the argument types of the function
    template <typename TClass, typename TReturn, typename... TArgs>
    struct FunctionIdentifier<TReturn (TClass::*)(TArgs..., ...) const volatile> {
        using type = MemberFunction<true, TReturn (TClass::*)(TArgs...) const volatile>;
    };

    /// @brief Identifies functors.
    /// @tparam TFunc the functor type
    template <typename TFunc>
        requires requires() { &TFunc::operator(); }
    struct FunctionIdentifier<TFunc> {
        using type = Functor<decltype(&TFunc::operator())>;
    };

} // namespace detail

// ----------------------------------------------------------------------------

/// @brief Exposes information about a function type.
///
/// If `TFunc` is a valid callable function, functor or member function pointer, the following fields and types are
/// defined:
/// - type `pointer_type`: the function pointer type: `TReturn ([TClass::]*)(TArgs...[, ...])` or `TClass *`
/// - type `function_type`: the function type: `TReturn(TArgs...[, ...])`
/// - type `return_type`: the return type of the function: `TReturn`
/// - type `arg_types`: a tuple of the function's argument types: `std::tuple<TArgs...>`
/// - `static bool is_member_function_pointer`: indicates if the function is a member function pointer
/// - `static bool is_variadic`: indicates if the function is variadic (takes the ellipsis argument: `...`)
///
/// @tparam TFunc the (possible) function type
template <typename TFunc>
using FunctionInfo = typename detail::FunctionIdentifier<std::remove_cvref_t<std::decay_t<TFunc>>>::type;

/// @brief Determines if a function of type `T2` can be assigned to a variable of type `T1`.
///
/// Both functions must have the same signature and either be both member function pointers or both not.
///
/// @tparam T1 the first function type
/// @tparam T2 the second function type
template <typename T1, typename T2>
inline constexpr bool IsAssignable =
    std::is_same_v<typename fninfo::FunctionInfo<T1>::function_type,
                   typename fninfo::FunctionInfo<T2>::function_type> &&
    fninfo::FunctionInfo<T1>::is_member_function_pointer == fninfo::FunctionInfo<T2>::is_member_function_pointer;

} // namespace fninfo
