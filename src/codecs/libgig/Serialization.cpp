/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017-2026 Christian Schoenebeck                         *
 *                           <cuse@users.sourceforge.net>                  *
 *                                                                         *
 *   This library is part of libgig.                                       *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this library; if not, write to the Free Software           *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston,                 *
 *   MA  02111-1307  USA                                                   *
 ***************************************************************************/

// enable implementation specific declarations in Serialization.h required to
// build this C++ unit, which should be ignored in the public API though
#define LIBGIG_SERIALIZATION_INTERNAL 1

#include "Serialization.h"

#include <iostream>
#include <string.h> // for memcpy()
#include <cstdint> // for uintptr_t
#ifdef _MSC_VER
# include <windows.h>
# include <dbghelp.h>
#else
# include <cxxabi.h>
#endif
#include "helper.h"
#include "srx/SrxFormat.h"
#include "srx/SrxJSONFormat.h"

#define LIBGIG_EPOCH_TIME ((time_t)0)

namespace Serialization {

    // *************** UID ***************
    // *

    static UID _createNullUID() {
        const UID uid = { NULL, 0 };
        return uid;
    }

    const UID NO_UID = _createNullUID();

    /** @brief Check whether this is a valid unique identifier.
     *
     * Returns @c false if this UID can be considered an invalid unique
     * identifier. This is for example the case if this UID object was not
     * explicitly set to some certain meaningful unique identifier value, or if
     * this UID object was intentionally assigned the constant @c NO_UID value.
     * Both represent essentially an UID object which is all zero.
     *
     * Note that this class also implements the @c bool operator, both return
     * the same boolean result.
     */
    bool UID::isValid() const {
        return id != NULL && id != (void*)-1 && size;
    }

    // *************** DataType ***************
    // *

    /** @brief Allocators for all known native data types.
     *
     * Stores a lambda function for each known native data type, sorted by
     * @c DataType::internalData() as key. The respective lambda function, when
     * being called, allocates a new instance of the native data type on the
     * heap and calls the native data type's default constructor for
     * initializing the allocated data.
     *
     * Consequently there are only lambda functions for native data types that
     * do have a default constructor.
     */
    std::map<String,DataType::NativeType> DataType::m_nativeTypes;

    /** @brief Default constructor (as "invalid" DataType).
     *
     * Initializes a DataType object as being an "invalid" DataType object.
     * Thus calling isValid(), after creating a DataType object with this
     * constructor, would return @c false.
     *
     * To create a valid and meaningful DataType object instead, call the static
     * function DataType::dataTypeOf() instead.
     */
    DataType::DataType() {
        m_size = 0;
        m_isPointer = false;
    }

    /** @brief Constructs a valid DataType object.
     *
     * Initializes this object as "valid" DataType object, with specific and
     * useful data type information.
     *
     * This is a protected constructor which should not be called directly by
     * applications, as its argument list is somewhat implementation specific
     * and might change at any time. Applications should call the static
     * function DataType::dataTypeOf() instead.
     *
     * @param isPointer - whether pointer type (i.e. a simple memory address)
     * @param size - native size of data type in bytes (i.e. according to
     *               @c sizeof() C/C++ keyword)
     * @param baseType - this framework's internal name for specifying the base
     *                   type in a coarse way, which must be either one of:
     *                   "int8", "uint8", "int16", "uint16", "int32", "uint32",
     *                   "int64", "uint64", "bool", "real32", "real64",
     *                   "String", "Array", "Set", "enum", "union" or "class"
     * @param customType1 - this is only used for base types "enum", "union",
     *                      "class", "Array", "Set" or "Map", in which case this
     *                      identifies the user defined type name (e.g. "Foo" for
     *                      @c class @c Foo or e.g. "Bar" for @c Array<Bar>
     *                      respectively), for all other types this is empty
     * @param customType2 - this is only used for @c Map<> objects in which case
     *                      it identifies the map's value type (i.e. 2nd
     *                      template parameter of map)
     */
    DataType::DataType(bool isPointer, int size, String baseType,
                       String customType1, String customType2)
    {
        m_size = size;
        m_isPointer = isPointer;
        m_baseTypeName = baseType;
        m_customTypeName = customType1;
        m_customTypeName2 = customType2;
    }

    /** @brief Check if this is a valid DataType object.
     *
     * Returns @c true if this DataType object is reflecting a valid data type.
     * The default constructor creates DataType objects initialized to be
     * "invalid" DataType objects by default. That way one can detect whether
     * a DataType object was ever assigned to something meaningful.
     *
     * Note that this class also implements the @c bool operator, both return
     * the same boolean result.
     */
    bool DataType::isValid() const {
        return m_size;
    }

    /** @brief Whether this is reflecting a C/C++ pointer type.
     *
     * Returns @true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a C/C++ pointer type.
     */
    bool DataType::isPointer() const {
        return m_isPointer;
    }

    /** @brief Whether this is reflecting a C/C++ @c struct or @c class type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a C/C++ @c struct or @c class
     * type.
     *
     * @note: Data types which enjoy out of the box serialization support by
     * this framework, like @c String and @c Array<> are @b NOT handled as class
     * data types by this framwork. So @c isClass() returns @c false for e.g.
     * @c String and any @c Array<> based data type.
     *
     * Note that in the following example:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     * };
     * Foo foo;
     * Foo* pFoo;
     * @endcode
     * the DataType objects of both @c foo, as well as of the C/C++ pointer
     * @c pFoo would both return @c true for isClass() here!
     *
     * @see isPointer()
     */
    bool DataType::isClass() const {
        return m_baseTypeName == "class";
    }

    /** @brief Whether this is reflecting a fundamental C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a primitive, fundamental C/C++
     * data type. Those are fundamental data types which are already predefined
     * by the C/C++ language, for example: @c char, @c int, @c float, @c double,
     * @c bool, but also @c String objects and @b any pointer types like
     * @c int*, @c double**, but including pointers to user defined types like:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     * };
     * Foo* pFoo;
     * @endcode
     * So the DataType object of @c pFoo in the latter example would also return
     * @c true for isPrimitive() here!
     *
     * @see isPointer()
     */
    bool DataType::isPrimitive() const {
        return !isClass() && !isArray() && !isSet() && !isMap();
    }

    /** @brief Whether this is a C++ @c String data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a C++ @c String object (a.k.a.
     * @c std::string from the C++ STL).
     *
     * Note that this framework handles @c String objects as if they were a
     * fundamental, primitive C/C++ data type, so @c isPrimitive() returns
     * @c true for strings.
     */
    bool DataType::isString() const {
        return m_baseTypeName == "String";
    }

    /** @brief Whether this is a character C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is @b EXACTLY of C/C++ type
     * @c char .
     *
     * @b CAUTION: it does @b NOT return @c true for C/C++ type
     * @c unsigned @c char , nor for @c signed @c char , as these are defined by
     * the C++ standard as 3 distinctive data types. The latter two types are
     * therefore handled as integer types by this framework instead.
     *
     * Note that this method also returns @c true on char @b pointer types!
     *
     * @see isInteger(), isPointer()
     */
    bool DataType::isChar() const {
        return m_baseTypeName.substr(0, 4) == "char";
    }

    /** @brief Whether this is an integer C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a (fundamental, primitive)
     * integer data type. So these are foremost all @c int and
     * @c unsigned @c int types of any size.
     *
     * It also includes @c signed @c char and @c unsigned @c char, but it does
     * @b NOT include @c char (see isChar() for details on the latter)!
     *
     * It also does not include floating point ("real") types.
     *
     * You may use isSigned() to further check whether this data type allows
     * negative numbers.
     *
     * Note that this method also returns @c true on integer pointer types!
     *
     * @see isChar(), isPointer()
     */
    bool DataType::isInteger() const {
        return m_baseTypeName.substr(0, 3) == "int" ||
               m_baseTypeName.substr(0, 4) == "uint";
    }

    /** @brief Whether this is a floating point based C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a (fundamental, primitive)
     * floating point based data type. So these are currently the C/C++ @c float
     * and @c double types. It does not include integer types though.
     *
     * Note that this method also returns @c true on @c float pointer and
     * @c double pointer types!
     *
     * @see isPointer()
     */
    bool DataType::isReal() const {
        return m_baseTypeName.substr(0, 4) == "real";
    }

    /** @brief Whether this is a numeric C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a (fundamental, primitive)
     * numeric data type (i.e. either integer or floating point data type). So
     * these are all @c int and @c unsigned @c int, @c float and @c double types
     * of any size. It also includes @c signed @c char and @c unsigned @c char,
     * but it does @b NOT include @c char (see isChar() for details on the
     * latter)!
     *
     * Note that this method also returns @c true on (numeric) pointer types!
     *
     * @see isPointer()
     */
    bool DataType::isNumber() const {
        return isInteger() || isReal();
    }

    /** @brief Whether this is a boolean C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a (fundamental, primitive)
     * boolean data type. So this is the case for the C++ @c bool data type.
     * It does not include integer or floating point types though.
     *
     * Note that this method also returns @c true on @c bool pointer types!
     *
     * @see isPointer()
     */
    bool DataType::isBool() const {
        return m_baseTypeName == "bool";
    }

    /** @brief Whether this is a C/C++ @c enum data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a user defined enumeration
     * data type. So this is the case for all C/C++ @c enum data types.
     * It does not include integer (or even floating point) types though.
     *
     * Note that this method also returns @c true on @c enum pointer types!
     *
     * @see isPointer()
     */
    bool DataType::isEnum() const {
        return m_baseTypeName == "enum";
    }

    /** @brief Whether this is a C++ @c Array<> object type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a C++ @c Array<> container
     * object type.
     *
     * @note: This framework handles @c Array<> types neither as primitive
     * types, nor as class types. So @c isPrimitive() and @c isClass() both
     * return @c false for arrays.
     *
     * @see isPointer()
     */
    bool DataType::isArray() const {
        return m_baseTypeName == "Array";
    }

    /** @brief Whether this is a C++ @c Set<> object type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a C++ @c Set<> unique container
     * object type.
     *
     * @note: This framework handles @c Set<> types neither as primitive
     * types, nor as class types. So @c isPrimitive() and @c isClass() both
     * return @c false for sets.
     *
     * @see isPointer()
     */
    bool DataType::isSet() const {
        return m_baseTypeName == "Set";
    }

    /** @brief Whether this is a C++ @c Map<> object type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is an associative sorted C++
     * @c Map<> container object type.
     *
     * @note: This framework handles @c Map<> types neither as primitive
     * types, nor as class types. So @c isPrimitive() and @c isClass() both
     * return @c false for maps.
     *
     * @see isPointer()
     */
    bool DataType::isMap() const {
        return m_baseTypeName == "Map";
    }

    /** @brief Whether this is a signed integer C/C++ data type.
     *
     * Returns @c true if the respective native C/C++ object, member or variable
     * (this DataType instance is reflecting) is a (fundamental, primitive)
     * signed integer data type.
     *
     * @b NOTE: Behaviour of this method is restricted to data types where
     * isInteger() returns true. For any other data type the result of this
     * method is undefined.
     *
     * Note that this method also returns @c true on signed integer pointer
     * types!
     *
     * @see isInteger();
     */
    bool DataType::isSigned() const {
        return m_baseTypeName.substr(0, 3) == "int" ||
               isReal();
    }

    /** @brief Comparison for equalness.
     *
     * Returns @c true if the two DataType objects being compared can be
     * considered to be "equal" C/C++ data types. They are considered to be
     * equal if their underlying C/C++ data types are exactly identical. For
     * example comparing @c int and @c unsigned int data types are considere to
     * be @b not equal, since they are differently signed. Furthermore @c short
     * @c int and @c long @c int would also not be considered to be equal, since
     * they do have a different memory size. Additionally pointer type
     * characteristic is compared as well. So a @c double type and @c double*
     * type are also considered to be not equal data types and hence this method
     * would return @c false.
     *
     * As an exception here, classes and structs with the same class/struct name
     * but different sizes are also considered to be "equal". This relaxed
     * requirement is necessary to retain backward compatibility to older
     * versions of the same native C++ classes/structs.
     */
    bool DataType::operator==(const DataType& other) const {
        return m_baseTypeName   == other.m_baseTypeName &&
               m_customTypeName == other.m_customTypeName &&
               m_customTypeName2 == other.m_customTypeName2 &&
               (m_size == other.m_size || (isClass() && other.isClass())) &&
               m_isPointer      == other.m_isPointer;
    }

    /** @brief Unique key for native data type, for internal purposes only.
     *
     * This is more or less just a random string acting as a key for the native
     * data type. The way this string is assembled may change at any time in
     * future, so it is only constant for the data type during current runtime.
     *
     * The generated key is used to store lambdas for each data type, e.g. for
     * allocation and initialization of the actual native data type via the
     * reflection API.
     *
     * @see newInstance()
     */
    String DataType::internalID() const {
        return m_baseTypeName + "," +
               m_customTypeName + "," +
               m_customTypeName2 + "," +
               ToString(isClass()) + "," +
               ToString(m_isPointer);
    }

    /** @brief Comparison for inequalness.
     *
     * Returns the inverse result of what DataType::operator==() would return.
     * So refer to the latter for more details.
     */
    bool DataType::operator!=(const DataType& other) const {
        return !operator==(other);
    }

    /** @brief Smaller than comparison.
     *
     * Returns @c true if this DataType object can be consider to be "smaller"
     * than the @a other DataType object being compared with. This operator
     * is actually quite arbitrarily implemented and may change at any time,
     * and thus result for the same data types may change in future at any time.
     *
     * This operator is basically implemented for allowing this DataType class
     * to be used with various standard template library (STL) classes, which
     * require sorting operators to be implemented.
     */
    bool DataType::operator<(const DataType& other) const {
        return m_baseTypeName  < other.m_baseTypeName ||
              (m_baseTypeName == other.m_baseTypeName &&
              (m_customTypeName  < other.m_customTypeName ||
              (m_customTypeName == other.m_customTypeName &&
              (m_customTypeName2  < other.m_customTypeName2 ||
              (m_customTypeName2 == other.m_customTypeName2 &&
              (m_size  < other.m_size ||
              (m_size == other.m_size &&
               m_isPointer < other.m_isPointer)))))));
    }

    /** @brief Greater than comparison.
     *
     * Returns @c true if this DataType object can be consider to be "greater"
     * than the @a other DataType object being compared with. This operator
     * is actually quite arbitrarily implemented and may change at any time,
     * and thus result for the same data types may change in future at any time.
     *
     * This operator is basically implemented for allowing this DataType class
     * to be used with various standard template library (STL) classes, which
     * require sorting operators to be implemented.
     */
    bool DataType::operator>(const DataType& other) const {
        return !(operator==(other) || operator<(other));
    }

    /** @brief Human readable long description for this data type.
     *
     * Returns a human readable long description for this data type, designed
     * for the purpose for being displayed to the user. Note that the
     * implementation for this method and thus the precise textual strings
     * returned by this method, may change at any time. So you should not rely
     * on precise strings for certain data types, and you should not use the
     * return values of this method for comparing data types with each other.
     *
     * This class implements various comparison operators, so you should use 
     * them for comparing DataTypes objects instead.
     *
     * @see baseTypeName(), customTypeName()
     */
    String DataType::asLongDescr() const {
        String s = m_baseTypeName;
        if (!m_customTypeName.empty())
            s += " " + customTypeName(true);
        if (!m_customTypeName2.empty())
            s += " " + customTypeName2(true);
        if (isPointer())
            s += " pointer";
        return s;
    }

    /** @brief The base type name of this data type.
     *
     * Returns a textual short string identifying the basic type of name of this
     * data type. For example for a 32 bit signed integer data type this method
     * would return @c "int32". For all user defined C/C++ @c enum types this
     * method would return "enum". For all user defined C/C++ @c struct @b and
     * @c class types this method would return "class" for both. Note that the
     * precise user defined type name (of i.e. @c enum, @c struct and @c class
     * types) is not included in the string returned by this method, use
     * customTypeName() to retrieve that information instead.
     *
     * The precise textual strings returned by this method are guaranteed to
     * retain equal with future versions of this framework. So you can rely on
     * them for using the return values of this method for comparison tasks in
     * your application. Note however that this class also implements various
     * comparison operators.
     *
     * Further it is important to know that this method returns the same string
     * for pointers and non-pointers of the same underlying data type. So in the
     * following example:
     * @code
     * #include <stdint.h>
     * uint64_t i;
     * uint64_t* pi;
     * @endcode
     * this method would return for both @c i and @c pi the string @c "uint64" !
     *
     * @see isPointer(), customTypeName(), customTypeName2()
     */
    String DataType::baseTypeName() const {
        return m_baseTypeName;
    }

    static String _demangleTypeName(const char* name) {
#ifdef _MSC_VER
        const size_t MAXLENGTH = 1024;
        char result[MAXLENGTH];

        //FIXME: calling UnDecorateSymbolName() is not thread safe!
        //Skip the first char
        size_t size = UnDecorateSymbolName(name + 1, result, MAXLENGTH, UNDNAME_32_BIT_DECODE | UNDNAME_NO_ARGUMENTS);
        if (size)
        {
            return result;
        }
        return name;
#else
        int status;
        char* result =
            abi::__cxa_demangle(name, 0, 0, &status);
        String sResult = result;
        free(result);
        return (status == 0) ? sResult : name;
#endif
    }

    /** @brief The 1st user defined C/C++ data type name of this data type.
     *
     * Call this method on user defined C/C++ data types like @c enum,
     * @c struct, @c class or @c Array<> types to retrieve the user defined type
     * name portion of those data types. Note that this method is only intended
     * for such user defined data types. For all fundamental, primitive data
     * types (like i.e. @c int) this method returns an empty string instead.
     *
     * This method takes an optional boolean argument @b demangle, which allows
     * you define whether you are interested in the raw C++ type name or rather
     * the demangled custom type name. By default this method returns the raw
     * C++ type name. The raw C++ type name is the one that is actually used
     * in the compiled binaries and should be preferred for comparions tasks.
     * The demangled C++ type name is a human readable representation of the
     * type name instead, which you may use for displaying the user defined type
     * name portion to the user, however you should not use the demangled
     * representation for comparison tasks.
     *
     * Note that in the following example:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     * };
     * Foo foo;
     * Foo* pFoo;
     * @endcode
     * this method would return the same string for both @c foo and @c pFoo !
     * In the latter example @c customTypeName(true) would return for both
     * @c foo and @c pFoo the string @c "Foo" as return value of this method.
     *
     * @b Windows: please note that the current implementation of this method
     * on Windows is @b not thread safe!
     *
     * @see baseTypeName(), customTypeName2(), isPointer()
     */
    String DataType::customTypeName(bool demangle) const {
        if (!demangle) return m_customTypeName;
        return _demangleTypeName(m_customTypeName.c_str());
    }

    /** @brief The 2nd user defined C/C++ data type name of this data type.
     *
     * This is currently only used for @c Map<> data types in which case this
     * method returns the map's value type (i.e. map's 2nd template parameter).
     *
     * @see baseTypeName(), customTypeName()
     */
    String DataType::customTypeName2(bool demangle) const {
        if (!demangle) return m_customTypeName2;
        return _demangleTypeName(m_customTypeName2.c_str());
    }

    /** @brief Allocate and initialize a native instance of data type.
     *
     * This is basically like calling the C++ @c new operator for the native
     * data type, though by calling this reflection API instead. Thus memory for
     * one instance of the data type is allocated on the heap and initialized by
     * data types's default constructor.
     *
     * Consequently this method can only be called for native data types that do
     * have a default constructor. For data types that do not have a default
     * constructor, calling this method will cause an assertion fault.
     *
     * @returns new Object reflecting the newly allocated native data instance
     */
    Object DataType::newInstance(Archive* archive) const {
        const String id = internalID();
        auto itNativeType = m_nativeTypes.find(id);
        if (itNativeType == m_nativeTypes.end()) {
            fprintf(stderr,
                    "(De)serialization Failure: cannot create new instance of "
                    "unknown data type '%s' !\n",
                    asLongDescr().c_str());
            assert(false && "You may need to explicitly register this data "
                            "type by either calling "
                            "DataType::registerNativeType<T>() or using class "
                            "NativeDataTypeRegistry");
        }
        const std::function<Object(Archive*)>& allocFn = itNativeType->second.allocFn;
        return allocFn(archive);
    }

    // *************** Member ***************
    // *

    /** @brief Default constructor.
     *
     * Initializes a Member object as being an "invalid" Member object.
     * Thus calling isValid(), after creating a Member object with this
     * constructor, would return @c false.
     *
     * You are currently not supposed to create (meaningful) Member objects on
     * your own. This framework automatically create such Member objects for
     * you instead.
     *
     * @see Object::members()
     */
    Member::Member() {
        m_uid = NO_UID;
        m_offset = 0;
        m_parentUID = NO_UID;
    }

    Member::Member(String name, UID uid, ssize_t offset, DataType type, const Object& parent) {
        m_name = name;
        m_uid  = uid;
        m_offset = offset;
        m_type = type;
        m_parentUID = parent.uid();
    }

    /** @brief Unique identifier of this member instance.
     *
     * Returns the unique identifier of the original C/C++ member instance of
     * your C++ class. It is important to know that this unique identifier is
     * not meant to be unique for Member instances themselves, but it is rather
     * meant to be unique for the original native C/C++ data these Member
     * instances are representing. So that means no matter how many individual
     * Member objects are created, as long as they are representing the same
     * original native member variable of the same original native
     * instance of your C++ class, then all those separately created Member
     * objects return the same unique identifier here.
     *
     * @see UID for more details
     */
    UID Member::uid() const {
        return m_uid;
    }

    /** @brief Unique identifier of parent object.
     *
     * Returns the unique identifier of the parent object that contains and owns
     * this member variable.
     *
     * @see UID for more details
     */
    UID Member::parentUID() const {
        return m_parentUID;
    }

    /** @brief Name of the member.
     *
     * Returns the name of the native C/C++ member variable as originally typed
     * in its C++ source code. So in the following example:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     *     double someValue;
     * };
     * @endcode
     * this method would usually return @c "a" for the first member of object
     * instances of your native C/C++ @c struct @c Foo, and this method would
     * usually return @c "someValue" for its third member.
     *
     * Note that when you implement the @c serialize() method of your own C/C++
     * clases or strucs, you are able to override defining the precise name of
     * your members. In that case this method would of course return the member
     * names as explicitly forced by you instead.
     */
    String Member::name() const {
        return m_name;
    }

    /** @brief Offset of member in its containing parent data structure.
     *
     * Returns the offset of this member (in bytes) within its containing parent
     * user defined data structure or class. So in the following example:
     * @code
     * #include <stdint.h>
     * struct Foo __attribute__ ((__packed__)) {
     *     int32_t a;
     *     bool b;
     *     double c;
     * };
     * @endcode
     * this method would typically return @c 0 for member @c a, @c 4 for member
     * @c b and @c 5 for member @c c. As you have noted in the latter example,
     * the structure @c Foo was declared to have "packed" data members. That
     * means the compiler is instructed to add no memory spaces between the
     * individual members. Because by default the compiler might add memory
     * spaces between individual members to align them on certain memory address
     * boundaries for increasing runtime performance while accessing the
     * members. So if you declared the previous example without the "packed"
     * attribute like:
     * @code
     * #include <stdint.h>
     * struct Foo {
     *     int32_t a;
     *     bool b;
     *     double c;
     * };
     * @endcode
     * then this method would usually return a different offset for members
     * @c b and @c c instead. For most 64 bit architectures this example would
     * now still return @c 0 for member @c a, but @c 8 for member @c b and @c 16
     * for member @c c.
     *
     * @note Offset is intended for native members only, that is member
     * variables which are memory located directly within the associated parent
     * data structure. For members allocated on the heap @c offset() always
     * returns @c -1 instead since there is no constant, static offset
     * relationship between data on the heap and the parent structure owning
     * their life-time control.
     */
    ssize_t Member::offset() const {
        return m_offset;
    }

    /** @brief C/C++ Data type of this member.
     *
     * Returns the precise data type of the original native C/C++ member.
     */
    const DataType& Member::type() const {
        return m_type;
    }

    /** @brief Check if this is a valid Member object.
     *
     * Returns @c true if this Member object is reflecting a "valid" member
     * object. The default constructor creates Member objects initialized to be
     * "invalid" Member objects by default. That way one can detect whether
     * a Member object was ever assigned to something meaningful.
     *
     * Note that this class also implements the @c bool operator, both return
     * the same boolean result value.
     */
    bool Member::isValid() const {
        return m_uid && !m_name.empty() && m_type;
    }

    /** @brief Comparison for equalness.
     *
     * Returns @c true if the two Member objects being compared can be
     * considered to be "equal" C/C++ members. They are considered to be
     * equal if their data type, member name, their offset within their parent
     * containing C/C++ data structure, as well as their original native C/C++
     * instance were exactly identical.
     */
    bool Member::operator==(const Member& other) const {
        return m_uid    == other.m_uid &&
               m_offset == other.m_offset &&
               m_name   == other.m_name &&
               m_type   == other.m_type;
    }

    /** @brief Comparison for inequalness.
     *
     * Returns the inverse result of what Member::operator==() would return.
     * So refer to the latter for more details.
     */
    bool Member::operator!=(const Member& other) const {
        return !operator==(other);
    }

    /** @brief Smaller than comparison.
     *
     * Returns @c true if this Member object can be consider to be "smaller"
     * than the @a other Member object being compared with. This operator
     * is actually quite arbitrarily implemented and may change at any time,
     * and thus result for the same member representations may change in
     * future at any time.
     *
     * This operator is basically implemented for allowing this DataType class
     * to be used with various standard template library (STL) classes, which
     * require sorting operators to be implemented.
     */
    bool Member::operator<(const Member& other) const {
        return m_uid  < other.m_uid ||
              (m_uid == other.m_uid &&
              (m_offset  < other.m_offset ||
              (m_offset == other.m_offset &&
              (m_name  < other.m_name ||
              (m_name == other.m_name &&
               m_type < other.m_type)))));
    }

    /** @brief Greater than comparison.
     *
     * Returns @c true if this Member object can be consider to be "greater"
     * than the @a other Member object being compared with. This operator
     * is actually quite arbitrarily implemented and may change at any time,
     * and thus result for the same member representations may change in
     * future at any time.
     *
     * This operator is basically implemented for allowing this DataType class
     * to be used with various standard template library (STL) classes, which
     * require sorting operators to be implemented.
     */
    bool Member::operator>(const Member& other) const {
        return !(operator==(other) || operator<(other));
    }

    // *************** Object ***************
    // *

    /** @brief Default constructor (for an "invalid" Object).
     *
     * Initializes an Object instance as being an "invalid" Object.
     * Thus calling isValid(), after creating an Object instance with this
     * constructor, would return @c false.
     *
     * Usually you are not supposed to create (meaningful) Object instances on
     * your own. They are typically constructed by the Archive class for you.
     *
     * @see Archive::rootObject(), Archive::objectByUID()
     */
    Object::Object() {
        m_parentUID = NO_UID;
        m_version = 0;
        m_minVersion = 0;
    }

    /** @brief Constructor for a "meaningful" Object.
     *
     * Initializes a "meaningful" Object instance as being. Thus calling
     * isValid(), after creating an Object instance with this constructor,
     * should return @c true, provided that the arguments passed to this
     * constructor construe a valid object representation.
     *
     * Usually you are not supposed to create (meaningful) Object instances on
     * your own. They are typically constructed by the Archive class for you.
     *
     * @see Archive::rootObject(), Archive::objectByUID()
     *
     * @param uidChain - unique identifier chain of the object to be constructed
     * @param type - C/C++ data type of the actual native object this abstract
     *               Object instance should reflect after calling this
     *               constructor
     */
    Object::Object(UIDChain uidChain, DataType type, const Object& parent) {
        m_type = type;
        m_uid  = uidChain;
        m_parentUID = parent.uid();
        m_version = 0;
        m_minVersion = 0;
        //m_data.resize(type.size());
    }

    /** @brief Check if this is a valid Object instance.
     *
     * Returns @c true if this Object instance is reflecting a "valid" Object.
     * The default constructor creates Object instances initialized to be
     * "invalid" Objects by default. That way one can detect whether an Object
     * instance was ever assigned to something meaningful.
     *
     * Note that this class also implements the @c bool operator, both return
     * the same boolean result value.
     */
    bool Object::isValid() const {
        return m_type && !m_uid.empty();
    }

    /** @brief Unique identifier of this Object.
     *
     * Returns the unique identifier for the original native C/C++ data this
     * abstract Object instance is reflecting. If this Object is representing
     * a C/C++ pointer (of first degree) then @c uid() (or @c uid(0) ) returns
     * the unique identifier of the pointer itself, whereas @c uid(1) returns
     * the unique identifier of the original C/C++ data that pointer was
     * actually pointing to.
     *
     * @see UIDChain for more details about this overall topic.
     */
    UID Object::uid(int index) const {
        return (index < m_uid.size()) ? m_uid[index] : NO_UID;
    }

    /** @brief Unique identifier of parent object.
     *
     * Returns the unique identifier of the parent object that contains and owns
     * this object, which is the case for all member variables of classes and
     * structs for example.
     *
     * If the native data represented by this Object is located on the memory
     * heap instead, i.e. not a member within the boundaries of another object,
     * then this method will return @c NO_UID instead.
     *
     * @see UID for more details
     */
    UID Object::parentUID() const {
        return m_parentUID;
    }

    static void _setNativeValueFromString(void* ptr, const DataType& type, const char* s) {
        if (type.isPointer()) {
            // assuming hex encoding
            uintptr_t addr = std::stoull(s, NULL, 16);
            *(void**)ptr = reinterpret_cast<void*>(addr);
        } else if (type.isPrimitive()) {
            if (type.isInteger() || type.isEnum()) {
                if (type.isSigned()) {
                    if (type.size() == 1)
                        *(int8_t*)ptr = strTo<int8_t>(s);
                    else if (type.size() == 2)
                        *(int16_t*)ptr = strTo<int16_t>(s);
                    else if (type.size() == 4)
                        *(int32_t*)ptr = strTo<int32_t>(s);
                    else if (type.size() == 8)
                        *(int64_t*)ptr = strTo<int64_t>(s);
                    else
                        assert(false /* unknown signed int type size */);
                } else {
                    if (type.size() == 1)
                        *(uint8_t*)ptr = strTo<uint8_t>(s);
                    else if (type.size() == 2)
                        *(uint16_t*)ptr = strTo<uint16_t>(s);
                    else if (type.size() == 4)
                        *(uint32_t*)ptr = strTo<uint32_t>(s);
                    else if (type.size() == 8)
                        *(uint64_t*)ptr = strTo<uint64_t>(s);
                    else
                        assert(false /* unknown unsigned int type size */);
                }
            } else if (type.isReal()) {
                if (type.size() == sizeof(float))
                    *(float*)ptr = strTo<float>(s);
                else if (type.size() == sizeof(double))
                    *(double*)ptr = strTo<double>(s);
                else
                    assert(false /* unknown floating point type */);
            } else if (type.isChar()) {
                *(char*)ptr = s[0];
            } else if (type.isBool()) {
                String lower = toLowerCase(s);
                const bool b = lower != "0" && lower != "false" && lower != "no";
                *(bool*)ptr = b;
            } else if (type.isString()) {
                *(String*)ptr = s;
            } else {
                assert(false /* no built-in cast from string support for this data type */);
            }
        }
    }

    /** @brief Cast from string to object's data type and assign value natively.
     *
     * The passed String @a s is decoded from its string representation to this
     * object's corresponding native data type, then that casted value is
     * assigned to the native memory location this Object is referring to.
     *
     * Note: This method may only be called for data types which enjoy built-in
     * support for casting from string to their native data type, which are
     * basically primitive data types (e.g. @c int, @c bool, @c double, etc.) or
     * @c String objects. For all other data types calling this method will
     * cause an assertion fault at runtime.
     *
     * @param s - textual string representation of the value to be assigned to
     *            this object
     */
    void Object::setNativeValueFromString(const String& s) {
        const ID& id = uid().id;
        void* ptr = (void*)id;
        _setNativeValueFromString(ptr, m_type, s.c_str());
    }

    /** @brief Unique identifier chain of this Object.
     *
     * Returns the entire unique identifier chain of this Object.
     *
     * @see uid() and UIDChain for more details about this overall topic.
     */
    const UIDChain& Object::uidChain() const {
        return m_uid;
    }

    /** @brief C/C++ data type this Object is reflecting.
     *
     * Returns the precise original C/C++ data type of the original native
     * C/C++ object or data this Object instance is reflecting.
     */
    const DataType& Object::type() const {
        return m_type;
    }

    /** @brief Raw data of the original native C/C++ data.
     *
     * Returns the raw data value of the original C/C++ data this Object is
     * reflecting. So the precise raw data value, layout and size is dependent
     * to the precise C/C++ data type of the original native C/C++ data. However
     * potentially required endian correction is already automatically applied
     * for you. That means you can safely, directly C-cast the raw data returned
     * by this method to the respective native C/C++ data type in order to
     * access and use the value for some purpose, at least if the respective
     * data is of any fundamental, primitive C/C++ data type, or also to a
     * certain extent if the type is user defined @c enum type.
     *
     * However directly C-casting this raw data for user defined @c struct or
     * @c class types is not possible. For those user defined data structures
     * this method always returns empty raw data instead.
     *
     * Note however that there are more convenient methods in the Archive class
     * to get the right value for the individual data types instead.
     *
     * @see Archive::valueAsInt(), Archive::valueAsReal(), Archive::valueAsBool(),
     *      Archive::valueAsString()
     */
    const RawData& Object::rawData() const {
        return m_data;
    }

    /** @brief Version of original user defined C/C++ @c struct or @c class.
     *
     * In case this Object is reflecting a native C/C++ @c struct or @c class
     * type, then this method returns the version of that native C/C++ @c struct
     * or @c class layout or implementation. For primitive, fundamental C/C++
     * data types (including @c String objects) the return value of this method
     * has no meaning.
     *
     * @see Archive::setVersion() for more details about this overall topic.
     */
    Version Object::version() const {
        return m_version;
    }

    /** @brief Minimum version of original user defined C/C++ @c struct or @c class.
     *
     * In case this Object is reflecting a native C/C++ @c struct or @c class
     * type, then this method returns the "minimum" version of that native C/C++
     * @c struct or @c class layout or implementation which it may be compatible
     * with. For primitive, fundamental C/C++ data types (including @c String
     * objects) the return value of this method has no meaning.
     *
     * @see Archive::setVersion() and Archive::setMinVersion() for more details
     *      about this overall topic.
     */
    Version Object::minVersion() const {
        return m_minVersion;
    }

    /** @brief All members of the original native C/C++ @c struct or @c class instance.
     *
     * In case this Object is reflecting a native C/C++ @c struct or @c class
     * type, then this method returns all member variables of that original
     * native C/C++ @c struct or @c class instance. For primitive, fundamental
     * C/C++ data types this method returns an empty vector instead.
     *
     * Example:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     *     double someValue;
     * };
     * @endcode
     * Considering above's C++ code, a serialized Object representation of such
     * a native @c Foo class would have 3 members @c a, @c b and @c someValue.
     *
     * Note that the respective serialize() method implementation of that
     * fictional C++ @c struct @c Foo actually defines which members are going
     * to be serialized and deserialized for instances of class @c Foo. So in
     * practice the members returned by method members() here might return a
     * different set of members as actually defined in the original C/C++ struct
     * header declaration.
     *
     * The precise sequence of the members returned by this method here depends
     * on the actual serialize() implementation of the user defined C/C++
     * @c struct or @c class.
     *
     * @see Object::sequenceIndexOf() for more details about the precise order
     *      of members returned by this method in the same way.
     */
    std::vector<Member>& Object::members() {
        return m_members;
    }

    /** @brief All members of the original native C/C++ @c struct or @c class instance (read only).
     *
     * Returns the same result as overridden members() method above, it just
     * returns a read-only result instead. See above's method description for
     * details for the return value of this method instead.
     */
    const std::vector<Member>& Object::members() const {
        return m_members;
    }

    /** @brief Comparison for equalness.
     *
     * Returns @c true if the two Object instances being compared can be
     * considered to be "equal" native C/C++ object instances. They are
     * considered to be equal if they are representing the same original
     * C/C++ data instance, which is essentially the case if the original
     * reflecting native C/C++ data are sharing the same memory address and
     * memory size (thus the exact same memory space) and originally had the
     * exact same native C/C++ types.
     */
    bool Object::operator==(const Object& other) const {
        // ignoring all other member variables here
        // (since UID stands for "unique" ;-) )
        return m_uid  == other.m_uid &&
               m_type == other.m_type;
    }

    /** @brief Comparison for inequalness.
     *
     * Returns the inverse result of what Object::operator==() would return.
     * So refer to the latter for more details.
     */
    bool Object::operator!=(const Object& other) const {
        return !operator==(other);
    }

    /** @brief Smaller than comparison.
     *
     * Returns @c true if this Object instance can be consider to be "smaller"
     * than the @a other Object instance being compared with. This operator
     * is actually quite arbitrarily implemented and may change at any time,
     * and thus result for the same Object representations may change in future
     * at any time.
     *
     * This operator is basically implemented for allowing this DataType class
     * to be used with various standard template library (STL) classes, which
     * require sorting operators to be implemented.
     */
    bool Object::operator<(const Object& other) const {
        // ignoring all other member variables here
        // (since UID stands for "unique" ;-) )
        return m_uid  < other.m_uid ||
              (m_uid == other.m_uid &&
               m_type < other.m_type);
    }

    /** @brief Greater than comparison.
     *
     * Returns @c true if this Object instance can be consider to be "greater"
     * than the @a other Object instance being compared with. This operator
     * is actually quite arbitrarily implemented and may change at any time,
     * and thus result for the same Object representations may change in future
     * at any time.
     *
     * This operator is basically implemented for allowing this DataType class
     * to be used with various standard template library (STL) classes, which
     * require sorting operators to be implemented.
     */
    bool Object::operator>(const Object& other) const {
        return !(operator==(other) || operator<(other));
    }

    /** @brief Check version compatibility between Object instances.
     *
     * Use this method to check whether the two original C/C++ instances those
     * two Objects are reflecting, were using a C/C++ data type which are version
     * compatible with each other. By default all C/C++ Objects are considered
     * to be version compatible. They might only be version incompatible if you
     * enforced a certain backward compatibility constraint with your
     * serialize() method implementation of your custom C/C++ @c struct or
     * @c class types.
     *
     * You must only call this method on two Object instances which are
     * representing the same data type, for example if both Objects reflect
     * instances of the same user defined C++ class. Calling this method on
     * completely different data types does not cause an error or exception, but
     * its result would simply be useless for any purpose.
     *
     * @see Archive::setVersion() for more details about this overall topic.
     */
    bool Object::isVersionCompatibleTo(const Object& other) const {
        if (this->version() == other.version())
            return true;
        if (this->version() > other.version())
            return this->minVersion() <= other.version();
        else
            return other.minVersion() <= this->version();
    }

    void Object::setVersion(Version v) {
        m_version = v;
    }

    void Object::setMinVersion(Version v) {
        m_minVersion = v;
    }

    /** @brief Get the member of this Object with given name.
     *
     * In case this Object is reflecting a native C/C++ @c struct or @c class
     * type, then this method returns the abstract reflection of the requested
     * member variable of the original native C/C++ @c struct or @c class
     * instance. For primitive, fundamental C/C++ data types this method always
     * returns an "invalid" Member instance instead.
     *
     * Example:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     *     double someValue;
     * };
     * @endcode
     * Consider that you serialized the native C/C++ @c struct as shown in this
     * example, and assuming that you implemented the respective serialize()
     * method of this C++ @c struct to serialize all its members, then you might
     * call memberNamed("someValue") to get the details of the third member in
     * this example for instance. In case the passed @a name is an unknown
     * member name, then this method will return an "invalid" Member object
     * instead.
     *
     * @param name - original name of the sought serialized member variable of
     *               this Object reflection
     * @returns abstract reflection of the sought member variable
     * @see Member::isValid(), Object::members()
     */
    Member Object::memberNamed(String name) const {
        for (int i = 0; i < m_members.size(); ++i)
            if (m_members[i].name() == name)
                return m_members[i];
        return Member();
    }

    /** @brief Get the member of this Object with given unique identifier.
     *
     * This method behaves similar like method memberNamed() described above,
     * but instead of searching for a member variable by name, it searches for
     * a member with an abstract unique identifier instead. For primitive,
     * fundamental C/C++ data types, for invalid or unknown unique identifiers,
     * and for members which are actually not member instances of the original
     * C/C++ @c struct or @c class instance this Object is reflecting, this
     * method returns an "invalid" Member instance instead.
     *
     * @param uid - unique identifier of the member variable being sought
     * @returns abstract reflection of the sought member variable
     * @see Member::isValid(), Object::members(), Object::memberNamed()
     */
    Member Object::memberByUID(const UID& uid) const {
        if (!uid) return Member();
        for (int i = 0; i < m_members.size(); ++i)
            if (m_members[i].uid() == uid)
                return m_members[i];
        return Member();
    }

    void Object::remove(const Member& member) {
        for (int i = 0; i < m_members.size(); ++i) {
            if (m_members[i] == member) {
                m_members.erase(m_members.begin() + i);
                return;
            }
        }
    }

    /** @brief Get all members of this Object with given data type.
     *
     * In case this Object is reflecting a native C/C++ @c struct or @c class
     * type, then this method returns all member variables of that original
     * native C/C++ @c struct or @c class instance which are matching the given
     * requested data @a type. If this Object is reflecting a primitive,
     * fundamental data type, or if there are no members of this Object with the
     * requested precise C/C++ data type, then this method returns an empty
     * vector instead.
     *
     * @param type - the precise C/C++ data type of the sought member variables
     *               of this Object
     * @returns vector with abstract reflections of the sought member variables
     * @see Object::members(), Object::memberNamed()
     */
    std::vector<Member> Object::membersOfType(const DataType& type) const {
        std::vector<Member> v;
        for (int i = 0; i < m_members.size(); ++i) {
            const Member& member = m_members[i];
            if (member.type() == type)
                v.push_back(member);
        }
        return v;
    }

    /** @brief Serialization/deserialization sequence number of the requested member.
     *
     * Returns the precise serialization/deserialization sequence number of the
     * requested @a member variable.
     *
     * Example:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     *     double c;
     *
     *     void serialize(Serialization::Archive* archive);
     * };
     * @endcode
     * Assuming the declaration of the user defined native C/C++ @c struct
     * @c Foo above, and assuming the following implementation of serialize():
     * @code
     * #define SRLZ(member) \
     *   archive->serializeMember(*this, member, #member);
     *
     * void Foo::serialize(Serialization::Archive* archive) {
     *     SRLZ(c);
     *     SRLZ(a);
     *     SRLZ(b);
     * }
     * @endcode
     * then @c sequenceIndexOf(obj.memberNamed("a")) returns 1,
     * @c sequenceIndexOf(obj.memberNamed("b")) returns 2, and
     * @c sequenceIndexOf(obj.memberNamed("c")) returns 0.
     */
    int Object::sequenceIndexOf(const Member& member) const {
        for (int i = 0; i < m_members.size(); ++i)
            if (m_members[i] == member)
                return i;
        return -1;
    }

    // *************** Archive ***************
    // *

    static Archive::format_t _autoDetectEncodingFormat(const RawData& data) {
        if (SrxFormat::checkMagicStart(data))
            return Archive::FORMAT_SRX;
        if (SrxJSONFormat::checkMagicStart(data))
            return Archive::FORMAT_SRX_JSON;
        return Archive::FORMAT_AUTO;
    }

    /** @brief Create an "empty" archive.
     *
     * This default constructor creates an "empty" archive which you then
     * subsequently for example might fill with serialized data like:
     * @code
     * Archive a;
     * a.serialize(&myRootObject);
     * @endcode
     * Or:
     * @code
     * Archive a;
     * a << myRootObject;
     * @endcode
     * Or you might also subsequently assign an already existing non-empty
     * to this empty archive, which effectively clones the other
     * archive (deep copy) or call decode() later on to assign a previously
     * serialized raw data stream.
     *
     * @param format - used and expected encoding format of serialized data
     *                 (default: automatically handle encoding format)
     */
    Archive::Archive(format_t format) {
        m_format = format;
        m_operation = OPERATION_NONE;
        m_root = NO_UID;
        m_isModified = false;
        m_timeCreated = m_timeModified = LIBGIG_EPOCH_TIME;
    }

    /** @brief Create and fill the archive with the given serialized raw data.
     *
     * This constructor decodes the given raw @a data and constructs a
     * (non-empty) Archive object according to that given serialized data
     * stream.
     *
     * After this constructor returned, you may then traverse the individual
     * objects by starting with accessing the rootObject() for example. Finally
     * you might call deserialize() to restore your native C++ objects with the
     * content of this archive.
     *
     * @param data - the previously serialized raw data stream to be decoded
     * @param format - used and expected encoding format of serialized data
     *                 (default: automatically handle encoding format)
     * @throws Exception if the provided raw @a data uses an invalid, unknown,
     *         incompatible or corrupt data stream or format.
     */
    Archive::Archive(const RawData& data, format_t format) {
        m_format = format;
        m_operation = OPERATION_NONE;
        m_root = NO_UID;
        m_isModified = false;
        m_timeCreated = m_timeModified = LIBGIG_EPOCH_TIME;
        decode(data);
    }

    /** @brief Create and fill the archive with the given serialized raw C-buffer data.
     *
     * This constructor essentially works like the constructor above, but just
     * uses another data type for the serialized raw data stream being passed to
     * this class.
     *
     * This constructor decodes the given raw @a data and constructs a
     * (non-empty) Archive object according to that given serialized data
     * stream.
     *
     * After this constructor returned, you may then traverse the individual
     * objects by starting with accessing the rootObject() for example. Finally
     * you might call deserialize() to restore your native C++ objects with the
     * content of this archive.
     *
     * @param data - the previously serialized raw data stream to be decoded
     * @param size - size of @a data in bytes
     * @param format - used and expected encoding format of serialized data
     *                 (default: automatically handle encoding format)
     * @throws Exception if the provided raw @a data uses an invalid, unknown,
     *         incompatible or corrupt data stream or format.
     */
    Archive::Archive(const uint8_t* data, size_t size, format_t format) {
        m_format = format;
        m_operation = OPERATION_NONE;
        m_root = NO_UID;
        m_isModified = false;
        m_timeCreated = m_timeModified = LIBGIG_EPOCH_TIME;
        decode(data, size);
    }

    Archive::~Archive() {
    }

    /** @brief Root C++ object of this archive.
     *
     * In case this is a non-empty Archive, then this method returns the so
     * called "root" C++ object. If this is an empty archive, then this method
     * returns an "invalid" Object instance instead.
     *
     * @see Archive::serialize() for more details about the "root" object concept.
     * @see Object for more details about the overall object reflection concept.
     * @returns reflection of the original native C++ root object
     */
    Object& Archive::rootObject() {
        return m_allObjects[m_root];
    }

    String Archive::primitiveObjectValueToString(const Object& obj) {
        String s;
        const DataType& type = obj.type();
        const ID& id = obj.uid().id;
        void* ptr = obj.m_data.empty() ? (void*)id : (void*)&obj.m_data[0];
        if (!obj.m_data.empty())
            assert(type.size() == obj.m_data.size());
        if (type.isPrimitive() && !type.isPointer()) {
            if (type.isInteger() || type.isEnum()) {
                if (type.isSigned()) {
                    if (type.size() == 1)
                        s = ToString((int16_t)*(int8_t*)ptr); // int16_t: prevent ToString() to render an ASCII character
                    else if (type.size() == 2)
                        s = ToString(*(int16_t*)ptr);
                    else if (type.size() == 4)
                        s = ToString(*(int32_t*)ptr);
                    else if (type.size() == 8)
                        s = ToString(*(int64_t*)ptr);
                    else
                        assert(false /* unknown signed int type size */);
                } else {
                    if (type.size() == 1)
                        s = ToString((uint16_t)*(uint8_t*)ptr); // uint16_t: prevent ToString() to render an ASCII character
                    else if (type.size() == 2)
                        s = ToString(*(uint16_t*)ptr);
                    else if (type.size() == 4)
                        s = ToString(*(uint32_t*)ptr);
                    else if (type.size() == 8)
                        s = ToString(*(uint64_t*)ptr);
                    else
                        assert(false /* unknown unsigned int type size */);
                }
            } else if (type.isReal()) {
                if (type.size() == sizeof(float))
                    s = ToString(*(float*)ptr);
                else if (type.size() == sizeof(double))
                    s = ToString(*(double*)ptr);
                else
                    assert(false /* unknown floating point type */);
            } else if (type.isChar()) {
                s.resize(1);
                s[0] = *(char*)ptr;
            } else if (type.isBool()) {
                s = ToString(*(bool*)ptr);
            } else if (type.isString()) {
                s = obj.m_data.empty() ? *(String*)ptr : String((const char*)ptr);
            } else {
                assert(false /* unknown primitive type */);
            }
        }
        return s;
    }

    template<typename T>
    inline T _stringToNumber(const String& s) {
        assert(false /* String cast to unknown primitive number type */);
    }

    template<>
    inline int64_t _stringToNumber(const String& s) {
        return strTo<int64_t>(s);
    }

    template<>
    inline double _stringToNumber(const String& s) {
        return strTo<double>(s);
    }

    template<>
    inline bool _stringToNumber(const String& s) {
        return strTo<bool>(s);
    }

    template<typename T>
    static T _primitiveObjectValueToNumber(const Object& obj) {
        T value = 0;
        const DataType& type = obj.type();
        const ID& id = obj.uid().id;
        void* ptr = obj.m_data.empty() ? (void*)id : (void*)&obj.m_data[0];
        if (!obj.m_data.empty())
            assert(type.size() == obj.m_data.size());
        if (type.isPrimitive() && !type.isPointer()) {
            if (type.isInteger() || type.isEnum()) {
                if (type.isSigned()) {
                    if (type.size() == 1)
                        value = (T)*(int8_t*)ptr;
                    else if (type.size() == 2)
                        value = (T)*(int16_t*)ptr;
                    else if (type.size() == 4)
                        value = (T)*(int32_t*)ptr;
                    else if (type.size() == 8)
                        value = (T)*(int64_t*)ptr;
                    else
                        assert(false /* unknown signed int type size */);
                } else {
                    if (type.size() == 1)
                        value = (T)*(uint8_t*)ptr;
                    else if (type.size() == 2)
                        value = (T)*(uint16_t*)ptr;
                    else if (type.size() == 4)
                        value = (T)*(uint32_t*)ptr;
                    else if (type.size() == 8)
                        value = (T)*(uint64_t*)ptr;
                    else
                        assert(false /* unknown unsigned int type size */);
                }
            } else if (type.isReal()) {
                if (type.size() == sizeof(float))
                    value = (T)*(float*)ptr;
                else if (type.size() == sizeof(double))
                    value = (T)*(double*)ptr;
                else
                    assert(false /* unknown floating point type */);
            } else if (type.isChar()) {
                value = (T)*(char*)ptr;
            } else if (type.isBool()) {
                value = (T)*(bool*)ptr;
            } else if (type.isString()) {
                value = _stringToNumber<T>(
                    obj.m_data.empty() ? *(String*)ptr : String((const char*)ptr)
                );
            } else {
                assert(false /* unknown primitive type */);
            }
        }
        return value;
    }

    void Archive::encode() {
        encode(m_format);
    }

    void Archive::encode(format_t format) {
        m_rawData.clear();
        m_timeModified = time(NULL);
        if (m_timeCreated == LIBGIG_EPOCH_TIME)
            m_timeCreated = m_timeModified;
    again:
        switch (format) {
            case FORMAT_AUTO:
                format = (m_format != FORMAT_AUTO) ? m_format : FORMAT_SRX;
                goto again;
            case FORMAT_SRX:
                m_rawData = SrxFormat::encode(*this);
                m_format = FORMAT_SRX;
                goto done;
            case FORMAT_SRX_JSON: {
                SrxJSONEncoder jsonEncoder;
                m_rawData = jsonEncoder.encode(*this);
                m_format = FORMAT_SRX_JSON;
                goto done;
            }
        }
        throw Exception("Requested encoding format unknown: %d", format);
    done:
        m_isModified = false;
    }

    /** @brief Fill this archive with the given serialized raw data.
     *
     * Calling this method will decode the given raw @a data and constructs a
     * (non-empty) Archive object according to that given serialized @a data
     * stream.
     *
     * After this method returned, you may then traverse the individual
     * objects by starting with accessing the rootObject() for example. Finally
     * you might call deserialize() to restore your native C++ objects with the
     * content of this archive.
     *
     * @param data - the previously serialized raw data stream to be decoded
     * @throws Exception if the provided raw @a data uses an invalid, unknown,
     *         incompatible or corrupt data stream or format.
     */
    void Archive::decode(const RawData& data) {
        decode(data, m_format);
    }

    /** @brief Fill this archive with the given serialized raw data.
     *
     * Calling this method will decode the given raw @a data and constructs a
     * (non-empty) Archive object according to that given serialized @a data
     * stream.
     *
     * After this method returned, you may then traverse the individual
     * objects by starting with accessing the rootObject() for example. Finally
     * you might call deserialize() to restore your native C++ objects with the
     * content of this archive.
     *
     * @param data - the previously serialized raw data stream to be decoded
     * @param format - expected encoding format of serialized data
     *                 (default: automatically detect encoding format)
     * @throws Exception if the provided raw @a data uses an invalid, unknown,
     *         incompatible or corrupt data stream or format.
     */
    void Archive::decode(const RawData& data, format_t format) {
        m_rawData = data;
        m_allObjects.clear();
        m_isModified = false;
        m_timeCreated = m_timeModified = LIBGIG_EPOCH_TIME;
    again:
        switch (format) {
            case FORMAT_AUTO:
                format = _autoDetectEncodingFormat(data);
                if (format == FORMAT_AUTO)
                    throw Exception("Unknown encoding format");
                goto again;
            case FORMAT_SRX:
                SrxFormat::decode(*this, data);
                m_format = format;
                return;
            case FORMAT_SRX_JSON: {
                SrxJSONDecoder::decode(*this, data);
                m_format = format;
                return;
            }
        }
        throw Exception("Requested encoding format unknown: %d", format);
    }

    /** @brief Fill this archive with the given serialized raw C-buffer data.
     *
     * This method essentially works like the decode() method above, but just
     * uses another data type for the serialized raw data stream being passed to
     * this method.
     *
     * Calling this method will decode the given raw @a data and constructs a
     * (non-empty) Archive object according to that given serialized @a data
     * stream.
     *
     * After this method returned, you may then traverse the individual
     * objects by starting with accessing the rootObject() for example. Finally
     * you might call deserialize() to restore your native C++ objects with the
     * content of this archive.
     *
     * @param data - the previously serialized raw data stream to be decoded
     * @param size - size of @a data in bytes
     * @throws Exception if the provided raw @a data uses an invalid, unknown,
     *         incompatible or corrupt data stream or format.
     */
    void Archive::decode(const uint8_t* data, size_t size) {
        decode(data, size, m_format);
    }

    /** @brief Fill this archive with the given serialized raw C-buffer data.
     *
     * This method essentially works like the decode() method above, but just
     * uses another data type for the serialized raw data stream being passed to
     * this method.
     *
     * Calling this method will decode the given raw @a data and constructs a
     * (non-empty) Archive object according to that given serialized @a data
     * stream.
     *
     * After this method returned, you may then traverse the individual
     * objects by starting with accessing the rootObject() for example. Finally
     * you might call deserialize() to restore your native C++ objects with the
     * content of this archive.
     *
     * @param data - the previously serialized raw data stream to be decoded
     * @param size - size of @a data in bytes
     * @param format - expected encoding format of serialized data
     *                 (default: automatically detect encoding format)
     * @throws Exception if the provided raw @a data uses an invalid, unknown,
     *         incompatible or corrupt data stream or format.
     */
    void Archive::decode(const uint8_t* data, size_t size, format_t format) {
        RawData rawData;
        rawData.resize(size);
        memcpy(&rawData[0], data, size);
        decode(rawData, format);
    }

    /** @brief Raw data stream of this archive content.
     *
     * Call this method to get a raw data stream for the current content of this
     * archive, which you may use to i.e. store on disk or send vie network to
     * another machine for deserializing there. This method only returns a
     * meaningful content if this is a non-empty archive, that is if you either
     * serialized with this Archive object or decoded a raw data stream to this
     * Archive object before. If this is an empty archive instead, then this
     * method simply returns an empty raw data stream (of size 0) instead.
     *
     * Note that whenever you call this method, the "modified" state of this 
     * archive will be reset to @c false.
     *
     * @see isModified()
     */
    const RawData& Archive::rawData() {
        if (m_isModified) encode();
        return m_rawData;
    }

    /** @brief Name of the encoding format used by this Archive class.
     *
     * This method returns the name of the encoding format used to encode
     * serialized raw data streams.
     */
    String Archive::rawDataFormat() const {
        switch (m_format) {
            case FORMAT_AUTO:
            case FORMAT_SRX:
                return SrxFormat::name();
            case FORMAT_SRX_JSON:
                return SrxJSONFormat::name();
        }
        return "";
    }

    /** @brief Whether this archive was modified.
     *
     * This method returns the current "modified" state of this archive. When
     * either decoding a previously serialized raw data stream or after
     * serializing native C++ objects to this archive the modified state will
     * initially be set to @c false. However whenever you are modifying the
     * abstract data model of this archive afterwards, for example by removing
     * objects from this archive by calling remove() or removeMember(), or by
     * altering object values for example by calling setIntValue(), then the
     * "modified" state of this archive will automatically be set to @c true.
     *
     * You can reset the "modified" state explicitly at any time, by calling
     * rawData().
     */
    bool Archive::isModified() const {
        return m_isModified;
    }

    /** @brief Clear content of this archive.
     *
     * Drops the entire content of this archive and thus resets this archive
     * back to become an empty archive.
     */
    void Archive::clear() {
        m_allObjects.clear();
        m_operation = OPERATION_NONE;
        m_root = NO_UID;
        m_rawData.clear();
        m_isModified = false;
        m_timeCreated = m_timeModified = LIBGIG_EPOCH_TIME;
    }

    /** @brief Optional name of this archive.
     *
     * Returns the optional name of this archive that you might have assigned
     * to this archive before by calling setName(). If you haven't assigned any
     * name to this archive before, then this method simply returns an empty
     * string instead.
     */
    String Archive::name() const {
        return m_name;
    }

    /** @brief Assign a name to this archive.
     *
     * You may optionally assign an arbitrary name to this archive. The name
     * will be stored along with the archive, that is it will encoded with the
     * resulting raw data stream, and accordingly it will be decoded from the
     * raw data stream later on.
     *
     * @param name - arbitrary new name for this archive
     */
    void Archive::setName(String name) {
        if (m_name == name) return;
        m_name = name;
        m_isModified = true;
    }

    /** @brief Optional comments for this archive.
     *
     * Returns the optional comments for this archive that you might have
     * assigned to this archive before by calling setComment(). If you haven't
     * assigned any comment to this archive before, then this method simply
     * returns an empty string instead.
     */
    String Archive::comment() const {
        return m_comment;
    }

    /** @brief Assign a comment to this archive.
     *
     * You may optionally assign arbitrary comments to this archive. The comment
     * will be stored along with the archive, that is it will encoded with the
     * resulting raw data stream, and accordingly it will be decoded from the
     * raw data stream later on.
     *
     * @param comment - arbitrary new comment for this archive
     */
    void Archive::setComment(String comment) {
        if (m_comment == comment) return;
        m_comment = comment;
        m_isModified = true;
    }

    static tm _convertTimeStamp(const time_t& time, time_base_t base) {
        tm* pTm;
        switch (base) {
            case LOCAL_TIME:
                pTm = localtime(&time);
                break;
            case UTC_TIME:
                pTm = gmtime(&time);
                break;
            default:
                throw Exception("Time stamp with unknown time base (" + ToString((int64_t)base) + ") requested");
        }
        if (!pTm)
            throw Exception("Failed assembling time stamp structure");
        return *pTm;
    }

    /** @brief Date and time when this archive was initially created.
     *
     * Returns a UTC time stamp (date and time) when this archive was initially
     * created.
     */
    time_t Archive::timeStampCreated() const {
        return m_timeCreated;
    }

    /** @brief Date and time when this archive was modified for the last time.
     *
     * Returns a UTC time stamp (date and time) when this archive was modified
     * for the last time.
     */
    time_t Archive::timeStampModified() const {
        return m_timeModified;
    }

    /** @brief Date and time when this archive was initially created.
     *
     * Returns a calendar time information representing the date and time when
     * this archive was initially created. The optional @a base parameter may
     * be used to define to which time zone the returned data and time shall be
     * related to.
     *
     * @param base - (optional) time zone the result shall relate to, by default
     *               UTC time (Greenwhich Mean Time) is assumed instead
     */
    tm Archive::dateTimeCreated(time_base_t base) const {
        return _convertTimeStamp(m_timeCreated, base);
    }

    /** @brief Date and time when this archive was modified for the last time.
     *
     * Returns a calendar time information representing the date and time when
     * this archive has been modified for the last time. The optional @a base
     * parameter may be used to define to which time zone the returned date and
     * time shall be related to.
     *
     * @param base - (optional) time zone the result shall relate to, by default
     *               UTC time (Greenwhich Mean Time) is assumed instead
     */
    tm Archive::dateTimeModified(time_base_t base) const {
        return _convertTimeStamp(m_timeModified, base);
    }

    /** @brief Remove a member variable from the given object.
     *
     * Removes the member variable @a member from its containing object
     * @a parent and sets the modified state of this archive to @c true.
     * If the given @a parent object does not contain the given @a member then
     * this method does nothing.
     *
     * This method provides a means of "partial" deserialization. By removing
     * either objects or members from this archive before calling deserialize(),
     * only the remaining objects and remaining members will be restored by this
     * framework, all other data of your C++ classes remain untouched.
     *
     * @param parent - Object which contains @a member
     * @param member - member to be removed
     * @see isModified() for details about the modified state.
     * @see Object for more details about the overall object reflection concept.
     */
    void Archive::removeMember(Object& parent, const Member& member) {
        parent.remove(member);
        m_isModified = true;
    }

    /** @brief Remove an object from this archive.
     *
     * Removes the object @obj from this archive and sets the modified state of
     * this archive to @c true. If the passed object is either invalid, or does
     * not exist in this archive, then this method does nothing.
     *
     * This method provides a means of "partial" deserialization. By removing
     * either objects or members from this archive before calling deserialize(),
     * only the remaining objects and remaining members will be restored by this
     * framework, all other data of your C++ classes remain untouched.
     *
     * @param obj - the object to be removed from this archive
     * @see isModified() for details about the modified state.
     * @see Object for more details about the overall object reflection concept.
     */
    void Archive::remove(const Object& obj) {
        //FIXME: Should traverse from root object and remove all members associated with this object
        if (!obj.uid()) return;
        m_allObjects.erase(obj.uid());
        m_isModified = true;
    }

    /** @brief Access object by its unique identifier.
     *
     * Returns the object of this archive with the given unique identifier
     * @a uid. If the given @a uid is invalid, or if this archive does not
     * contain an object with the given unique identifier, then this method
     * returns an invalid object instead.
     *
     * @param uid - unique identifier of sought object
     * @see Object for more details about the overall object reflection concept.
     * @see Object::isValid() for valid/invalid objects
     * @see Object::objectByBaseUID() for finding true object of a base pointer
     */
    Object& Archive::objectByUID(const UID& uid) {
        return m_allObjects[uid];
    }

    /** @brief Find true object for a base pointer.
     *
     * Behaves equivalent to objectByUID(), with the difference that the latter
     * only returns an object that matches exactly the passed UID, which is fine
     * in most situations, however when dealing with base pointers in a
     * polymorphic scenario, then objectByUID() might fail to find the pointed
     * object because of a slight difference in the UID, as the pointer could
     * have been of a base type, whereas the actual object being pointed to
     * might be of a derived, higher type. In this particular situation this
     * method here would proceed and resolve the actual true object and return
     * it.
     *
     * @param uid - unique identifier of sought object
     */
    Object& Archive::objectByBaseUID(const UID& uid) {
        ObjectPool::iterator it = m_allObjects.find(uid);
        if (it != m_allObjects.end())
            return it->second;

        // find all objects with the same ID (i.e. memory address) AND being
        // larger than expected by passed base UID
        std::map<size_t,UID> matches;
        for (const auto& it : m_allObjects)
            if (it.first.id == uid.id && it.first.size > uid.size)
                matches[it.first.size] = it.first;

        if (matches.empty())
            return ObjectPool::invalidObject();

        // return the next larger object found
        return m_allObjects[matches.begin()->second];
    }

    /** @brief Access parent of supplied object.
     *
     * If the passed object is contained within the boundaries of another
     * object, then this method will return that parent object.
     *
     * If the object is located on the heap memory (e.g. pointed by a strong
     * pointer), then this method returns an invalid Object instead.
     */
    Object& Archive::parentObjectOf(const Object& obj) {
        return m_allObjects[obj.parentUID()];
    }

    /** @brief Access parent of supplied member.
     *
     * Returns the parent object that contains and owns the passed member
     * variable. Consequently this method always returns a valid Object.
     */
    Object& Archive::parentObjectOf(const Member& member) {
        return m_allObjects[member.parentUID()];
    }

    /** @brief Set the current version for the given object.
     *
     * Essentially behaves like above's setVersion() method, it just uses the
     * abstract reflection data type instead for the respective @a object being
     * passed to this method. Refer to above's setVersion() documentation about
     * the precise behavior details of setVersion().
     *
     * @param object - object to set the current version for
     * @param v - new current version to set for @a object
     */
    void Archive::setVersion(Object& object, Version v) {
        if (!object) return;
        object.setVersion(v);
        m_isModified = true;
    }

    /** @brief Set the minimum version for the given object.
     *
     * Essentially behaves like above's setMinVersion() method, it just uses the
     * abstract reflection data type instead for the respective @a object being
     * passed to this method. Refer to above's setMinVersion() documentation
     * about the precise behavior details of setMinVersion().
     *
     * @param object - object to set the minimum version for
     * @param v - new minimum version to set for @a object
     */
    void Archive::setMinVersion(Object& object, Version v) {
        if (!object) return;
        object.setMinVersion(v);
        m_isModified = true;
    }

    /** @brief Set new value for given @c enum object.
     *
     * Sets the new @a value to the given @c enum @a object.
     *
     * @param object - the @c enum object to be changed
     * @param value - the new value to be assigned to the @a object
     * @throws Exception if @a object is not an @c enum type.
     */
    void Archive::setEnumValue(Object& object, uint64_t value) {
        if (!object) return;
        if (!object.type().isEnum())
            throw Exception("Not an enum data type");
        Object* pObject = &object;
        if (object.type().isPointer()) {
            Object& obj = objectByUID(object.uid(1));
            if (!obj) return;
            pObject = &obj;
        }
        const int nativeEnumSize = sizeof(enum operation_t);
        DataType& type = const_cast<DataType&>( pObject->type() );
        // original serializer ("sender") might have had a different word size
        // than this machine, adjust type object in this case
        if (type.size() != nativeEnumSize) {
            type.m_size = nativeEnumSize;
        }
        pObject->m_data.resize(type.size());
        void* ptr = &pObject->m_data[0];
        if (type.size() == 1)
            *(uint8_t*)ptr = (uint8_t)value;
        else if (type.size() == 2)
            *(uint16_t*)ptr = (uint16_t)value;
        else if (type.size() == 4)
            *(uint32_t*)ptr = (uint32_t)value;
        else if (type.size() == 8)
            *(uint64_t*)ptr = (uint64_t)value;
        else
            assert(false /* unknown enum type size */);
        m_isModified = true;
    }

    /** @brief Set new integer value for given integer object.
     *
     * Sets the new integer @a value to the given integer @a object. Currently
     * this framework handles any integer data type up to 64 bit. For larger
     * integer types an assertion failure will be raised.
     *
     * @param object - the integer object to be changed
     * @param value - the new value to be assigned to the @a object
     * @throws Exception if @a object is not an integer type.
     */
    void Archive::setIntValue(Object& object, int64_t value) {
        if (!object) return;
        if (!object.type().isInteger())
            throw Exception("Not an integer data type");
        Object* pObject = &object;
        if (object.type().isPointer()) {
            Object& obj = objectByUID(object.uid(1));
            if (!obj) return;
            pObject = &obj;
        }
        const DataType& type = pObject->type();
        pObject->m_data.resize(type.size());
        void* ptr = &pObject->m_data[0];
        if (type.isSigned()) {
            if (type.size() == 1)
                *(int8_t*)ptr = (int8_t)value;
            else if (type.size() == 2)
                *(int16_t*)ptr = (int16_t)value;
            else if (type.size() == 4)
                *(int32_t*)ptr = (int32_t)value;
            else if (type.size() == 8)
                *(int64_t*)ptr = (int64_t)value;
            else
                assert(false /* unknown signed int type size */);
        } else {
            if (type.size() == 1)
                *(uint8_t*)ptr = (uint8_t)value;
            else if (type.size() == 2)
                *(uint16_t*)ptr = (uint16_t)value;
            else if (type.size() == 4)
                *(uint32_t*)ptr = (uint32_t)value;
            else if (type.size() == 8)
                *(uint64_t*)ptr = (uint64_t)value;
            else
                assert(false /* unknown unsigned int type size */);
        }
        m_isModified = true;
    }

    /** @brief Set new floating point value for given floating point object.
     *
     * Sets the new floating point @a value to the given floating point
     * @a object. Currently this framework supports single precision @c float
     * and double precision @c double floating point data types. For all other
     * floating point types this method will raise an assertion failure.
     *
     * @param object - the floating point object to be changed
     * @param value - the new value to be assigned to the @a object
     * @throws Exception if @a object is not a floating point based type.
     */
    void Archive::setRealValue(Object& object, double value) {
        if (!object) return;
        if (!object.type().isReal())
            throw Exception("Not a real data type");
        Object* pObject = &object;
        if (object.type().isPointer()) {
            Object& obj = objectByUID(object.uid(1));
            if (!obj) return;
            pObject = &obj;
        }
        const DataType& type = pObject->type();
        pObject->m_data.resize(type.size());
        void* ptr = &pObject->m_data[0];
        if (type.size() == sizeof(float))
            *(float*)ptr = (float)value;
        else if (type.size() == sizeof(double))
            *(double*)ptr = (double)value;
        else
            assert(false /* unknown real type size */);
        m_isModified = true;
    }

    /** @brief Set new char value for given character object.
     *
     * Sets the new character @a value to the given char @a object.
     *
     * @param object - the char object to be changed
     * @param value - the new value to be assigned to the @a object
     * @throws Exception if @a object is not a char type.
     */
    void Archive::setCharValue(Object& object, char value) {
        if (!object) return;
        if (!object.type().isChar())
            throw Exception("Not a char data type");
        Object* pObject = &object;
        if (object.type().isPointer()) {
            Object& obj = objectByUID(object.uid(1));
            if (!obj) return;
            pObject = &obj;
        }
        const DataType& type = pObject->type();
        pObject->m_data.resize(type.size());
        char* ptr = (char*)&pObject->m_data[0];
        *ptr = value;
        m_isModified = true;
    }

    /** @brief Set new boolean value for given boolean object.
     *
     * Sets the new boolean @a value to the given boolean @a object.
     *
     * @param object - the boolean object to be changed
     * @param value - the new value to be assigned to the @a object
     * @throws Exception if @a object is not a boolean type.
     */
    void Archive::setBoolValue(Object& object, bool value) {
        if (!object) return;
        if (!object.type().isBool())
            throw Exception("Not a bool data type");
        Object* pObject = &object;
        if (object.type().isPointer()) {
            Object& obj = objectByUID(object.uid(1));
            if (!obj) return;
            pObject = &obj;
        }
        const DataType& type = pObject->type();
        pObject->m_data.resize(type.size());
        bool* ptr = (bool*)&pObject->m_data[0];
        *ptr = value;
        m_isModified = true;
    }

    /** @brief Set new textual string for given String object.
     *
     * Sets the new textual string @a value to the given String @a object.
     *
     * @param object - the String object to be changed
     * @param value - the new textual string to be assigned to the @a object
     * @throws Exception if @a object is not a String type.
     */
    void Archive::setStringValue(Object& object, String value) {
        if (!object) return;
        if (!object.type().isString())
            throw Exception("Not a String data type");
        Object* pObject = &object;
        if (object.type().isPointer()) {
            Object& obj = objectByUID(object.uid(1));
            if (!obj) return;
            pObject = &obj;
        }
        pObject->m_data.resize(value.length() + 1);
        char* ptr = (char*) &pObject->m_data[0];
        strcpy(ptr, &value[0]);
        m_isModified = true;
    }

    /** @brief Automatically cast and assign appropriate value to object.
     *
     * This method automatically converts the given @a value from textual string
     * representation into the appropriate data format of the requested
     * @a object. So this method is a convenient way to change values of objects
     * in this archive with your applications in automated way, i.e. for
     * implementing an editor where the user is able to edit values of objects
     * in this archive by entering the values as text with a keyboard.
     *
     * @throws Exception if the passed @a object is not a fundamental, primitive
     *         data type or if the provided textual value cannot be converted
     *         into an appropriate value for the requested object.
     */
    void Archive::setAutoValue(Object& object, String value) {
        if (!object) return;
        const DataType& type = object.type();
        if (type.isInteger())
            setIntValue(object, strTo<int64_t>(value));
        else if (type.isReal())
            setRealValue(object, strTo<double>(value));
        else if (type.isChar()) {
            if (value.empty())
                throw Exception("Empty string provided for char type");
            setCharValue(object, value[0]);
        } else if (type.isBool()) {
            String val = toLowerCase(value);
            if (val == "true" || val == "yes" || val == "1")
                setBoolValue(object, true);
            else if (val == "false" || val == "no" || val == "0")
                setBoolValue(object, false);
            else
                setBoolValue(object, strTo<bool>(value));
        } else if (type.isString())
            setStringValue(object, value);
        else if (type.isEnum())
            setEnumValue(object, strTo<uint64_t>(value));
        else
            throw Exception("Not a primitive data type");
    }

    /** @brief Get value of object as string.
     *
     * Converts the current value of the given @a object into a textual string
     * and returns that string.
     *
     * @param object - object whose value shall be retrieved
     * @throws Exception if the given object is either invalid, or if the object
     *         is not a fundamental, primitive data type.
     */
    String Archive::valueAsString(const Object& object) {
        if (!object)
            throw Exception("Invalid object");
        if (object.type().isClass())
            throw Exception("Object is class type");
        const Object* pObject = &object;
        if (object.type().isPointer()) {
            const Object& obj = objectByUID(object.uid(1));
            if (!obj) return "";
            pObject = &obj;
        }
        return primitiveObjectValueToString(*pObject);
    }

    /** @brief Get integer value of object.
     *
     * Returns the current integer value of the requested integer @a object or
     * @c enum object.
     *
     * @param object - object whose value shall be retrieved
     * @throws Exception if the given object is either invalid, or if the object
     *         is neither an integer nor @c enum data type.
     */
    int64_t Archive::valueAsInt(const Object& object) {
        if (!object)
            throw Exception("Invalid object");
        if (!object.type().isInteger() && !object.type().isEnum())
            throw Exception("Object is neither an integer nor an enum");
        const Object* pObject = &object;
        if (object.type().isPointer()) {
            const Object& obj = objectByUID(object.uid(1));
            if (!obj) return 0;
            pObject = &obj;
        }
        return _primitiveObjectValueToNumber<int64_t>(*pObject);
    }

    /** @brief Get floating point value of object.
     *
     * Returns the current floating point value of the requested floating point
     * @a object.
     *
     * @param object - object whose value shall be retrieved
     * @throws Exception if the given object is either invalid, or if the object
     *         is not a floating point based type.
     */
    double Archive::valueAsReal(const Object& object) {
        if (!object)
            throw Exception("Invalid object");
        if (!object.type().isReal())
            throw Exception("Object is not an real type");
        const Object* pObject = &object;
        if (object.type().isPointer()) {
            const Object& obj = objectByUID(object.uid(1));
            if (!obj) return 0;
            pObject = &obj;
        }
        return _primitiveObjectValueToNumber<double>(*pObject);
    }

    /** @brief Get char value of object.
     *
     * Returns the current character value of the requested char @a object.
     *
     * @param object - object whose value shall be retrieved
     * @throws Exception if the given object is either invalid, or if the object
     *         is not a char data type.
     */
    char Archive::valueAsChar(const Object& object) {
        if (!object)
            throw Exception("Invalid object");
        if (!object.type().isChar())
            throw Exception("Object is not a char type");
        const Object* pObject = &object;
        if (object.type().isPointer()) {
            const Object& obj = objectByUID(object.uid(1));
            if (!obj) return 0;
            pObject = &obj;
        }
        return _primitiveObjectValueToNumber<char>(*pObject);
    }

    /** @brief Get boolean value of object.
     *
     * Returns the current boolean value of the requested boolean @a object.
     *
     * @param object - object whose value shall be retrieved
     * @throws Exception if the given object is either invalid, or if the object
     *         is not a boolean data type.
     */
    bool Archive::valueAsBool(const Object& object) {
        if (!object)
            throw Exception("Invalid object");
        if (!object.type().isBool())
            throw Exception("Object is not a bool type");
        const Object* pObject = &object;
        if (object.type().isPointer()) {
            const Object& obj = objectByUID(object.uid(1));
            if (!obj) return 0;
            pObject = &obj;
        }
        return _primitiveObjectValueToNumber<bool>(*pObject);
    }

    Archive::operation_t Archive::operation() const {
        return m_operation;
    }

    // *************** Archive::Syncer ***************
    // *

    Archive::Syncer::Syncer(Archive& dst, Archive& src)
       : m_dst(dst), m_src(src)
    {
        const Object srcRootObj = src.rootObject();
        const Object dstRootObj = dst.rootObject();
        if (!srcRootObj)
            throw Exception("No source root object!");
        if (!dstRootObj)
            throw Exception("Expected destination root object not found!");
        syncObject(dstRootObj, srcRootObj);
    }

    void Archive::Syncer::syncPrimitive(const Object& dstObj, const Object& srcObj) {
        assert(srcObj.rawData().size() == dstObj.type().size());
        void* pDst = (void*)dstObj.uid().id;
        memcpy(pDst, &srcObj.rawData()[0], dstObj.type().size());
    }

    void Archive::Syncer::syncString(const Object& dstObj, const Object& srcObj) {
        assert(dstObj.type().isString());
        assert(dstObj.type() == srcObj.type());
        String* pDst = (String*)(void*)dstObj.uid().id;
        *pDst = (String) (const char*) &srcObj.rawData()[0];
    }

    void Archive::Syncer::syncArray(const Object& dstObj, const Object& srcObj) {
        assert(dstObj.type().isArray());
        assert(dstObj.type() == srcObj.type());
        dstObj.m_sync(const_cast<Object&>(dstObj), srcObj, this);
    }

    void Archive::Syncer::syncSet(const Object& dstObj, const Object& srcObj) {
        assert(dstObj.type().isSet());
        assert(dstObj.type() == srcObj.type());
        dstObj.m_sync(const_cast<Object&>(dstObj), srcObj, this);
    }

    void Archive::Syncer::syncMap(const Object& dstObj, const Object& srcObj) {
        assert(dstObj.type().isMap());
        assert(dstObj.type() == srcObj.type());
        dstObj.m_sync(const_cast<Object&>(dstObj), srcObj, this);
    }

    void Archive::Syncer::syncPointer(const Object& dstObj, const Object& srcObj) {
        assert(dstObj.type().isPointer());
        assert(dstObj.type() == srcObj.type());
        void** ppDst = (void**)dstObj.uid().id;
        if (!srcObj.uid(1)) { // NULL pointer on source side ...
            *ppDst = NULL;
            return;
        }
        const Object& pointedSrcObject = m_src.m_allObjects[srcObj.uid(1)];
        assert(pointedSrcObject);
        std::map<UID,UID>::iterator uidRelation = m_counterparts.find(srcObj.uid(1));
        if (pointedSrcObject.parentUID() || uidRelation != m_counterparts.end()) { // "weak" pointer to object ...
            assert(uidRelation != m_counterparts.end());
            const Object& pointedDstObject = m_dst.m_allObjects[uidRelation->second];
            assert(pointedDstObject);
            *ppDst = (void*)pointedDstObject.uid().id;
        } else { // "strong" pointer to object, allocation required ...
            assert(pointedSrcObject.type());
            Object pointedDstObject = pointedSrcObject.type().newInstance(&m_dst);
            assert(pointedDstObject);

            m_dst.m_allObjects[pointedDstObject.uid()] = pointedDstObject;
            *ppDst = (void*)pointedDstObject.uid().id;
            syncObject(pointedDstObject, pointedSrcObject);
        }
    }

    void Archive::Syncer::syncObject(const Object& dstObj, const Object& srcObj) {
        assert(dstObj && srcObj);

        // remember UID mapping between source and destination side
        //
        // As UIDs differ between source archive side and destination archive
        // side (as UIDs always translate to native data's real memory address),
        // remember their relationship, such that we can later on translate
        // objects from source archive side correctly to objects on destination
        // side, which is required to sync native pointers correctly for
        // pointing to respective correct, real memory address.
        const bool alreadySynced =
            !m_counterparts.insert({srcObj.uid(), dstObj.uid()}).second;

        // Prevent syncing this object again, and thus also prevent endless
        // loop on data structures with cyclic relations.
        if (alreadySynced)
            return; // end of recursion

        if (dstObj.type() != srcObj.type())
            throw Exception("Incompatible data structure type (destination type " +
                            dstObj.type().asLongDescr() + " vs. source type " +
                            srcObj.type().asLongDescr() + ")");
        if (!dstObj.isVersionCompatibleTo(srcObj))
            throw Exception("Version incompatible (destination version " +
                            ToString(dstObj.version()) + " [min. version " +
                            ToString(dstObj.minVersion()) + "], source version " +
                            ToString(srcObj.version()) + " [min. version " +
                            ToString(srcObj.minVersion()) + "])");

        if (dstObj.type().isPrimitive() && !dstObj.type().isPointer()) {
            if (dstObj.type().isString())
                syncString(dstObj, srcObj);
            else
                syncPrimitive(dstObj, srcObj);
            return; // end of recursion
        }

        if (dstObj.type().isArray()) {
            syncArray(dstObj, srcObj);
            return;
        }

        if (dstObj.type().isSet()) {
            syncSet(dstObj, srcObj);
            return;
        }

        if (dstObj.type().isMap()) {
            syncMap(dstObj, srcObj);
            return;
        }

        if (dstObj.type().isPointer()) {
            syncPointer(dstObj, srcObj);
            return;
        }

        assert(dstObj.type().isClass());
        for (int iMember = 0; iMember < srcObj.members().size(); ++iMember) {
            const Member& srcMember = srcObj.members()[iMember];
            Member dstMember = dstMemberMatching(dstObj, srcObj, srcMember);
            if (!dstMember)
                throw Exception("Expected member missing in destination object");
            syncMember(dstMember, srcMember);
        }
    }

    Member Archive::Syncer::dstMemberMatching(const Object& dstObj, const Object& srcObj, const Member& srcMember) {
        Member dstMember = dstObj.memberNamed(srcMember.name());
        if (dstMember)
            return (dstMember.type() == srcMember.type()) ? dstMember : Member();
        std::vector<Member> members = dstObj.membersOfType(srcMember.type());
        if (members.size() <= 0)
            return Member();
        if (members.size() == 1)
            return members[0];
        for (int i = 0; i < members.size(); ++i)
            if (members[i].offset() == srcMember.offset())
                return members[i];
        const int srcSeqNr = srcObj.sequenceIndexOf(srcMember);
        assert(srcSeqNr >= 0); // should never happen, otherwise there is a bug
        for (int i = 0; i < members.size(); ++i) {
            const int dstSeqNr = dstObj.sequenceIndexOf(members[i]);
            if (dstSeqNr == srcSeqNr)
                return members[i];
        }
        return Member(); // give up!
    }

    void Archive::Syncer::syncMember(const Member& dstMember, const Member& srcMember) {
        assert(dstMember && srcMember);
        assert(dstMember.type() == srcMember.type());
        const Object dstObj = m_dst.m_allObjects[dstMember.uid()];
        const Object srcObj = m_src.m_allObjects[srcMember.uid()];
        syncObject(dstObj, srcObj);
    }

    // *************** Exception ***************
    // *

    Exception::Exception() {
    }

    Exception::Exception(String format, ...) {
        va_list arg;
        va_start(arg, format);
        Message = assemble(format, arg);
        va_end(arg);
    }

    Exception::Exception(String format, va_list arg) {
        Message = assemble(format, arg);
    }

    /** @brief Print exception message to stdout.
     *
     * Prints the message of this Exception to the currently defined standard
     * output (that is to the terminal console for example).
     */
    void Exception::PrintMessage() const {
        std::cout << "Serialization::Exception: " << Message << std::endl;
    }

    String Exception::assemble(String format, va_list arg) {
        char* buf = NULL;
        vasprintf(&buf, format.c_str(), arg);
        String s = buf;
        free(buf);
        return s;
    }

} // namespace Serialization
