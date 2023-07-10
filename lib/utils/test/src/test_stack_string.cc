#include "doctest.h"
#include "utils/stack_string.h"

using namespace FlexFlow;

TEST_CASE_TEMPLATE("StackStringConstruction", T, char) {
  constexpr std::size_t MAXSIZE = 5;
  using StackString = stack_string<MAXSIZE>;

  SUBCASE("DefaultConstruction") {
    StackString str;
    CHECK_EQ(str.size(), 0);
    CHECK_EQ(str.length(), 0);
    CHECK_EQ(static_cast<std::string>(str), "");
  }

  SUBCASE("CStringConstruction") {
    const char* cstr = "Hello";
    StackString str(cstr);
    CHECK_EQ(str.size(), 5);
    CHECK_EQ(str.length(), 5);
    CHECK_EQ(static_cast<std::string>(str), "Hello");
  }

  SUBCASE("StdStringConstruction") {
    std::basic_string<T> stdStr = "World";
    StackString str(stdStr);
    CHECK_EQ(str.size(), 5);
    CHECK_EQ(str.length(), 5);
    CHECK_EQ(static_cast<std::string>(str), "World");
  }
}

TEST_CASE_TEMPLATE("StackStringComparison", T, char) {
  constexpr std::size_t MAXSIZE = 5;
  using StackString = stack_string<MAXSIZE>;

  StackString str1{"abc"};
  StackString str2{"def"};
  StackString str3{"abc"};

  CHECK(str1 == str1);
  CHECK(str1 == str3);
  CHECK(str1 != str2);
  CHECK(str2 != str3);
  CHECK(str1 < str2);
}

TEST_CASE_TEMPLATE("StackStringSize", T, char) {
  constexpr std::size_t MAXSIZE = 5;
  using StackString = stack_string<MAXSIZE>;

  SUBCASE("EmptyString") {
    StackString str;
    CHECK_EQ(str.size(), 0);
    CHECK_EQ(str.length(), 0);
  }

  SUBCASE("NonEmptyString") {
    StackString str{"Hello"};
    CHECK_EQ(str.size(), 5);
    CHECK_EQ(str.length(), 5);
  }
}

TEST_CASE_TEMPLATE("StackStringConversion", T, char) {
  constexpr std::size_t MAXSIZE = 5;
  using StackString = stack_string<MAXSIZE>;

  StackString str{"Hello"};
  std::string stdStr = static_cast<std::string>(str);
  CHECK_EQ(stdStr, "Hello");
}
