#include <Functions/FunctionFactory.h>
#include <Functions/FunctionUnaryArithmetic.h>

namespace DB
{
namespace ErrorCodes
{
extern const int NOT_IMPLEMENTED;
}

namespace
{

template <typename T>
requires std::is_integral_v<T>
T byteSwap(T x)
{
    return std::byteswap(x);
}

template <typename T>
requires std::is_same_v<T, UInt128> || std::is_same_v<T, Int128> || std::is_same_v<T, UInt256> || std::is_same_v<T, Int256>
T byteSwap(T x)
{
    T dest;
    reverseMemcpy(&dest, &x, sizeof(T));
    return dest;
}

template <typename T>
T byteSwap(T)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "byteSwap is not implemented for {} datatype", demangle(typeid(T).name()));
}

template <typename T>
struct ByteSwapImpl
{
    using ResultType = T;
    static constexpr const bool allow_string_or_fixed_string = false;
    static T apply(T x) { return byteSwap<T>(x); }

#if USE_EMBEDDED_COMPILER
    static constexpr bool compilable = false;
#endif
};

struct NameByteSwap
{
    static constexpr auto name = "byteSwap";
};
using FunctionByteSwap = FunctionUnaryArithmetic<ByteSwapImpl, NameByteSwap, true>;

}

template <>
struct FunctionUnaryArithmeticMonotonicity<NameByteSwap>
{
    static bool has() { return false; }
    static IFunction::Monotonicity get(const IDataType &, const Field &, const Field &) { return {}; }
};

REGISTER_FUNCTION(ByteSwap)
{
    FunctionDocumentation::Description description = R"(
Reverses the bytes of an integer, i.e. changes its [endianness](https://en.wikipedia.org/wiki/Endianness).

The below example can be worked out in the following manner:

1. Convert the base-10 integer to its equivalent hexadecimal format in big-endian format, i.e. 3351772109 -> C7 C7 FB CD (4 bytes)
2. Reverse the bytes, i.e. C7 C7 FB CD -> CD FB C7 C7
3. Convert the result back to an integer assuming big-endian, i.e. CD FB C7 C7 -> 3455829959
One use case of this function is reversing IPv4s:

```result
┌─toIPv4(byteSwap(toUInt32(toIPv4('205.251.199.199'))))─┐
│ 199.199.251.205                                       │
└───────────────────────────────────────────────────────┘
```
    )";
    FunctionDocumentation::Syntax syntax = "byteSwap(x)";
    FunctionDocumentation::Argument argument1 = {"x", "An integer value.", {"(U)Int*"}};
    FunctionDocumentation::Arguments arguments = {argument1};
    FunctionDocumentation::ReturnedValue returned_value = {"Returns `x` with bytes reversed.", {"(U)Int*"}};
    FunctionDocumentation::Example example1 = {"Usage example", "SELECT byteSwap(3351772109)", "3455829959"};
    FunctionDocumentation::Example example2 = {"8-bit", "SELECT byteSwap(54)", "54"};
    FunctionDocumentation::Example example3 = {"16-bit", "SELECT byteSwap(4135)", "10000"};
    FunctionDocumentation::Example example4 = {"32-bit", "SELECT byteSwap(3351772109)", "3455829959"};
    FunctionDocumentation::Example example5 = {"64-bit", "SELECT byteSwap(123294967295)", "18439412204227788800"};
    FunctionDocumentation::Examples examples = {example1, example2, example3, example4, example5};
    FunctionDocumentation::IntroducedIn introduced_in = {23, 10};
    FunctionDocumentation::Category categories = FunctionDocumentation::Category::Arithmetic;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, examples, introduced_in, categories};
    factory.registerFunction<FunctionByteSwap>(documentation,FunctionFactory::Case::Insensitive);
}

}
